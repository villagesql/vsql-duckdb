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

#include "engine.h"

#include <duckdb.hpp>

#include <chrono>
#include <thread>

namespace vsql_duckdb {
namespace {

using vsql::preview_keyring::KeyringCapability;

// Doubles every single quote so a value can be pasted into a DuckDB string
// literal. Used only for values this extension controls -- endpoints, regions
// and the credential -- never for caller-supplied query text.
std::string quote_literal(const std::string &v) {
  std::string out;
  out.reserve(v.size() + 2);
  for (char c : v) {
    out.push_back(c);
    if (c == '\'') out.push_back('\'');
  }
  return out;
}

// Options DuckDB accepts before the instance starts.
void apply_startup_options(duckdb::DBConfig &config, const Settings &s) {
  config.SetOptionByName("max_memory", duckdb::Value(std::to_string(s.memory_limit_mb) + "MB"));
  config.SetOptionByName("threads", duckdb::Value::BIGINT(s.threads));
  config.SetOptionByName("enable_external_access", duckdb::Value::BOOLEAN(s.enable_external_access));

  if (s.allow_local_files && !s.temp_directory.empty()) {
    config.SetOptionByName("temp_directory", duckdb::Value(s.temp_directory));
  }

  // The bundle carries every reader it supports. These two stop DuckDB
  // reaching for one by itself; DISABLE_EXTENSION_LOAD, set when DuckDB is
  // compiled, is what stops a caller asking for one by name.
  config.SetOptionByName("autoinstall_known_extensions", duckdb::Value::BOOLEAN(false));
  config.SetOptionByName("autoload_known_extensions", duckdb::Value::BOOLEAN(false));
  config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(false));
  config.SetOptionByName("allow_community_extensions", duckdb::Value::BOOLEAN(false));

  // The access key never reaches disk, and duckdb_secrets() cannot print it.
  config.SetOptionByName("allow_persistent_secrets", duckdb::Value::BOOLEAN(false));
  config.SetOptionByName("allow_unredacted_secrets", duckdb::Value::BOOLEAN(false));
}

// Refuses a DuckDB that can still fetch and load a reader at run time.
//
// The whole security story rests on DuckDB being compiled with
// DISABLE_EXTENSION_LOAD, because otherwise a caller's own query text can say
// INSTALL <reader> and make the database pull a shared library off the network
// and load it into mysqld. A compile flag in a build file is the kind of guard
// that goes missing quietly -- it did once here, present in the CI recipe and
// absent from the one the README documents -- so the extension asks the engine
// it actually linked rather than trusting how it was built.
//
// Fails closed. If a later DuckDB rewords the refusal, this reports a build
// problem rather than quietly serving queries with the guard gone, and the
// test suite says so on the first run after the version bump.
std::string check_extension_load_disabled(duckdb::Connection &con) {
  auto res = con.Query("LOAD vsql_duckdb_probe_no_such_reader");
  if (!res->HasError()) {
    return "DuckDB loaded an unknown reader, so this build can load code at "
           "run time. Rebuild DuckDB with -DDISABLE_EXTENSION_LOAD=1";
  }
  const std::string error = res->GetError();
  if (error.find("compile time flag") == std::string::npos) {
    return "DuckDB was built without -DDISABLE_EXTENSION_LOAD=1, so a query "
           "could install and load code into the server. Rebuild DuckDB with "
           "that flag. The engine refused the probe with: " +
           error;
  }
  return {};
}

// Closes the instance to further configuration. Everything a caller must not
// be able to undo from inside their own query text is settled by the time this
// runs: lock_configuration then refuses every SET and RESET, and
// allowed_configs is left empty so nothing is exempt.
//
// disabled_filesystems is here rather than in the startup options because
// DuckDB rejects it before the instance starts.
std::string seal(duckdb::Connection &con, const Settings &s) {
  // Asked before the filesystem is taken away, so the answer comes from the
  // extension loader rather than from a file it could not reach.
  std::string wrong_build = check_extension_load_disabled(con);
  if (!wrong_build.empty()) return wrong_build;

  // Reading and writing local files is one switch in DuckDB, so turning it off
  // also stops DuckDB spilling to disk. A query that needs more than
  // memory_limit_mb then fails instead of writing to the server's filesystem.
  if (!s.allow_local_files) {
    auto res = con.Query("SET disabled_filesystems = 'LocalFileSystem'");
    if (res->HasError()) return res->GetError();
  }
  auto res = con.Query("SET lock_configuration = true");
  if (res->HasError()) return res->GetError();
  return {};
}

// Registers the object storage credential as a DuckDB secret. A secret is used
// rather than the s3_* settings because current_setting() can read a setting
// back and any caller could then print the access key.
//
// Returns an error string, empty on success. Does nothing when no key id is
// configured, which leaves public buckets reachable and private ones not.
std::string create_secret(duckdb::Connection &con, const Settings &s) {
  if (s.s3_key_id.empty()) return {};

  // Caught here rather than left to DuckDB, whose message for a bad url style
  // does not say which of our variables produced it.
  if (!s.s3_url_style.empty() && s.s3_url_style != "vhost" && s.s3_url_style != "path") {
    return "vsql_duckdb.s3_url_style must be 'vhost' or 'path', not '" + s.s3_url_style + "'";
  }

  std::string secret_value;
  if (!s.s3_secret_keyring_id.empty()) {
    auto read = g_keyring.read(s.s3_secret_keyring_id, s.s3_secret_keyring_auth_id);
    if (read.status != KeyringCapability::Status::OK) {
      return "keyring has no secret under data id '" + s.s3_secret_keyring_id +
             "' for owner '" + s.s3_secret_keyring_auth_id +
             "'. A key stored with keyring_key_store() belongs to the account that "
             "stored it; name that account in vsql_duckdb.s3_secret_keyring_auth_id";
    }
    secret_value = std::move(read.value);
  }

  std::string stmt = "CREATE OR REPLACE SECRET vsql_duckdb_s3 (TYPE S3";
  stmt += ", KEY_ID '" + quote_literal(s.s3_key_id) + "'";
  stmt += ", SECRET '" + quote_literal(secret_value) + "'";
  if (!s.s3_region.empty()) stmt += ", REGION '" + quote_literal(s.s3_region) + "'";
  if (!s.s3_endpoint.empty()) stmt += ", ENDPOINT '" + quote_literal(s.s3_endpoint) + "'";
  if (!s.s3_url_style.empty()) stmt += ", URL_STYLE '" + quote_literal(s.s3_url_style) + "'";
  stmt += ", USE_SSL ";
  stmt += s.s3_use_ssl ? "true" : "false";
  stmt += ")";

  auto res = con.Query(stmt);
  if (res->HasError()) return res->GetError();
  return {};
}

}  // namespace

Engine &Engine::get() {
  static Engine engine;
  return engine;
}

Engine::Acquired Engine::acquire_locked(const Settings &settings) {
  if (built_ && settings.same_instance_as(built_from_)) {
    // A credential that failed earlier is retried against the running
    // instance rather than remembered. Otherwise an operator who named a
    // keyring secret before creating it would keep getting the same error
    // afterwards, because no system variable moved. Retrying the one step
    // that failed also avoids rebuilding a perfectly good instance on every
    // query while the credential is wrong.
    if (!secret_error_.empty()) {
      duckdb::Connection con(*db_);
      secret_error_ = create_secret(con, built_from_);
    }
    return {db_, secret_error_};
  }

  db_.reset();
  built_ = false;
  built_from_ = settings;
  build_error_.clear();
  secret_error_.clear();
  duckdb_version_.clear();
  readers_.clear();

  try {
    duckdb::DBConfig config;
    apply_startup_options(config, settings);
    auto db = std::make_shared<duckdb::DuckDB>(nullptr, &config);
    duckdb::Connection setup(*db);

    // Both of these need the local filesystem, which seal() is about to take
    // away, so they run first.
    auto version = setup.Query("SELECT version()");
    if (!version->HasError() && version->RowCount() == 1) {
      duckdb_version_ = version->GetValue(0, 0).ToString();
    }
    auto readers =
        setup.Query("SELECT extension_name FROM duckdb_extensions() WHERE loaded ORDER BY 1");
    if (!readers->HasError()) {
      for (size_t row = 0; row < readers->RowCount(); ++row) {
        readers_.push_back(readers->GetValue(0, row).ToString());
      }
    }

    secret_error_ = create_secret(setup, settings);
    // Sealed whether or not the credential loaded, so an instance can never
    // be left open to reconfiguration by a caller's query text.
    build_error_ = seal(setup, settings);
    if (build_error_.empty()) {
      db_ = std::move(db);
      built_ = true;
    }
  } catch (const std::exception &e) {
    build_error_ = e.what();
  }

  if (!built_) return {nullptr, build_error_};
  return {db_, secret_error_};
}

Engine::Info Engine::info(const Settings &settings) {
  std::lock_guard<std::mutex> lock(mu_);
  Acquired acquired = acquire_locked(settings);
  return Info{duckdb_version_, readers_, std::move(acquired.error)};
}

Engine::Outcome Engine::run(const std::string &sql, const Settings &settings) {
  Outcome out;
  Acquired acquired;
  {
    std::lock_guard<std::mutex> lock(mu_);
    acquired = acquire_locked(settings);
  }

  // A credential that failed to load is reported once per query rather than
  // left to surface as an unexplained access denial from the object store.
  if (!acquired.error.empty()) {
    out.error = std::move(acquired.error);
    return out;
  }
  if (acquired.db == nullptr) {
    out.error = "DuckDB failed to start";
    return out;
  }

  // Held for as long as the result lives, so replacing the shared instance on
  // another thread cannot free memory this result still points at.
  out.db = acquired.db;

  duckdb::Connection con(*out.db);
  auto pending = con.PendingQuery(sql);
  if (pending->HasError()) {
    out.error = pending->GetError();
    return out;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(settings.timeout_ms);
  for (;;) {
    auto state = pending->ExecuteTask();
    // IsResultReady() is also true once execution has failed, so the error
    // state has to be taken before Execute() is called on a dead result.
    if (state == duckdb::PendingExecutionResult::EXECUTION_ERROR) {
      out.error = pending->GetError();
      return out;
    }
    if (duckdb::PendingQueryResult::IsResultReady(state)) break;
    if (settings.timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
      con.Interrupt();
      out.error = "query passed vsql_duckdb.timeout_ms (" +
                  std::to_string(settings.timeout_ms) + " ms) and was stopped";
      return out;
    }
    // RESULT_NOT_READY is the one state that means this thread just did work,
    // so it is the one state worth looping on immediately. Sleeping on
    // everything else keeps a future DuckDB state from turning this into a
    // spin that holds a server thread against a busy core.
    if (state != duckdb::PendingExecutionResult::RESULT_NOT_READY) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  auto result = pending->Execute();
  if (result->HasError()) {
    out.error = result->GetError();
    return out;
  }
  out.result = duckdb::unique_ptr_cast<duckdb::QueryResult, duckdb::MaterializedQueryResult>(
      std::move(result));
  return out;
}

}  // namespace vsql_duckdb
