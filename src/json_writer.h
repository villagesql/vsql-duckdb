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

#ifndef VSQL_DUCKDB_JSON_WRITER_H
#define VSQL_DUCKDB_JSON_WRITER_H

#include <cstddef>
#include <string>
#include <string_view>

namespace duckdb {
class MaterializedQueryResult;
}

namespace vsql_duckdb {

// Renders a result as a JSON array of objects, one object per row, keys in
// column order. Numbers and booleans keep their JSON types; everything else,
// including dates, timestamps, blobs, lists and structs, becomes a JSON
// string of the value DuckDB printed.
//
// Writes into `out` and returns an empty string on success. Returns a short
// phrase naming the limit and the row it was reached on, leaving `out`
// unspecified, when the array would pass max_bytes. The caller says which
// limit that was.
//
// max_bytes bounds the finished array, closing bracket included, so a caller
// may hand the result straight to a buffer of exactly that size.
std::string write_rows_json(duckdb::MaterializedQueryResult &result, size_t max_bytes,
                            std::string &out);

// Appends `value` to `out` as a JSON string literal, quotes included.
void append_json_string(std::string_view value, std::string &out);

}  // namespace vsql_duckdb

#endif  // VSQL_DUCKDB_JSON_WRITER_H
