# VillageSQL DuckDB Extension

Aggregate Parquet, CSV and JSON files held in object storage from an ordinary
MySQL connection, without exporting the data first or running a second query
engine beside the database.

```sql
INSTALL EXTENSION vsql_duckdb;

SELECT duckdb_scalar('SELECT count(*) FROM read_parquet(''s3://sales/2026/*.parquet'')');
```

The extension embeds DuckDB inside the VillageSQL server process. Your SQL text
is handed to DuckDB unchanged, so DuckDB plans the query itself and skips the
row groups and columns it never needs to read.

This is a port of the PostgreSQL extension
[pg_duckdb](https://github.com/duckdb/pg_duckdb), narrowed to what the
VillageSQL Extension Framework supports today. See
[Migrating from PostgreSQL](#migrating-from-postgresql) for the mapping, and
[Known Limitations](#known-limitations) for what is missing and why.

## Contents

- [Building](#building)
- [Installing](#installing)
- [Configuration](#configuration)
- [Function Reference](#function-reference)
- [Returning more than one megabyte](#returning-more-than-one-megabyte)
- [Adding more readers](#adding-more-readers)
- [Migrating from PostgreSQL](#migrating-from-postgresql)
- [Known Limitations](#known-limitations)
- [Security Considerations](#security-considerations)
- [Testing](#testing)
- [Contributing](#contributing)
- [Reporting Bugs and Requesting Features](#reporting-bugs-and-requesting-features)
- [Contact](#contact)
- [License](#license)

## Building

The extension is written in C++ because it is a thin adapter over DuckDB's own
C++ objects. `AGENTS.md` records that decision in full.

DuckDB is built from source as part of this build. That takes a few minutes the
first time and is then reused. Nothing is downloaded at run time.

### What you need

| | |
|---|---|
| A VillageSQL build or dev server, 0.0.6 or newer | |
| A C++17 compiler and CMake 3.18 or newer | |
| OpenSSL and libcurl development headers | DuckDB's HTTP reader uses both |
| Git | DuckDB is fetched from its own repository at a pinned tag |

Debian and Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git libssl-dev libcurl4-openssl-dev
```

RHEL and Fedora:

```bash
sudo dnf install -y gcc-c++ cmake git openssl-devel libcurl-devel
```

macOS, with Homebrew:

```bash
brew install cmake openssl@3
```

### Build

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
./build.sh
```

Or with CMake directly, which is what `build.sh` runs:

```bash
cmake -S . -B build -DVillageSQL_BUILD_DIR="$VillageSQL_BUILD_DIR"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"
cmake --install build
```

On macOS add `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3` to the first
command.

`cmake --install` writes `vsql_duckdb.veb` into the server's VEB directory. Ask
the server where that is with `SHOW VARIABLES LIKE 'veb_dir'`.

### Build options

| Option | Default | Meaning |
|---|---|---|
| `DUCKDB_TAG` | `v1.5.5` | The DuckDB release to build against |
| `DUCKDB_READERS` | `httpfs;json` | Which DuckDB readers to link in. `parquet` and `core_functions` are always present |
| `DUCKDB_PREBUILT_ROOT` | empty | Reuse a DuckDB tree built earlier with the same options, instead of building one |
| `VCPKG_TOOLCHAIN_PATH` | empty | Needed only by readers that depend on vcpkg packages |

## Installing

The extension declares two preview capabilities, so the server must allow
preview extensions. `SET PERSIST` takes effect immediately and survives a
restart:

```sql
SET PERSIST vsql_allow_preview_extensions = ON;
INSTALL EXTENSION vsql_duckdb;
```

Check what you got:

```sql
SELECT duckdb_status();
```

```json
{"duckdb_version":"v1.5.5","readers":["core_functions","httpfs","json","parquet"],
 "engine_error":"","object_storage_credential":"not configured", ...}
```

## Configuration

Every setting is a system variable under `vsql_duckdb.`. Use `SET PERSIST` so
the value survives a restart.

### Object storage

| Variable | Default | Meaning |
|---|---|---|
| `s3_region` | empty | AWS region for `s3://` paths |
| `s3_endpoint` | empty | Endpoint host, and port if it is not the default. Empty means AWS S3. Use `storage.googleapis.com` for Google Cloud Storage |
| `s3_url_style` | `vhost` | `vhost` or `path`. MinIO and most S3-compatible stores want `path` |
| `s3_use_ssl` | `ON` | Reach the endpoint over HTTPS |
| `s3_key_id` | empty | Access key id. Not a secret, so it lives here |
| `s3_secret_keyring_id` | empty | The keyring data id holding the secret access key |
| `s3_secret_keyring_auth_id` | empty | Which keyring owner holds that secret. See below |

The secret access key never appears in SQL and is never written to disk. It
comes from the server's keyring, and these variables only name where to find
it.

The server needs a keyring component for any of this; without one, leave
`s3_key_id` empty and public buckets are still readable. Storing the secret
from SQL needs the `keyring_udf` plugin:

```sql
SELECT keyring_key_store('duckdb_s3_secret', 'AES', 'the-secret-access-key');
```

**A key stored that way belongs to the account that stored it**, so name that
account in `s3_secret_keyring_auth_id`. Leaving it empty asks for an internal
key, which is what a key provisioned outside SQL is, and a key stored by
`keyring_key_store` will not be found:

```sql
SET PERSIST vsql_duckdb.s3_region = 'eu-north-1';
SET PERSIST vsql_duckdb.s3_key_id = 'AKIAEXAMPLE';
SET PERSIST vsql_duckdb.s3_secret_keyring_id = 'duckdb_s3_secret';
SET PERSIST vsql_duckdb.s3_secret_keyring_auth_id = 'root@localhost';
```

The owner is the full `user@host` form. `root` and `root@%` do not match a key
stored by `root@localhost`.

`SELECT duckdb_status()` reports whether the credential loaded, and says so
when it did not.

Google Cloud Storage is reached through the same variables, using a
[GCS HMAC key](https://cloud.google.com/storage/docs/authentication/hmackeys):

```sql
SET PERSIST vsql_duckdb.s3_endpoint = 'storage.googleapis.com';
SET PERSIST vsql_duckdb.s3_url_style = 'path';
SET PERSIST vsql_duckdb.s3_key_id = 'GOOG1E...';        -- the HMAC access id
SET PERSIST vsql_duckdb.s3_secret_keyring_id = 'duckdb_gcs_secret';
```

Paths still start with `s3://`. That configuration was checked as far as
Google answering the request; the HMAC key itself was not exercised.

### Limits

| Variable | Default | Range | Meaning |
|---|---|---|---|
| `memory_limit_mb` | `1024` | 64 – 1048576 | Memory DuckDB may use |
| `threads` | `2` | 1 – 1024 | Worker threads DuckDB may start |
| `timeout_ms` | `30000` | 0 – 3600000 | Deadline for one query. `0` means no deadline |
| `max_result_bytes` | `1048576` | 1024 – 16777216 | Largest result a query may return |

DuckDB starts its own worker threads and holds its own memory. The server
neither schedules the first nor accounts for the second, so both belong under
limits you set.

### Access

| Variable | Default | Meaning |
|---|---|---|
| `enable_external_access` | `ON` | Let DuckDB reach anything outside the server process. Turning this off also stops object storage reads |
| `allow_local_files` | `OFF` | Let queries read files on the server host |
| `temp_directory` | empty | Where DuckDB spills. Only used when `allow_local_files` is `ON` |

Reading local files and spilling to disk are one switch in DuckDB, so
`allow_local_files = OFF` also stops DuckDB spilling. A query that needs more
than `memory_limit_mb` then fails rather than writing to the server's
filesystem. Turn it on if you want either.

**Changing any variable except `timeout_ms` and `max_result_bytes` rebuilds the
DuckDB instance on the next query.** That is deliberate: those settings are
fixed when the instance starts and the instance is then locked, so a caller
cannot loosen them from inside their own query text.

## Function Reference

All three functions block the connection that called them while DuckDB works,
and for object storage that means waiting on the network. A call costs whatever
the query costs — reading a remote Parquet file is seconds, not microseconds —
so these belong in a statement a person or a job is waiting on, not in a
`WHERE` clause evaluated per row. `vsql_duckdb.timeout_ms` bounds the wait.

### `duckdb_query(sql VARCHAR) -> VARCHAR`

Runs one DuckDB statement and returns the whole result as a JSON array, one
object per row, keys in column order.

```sql
SELECT duckdb_query('SELECT city, sum(n) AS total FROM read_parquet(''s3://b/s.parquet'')
                     GROUP BY city ORDER BY city');
```

```json
[{"city":"bergen","total":5},{"city":"oslo","total":30}]
```

Integers, decimals, floats and booleans keep their JSON types. Every other
value, including dates, timestamps, blobs, lists and structs, becomes a JSON
string of the text DuckDB printed. A float holding `nan` or `inf` becomes a
string, because JSON cannot spell either.

The result is `utf8mb4`, so MySQL's JSON functions read it with no cast:

```sql
SELECT t.city, t.total
FROM JSON_TABLE(duckdb_query('SELECT city, sum(n) AS total FROM read_parquet(''s3://b/s.parquet'')
                              GROUP BY city'),
     '$[*]' COLUMNS (city VARCHAR(64) PATH '$.city', total BIGINT PATH '$.total')) AS t;
```

- A NULL argument returns NULL, without reaching DuckDB.
- A query matching no rows returns `[]`.
- Two columns with the same name produce two keys with that name in each
  object. That parses, but which one a JSON reader gives you back is its
  choice. Alias them apart.
- A query DuckDB rejects raises an error. It does not return NULL — see
  [Errors, not NULL](#errors-not-null).
- Only one statement per call. A second statement is rejected.

### `duckdb_scalar(sql VARCHAR) -> VARCHAR`

Runs one DuckDB statement and returns the first column of the first row as
text. This is the counts-and-sums path, with no JSON to unwrap.

```sql
SELECT duckdb_scalar('SELECT count(*) FROM read_parquet(''s3://sales/*.parquet'')');
```

- A NULL argument returns NULL.
- A query matching no rows returns NULL.
- A NULL value in the first column returns NULL.
- A query DuckDB rejects raises an error.

### `duckdb_status() -> VARCHAR`

Returns a JSON object describing the engine and its settings. It answers two
questions `SHOW VARIABLES` cannot: which readers this bundle was built with,
and whether the object storage credential loaded.

| Key | Meaning |
|---|---|
| `duckdb_version` | The DuckDB release compiled in |
| `readers` | The readers linked into this bundle |
| `engine_error` | Empty when DuckDB started and locked itself cleanly |
| `object_storage_credential` | `not configured`, or the key id that loaded. Never the secret |

The remaining keys echo the settings above. Never the secret access key.

### Errors, not NULL

A query DuckDB rejects raises `ERROR 3200` rather than returning NULL. This
differs from the usual MySQL habit of returning NULL for bad input, on purpose:
NULL is a legitimate answer here, because `max()` over an empty file is NULL. A
caller who got NULL could not tell an empty bucket from a missing one.

The DuckDB message comes through in the error, so a wrong path, a denied
bucket or a bad function name each say so.

## Returning more than one megabyte

`duckdb_query` refuses a result larger than one megabyte:

```
ERROR 3200 (HY000): VDF error in function 'duckdb_query': vsql_duckdb: result
reached 1048576 bytes at row 8748, the limit set by vsql_duckdb.max_result_bytes.
Narrow the query or add a LIMIT
```

Raising `vsql_duckdb.max_result_bytes` alone does not lift it. The extension is
built against the stable extension SDK, whose result writer copies what fits
and reports only that much, so the server is never told the value overflowed
and cannot grow the buffer. A result past the buffer would come back cut, with
no warning. The extension refuses instead, because a cut JSON array loses its
closing bracket and no longer parses.

The development SDK's writer reports the full size, and the server then grows
the buffer and calls the function again. Measured on
`mysql-8.4_0.0.7-dev`, same server, one probe extension built each way against
a 1 KiB buffer: the development build returned 5000 bytes after two calls with
the buffer grown to 5120, and the stable build returned 1024 bytes after one
call. A 20 MB result came back whole on the development build.

So the way to lift the ceiling is to build against the development ABI. That
is a real decision, not a flag: the development ABI is shipped and tested, but
it is not the stable surface, and an extension built against it needs a server
that matches.

To take that route, point the include path at the SDK's `include-dev` tree
ahead of `include`, and raise the upper bound of `max_result_bytes` in
`src/settings.h`. Then confirm on your target server that a large result comes
back whole:

```sql
SELECT JSON_VALID(duckdb_query('SELECT i, repeat(''x'',100) AS pad FROM range(20000) t(i)'));
```

Before doing any of that, consider whether the query should return fewer rows.
This is built for counts, sums and low-cardinality rollups, and a result of
that shape does not approach the limit.

### Storing a result in a table

`CREATE TABLE ... AS SELECT duckdb_query(...)` fails for all but the smallest
results, on either ABI:

```
ERROR 1406 (22001): Data too long for column 'v' at row 1
```

The column is sized from the width of the SQL text you passed in, not from the
result. Measured: a query whose text was 112 characters produced
`varchar(112)`, so only a result shorter than the query that made it will
store. It is an honest error rather than a silent truncation, but it means the
result has to be consumed where it is produced.

Unpack it into real columns instead of storing the array:

```sql
CREATE TABLE totals AS
SELECT t.city, t.total
FROM JSON_TABLE(duckdb_query('SELECT city, sum(n) AS total FROM read_parquet(''s3://b/*.parquet'') GROUP BY city'),
     '$[*]' COLUMNS (city VARCHAR(64) PATH '$.city', total BIGINT PATH '$.total')) AS t;
```

Declaring `.max_result_length()` on the function would widen the column, and
carries the same development-ABI decision as above.

## Adding more readers

`DUCKDB_READERS` takes any DuckDB reader. Some need a Rust toolchain, and some
need vcpkg packages. Measured on ten cores:

| Readers | Cold build | Extra tooling |
|---|---|---|
| `httpfs;json` (the default) | 124 s | none |
| plus `delta;vortex` | 389 s | a Rust toolchain |
| plus `iceberg` | not measured | vcpkg, for `avro-c`, `roaring` and `aws-sdk-cpp` |
| plus `mysql_scanner` | not measured | vcpkg, for `libmariadb` |

Delta and Vortex, with Rust installed:

```bash
cmake -S . -B build -DVillageSQL_BUILD_DIR="$VillageSQL_BUILD_DIR" \
      -DDUCKDB_READERS="httpfs;json;delta;vortex"
```

Iceberg and the MySQL reader additionally need a vcpkg toolchain:

```bash
git clone https://github.com/microsoft/vcpkg /opt/vcpkg
/opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake -S . -B build -DVillageSQL_BUILD_DIR="$VillageSQL_BUILD_DIR" \
      -DDUCKDB_READERS="httpfs;json;iceberg" \
      -DVCPKG_TOOLCHAIN_PATH=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake
```

The `mysql_scanner` reader deserves a caution. It would let a DuckDB query
`ATTACH` this server over its own connection and read your InnoDB tables, which
makes a join between a Parquet file and a table reachable. It comes with real
costs: it is a second client session, so it reads a different snapshot than the
statement that called it, it takes a connection from the pool, and it blocks if
the caller holds locks on the rows it wants. It is not in the default build for
those reasons rather than for build cost.

## Migrating from PostgreSQL

### What replaces what

| pg_duckdb | vsql_duckdb | Note |
|---|---|---|
| `duckdb.query(sql)` | `duckdb_query(sql)` | Returns a JSON array, not a row set. Use `JSON_TABLE` to get rows |
| `duckdb.raw_query(sql)` | `duckdb_query(sql)` | There is no server log to print to, so the result comes back instead |
| `duckdb.recycle_ddb()` | none needed | One instance serves the whole server and nothing accumulates per connection. Changing a setting replaces it |
| `read_parquet`, `read_csv`, `read_json`, `read_text`, `read_blob` | the same names, inside the query text | They are DuckDB functions, not MySQL functions |
| `duckdb.create_simple_secret(...)` | `vsql_duckdb.s3_*` variables plus the keyring | The secret never passes through SQL |
| MAP, union, JSON, time functions, `approx_count_distinct`, `TABLESAMPLE` | the same names, inside the query text | Reachable through `duckdb_query`, not as MySQL functions |
| `duckdb.install_extension`, `load_extension`, `autoload_extension` | none | Refused by design. Readers ship inside the bundle. See Security Considerations |
| `iceberg_scan`, `iceberg_metadata`, `iceberg_snapshots`, `delta_scan`, `read_vortex` | not in the default build | See [Adding more readers](#adding-more-readers) |
| `duckdb.create_azure_secret` | none | The Azure reader is not bundled |
| `duckdb.enable_motherduck` and the other MotherDuck functions | none | Out of scope |
| `duckdb.force_execution` | none | Routing ordinary SQL to DuckDB needs a server hook that does not exist |

### What replaces which setting

| pg_duckdb | vsql_duckdb |
|---|---|
| `duckdb.max_memory` / `duckdb.memory_limit` | `vsql_duckdb.memory_limit_mb` |
| `duckdb.threads` / `duckdb.worker_threads` | `vsql_duckdb.threads` |
| `duckdb.enable_external_access` | `vsql_duckdb.enable_external_access` |
| `duckdb.disabled_filesystems` | `vsql_duckdb.allow_local_files` |
| `duckdb.temporary_directory` | `vsql_duckdb.temp_directory` |
| `duckdb.postgres_role` | none. MySQL grants are the wrong shape for this; see Security Considerations |
| `duckdb.autoinstall_known_extensions`, `autoload_known_extensions`, `allow_community_extensions`, `allow_unsigned_extensions` | none. All four are forced off, and the loader is compiled out |
| `duckdb.max_temp_directory_size`, `duckdb.default_collation`, `duckdb.custom_user_agent`, `duckdb.log_pg_explain`, the MotherDuck settings | none |
| none | `vsql_duckdb.timeout_ms`, `vsql_duckdb.max_result_bytes`, the `s3_*` variables |

### Before and after

Counting rows in a Parquet file:

```sql
-- PostgreSQL
SELECT count(*) FROM read_parquet('s3://sales/2026/*.parquet');

-- VillageSQL
SELECT duckdb_scalar('SELECT count(*) FROM read_parquet(''s3://sales/2026/*.parquet'')');
```

A grouped rollup, as rows:

```sql
-- PostgreSQL
SELECT city, sum(n) FROM read_parquet('s3://sales/*.parquet') GROUP BY city;

-- VillageSQL
SELECT t.city, t.total
FROM JSON_TABLE(
  duckdb_query('SELECT city, sum(n) AS total FROM read_parquet(''s3://sales/*.parquet'')
                GROUP BY city'),
  '$[*]' COLUMNS (city VARCHAR(64) PATH '$.city', total BIGINT PATH '$.total')) AS t;
```

Note the doubled single quotes: the DuckDB query is a MySQL string literal, so
every quote inside it is written twice.

Setting up a credential:

```sql
-- PostgreSQL
SELECT duckdb.create_simple_secret('S3', 'AKIAEXAMPLE', 'the-secret', 'eu-north-1');

-- VillageSQL
SET PERSIST vsql_duckdb.s3_key_id = 'AKIAEXAMPLE';
SET PERSIST vsql_duckdb.s3_secret_keyring_id = 'duckdb_s3_secret';
SET PERSIST vsql_duckdb.s3_region = 'eu-north-1';
```

### Behavioural differences

- **Row sets become JSON.** Every pg_duckdb function that returns
  `SETOF duckdb.row` returns a JSON array here, in the order DuckDB produced
  the rows. `JSON_TABLE` turns it back into rows.
- **Errors, not silence.** pg_duckdb raises an exception on a failed query and
  so does this extension, rather than following the MySQL habit of returning
  NULL. The reason is in [Errors, not NULL](#errors-not-null).
- **One statement per call.** pg_duckdb's `raw_query` accepts several.
- **There is no per-role gate.** `duckdb.postgres_role` limits pg_duckdb to one
  PostgreSQL role. There is no equivalent here; see Security Considerations.
- **Readers cannot be added at run time.** pg_duckdb can install a reader from
  SQL. Here the readers are chosen when the bundle is built.

## Known Limitations

| | What it means | What would remove it |
|---|---|---|
| A function returns one value, not rows | `duckdb_query` returns a JSON array and `JSON_TABLE` unpacks it | [#549](https://github.com/villagesql/villagesql-server/issues/549) |
| The result is capped, and the cap is refused rather than cut | One megabyte by default. The stable extension SDK cannot report an overflow to the server, so a longer value would come back truncated — and truncated JSON loses its closing bracket and stops parsing. The extension raises an error instead | [#1135](https://github.com/villagesql/villagesql-server/issues/1135), and see [Returning more than one megabyte](#returning-more-than-one-megabyte) |
| A result cannot be stored in a column | `CREATE TABLE ... AS SELECT duckdb_query(...)` gives `ERROR 1406`, because the column is sized from the width of the SQL text, not the result. Unpack with `JSON_TABLE` instead | `.max_result_length()`, which needs the development ABI |
| Settings are read through the server, not from their own storage | Reading the storage a system variable points at is unsafe on a query thread, and there is no hook that runs once the variables exist | [#1136](https://github.com/villagesql/villagesql-server/issues/1136) |
| Your own tables are out of reach | A DuckDB query cannot read InnoDB tables. The `mysql_scanner` reader gets close, over a second connection at a different snapshot | [#597](https://github.com/villagesql/villagesql-server/issues/597), and [#286](https://github.com/villagesql/villagesql-server/issues/286) for consistency with InnoDB |
| Ordinary SQL is not routed to DuckDB | The caller writes `duckdb_query('...')` explicitly | [#261](https://github.com/villagesql/villagesql-server/issues/261) |
| A dataset is not a table | There is no `CREATE FOREIGN TABLE`, and no predicate pushdown from MySQL into DuckDB | [#277](https://github.com/villagesql/villagesql-server/issues/277), [#278](https://github.com/villagesql/villagesql-server/issues/278), [#279](https://github.com/villagesql/villagesql-server/issues/279), and [#142](https://github.com/villagesql/villagesql-server/issues/142) for a DuckDB-backed table |
| The function cannot tell it was killed | `KILL QUERY` does not reach it, so the extension keeps its own deadline in `timeout_ms` | [#454](https://github.com/villagesql/villagesql-server/issues/454) |
| The deadline lands between units of work | DuckDB finishes the task in hand before the deadline is checked, so a query can run a little past `timeout_ms`. Measured at 1.016 s against a 1000 ms setting. Setting `timeout_ms` to `0` removes the bound, and then nothing stops a query short of restarting the server | Nothing; it is how DuckDB hands control back |
| `duckdb_scalar` reads the whole result to return one value | A scalar over a large dataset builds the result in memory first. `memory_limit_mb` bounds it | Streaming the first chunk instead |
| Iceberg, Delta and Vortex are not bundled | See [Adding more readers](#adding-more-readers) | Nothing; it is a build choice |
| The version stays below 1.0.0 | The extension declares the `sys_var` and `keyring` preview capabilities, and either may change under it | Those two capabilities reaching general availability |
| Reinstalling resets the settings | `UNINSTALL` then `INSTALL` puts every variable back to its default in the running server, including values set with `SET PERSIST` | Re-apply them, or restart the server |
| Local files and spilling are one switch | `allow_local_files = OFF` also stops DuckDB spilling to disk | Nothing; DuckDB gates both through one setting |

## Security Considerations

**Anyone who can run SQL on this server can call these functions, and therefore
can read anything the configured credential reaches.** A user holding nothing
but `GRANT SELECT ON *.*` can call `duckdb_query`. Extension functions are not
grantable the way stored functions are, so there is no way to limit them to one
role. Treat installing this extension as granting every user of the server read
access to the buckets you configure. If that is not what you want, do not
configure a credential with wider reach than every user should have.

**A caller can make the server fetch any URL.** This is the surface that
surprises people, so read it before installing. The bundled HTTP reader is
what makes `s3://` work, and it is not limited to object storage:

```sql
SELECT duckdb_scalar('SELECT content FROM read_text(''https://anything.example/'')');
```

That request leaves the database host, from the database process, with the
database's network position. On a cloud instance the dangerous target is the
metadata endpoint at `169.254.169.254`: where IMDSv1 is enabled, a caller can
read the instance's IAM role credentials, which is a larger compromise than
the object storage key this extension is configured with, and does not depend
on that key at all. Internal services that trust the database's address are
reachable the same way.

Neither DuckDB nor its HTTP reader offers a host allowlist, so there is no
setting here that permits `s3://` while refusing everything else. What you can
do:

- Require IMDSv2 on the instance, or block egress to `169.254.169.254`.
- Restrict the host's outbound traffic to the object storage endpoints you
  actually use.
- Set `vsql_duckdb.enable_external_access = OFF` where you do not need object
  storage. It refuses every remote read, `s3://` included, and is the only
  control inside the extension that closes this.

Within that, the engine is closed as tightly as DuckDB allows.

- **No code is fetched or loaded at run time.** DuckDB is compiled with its
  install-and-load path removed, so `INSTALL` and `LOAD` in a caller's query
  text are refused outright. This matters more than it sounds: without it, and
  with local file access on, `INSTALL <reader>` downloads a reader from
  DuckDB's own repository and loads it into the server process. DuckDB does
  check a signature first, so the file that arrives is one DuckDB built — but
  that still means the database reaching a third-party network host, writing a
  shared library onto the server, and possibly pulling in readers with their
  own credential and network reach. Turning off `autoinstall_known_extensions`
  and `autoload_known_extensions` does not close this; those two only stop
  DuckDB loading a reader by itself.
- **A caller cannot loosen the engine.** Every setting is fixed when the DuckDB
  instance starts, and the instance is then locked. `SET` and `RESET` inside a
  caller's query text are refused, so the guards below cannot be turned off
  from SQL.
- **Local files are off by default.** A query cannot read files on the server
  host unless you turn `allow_local_files` on.
- **The credential cannot be printed.** The secret access key is stored as a
  DuckDB secret rather than a setting, because a setting can be read back with
  `current_setting()`. Printing secrets unredacted is disabled and locked, so
  `duckdb_secrets(redact=false)` is refused. The key is never written to disk,
  never appears in SQL, and never appears in `duckdb_status()`.
- **A runaway query stops itself.** `timeout_ms` bounds every call, because a
  function cannot learn that its statement was killed.
- **DuckDB's memory and threads are bounded** by `memory_limit_mb` and
  `threads`, since the server accounts for neither.

Two things to keep in mind when you configure it. A credential with write
permission lets a caller write to your bucket through `COPY ... TO`; give the
key read-only permission unless you need otherwise. And turning on
`allow_local_files` lets any caller read any file the server process can read,
which on most hosts includes the database's own data directory.

## Testing

See [TESTING.md](TESTING.md).

## Contributing

See the [VillageSQL Contributing Guide](https://github.com/villagesql/villagesql-server/blob/main/CONTRIBUTING.md).

## Reporting Bugs and Requesting Features

Open an issue at
[villagesql/villagesql-server/issues](https://github.com/villagesql/villagesql-server/issues).

## Contact

- Discord: https://discord.gg/KSr6whd3Fr
- GitHub Issues: https://github.com/villagesql/villagesql-server/issues

## License

GPL-2.0. See [LICENSE](LICENSE).

DuckDB is built from source as part of this build and is distributed under the
MIT License.
