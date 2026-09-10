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

#ifndef VSQL_DUCKDB_SETTINGS_H
#define VSQL_DUCKDB_SETTINGS_H

#include <villagesql/preview/keyring.h>
#include <villagesql/preview/sys_var.h>

#include <mutex>
#include <string>

namespace vsql_duckdb {

// Storage behind the system variables. These belong to the server, which
// writes them from whichever thread ran the SET. Nothing outside this file
// reads them: a SET on a string variable frees the previous buffer, so a
// reader on another thread can be walking memory that has just gone away.
// snapshot() below is the only supported way to read a setting.
inline char *g_s3_region = nullptr;
inline char *g_s3_endpoint = nullptr;
inline char *g_s3_url_style = nullptr;
inline char *g_s3_key_id = nullptr;
inline char *g_s3_secret_keyring_id = nullptr;
inline char *g_s3_secret_keyring_auth_id = nullptr;
inline char *g_temp_directory = nullptr;
inline bool g_s3_use_ssl = true;
inline bool g_enable_external_access = true;
inline bool g_allow_local_files = false;
inline long long g_memory_limit_mb = 1024;
inline long long g_threads = 2;
inline long long g_timeout_ms = 30000;
inline long long g_max_result_bytes = 1048576;

// One consistent read of every system variable.
struct Settings {
  std::string s3_region;
  std::string s3_endpoint;
  std::string s3_url_style;
  std::string s3_key_id;
  std::string s3_secret_keyring_id;
  std::string s3_secret_keyring_auth_id;
  std::string temp_directory;
  bool s3_use_ssl = true;
  bool enable_external_access = true;
  bool allow_local_files = false;
  long long memory_limit_mb = 1024;
  long long threads = 2;
  long long timeout_ms = 30000;
  long long max_result_bytes = 1048576;

  // Whether two snapshots would produce the same DuckDB instance. Every
  // setting except the two the engine reads per query is fixed when the
  // instance is built, so a change to any of them replaces it.
  bool same_instance_as(const Settings &o) const {
    return s3_region == o.s3_region && s3_endpoint == o.s3_endpoint &&
           s3_url_style == o.s3_url_style && s3_key_id == o.s3_key_id &&
           s3_secret_keyring_id == o.s3_secret_keyring_id &&
           s3_secret_keyring_auth_id == o.s3_secret_keyring_auth_id &&
           temp_directory == o.temp_directory && s3_use_ssl == o.s3_use_ssl &&
           enable_external_access == o.enable_external_access &&
           allow_local_files == o.allow_local_files &&
           memory_limit_mb == o.memory_limit_mb && threads == o.threads;
  }
};

inline std::mutex g_settings_mu;
inline Settings g_settings_cache;
inline bool g_settings_primed = false;

inline std::string str_or_empty(const char *s) {
  return s == nullptr ? std::string() : std::string(s);
}

// Copies every global into a Settings. Only safe to call from a thread that
// no writer can race: the on_change callback, which the server invokes while
// holding its own system-variable lock.
inline Settings read_globals() {
  Settings s;
  s.s3_region = str_or_empty(g_s3_region);
  s.s3_endpoint = str_or_empty(g_s3_endpoint);
  s.s3_url_style = str_or_empty(g_s3_url_style);
  s.s3_key_id = str_or_empty(g_s3_key_id);
  s.s3_secret_keyring_id = str_or_empty(g_s3_secret_keyring_id);
  s.s3_secret_keyring_auth_id = str_or_empty(g_s3_secret_keyring_auth_id);
  s.temp_directory = str_or_empty(g_temp_directory);
  s.s3_use_ssl = g_s3_use_ssl;
  s.enable_external_access = g_enable_external_access;
  s.allow_local_files = g_allow_local_files;
  s.memory_limit_mb = g_memory_limit_mb;
  s.threads = g_threads;
  s.timeout_ms = g_timeout_ms;
  s.max_result_bytes = g_max_result_bytes;
  return s;
}

// Refreshes the cache every time the server changes one of our variables.
// This runs while the server holds its global system-variable lock, so it
// does nothing but take a short private mutex and copy: running SQL, reading
// another system variable, or waiting on a thread that does either would
// deadlock on that lock. Every variable carries this callback, so a variable
// without one would silently freeze at the value it had when the cache was
// first filled.
inline void note_change(vsql::preview_sys_var::SysVarChange) {
  std::lock_guard<std::mutex> lock(g_settings_mu);
  g_settings_cache = read_globals();
  g_settings_primed = true;
}

// Fills the cache for the first time, by asking the server for each value
// rather than reading the raw globals.
//
// The globals cannot be read safely from here. A SET on a string variable
// frees the buffer the old pointer names, and this runs on a query thread
// with nothing holding the server's system-variable lock, so a first query
// racing a first SET could read memory that has just gone away. The
// capability's own get() asks the server, which reads under that lock.
//
// It has to happen here rather than at registration: on_init() runs before
// the server installs the variables, so priming there would freeze every
// setting at its compiled default until someone happened to run a SET.
// Declared below, because it needs the capability object.
void prime_cache_locked();

// The only supported read. Returns one consistent set of values.
inline Settings snapshot() {
  std::lock_guard<std::mutex> lock(g_settings_mu);
  if (!g_settings_primed) {
    prime_cache_locked();
    g_settings_primed = true;
  }
  return g_settings_cache;
}

namespace sv = vsql::preview_sys_var;

inline auto g_sys_vars = sv::make_capability({
    sv::make_str("s3_region", "AWS region for s3:// paths", &g_s3_region, "")
        .on_change<&note_change>(),
    sv::make_str("s3_endpoint",
                 "Object storage endpoint, host and optional port. Empty means "
                 "AWS S3. Set storage.googleapis.com for Google Cloud Storage",
                 &g_s3_endpoint, "")
        .on_change<&note_change>(),
    sv::make_str("s3_url_style", "vhost or path", &g_s3_url_style, "vhost")
        .on_change<&note_change>(),
    sv::make_str("s3_key_id", "Access key id. The matching secret comes from the keyring",
                 &g_s3_key_id, "")
        .on_change<&note_change>(),
    sv::make_str("s3_secret_keyring_id",
                 "Keyring data id holding the secret access key. Empty means no credential",
                 &g_s3_secret_keyring_id, "")
        .on_change<&note_change>(),
    sv::make_str("s3_secret_keyring_auth_id",
                 "Keyring owner of that secret. Empty reads an internal key, which is "
                 "what a key provisioned outside SQL is. A key stored with "
                 "keyring_key_store() belongs to the account that stored it, so name "
                 "that account here",
                 &g_s3_secret_keyring_auth_id, "")
        .on_change<&note_change>(),
    sv::make_str("temp_directory",
                 "Directory DuckDB spills to. Only used when allow_local_files is ON",
                 &g_temp_directory, "")
        .on_change<&note_change>(),
    sv::make_bool("s3_use_ssl", "Reach the endpoint over HTTPS", &g_s3_use_ssl, true)
        .on_change<&note_change>(),
    sv::make_bool("enable_external_access",
                  "Let DuckDB reach anything outside the server process", &g_enable_external_access,
                  true)
        .on_change<&note_change>(),
    sv::make_bool("allow_local_files",
                  "Let queries read files on the server host, and let DuckDB spill to disk",
                  &g_allow_local_files, false)
        .on_change<&note_change>(),
    sv::make_int("memory_limit_mb", "Memory DuckDB may use, in MB", &g_memory_limit_mb, 1024, 64,
                 1048576)
        .on_change<&note_change>(),
    sv::make_int("threads", "Worker threads DuckDB may start", &g_threads, 2, 1, 1024)
        .on_change<&note_change>(),
    sv::make_int("timeout_ms", "Deadline for one query, in milliseconds. 0 means no deadline",
                 &g_timeout_ms, 30000, 0, 3600000)
        .on_change<&note_change>(),
    sv::make_int("max_result_bytes", "Largest result a query may return, in bytes",
                 &g_max_result_bytes, 1048576, 1024, 16777216)
        .on_change<&note_change>(),
});

inline vsql::preview_keyring::KeyringCapability g_keyring;

// See the declaration above snapshot() for why this asks the server instead
// of reading the globals.
inline void prime_cache_locked() {
  const char *kExtension = "vsql_duckdb";
  auto text = [&](const char *name, std::string &out) {
    std::string value;
    if (!g_sys_vars.get(kExtension, name, value)) out = std::move(value);
  };
  auto flag = [&](const char *name, bool &out) {
    std::string value;
    if (g_sys_vars.get(kExtension, name, value)) return;
    // The server spells a boolean "ON"/"OFF" here, and older ones "1"/"0".
    out = value == "ON" || value == "on" || value == "1";
  };
  auto number = [&](const char *name, long long &out) {
    std::string value;
    if (g_sys_vars.get(kExtension, name, value)) return;
    try {
      out = std::stoll(value);
    } catch (const std::exception &) {
      // Leave the compiled default rather than a wrong number.
    }
  };

  text("s3_region", g_settings_cache.s3_region);
  text("s3_endpoint", g_settings_cache.s3_endpoint);
  text("s3_url_style", g_settings_cache.s3_url_style);
  text("s3_key_id", g_settings_cache.s3_key_id);
  text("s3_secret_keyring_id", g_settings_cache.s3_secret_keyring_id);
  text("s3_secret_keyring_auth_id", g_settings_cache.s3_secret_keyring_auth_id);
  text("temp_directory", g_settings_cache.temp_directory);
  flag("s3_use_ssl", g_settings_cache.s3_use_ssl);
  flag("enable_external_access", g_settings_cache.enable_external_access);
  flag("allow_local_files", g_settings_cache.allow_local_files);
  number("memory_limit_mb", g_settings_cache.memory_limit_mb);
  number("threads", g_settings_cache.threads);
  number("timeout_ms", g_settings_cache.timeout_ms);
  number("max_result_bytes", g_settings_cache.max_result_bytes);
}

}  // namespace vsql_duckdb

#endif  // VSQL_DUCKDB_SETTINGS_H
