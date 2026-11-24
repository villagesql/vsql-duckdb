# VillageSQL DuckDB Extension

Aggregate Parquet, CSV and JSON files and Iceberg tables from an ordinary MySQL connection,
without exporting the data first or running a second query engine beside the
database. It reads the files wherever they live — an `s3://` bucket, an
`https://` URL, or the server's own disk — so where they are stored does not
change how you query them. Remote reads work as installed; local-file reads
are off until you turn on `allow_local_files`.

**Docs:** [VillageSQL documentation](https://villagesql.com/docs) ·
[Install VillageSQL Server](https://villagesql.com/install)

```sql
INSTALL EXTENSION vsql_duckdb;

-- A public Parquet file, over HTTPS, no credentials.
SELECT duckdb_scalar('SELECT count(*) FROM read_parquet(''https://blobs.duckdb.org/data/taxi_2019_04.parquet'')');
-- 7433139
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
- [Example: aggregating a remote Parquet file](#example-aggregating-a-remote-parquet-file)
- [Joining a dataset to your own tables](#joining-a-dataset-to-your-own-tables)
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
| A VillageSQL build or dev server, 0.0.8 or newer | The extension is built for extension protocol 5, which 0.0.8 introduced. A 0.0.7 server refuses to install it |
| A C++17 compiler and CMake 3.18 or newer | |
| OpenSSL and libcurl development headers | DuckDB's HTTP reader uses both |
| Git | DuckDB is fetched from its own repository at a pinned tag |
| vcpkg | Builds the seven libraries that the Iceberg and Avro readers link, as static archives |

Debian and Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git libssl-dev libcurl4-openssl-dev curl zip unzip tar
```

RHEL and Fedora:

```bash
sudo dnf install -y gcc-c++ cmake git openssl-devel libcurl-devel curl zip unzip tar
```

macOS, with Homebrew:

```bash
brew install cmake openssl@3
```

vcpkg, on every system. The build finds it through `VCPKG_ROOT`:

```bash
git clone --branch 2026.10.16 https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg"
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
| `DUCKDB_TAG` | `v2.0.1` | The DuckDB release to build against |
| `DUCKDB_READERS` | `httpfs;json;avro;iceberg` | Which DuckDB readers to link in. `parquet` and `core_functions` are always present. See [Adding more readers](#adding-more-readers) |
| `DUCKDB_PREBUILT_ROOT` | empty | Reuse a DuckDB tree built earlier, instead of building one. `DUCKDB_READERS` must name exactly that tree's readers |
| `VCPKG_TOOLCHAIN_PATH` | `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` | Needed to build DuckDB with the `avro` or `iceberg` reader. Not needed with `DUCKDB_PREBUILT_ROOT`, because the prebuilt tree holds the libraries |

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
{
  "duckdb_version": "v2.0.1",
  "readers": ["core_functions", "httpfs", "json", "parquet"],
  "engine_error": "",
  "object_storage_credential": "not configured",
  "s3_endpoint": "",
  "s3_region": "",
  "s3_url_style": "vhost",
  "s3_use_ssl": true,
  "enable_external_access": true,
  "allow_local_files": false,
  "memory_limit_mb": 1024,
  "threads": 2,
  "timeout_ms": 30000,
  "max_result_bytes": 1048576
}
```

At the `mysql` prompt the result prints on these separate lines. Batch mode
(`mysql -e`) escapes the newlines to `\n`; use `\G` or `--raw` to read it there.

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

These variables only name where the secret access key lives; none of them
holds it. The extension never writes the key to disk and never puts it in a
setting. The key does pass through SQL once, in the `keyring_key_store()` call
below, and after that it rests wherever the keyring component keeps it.

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
SELECT duckdb_query('SELECT passenger_count, count(*) AS trips
                     FROM read_parquet(''https://blobs.duckdb.org/data/taxi_2019_04.parquet'')
                     GROUP BY passenger_count ORDER BY trips DESC LIMIT 3');
```

```json
[{"passenger_count":1,"trips":5210818},{"passenger_count":2,"trips":1113704},{"passenger_count":3,"trips":317979}]
```

Integers, decimals, floats and booleans keep their JSON types. Every other
value, including dates, timestamps, blobs, lists and structs, becomes a JSON
string of the text DuckDB printed. A float holding `nan` or `inf` becomes a
string, because JSON cannot spell either.

The result is `utf8mb4`, so MySQL's JSON functions read it with no cast:

```sql
SELECT t.passenger_count, t.trips
FROM JSON_TABLE(duckdb_query('SELECT passenger_count, count(*) AS trips
                              FROM read_parquet(''https://blobs.duckdb.org/data/taxi_2019_04.parquet'')
                              GROUP BY passenger_count ORDER BY trips DESC LIMIT 3'),
     '$[*]' COLUMNS (passenger_count INT PATH '$.passenger_count', trips BIGINT PATH '$.trips')) AS t;
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
SELECT duckdb_scalar('SELECT round(sum(fare_amount)) FROM read_parquet(''https://blobs.duckdb.org/data/taxi_2019_04.parquet'')');
-- 98075288.0
```

- A NULL argument returns NULL.
- A query matching no rows returns NULL.
- A NULL value in the first column returns NULL.
- A query DuckDB rejects raises an error.

### `duckdb_status() -> VARCHAR`

Returns a JSON object describing the engine and its settings, printed one field
per line. It answers two questions `SHOW VARIABLES` cannot: which readers this
bundle was built with, and whether the object storage credential loaded.

| Key | Meaning |
|---|---|
| `duckdb_version` | The DuckDB release compiled in |
| `readers` | The readers linked into this bundle |
| `engine_error` | Empty when DuckDB started and locked itself cleanly |
| `object_storage_credential` | `not configured`, or the key id that loaded. Never the secret |

The remaining keys echo the `vsql_duckdb.*` settings, so you can confirm the
whole engine posture in one call. Never the secret access key.

### Errors, not NULL

A query DuckDB rejects raises `ERROR 3200` rather than returning NULL. This
differs from the usual MySQL habit of returning NULL for bad input, on purpose:
NULL is a legitimate answer here, because `max()` over an empty file is NULL. A
caller who got NULL could not tell an empty bucket from a missing one.

The DuckDB message comes through in the error, so a wrong path, a denied
bucket or a bad function name each say so.

## Example: aggregating a remote Parquet file

The busiest pickup hours for New York yellow cabs in January 2024, read
straight from the city's public Parquet file of about three million trips.
DuckDB fetches the file over HTTPS and does the aggregation; only five rows
come back to MySQL, and `JSON_TABLE` turns them into typed columns.

```sql
SELECT t.*
FROM JSON_TABLE(
  duckdb_query("
    SELECT hour(tpep_pickup_datetime)            AS pickup_hour,
           count(*)                              AS trips,
           round(avg(trip_distance), 2)          AS avg_miles,
           round(avg(total_amount), 2)           AS avg_total,
           round(100.0 * avg(tip_amount / nullif(fare_amount, 0))
                 FILTER (WHERE payment_type = 1), 1) AS card_tip_pct
    FROM 'https://d37ci6vzurychx.cloudfront.net/trip-data/yellow_tripdata_2024-01.parquet'
    WHERE fare_amount > 0
    GROUP BY pickup_hour
    ORDER BY trips DESC
    LIMIT 5
  "),
  '$[*]' COLUMNS (
    pickup_hour  INT           PATH '$.pickup_hour',
    trips        BIGINT        PATH '$.trips',
    avg_miles    DECIMAL(6,2)  PATH '$.avg_miles',
    avg_total    DECIMAL(8,2)  PATH '$.avg_total',
    card_tip_pct DECIMAL(5,1)  PATH '$.card_tip_pct'
  )
) AS t;
```

```
+-------------+--------+-----------+-----------+--------------+
| pickup_hour | trips  | avg_miles | avg_total | card_tip_pct |
+-------------+--------+-----------+-----------+--------------+
|          18 | 210057 |      2.81 |     26.94 |         30.5 |
|          17 | 203768 |      3.01 |     28.26 |         27.1 |
|          16 | 187826 |      3.35 |     29.87 |         26.7 |
|          15 | 186997 |      3.90 |     27.51 |         24.8 |
|          19 | 181661 |      3.11 |     27.76 |         27.3 |
+-------------+--------+-----------+-----------+--------------+
```

- The text inside `duckdb_query` is DuckDB SQL, passed through unchanged. That
  is why it can name a URL as a table and use `hour()` and `FILTER (WHERE ...)`.
  `payment_type = 1` is a card payment, the only kind with recorded tips.
- Wrapping the DuckDB SQL in double quotes lets the URL keep its single quotes
  without doubling them. That relies on `ANSI_QUOTES` being off, which is
  MySQL's default. With it on, use single quotes outside and write the URL as
  `''https://...''`, as the other examples here do.
- Keep the aggregation in DuckDB. A result larger than
  `vsql_duckdb.max_result_bytes` raises an error rather than coming back cut
  short.

## Joining a dataset to your own tables

DuckDB cannot see your tables. Naming one fails in DuckDB's catalog rather
than in MySQL:

```
ERROR 3200 (HY000): VDF error in function 'duckdb_scalar': vsql_duckdb: Catalog Error: Table with name "shop.party_size" does not exist because schema "shop" does not exist.
```

The join belongs in the outer MySQL query. `JSON_TABLE` turns the JSON array
into rows, and those rows join against a real table like any others. This runs
against the public taxi file and a small local lookup table:

```sql
CREATE DATABASE shop;
CREATE TABLE shop.party_size (passengers INT PRIMARY KEY, label VARCHAR(20));
INSERT INTO shop.party_size VALUES (1,'solo'),(2,'couple'),(3,'small group');

SELECT p.label, t.trips
FROM JSON_TABLE(
  duckdb_query('SELECT passenger_count, count(*) AS trips
                FROM read_parquet(''https://blobs.duckdb.org/data/taxi_2019_04.parquet'')
                GROUP BY passenger_count ORDER BY trips DESC LIMIT 3'),
  '$[*]' COLUMNS (passengers INT PATH '$.passenger_count', trips BIGINT PATH '$.trips')) AS t
JOIN shop.party_size p ON p.passengers = t.passengers
ORDER BY t.trips DESC;
```

```
+-------------+---------+
| label       | trips   |
+-------------+---------+
| solo        | 5210818 |
| couple      | 1113704 |
| small group |  317979 |
+-------------+---------+
```

DuckDB aggregates the files and MySQL joins the result to your data. One JSON
string crosses between them, so keep the DuckDB side to counts, sums and
rollups rather than raw rows — see [Returning more than one
megabyte](#returning-more-than-one-megabyte). The file DuckDB reads can be an
`https://` URL, an `s3://` path, or a local file; the outer join is the same.

## Reading an Iceberg table

`iceberg_scan` reads an Apache Iceberg table, and `iceberg_snapshots` lists the
table's snapshots. Neither needs a catalog service. DuckDB publishes a sample
table as a zip file. Unzip it where the server can read it:

```bash
mkdir -p /tmp/vsql-iceberg && cd /tmp/vsql-iceberg
curl -LO https://duckdb.org/data/iceberg_data.zip && unzip iceberg_data.zip
```

Reading from the server's disk needs `vsql_duckdb.allow_local_files = ON`,
which lets any caller read any file the server process can read — see
[Security Considerations](#security-considerations). The sample's metadata
names its files by relative paths, so `allow_moved_paths := true` tells the
reader to find them in the folder you give it:

```sql
SET GLOBAL vsql_duckdb.allow_local_files = ON;

SELECT t.l_returnflag, t.n
FROM JSON_TABLE(
  duckdb_query('SELECT l_returnflag, count(*) AS n
                FROM iceberg_scan(''/tmp/vsql-iceberg/data/iceberg/lineitem_iceberg'', allow_moved_paths := true)
                GROUP BY l_returnflag ORDER BY l_returnflag'),
  '$[*]' COLUMNS (l_returnflag VARCHAR(1) PATH '$.l_returnflag', n BIGINT PATH '$.n')) AS t;
```

```
+--------------+-------+
| l_returnflag | n     |
+--------------+-------+
| A            | 12800 |
| N            | 26125 |
| R            | 12868 |
+--------------+-------+
```

A table read where it was written does not need an extra option. Pass its
newest metadata file, such as
`iceberg_scan('/data/warehouse/sales/orders/metadata/00002-<uuid>.metadata.json')`.
To read a table that was copied somewhere else, pass three things: its folder,
`allow_moved_paths := true`, and, when the folder has no `version-hint.text`,
`version := '<newest metadata file name without .metadata.json>'`.

## Returning more than one megabyte

`duckdb_query` refuses a result larger than one megabyte:

```
ERROR 3200 (HY000): VDF error in function 'duckdb_query': vsql_duckdb: result
reached 1048576 bytes at row 8748, the limit set by vsql_duckdb.max_result_bytes.
Narrow the query or add a LIMIT
```

Raising `vsql_duckdb.max_result_bytes` alone does not lift it. The ceiling is
the result buffer each function declares, `kQueryBufferSize` in
`src/vsql_duckdb.cc`; `effective_cap()` clamps to it whatever
`max_result_bytes` says.

On protocol 5 the server can grow a result buffer that turns out too small, but
it does so by calling the function again. For `duckdb_query` that would run the
DuckDB query a second time, reading the same remote files twice. The extension
refuses a result that does not fit its buffer instead, so a query never runs
more than once.

To lift the ceiling, raise `kQueryBufferSize`. `src/settings.h` already admits
up to 16 MB, so it needs no edit below that. Then confirm on your target server
that a large result comes back whole:

```sql
SELECT JSON_VALID(duckdb_query('SELECT i, repeat(''x'',100) AS pad FROM range(20000) t(i)'));
```

Before doing any of that, consider whether the query should return fewer rows.
This is built for counts, sums and low-cardinality rollups, and a result of
that shape does not approach the limit.

### Storing a result in a table

`CREATE TABLE ... AS SELECT duckdb_query(...)` sizes the column from
`.max_result_length()`, which `duckdb_query` declares as `kQueryBufferSize` --
the same number that bounds the result the function can return in the first
place -- so the column is wide enough to hold whatever the function actually
produces:

```sql
CREATE TABLE materialized AS
SELECT duckdb_query('SELECT repeat(''x'', 5000) AS s') AS v;
```

Measured with `kQueryBufferSize = 1048576`, `SHOW CREATE TABLE materialized`
reports `v` as `longtext`. A result past that ceiling still does not store:
`duckdb_query` itself raises the same "result reached ... the limit set by
vsql_duckdb.max_result_bytes" error it would raise on its own (see
[Returning more than one megabyte](#returning-more-than-one-megabyte)), rather
than a silent or a mid-row truncation.

Unpacking into real columns is still worth doing when the shape fits, since a
row-per-value table is more useful than one JSON blob:

```sql
CREATE TABLE trips_by_size AS
SELECT t.passengers, t.trips
FROM JSON_TABLE(duckdb_query('SELECT passenger_count, count(*) AS trips FROM read_parquet(''https://blobs.duckdb.org/data/taxi_2019_04.parquet'') GROUP BY passenger_count'),
     '$[*]' COLUMNS (passengers INT PATH '$.passenger_count', trips BIGINT PATH '$.trips')) AS t;
```

## Adding more readers

`DUCKDB_READERS` links a reader's `lib<name>_extension.a` into the extension.
The default four, `httpfs`, `json`, `avro` and `iceberg`, are known to work,
alongside the always-present `parquet` and `core_functions`. The `iceberg`
reader reads its manifest files through `avro`, so the two go together.

The `avro` and `iceberg` readers link seven libraries: bzip2, jansson, liblzma,
roaring, snappy, zlib and zstd. vcpkg builds them as static archives from the
manifest in `cmake/vcpkg/vcpkg.json`, so the bundle does not depend on whichever
copies a machine happens to have.

The other three readers do not produce a working build today:

| Reader | Where it stops |
|---|---|
| `delta` | Links only if `libdelta_kernel_ffi.a` (108 MB) and the platform frameworks its Rust half needs are added by hand |
| `vortex` | The same, with `libvortex_duckdb.a` (128 MB). Its `libvortex_extension.a` is a shim holding no reader code |
| `mysql_scanner` | Needs `libmariadb` from vcpkg, which `cmake/vcpkg/vcpkg.json` does not list |

A Rust reader is two archives, and DuckDB installs only the C++ half into its
install prefix. The link list here names only that half, which is why `delta`
and `vortex` do not link. With both archives supplied by hand they do link, and
the engine then starts with no readers at all and reports that it cannot load
`parquet`. DuckDB loads its linked readers in one loop with no error handling,
so one reader throwing during load leaves the engine with none of them; which
of the two throws was not established.

The `mysql_scanner` reader deserves a caution. It would let a DuckDB query
`ATTACH` this server over its own connection and read your InnoDB tables, which
makes a join between a Parquet file and a table reachable. It comes with real
costs: it is a second client session, so it reads a different snapshot than the
statement that called it, it takes a connection from the pool, and it blocks if
the caller holds locks on the rows it wants. Those costs, not build time, are
why it would stay out of the default build even once it configures.

## Migrating from PostgreSQL

### What replaces what

| pg_duckdb | vsql_duckdb | Note |
|---|---|---|
| `duckdb.query(sql)` | `duckdb_query(sql)` | Returns a JSON array, not a row set. Use `JSON_TABLE` to get rows |
| `duckdb.raw_query(sql)` | `duckdb_query(sql)` | There is no server log to print to, so the result comes back instead |
| `duckdb.recycle_ddb()` | no equivalent | One instance serves the whole server, so a table or macro any caller creates is visible to every other caller until a setting change replaces the instance |
| `read_parquet`, `read_csv`, `read_json`, `read_text`, `read_blob` | the same names, inside the query text | They are DuckDB functions, not MySQL functions |
| `duckdb.create_simple_secret(...)` | `vsql_duckdb.s3_*` variables plus the keyring | No setting holds the secret; the variables only name the keyring entry that does |
| MAP, union, JSON, time functions, `approx_count_distinct`, `TABLESAMPLE` | the same names, inside the query text | Reachable through `duckdb_query`, not as MySQL functions |
| `duckdb.install_extension`, `load_extension`, `autoload_extension` | none | Refused by design. Readers ship inside the bundle. See Security Considerations |
| `iceberg_scan`, `iceberg_metadata`, `iceberg_snapshots` | the same names, inside the query text | See [Reading an Iceberg table](#reading-an-iceberg-table) |
| `delta_scan`, `read_vortex` | not in the default build | See [Adding more readers](#adding-more-readers) |
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
SELECT count(*) FROM read_parquet('https://blobs.duckdb.org/data/taxi_2019_04.parquet');

-- VillageSQL
SELECT duckdb_scalar('SELECT count(*) FROM read_parquet(''https://blobs.duckdb.org/data/taxi_2019_04.parquet'')');
```

A grouped rollup, as rows:

```sql
-- PostgreSQL
SELECT passenger_count, count(*) FROM read_parquet('https://blobs.duckdb.org/data/taxi_2019_04.parquet') GROUP BY passenger_count;

-- VillageSQL
SELECT t.passengers, t.trips
FROM JSON_TABLE(
  duckdb_query('SELECT passenger_count, count(*) AS trips FROM read_parquet(''https://blobs.duckdb.org/data/taxi_2019_04.parquet'')
                GROUP BY passenger_count'),
  '$[*]' COLUMNS (passengers INT PATH '$.passenger_count', trips BIGINT PATH '$.trips')) AS t;
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
| The result is capped, and the cap is refused rather than cut | One megabyte by default. The server could grow the buffer, but only by calling the function again, which would run the DuckDB query twice. The extension raises an error instead | Raising `kQueryBufferSize` — see [Returning more than one megabyte](#returning-more-than-one-megabyte) |
| Settings are read through the server, not from their own storage | Reading the storage a system variable points at is unsafe on a query thread, and there is no hook that runs once the variables exist | [#1136](https://github.com/villagesql/villagesql-server/issues/1136) |
| Your own tables are out of reach | A DuckDB query cannot read InnoDB tables. Join its output in the outer query instead — see [Joining a dataset to your own tables](#joining-a-dataset-to-your-own-tables). The `mysql_scanner` reader would close the gap, over a second connection at a different snapshot | [#597](https://github.com/villagesql/villagesql-server/issues/597), and [#286](https://github.com/villagesql/villagesql-server/issues/286) for consistency with InnoDB |
| Ordinary SQL is not routed to DuckDB | The caller writes `duckdb_query('...')` explicitly | [#261](https://github.com/villagesql/villagesql-server/issues/261) |
| A dataset is not a table | There is no `CREATE FOREIGN TABLE`, and no predicate pushdown from MySQL into DuckDB | [#277](https://github.com/villagesql/villagesql-server/issues/277), [#278](https://github.com/villagesql/villagesql-server/issues/278), [#279](https://github.com/villagesql/villagesql-server/issues/279), and [#142](https://github.com/villagesql/villagesql-server/issues/142) for a DuckDB-backed table |
| The function cannot tell it was killed | `KILL QUERY` does not reach it, so the extension keeps its own deadline in `timeout_ms` | [#454](https://github.com/villagesql/villagesql-server/issues/454) |
| The deadline lands between units of work | DuckDB finishes the task in hand before the deadline is checked, so a query can run a little past `timeout_ms`. Measured at 1.016 s against a 1000 ms setting. Setting `timeout_ms` to `0` removes the bound, and then nothing stops a query short of restarting the server | Nothing; it is how DuckDB hands control back |
| `duckdb_scalar` reads the whole result to return one value | A scalar over a large dataset builds the result in memory first. `memory_limit_mb` bounds it | Streaming the first chunk instead |
| Six readers work | `httpfs`, `json`, `parquet`, `core_functions`, `avro` and `iceberg`. Delta, Vortex and `mysql_scanner` fail to build or to start — see [Adding more readers](#adding-more-readers) | Link wiring for the Rust archives, and the `libmariadb` dependency for `mysql_scanner` |
| Iceberg reads were tested on local disk only | Reading a table in object storage, Iceberg writes, and an Iceberg REST catalog through `ATTACH` were not tested | Testing them |
| The version stays below 1.0.0 | The extension declares the `sys_var` and `keyring` preview capabilities, and either may change under it | Those two capabilities reaching general availability |
| Reinstalling resets the settings | `UNINSTALL` then `INSTALL` puts every variable back to its default. `UNINSTALL` also deletes them from `mysqld-auto.cnf`, so a restart does not bring a `SET PERSIST` value back | Re-apply them |
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
  `duckdb_secrets(redact=false)` is refused. The extension never writes the key
  to disk and it never appears in `duckdb_status()`. It reaches the keyring
  through one SQL statement, `keyring_key_store()`, which puts it in that
  session's history and in the general query log if one is running.
- **A runaway query stops itself.** `timeout_ms` bounds every call, because a
  function cannot learn that its statement was killed.
- **DuckDB's memory and threads are bounded** by `memory_limit_mb` and
  `threads`, since the server accounts for neither.

Two things to keep in mind when you configure it. A credential with write
permission lets a caller write to your bucket through `COPY ... TO` or the
Iceberg maintenance functions; give the
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
