# jsonbc

JSONB compression method for PostgreSQL

## Using

To use it the following patch should be applied to postgresql (git:master):

https://commitfest.postgresql.org/15/1294/

And something like this:

```
CREATE EXTENSION jsonbc;
CREATE COMPRESSION METHOD cm1 HANDLER jsonbc_compression_handler;
CREATE TABLE t(a JSONB);
ALTER TABLE t ALTER COLUMN a SET COMPRESSED cm1;
```
