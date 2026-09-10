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

#include <villagesql/vsql.h>

#include <duckdb.hpp>

#include <string>

#include "engine.h"
#include "json_writer.h"
#include "settings.h"

using vsql::make_extension;
using vsql::make_func;
using vsql::STRING;
using vsql::StringArg;
using vsql::StringResult;

namespace {

using vsql_duckdb::Engine;
using vsql_duckdb::Settings;
using vsql_duckdb::append_json_string;

// The result buffer the server hands the function. On protocol 3 this is also
// a hard ceiling: a longer string is cut to exactly this many bytes, with no
// warning and no second call, so every function below refuses to return more
// than its buffer holds. Raising these needs .max_result_length(), which
// requires the development ABI -- see "Returning more than one megabyte" in
// the README.
constexpr size_t kQueryBufferSize = 1048576;
constexpr size_t kScalarBufferSize = 65536;
constexpr size_t kStatusBufferSize = 16384;

// A failed query reports an error rather than returning NULL. NULL is a
// legitimate answer here -- max() over an empty file is NULL -- so a caller
// could not tell a missing bucket from an empty one.
void report(StringResult out, const std::string &message) {
  out.error("vsql_duckdb: " + message);
}

// The smaller of what the operator allows and what this call can carry back.
struct Cap {
  size_t bytes;
  const char *reason;
};

Cap effective_cap(const Settings &settings, size_t buffer_bytes) {
  const auto configured = static_cast<size_t>(settings.max_result_bytes);
  if (configured <= buffer_bytes) return {configured, "vsql_duckdb.max_result_bytes"};
  return {buffer_bytes, "the result buffer this function declares"};
}

void duckdb_query_impl(StringArg sql, StringResult out) try {
  if (sql.is_null()) {
    out.set_null();
    return;
  }
  const Settings settings = vsql_duckdb::snapshot();
  auto outcome = Engine::get().run(std::string(sql.value()), settings);
  if (!outcome.ok()) {
    report(out, outcome.error);
    return;
  }
  const Cap cap = effective_cap(settings, out.buffer().size());
  std::string json;
  const std::string error = vsql_duckdb::write_rows_json(*outcome.result, cap.bytes, json);
  if (!error.empty()) {
    report(out, "result " + error + ", the limit set by " + cap.reason +
                    ". Narrow the query or add a LIMIT");
    return;
  }
  out.set(json);
} catch (const std::exception &e) {
  report(out, e.what());
} catch (...) {
  report(out, "unknown failure");
}

void duckdb_scalar_impl(StringArg sql, StringResult out) try {
  if (sql.is_null()) {
    out.set_null();
    return;
  }
  const Settings settings = vsql_duckdb::snapshot();
  auto outcome = Engine::get().run(std::string(sql.value()), settings);
  if (!outcome.ok()) {
    report(out, outcome.error);
    return;
  }
  if (outcome.result->RowCount() == 0 || outcome.result->names.empty()) {
    out.set_null();
    return;
  }
  duckdb::Value value = outcome.result->GetValue(0, 0);
  if (value.IsNull()) {
    out.set_null();
    return;
  }
  const std::string text = value.ToString();
  const Cap cap = effective_cap(settings, out.buffer().size());
  if (text.size() > cap.bytes) {
    report(out, "value is " + std::to_string(text.size()) + " bytes, past the " +
                    std::to_string(cap.bytes) + " byte limit set by " + cap.reason);
    return;
  }
  out.set(text);
} catch (const std::exception &e) {
  report(out, e.what());
} catch (...) {
  report(out, "unknown failure");
}

// Appends `,"key":`, with the comma left off for the first field of an object.
void append_key(const char *key, std::string &out) {
  if (out.size() > 1) out.push_back(',');
  append_json_string(key, out);
  out.push_back(':');
}

void duckdb_status_impl(StringResult out) try {
  const Settings settings = vsql_duckdb::snapshot();

  // Two questions this answers that SHOW VARIABLES cannot: which readers this
  // bundle was built with, and whether the object storage credential loaded.
  const Engine::Info info = Engine::get().info(settings);

  std::string json = "{";

  append_key("duckdb_version", json);
  append_json_string(info.duckdb_version, json);

  append_key("readers", json);
  json.push_back('[');
  for (size_t i = 0; i < info.readers.size(); ++i) {
    if (i > 0) json.push_back(',');
    append_json_string(info.readers[i], json);
  }
  json.push_back(']');

  // Empty when the instance started, sealed itself and answered a query.
  append_key("engine_error", json);
  append_json_string(info.error, json);

  append_key("object_storage_credential", json);
  if (settings.s3_key_id.empty()) {
    append_json_string("not configured", json);
  } else {
    // The credential is only reported as loaded when the build that would
    // have loaded it reported nothing wrong. Saying "loaded" after a failed
    // keyring read sends the reader to the object store to debug a
    // permissions problem they do not have.
    append_json_string((info.error.empty() ? "loaded for key id " : "not loaded for key id ") +
                           settings.s3_key_id,
                       json);
  }

  append_key("s3_endpoint", json);
  append_json_string(settings.s3_endpoint, json);
  append_key("s3_region", json);
  append_json_string(settings.s3_region, json);
  append_key("s3_url_style", json);
  append_json_string(settings.s3_url_style, json);
  append_key("s3_use_ssl", json);
  json += settings.s3_use_ssl ? "true" : "false";
  append_key("enable_external_access", json);
  json += settings.enable_external_access ? "true" : "false";
  append_key("allow_local_files", json);
  json += settings.allow_local_files ? "true" : "false";
  append_key("memory_limit_mb", json);
  json += std::to_string(settings.memory_limit_mb);
  append_key("threads", json);
  json += std::to_string(settings.threads);
  append_key("timeout_ms", json);
  json += std::to_string(settings.timeout_ms);
  append_key("max_result_bytes", json);
  json += std::to_string(settings.max_result_bytes);

  json.push_back('}');

  // Bounded by the buffer alone, not by max_result_bytes. Status has to stay
  // readable when a caller has set that variable low, since finding out what
  // it is set to is one reason to call this.
  if (json.size() > out.buffer().size()) {
    report(out, "status is longer than its result buffer, which means a system "
                "variable holds an unusually long value");
    return;
  }
  out.set(json);
} catch (const std::exception &e) {
  report(out, e.what());
} catch (...) {
  report(out, "unknown failure");
}

}  // namespace

VEF_GENERATE_ENTRY_POINTS(
    make_extension()
        .with(vsql_duckdb::g_sys_vars)
        .with(vsql_duckdb::g_keyring)
        .func(make_func<&duckdb_query_impl>("duckdb_query")
                  .returns(STRING)
                  .param(STRING)
                  .buffer_size(kQueryBufferSize)
                  .build())
        .func(make_func<&duckdb_scalar_impl>("duckdb_scalar")
                  .returns(STRING)
                  .param(STRING)
                  .buffer_size(kScalarBufferSize)
                  .build())
        .func(make_func<&duckdb_status_impl>("duckdb_status")
                  .returns(STRING)
                  .no_params()
                  .buffer_size(kStatusBufferSize)
                  .build()))
