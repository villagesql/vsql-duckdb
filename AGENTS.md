# AGENTS.md

Guidance for AI coding assistants working in this repository.

Also read `AGENTS.local.md` when it is present; it carries machine-specific
paths and is not checked in.

## What this extension is

`vsql_duckdb` embeds DuckDB inside the VillageSQL server process so a caller
can aggregate Parquet, CSV and JSON files held in object storage over an
ordinary MySQL connection. It is a port of the PostgreSQL extension
`pg_duckdb`, narrowed to what the VillageSQL Extension Framework supports
today. The tracking issue is
[villagesql-server#1132](https://github.com/villagesql/villagesql-server/issues/1132).

Three SQL functions:

| Function | Returns |
|---|---|
| `duckdb_query(sql)` | A JSON array, one object per row |
| `duckdb_scalar(sql)` | The first column of the first row, as text |
| `duckdb_status()` | A JSON object describing the engine and its settings |

The caller's SQL text goes to DuckDB unchanged. This extension never rewrites
or translates it.

## Why C++ and not Rust

The extension is a thin adapter over DuckDB's own C++ objects — `DBConfig`,
`Connection`, `PendingQueryResult`, `Value`, `LogicalType`. Rust would mean
linking the same C++ static archives into a `cdylib` and reaching those objects
through the C API or a hand-written shim, which adds a boundary and a second
build system without changing what the extension can do. Both VillageSQL SDKs
carry the preview capabilities this extension needs, so the SDK was not the
deciding factor.

## Layout

| Path | Holds |
|---|---|
| `src/settings.h` | The system variables, the keyring capability, and `snapshot()` |
| `src/engine.h` / `src/engine.cc` | The shared DuckDB instance, its security settings, and the query deadline |
| `src/json_writer.h` / `src/json_writer.cc` | Turning a DuckDB result into JSON |
| `src/vsql_duckdb.cc` | The three VEF entry points and the registration block |
| `src/exported_symbols.txt` / `.map` | The two symbols the shared object may export |
| `cmake/DuckDB.cmake` | Fetching and building DuckDB, and choosing its readers |
| `mysql-test/t`, `mysql-test/r` | MTR tests and their recorded output |

## Building

DuckDB is the only dependency that is not already on a machine that builds
VillageSQL. `cmake/DuckDB.cmake` fetches and builds it, or reuses a tree you
built earlier:

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
cmake -S . -B build -DVillageSQL_BUILD_DIR="$VillageSQL_BUILD_DIR"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"
cmake --install build
```

`-DDUCKDB_PREBUILT_ROOT=<prefix>` skips the DuckDB build and links against an
existing one. `-DDUCKDB_READERS=...` chooses which DuckDB readers are linked
in; the default is `httpfs;json`, and `parquet` and `core_functions` are always
present.

On macOS add `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`.

## Three build settings that are not optional

Change any of these only with a reason written into the same commit.

- **`-DDISABLE_EXTENSION_LOAD=1`.** Without it, `INSTALL <reader>` inside a
  caller's query text downloads a shared library from DuckDB's extension
  repository and loads it into `mysqld`. Turning off `autoinstall_known_extensions`
  and `autoload_known_extensions` does NOT stop that; those two only govern
  DuckDB loading a reader by itself. This was measured, not assumed.

  DuckDB does verify an RSA signature against keys compiled into it before
  installing or loading, so the file that arrives is one DuckDB built. That is
  not enough on its own: it still reaches a third-party network host from
  inside `mysqld`, writes a shared library to the server host, and can bring in
  readers such as `azure`, `aws` and `postgres_scanner` that carry credential
  and network surface this extension did not choose. The compile-time flag
  removes the path rather than leaving it resting on two settings staying
  correct.
- **`-DENABLE_JEMALLOC=OFF`.** A second allocator inside `mysqld` is not
  acceptable.
- **The linker export lists.** `src/exported_symbols.txt` (macOS) and
  `src/exported_symbols.map` (Linux) hold the shared object to two exported
  symbols. DuckDB and `mysqld` each carry their own copies of fmt, re2,
  utf8proc, miniz and zstd, and without this the dynamic loader can bind one
  project's call to the other project's definition. Do not add
  `-fvisibility=hidden` alongside them: a symbol compiled hidden never reaches
  the symbol table, so the export list cannot put the entry points back, and
  the extension then fails to load with no exported `vef_register`.

Check the last one after any link change:

```bash
nm -gU build/vsql_duckdb_veb_staging/lib/vsql_duckdb.so   # exactly two symbols
```

## Testing

```bash
cd "$VillageSQL_BUILD_DIR/mysql-test"
perl mysql-test-run.pl --suite=/absolute/path/to/vsql-duckdb/mysql-test
perl mysql-test-run.pl --suite=/absolute/path/to/vsql-duckdb/mysql-test --record
```

Each test carries its own `<name>-master.opt` turning on
`vsql_allow_preview_extensions`, because a `suite.opt` is ignored when MTR runs
with `--suite=<path>`.

`duckdb_guards.test` is the one to keep honest. Every assertion in it stands
for a way a caller could reach past the engine, and each was written after
checking that the guard actually holds — one of them only after finding that it
did not.

## Things that will catch you out

- **A STRING result is cut at the buffer the function declares.** On protocol 3
  a longer value is truncated to exactly `.buffer_size()` bytes, with no warning
  and no second call, even though `vsql/func_builder.h` describes a grow-and-retry
  contract. Every entry point therefore compares its output against
  `out.buffer().size()` and raises an error instead. Do not remove those checks.
- **`ESCAPED`, `ROWS` and `ROLLUP` are reserved words in MySQL.** Two of them
  reached a test as column aliases and killed `mysqltest` with a syntax error
  that read like a test-runner fault.
- **`UNINSTALL EXTENSION` then `INSTALL EXTENSION` resets every
  `vsql_duckdb.*` variable** to its compiled default inside the running server,
  including values set with `SET PERSIST`.
- **`disabled_filesystems` cannot be set before the DuckDB instance starts.**
  It goes on the setup connection, along with `lock_configuration`, which must
  be last.
- **Local file access and disk spilling are one DuckDB switch.** Turning off
  `allow_local_files` also stops DuckDB spilling, so a query over
  `memory_limit_mb` fails rather than writing to disk.

## Conventions

- C++17, `lowercase_with_underscores`, GPL v2 header on every `.cc`, `.h` and
  `CMakeLists.txt`.
- The typed SDK API only: include `<villagesql/vsql.h>`. Never read or use
  anything under an `abi/` directory.
- Extension name uses underscores (`vsql_duckdb`); the repository uses a hyphen
  (`vsql-duckdb`).
- No PR or issue numbers in source or test files.
