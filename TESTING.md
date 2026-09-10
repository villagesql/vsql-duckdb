# Testing vsql_duckdb

The suite runs under the MySQL Test Runner that ships with the VillageSQL
build. Every test installs the extension, exercises it, and uninstalls it, so
the tests can run in any order and leave nothing behind.

## What you need

| Requirement | Why |
|---|---|
| A VillageSQL build or dev server, 0.0.6 or newer | Supplies `mysql-test-run.pl` and the SDK |
| `VillageSQL_BUILD_DIR` pointing at it | Read by `build.sh` and by CMake |
| The extension built and its `.veb` in the server's `veb_dir` | `cmake --install` puts it there |
| A machine that can build DuckDB, or a DuckDB tree you built earlier | See the README |

Check `AGENTS.local.md` in the repository root first; it holds machine
specific paths when it exists.

## Build and install

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
cmake -S . -B build -DVillageSQL_BUILD_DIR="$VillageSQL_BUILD_DIR"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"
cmake --install build
```

The first build compiles DuckDB, which takes a few minutes. Later builds reuse
it. `-DDUCKDB_PREBUILT_ROOT=<prefix>` reuses a DuckDB tree from another
checkout and skips that step.

Confirm the shared object exports its two entry points and nothing else. If it
exports more, DuckDB's copy of a third-party library can be bound to
`mysqld`'s copy of the same library:

```bash
nm -gU build/vsql_duckdb_veb_staging/lib/vsql_duckdb.so   # macOS
nm -D --defined-only build/vsql_duckdb_veb_staging/lib/vsql_duckdb.so | grep ' T '   # Linux
```

Both should list exactly `vef_register` and `vef_unregister`.

## Run the suite

Run these from the build's `mysql-test` directory. Anywhere else fails with a
Perl module path error.

```bash
cd "$VillageSQL_BUILD_DIR/mysql-test"
perl mysql-test-run.pl --suite=/absolute/path/to/vsql-duckdb/mysql-test
```

Regenerate the expected output after a deliberate behaviour change:

```bash
perl mysql-test-run.pl --suite=/absolute/path/to/vsql-duckdb/mysql-test --record
```

Read the resulting `.result` files before committing them. `--record` writes
down whatever happened, including a regression.

## The tests

| File | What it covers |
|---|---|
| `t/duckdb_basic.test` | The shape of every answer: a scalar, a JSON array, JSON types, escaping, an empty result, a NULL argument, a NULL value, a rejected query, and a rejected second statement |
| `t/duckdb_parquet.test` | Writing a small Parquet file, counting it, a grouped rollup, unpacking that rollup into rows with `JSON_TABLE`, and a projection |
| `t/duckdb_guards.test` | Every way a caller could reach past the engine: reading a local file, changing a locked setting, installing or loading a reader, printing the credential, exceeding the result limit, and exceeding the deadline |

Each test carries a `<name>-master.opt` that turns on
`vsql_allow_preview_extensions`. A `suite.opt` would not be read, because MTR
ignores it when the suite is given as a path.

`duckdb_guards.test` is the file to be careful with. Each assertion in it
stands for a way a caller could reach past the engine, and each was written
only after checking that the guard actually holds. One of them was added after
finding that it did not: `INSTALL <reader>` in a caller's query text used to
download a reader from DuckDB's repository and load it into the server. That
test now turns local file access ON before asserting the refusal, so the
refusal has to come from the extension loader rather than from the filesystem
being unavailable.

## Testing against object storage

Nothing in the suite reaches a real bucket, so the suite needs no credentials
and no network. To check a bucket by hand:

```sql
SET PERSIST vsql_duckdb.s3_region = 'eu-north-1';
SET PERSIST vsql_duckdb.s3_key_id = 'AKIA...';
SET PERSIST vsql_duckdb.s3_secret_keyring_id = 'duckdb_s3_secret';
SELECT duckdb_status();
SELECT duckdb_scalar('SELECT count(*) FROM read_parquet(''s3://your-bucket/data/*.parquet'')');
```

`duckdb_status()` reports whether the credential loaded. A bucket that does not
exist answers with an HTTP status from the object store, which is a useful sign
that the reader itself is working.

## Continuous integration

`.github/workflows/ci.yml` builds DuckDB, caches it against the DuckDB tag and
reader list from `cmake/DuckDB.cmake`, hands the result to the shared
`villagesql/extension-actions/cpp` action, and then checks the exported symbol
count. The cache only misses when the tag or the reader list changes.
