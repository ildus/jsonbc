#include "jsonbc.h"

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_compression_opt.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"

static bool						xact_started = false;
static bool						shutdown_requested = false;
static jsonbc_shm_worker	   *worker_state;
static MemoryContext			worker_context = NULL;

Oid jsonbc_dictionary_reloid	= InvalidOid;
Oid	jsonbc_keys_indoid			= InvalidOid;
Oid	jsonbc_id_indoid			= InvalidOid;

void worker_main(Datum arg);
static Oid jsonbc_get_dictionary_relid(void);

#define JSONBC_DICTIONARY_REL	"jsonbc_dictionary"

static const char *sql_dictionary = \
	"CREATE TABLE public." JSONBC_DICTIONARY_REL
	" (cmopt	OID NOT NULL,"
	"  id		INT4 NOT NULL,"
	"  key		TEXT NOT NULL);"
	"CREATE UNIQUE INDEX jsonbc_dict_on_id ON " JSONBC_DICTIONARY_REL "(cmopt, id);"
	"CREATE UNIQUE INDEX jsonbc_dict_on_key ON " JSONBC_DICTIONARY_REL " (cmopt, key);";

enum {
	JSONBC_DICTIONARY_REL_ATT_CMOPT = 1,
	JSONBC_DICTIONARY_REL_ATT_ID,
	JSONBC_DICTIONARY_REL_ATT_KEY,
	JSONBC_DICTIONARY_REL_ATT_COUNT
};

/*
 * Handle SIGTERM in BGW's process.
 */
static void
handle_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	shutdown_requested = true;

	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

static void
init_local_variables(int worker_num)
{
	shm_toc		   *toc = shm_toc_attach(JSONBC_SHM_MQ_MAGIC, workers_data);
	jsonbc_shm_hdr *hdr = shm_toc_lookup(toc, 0, false);
	hdr->workers_ready++;

	worker_state = shm_toc_lookup(toc, worker_num + 1, false);
	worker_state->proc = MyProc;

	/* input mq */
	shm_mq_set_receiver(worker_state->mqin, MyProc);

	/* output mq */
	shm_mq_set_sender(worker_state->mqout, MyProc);

	/* not busy at start */
	pg_atomic_clear_flag(&worker_state->busy);

	elog(LOG, "jsonbc dictionary worker %d started with pid: %d",
			worker_num + 1, MyProcPid);
}

static void
start_xact_command(void)
{
	if (IsTransactionState())
		return;

	if (!xact_started)
	{
		ereport(DEBUG3,
				(errmsg_internal("StartTransactionCommand")));
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		xact_started = true;
	}
}

static void
finish_xact_command(void)
{
	if (xact_started)
	{
		/* Now commit the command */
		ereport(DEBUG3,
				(errmsg_internal("CommitTransactionCommand")));

		PopActiveSnapshot();
		CommitTransactionCommand();
		xact_started = false;
	}
}

void
jsonbc_register_worker(int n)
{
	BackgroundWorker worker;

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
					   BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = 0;
	worker.bgw_notify_pid = 0;
	memcpy(worker.bgw_library_name, "jsonbc", BGW_MAXLEN);
	memcpy(worker.bgw_function_name, CppAsString(worker_main), BGW_MAXLEN);
	snprintf(worker.bgw_name, BGW_MAXLEN, "jsonbc dictionary worker %d", n + 1);
	worker.bgw_main_arg = (Datum) Int32GetDatum(n);
	RegisterBackgroundWorker(&worker);
}

/* Returns buffers with keys ordered by ids */
static char **
jsonbc_get_keys_slow(Oid cmoptoid, uint32 *ids, int nkeys)
{
	MemoryContext	old_mcxt;
	int				i;
	char		  **keys;

	Oid			relid = jsonbc_get_dictionary_relid();
	Relation	rel,
				idxrel;

	start_xact_command();

	rel = relation_open(relid, AccessShareLock);
	idxrel = index_open(jsonbc_id_indoid, AccessShareLock);

	keys = (char **) MemoryContextAlloc(worker_context,
										sizeof(char *) * nkeys);

	for (i = 0; i < nkeys; i++)
	{
		IndexScanDesc	scan;
		ScanKeyData		skey[2];
		Datum			key_datum;
		HeapTuple		tup;
		bool			isNull;

		ScanKeyInit(&skey[0],
					JSONBC_DICTIONARY_REL_ATT_CMOPT,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(cmoptoid));
		ScanKeyInit(&skey[1],
					JSONBC_DICTIONARY_REL_ATT_ID,
					BTEqualStrategyNumber,
					F_INT4EQ,
					Int32GetDatum(ids[i]));

		scan = index_beginscan(rel, idxrel, SnapshotAny, 2, 0);
		index_rescan(scan, skey, 2, NULL, 0);

		tup = index_getnext(scan, ForwardScanDirection);
		if (tup == NULL)
			elog(ERROR, "key not found for cmopt=%d and id=%d", cmoptoid, ids[i]);

		key_datum = heap_getattr(tup, JSONBC_DICTIONARY_REL_ATT_KEY,
							  RelationGetDescr(rel), &isNull);
		Assert(isNull == false);

		old_mcxt = MemoryContextSwitchTo(worker_context);
		keys[i] = TextDatumGetCString(key_datum);
		MemoryContextSwitchTo(old_mcxt);

		index_endscan(scan);
	}

	index_close(idxrel, AccessShareLock);
	relation_close(rel, AccessShareLock);

	finish_xact_command();
	return keys;
}

static void
jsonbc_bulk_insert_keys(Oid cmoptoid, char *buf, uint32 *idsbuf, int nkeys)
{
	Oid		relid = jsonbc_get_dictionary_relid();
	Relation rel;

	int			i,
				counter,
				hi_options;
	HeapTuple  *buffered;
	Datum		values[JSONBC_DICTIONARY_REL_ATT_COUNT];
	Datum		nulls[JSONBC_DICTIONARY_REL_ATT_COUNT];
	BulkInsertState	bistate;
	TupleTableSlot	*myslot;
	ResultRelInfo *resultRelInfo;
	ExprContext *econtext;
	EState	   *estate = CreateExecutorState();
	MemoryContext oldcontext = CurrentMemoryContext;

	start_xact_command();
	rel = heap_open(relid, RowExclusiveLock);
	bistate = GetBulkInsertState();
	myslot = MakeTupleTableSlot();
	ExecSetSlotDescriptor(myslot, RelationGetDescr(rel));

	/* we need resultRelInfo to insert to indexes */
	resultRelInfo = makeNode(ResultRelInfo);
	InitResultRelInfo(resultRelInfo, rel, 1, NULL, 0);

	ExecOpenIndices(resultRelInfo, false);
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* only one process can insert to dictionary at same time */
	LockDatabaseObject(relid, cmoptoid, 0, ExclusiveLock);

	buffered = palloc(sizeof(HeapTuple) * nkeys);
	for (i = 0; i < nkeys; i++)
	{
		HeapTuple	tuple;

		values[JSONBC_DICTIONARY_REL_ATT_CMOPT - 1] = ObjectIdGetDatum(cmoptoid);
		values[JSONBC_DICTIONARY_REL_ATT_ID - 1] = Int32GetDatum(counter++);
		values[JSONBC_DICTIONARY_REL_ATT_KEY - 1] = CStringGetTextDatum(buf);

		tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		tuple->t_tableOid = relid;

		buffered[i] = tuple;

		/* move to next key */
		while (*buf != '\0')
			buf++;

		buf++;
	}

	if (!XLogIsNeeded())
		hi_options |= HEAP_INSERT_SKIP_WAL;

	heap_multi_insert(rel,
					  buffered,
					  nkeys,
					  mycid,
					  hi_options,
					  bistate);

	/* update indexes */
	for (i = 0; i < nkeys; i++)
	{
		List	   *recheckIndexes;

		ExecStoreTuple(buffered[i], myslot, InvalidBuffer, false);
		recheckIndexes =
			ExecInsertIndexTuples(myslot, &(buffered[i]->t_self),
								  estate, false, NULL, NIL);
		list_free(recheckIndexes);
	}

	UnlockDatabaseObject(relid, cmoptoid, 0, ExclusiveLock);

	FreeBulkInsertState(bistate);
	heap_close(rel, RowExclusiveLock);
	finish_xact_command();
}

/*
 * Get key IDs using relation
 * TODO: change to direct access
 */
static void
jsonbc_get_key_ids_slow(Oid cmoptoid, char *buf, uint32 *idsbuf, int nkeys)
{
	Relation	rel;

	int		i;
	Oid		relid = jsonbc_get_dictionary_relid();

	start_xact_command();
	rel = relation_open(relid, ShareLock);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	for (i = 0; i < nkeys; i++)
	{
		Datum		datum;
		bool		isnull;
		char	   *sql;

		sql = psprintf("SELECT id FROM public.jsonbc_dictionary WHERE cmopt = %d"
					   "	AND key = '%s'", cmoptoid, buf);

		if (SPI_exec(sql, 0) != SPI_OK_SELECT)
			elog(ERROR, "SPI_exec failed");

		pfree(sql);

		if (SPI_processed == 0)
		{
			char *sql2 = psprintf("with t as (select (coalesce(max(id), 0) + 1) new_id from "
						"public.jsonbc_dictionary where cmopt = %d) insert into public.jsonbc_dictionary"
						" select %d, t.new_id, '%s' from t returning id",
						cmoptoid, cmoptoid, buf);

			if (SPI_exec(sql2, 0) != SPI_OK_INSERT_RETURNING)
				elog(ERROR, "SPI_exec failed");

			pfree(sql2);
		}

		datum = SPI_getbinval(SPI_tuptable->vals[0],
							  SPI_tuptable->tupdesc,
							  1,
							  &isnull);
		if (isnull)
			elog(ERROR, "id is NULL");

		idsbuf[i] = DatumGetInt32(datum);

		/* move to next key */
		while (*buf != '\0')
			buf++;

		buf++;
	}
	SPI_finish();
	relation_close(rel, ShareLock);
	finish_xact_command();
}

static char *
jsonbc_cmd_get_ids(int nkeys, Oid cmoptoid, char *buf, size_t *buflen)
{
	uint32		   *idsbuf;
	MemoryContext	old_mcxt = CurrentMemoryContext;;

	*buflen = nkeys * sizeof(uint32);
	idsbuf = (uint32 *) palloc(*buflen);

	PG_TRY();
	{
		start_xact_command();
		jsonbc_get_key_ids_slow(cmoptoid, buf, idsbuf, nkeys);
		finish_xact_command();
	}
	PG_CATCH();
	{
		ErrorData  *error;
		MemoryContextSwitchTo(old_mcxt);
		error = CopyErrorData();
		elog(LOG, "jsonbc: error occured: %s", error->message);
		FlushErrorState();
		pfree(error);

		idsbuf[0] = 0;
		*buflen = 1;
	}
	PG_END_TRY();

	return (char *) idsbuf;
}

static char **
jsonbc_cmd_get_keys(int nkeys, Oid cmoptoid, uint32 *ids)
{
	char		  **keys = NULL;
	MemoryContext	mcxt = CurrentMemoryContext;;

	PG_TRY();
	{
		keys = jsonbc_get_keys_slow(cmoptoid, ids, nkeys);
	}
	PG_CATCH();
	{
		ErrorData  *error;
		MemoryContextSwitchTo(mcxt);
		error = CopyErrorData();
		elog(LOG, "jsonbc: error occured: %s", error->message);
		FlushErrorState();
		pfree(error);
	}
	PG_END_TRY();

	return keys;
}

void
worker_main(Datum arg)
{
	shm_mq_handle  *mqh = NULL;

	/* Establish signal handlers before unblocking signals */
	pqsignal(SIGTERM, handle_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection("postgres", NULL);

	/* Create resource owner */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "jsonbc_worker");
	init_local_variables(DatumGetInt32(arg));

	worker_context = AllocSetContextCreate(TopMemoryContext,
										   "jsonbc worker context",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(worker_context);

	while (true)
	{
		int		rc;
		Size	nbytes;
		void   *data;

		shm_mq_result	resmq;

		if (!mqh)
			mqh = shm_mq_attach(worker_state->mqin, NULL, NULL);

		resmq = shm_mq_receive(mqh, &nbytes, &data, true);

		if (resmq == SHM_MQ_SUCCESS)
		{
			JsonbcCommand	cmd;
			Oid				cmoptoid;
			shm_mq_iovec   *iov = NULL;
			char		   *ptr = data;
			int				nkeys = *((int *) ptr);
			size_t			iovlen;

			ptr += sizeof(int);
			cmoptoid = *((Oid *) ptr);
			ptr += sizeof(Oid);
			cmd = *((JsonbcCommand *) ptr);
			ptr += sizeof(JsonbcCommand);

			switch (cmd)
			{
				case JSONBC_CMD_GET_IDS:
					iov = (shm_mq_iovec *) palloc(sizeof(shm_mq_iovec));
					iovlen = 1;
					iov->data = jsonbc_cmd_get_ids(nkeys, cmoptoid, ptr, &iov->len);
					break;
				case JSONBC_CMD_GET_KEYS:
				{
					char **keys = jsonbc_cmd_get_keys(nkeys, cmoptoid, (uint32 *) ptr);
					if (keys != NULL)
					{
						int i;

						iov = (shm_mq_iovec *) palloc(sizeof(shm_mq_iovec) * nkeys);
						iovlen = nkeys;
						for (i = 0; i < nkeys; i++)
						{
							iov[i].data = keys[i];
							iov[i].len = strlen(keys[i]) + 1;
						}
					}

					break;
				}
				default:
					elog(NOTICE, "jsonbc: got unknown command");
			}

			shm_mq_detach(mqh);
			mqh = shm_mq_attach(worker_state->mqout, NULL, NULL);

			if (iov != NULL)
				resmq = shm_mq_sendv(mqh, iov, iovlen, false);
			else
				resmq = shm_mq_sendv(mqh, &((shm_mq_iovec) {"\0", 1}), 1, false);

			if (resmq != SHM_MQ_SUCCESS)
				elog(NOTICE, "jsonbc: backend detached early");

			shm_mq_detach(mqh);
			MemoryContextReset(worker_context);

			/* mark we need new handle */
			mqh = NULL;
		}

		if (shutdown_requested)
			break;

		rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH,
			0, PG_WAIT_EXTENSION);

		if (rc & WL_POSTMASTER_DEATH)
			break;

		ResetLatch(&MyProc->procLatch);
	}

	elog(LOG, "jsonbc dictionary worker has ended its work");
	proc_exit(0);
}

static Oid
jsonbc_get_dictionary_relid(void)
{
	Oid relid,
		nspoid;

	if (OidIsValid(jsonbc_dictionary_reloid))
		return jsonbc_dictionary_reloid;

	start_xact_command();

	nspoid = get_namespace_oid("public", false);
	relid = get_relname_relid(JSONBC_DICTIONARY_REL, nspoid);
	if (relid == InvalidOid)
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");

		if (SPI_execute(sql_dictionary, false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not create \"jsonbc\" dictionary");

		SPI_finish();
		CommandCounterIncrement();

		finish_xact_command();
		start_xact_command();

		/* get just created table Oid */
		relid = get_relname_relid(JSONBC_DICTIONARY_REL, nspoid);
		jsonbc_id_indoid = InvalidOid;
		jsonbc_keys_indoid = InvalidOid;
	}

	/* fill index Oids too */
	if (jsonbc_id_indoid == InvalidOid)
	{
		Relation	 rel;
		ListCell	*lc;
		List		*indexes;

		Assert(relid != InvalidOid);

		rel = relation_open(relid, NoLock);
		indexes = RelationGetIndexList(rel);
		Assert(list_length(indexes) == 2);

		foreach(lc, indexes)
		{
			Oid			indOid = lfirst_oid(lc);
			Relation	indRel = index_open(indOid, NoLock);
			int			attnum = indRel->rd_index->indkey.values[1];

			if (attnum == JSONBC_DICTIONARY_REL_ATT_ID)
				jsonbc_id_indoid = indOid;
			else
			{
				Assert(attnum == JSONBC_DICTIONARY_REL_ATT_KEY);
				jsonbc_keys_indoid = indOid;
			}

			index_close(indRel, NoLock);
		}
		relation_close(rel, NoLock);
	}

	finish_xact_command();

	/* check we did fill global variables */
	Assert(OidIsValid(jsonbc_id_indoid));
	Assert(OidIsValid(jsonbc_keys_indoid));
	Assert(OidIsValid(relid));

	jsonbc_dictionary_reloid = relid;
	return relid;
}
