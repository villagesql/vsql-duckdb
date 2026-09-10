/* Copyright (c) 2026 VillageSQL Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VSQL_DUCKDB_ENGINE_H
#define VSQL_DUCKDB_ENGINE_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "settings.h"

namespace duckdb {
class DuckDB;
class MaterializedQueryResult;
}  // namespace duckdb

namespace vsql_duckdb {

// The whole server shares one DuckDB instance and each call opens its own
// connection on it. A DuckDB instance owns a thread pool and a memory budget,
// so one per MySQL connection would put both beyond any limit an operator sets.
//
// Every setting that shapes the instance is fixed when the instance is built,
// and the instance is then locked so a caller cannot loosen it from inside
// their own query text. Changing one of those system variables therefore
// replaces the instance rather than reconfiguring it.
class Engine {
 public:
  struct Outcome {
    // Declared first so it is destroyed last. A result holds memory the
    // instance allocated, and another thread can replace the shared instance
    // at any moment, so the caller has to keep this one alive for as long as
    // it reads the result.
    std::shared_ptr<duckdb::DuckDB> db;
    std::unique_ptr<duckdb::MaterializedQueryResult> result;
    std::string error;

    bool ok() const { return result != nullptr; }
  };

  // What the instance can report without running a caller's query.
  struct Info {
    std::string duckdb_version;
    std::vector<std::string> readers;
    std::string error;  // empty when the instance started and sealed cleanly
  };

  static Engine &get();

  // Runs one statement and materializes the result. Stops and reports a
  // deadline error once settings.timeout_ms has passed.
  Outcome run(const std::string &sql, const Settings &settings);

  // Read once while the instance is being built, because the table functions
  // that answer these questions need the local filesystem, which the instance
  // usually goes on to disable.
  Info info(const Settings &settings);

 private:
  struct Acquired {
    std::shared_ptr<duckdb::DuckDB> db;  // null when the instance failed to start
    std::string error;                   // set when the instance is unusable
  };

  // Returns the live instance, building or rebuilding it first if the settings
  // it was built from no longer match. The caller must hold mu_.
  Acquired acquire_locked(const Settings &settings);

  std::mutex mu_;
  std::shared_ptr<duckdb::DuckDB> db_;
  Settings built_from_;
  bool built_ = false;
  std::string build_error_;   // the instance itself is unusable
  std::string secret_error_;  // the instance is fine, the credential is not
  std::string duckdb_version_;
  std::vector<std::string> readers_;
};

}  // namespace vsql_duckdb

#endif  // VSQL_DUCKDB_ENGINE_H
