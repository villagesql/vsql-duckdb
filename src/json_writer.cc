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

#include "json_writer.h"

#include <duckdb.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace vsql_duckdb {
namespace {

// How a column's values are written, decided once per column rather than
// once per cell.
enum class Shape { kNumber, kBoolean, kFloating, kText };

Shape shape_of(duckdb::LogicalTypeId id) {
  switch (id) {
    case duckdb::LogicalTypeId::TINYINT:
    case duckdb::LogicalTypeId::SMALLINT:
    case duckdb::LogicalTypeId::INTEGER:
    case duckdb::LogicalTypeId::BIGINT:
    case duckdb::LogicalTypeId::HUGEINT:
    case duckdb::LogicalTypeId::UTINYINT:
    case duckdb::LogicalTypeId::USMALLINT:
    case duckdb::LogicalTypeId::UINTEGER:
    case duckdb::LogicalTypeId::UBIGINT:
    case duckdb::LogicalTypeId::UHUGEINT:
    case duckdb::LogicalTypeId::DECIMAL:
      return Shape::kNumber;
    case duckdb::LogicalTypeId::BOOLEAN:
      return Shape::kBoolean;
    case duckdb::LogicalTypeId::FLOAT:
    case duckdb::LogicalTypeId::DOUBLE:
      return Shape::kFloating;
    default:
      return Shape::kText;
  }
}

}  // namespace

void append_json_string(std::string_view value, std::string &out) {
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

std::string write_rows_json(duckdb::MaterializedQueryResult &result, size_t max_bytes,
                            std::string &out) {
  const size_t columns = result.names.size();
  const size_t rows = result.RowCount();

  // Escaping a column name and classifying its type give the same answer on
  // every row, so both happen once here rather than once per cell.
  std::vector<std::string> keys(columns);
  std::vector<Shape> shapes(columns);
  for (size_t col = 0; col < columns; ++col) {
    if (col > 0) keys[col].push_back(',');
    append_json_string(result.names[col], keys[col]);
    keys[col].push_back(':');
    shapes[col] = shape_of(result.types[col].id());
  }

  out.clear();
  out.reserve(std::min<size_t>(max_bytes, 64u * 1024u) + 1u);
  out.push_back('[');

  // Every check below leaves room for the bytes still to come: the row's
  // closing brace and the array's closing bracket. Without that the array can
  // finish one byte past a caller's buffer and lose its last character, which
  // is exactly the bracket that makes it parse.
  const size_t reserved = 2;

  for (size_t row = 0; row < rows; ++row) {
    if (row > 0) out.push_back(',');
    out.push_back('{');
    for (size_t col = 0; col < columns; ++col) {
      out += keys[col];

      duckdb::Value value = result.GetValue(col, row);
      if (value.IsNull()) {
        out += "null";
      } else if (shapes[col] == Shape::kBoolean) {
        out += value.GetValue<bool>() ? "true" : "false";
      } else {
        const std::string text = value.ToString();
        // Checked before appending, so one enormous value cannot grow the
        // buffer far past the cap before anything notices.
        if (out.size() + text.size() + reserved > max_bytes) {
          return "reached " + std::to_string(max_bytes) + " bytes at row " +
                 std::to_string(row + 1);
        }
        // JSON has no spelling for nan or inf, so a float carrying either is
        // written as a string rather than as an invalid bare token.
        const bool numeric =
            shapes[col] == Shape::kNumber ||
            (shapes[col] == Shape::kFloating && std::isfinite(value.GetValue<double>()));
        if (numeric) {
          out += text;
        } else {
          append_json_string(text, out);
        }
      }

      // Escaping can grow a value past what the check above allowed for.
      if (out.size() + reserved > max_bytes) {
        return "reached " + std::to_string(max_bytes) + " bytes at row " +
               std::to_string(row + 1);
      }
    }
    out.push_back('}');
  }

  out.push_back(']');
  return {};
}

}  // namespace vsql_duckdb
