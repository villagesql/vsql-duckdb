# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.18)
project(vsql_duckdb)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXTENSION_NAME "vsql_duckdb")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(VillageSQL REQUIRED)

message(STATUS "VillageSQL SDK found at: ${VillageSQL_PREFIX}")
message(STATUS "VillageSQL include dir: ${VillageSQL_INCLUDE_DIR}")

include(DuckDB)

add_library(vsql_duckdb SHARED
    src/vsql_duckdb.cc
    src/engine.cc
    src/json_writer.cc
)

target_include_directories(vsql_duckdb PRIVATE
    ${VillageSQLExtensionFramework_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(vsql_duckdb PRIVATE duckdb_static)

# DuckDB and mysqld each carry their own copies of common third-party code
# (fmt, re2, utf8proc, miniz, zstd). Exporting nothing but the two VEF entry
# points keeps the dynamic loader from binding one project's call to the other
# project's definition.
#
# The hiding is done by the linker rather than by -fvisibility=hidden: a symbol
# compiled hidden never reaches the symbol table, so the export list below
# could not put the entry points back.
if(APPLE)
    target_link_options(vsql_duckdb PRIVATE
        "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/exported_symbols.txt")
elseif(UNIX)
    # --no-undefined turns an unresolved symbol into a link error. Nothing here
    # binds against mysqld at load time, so a leftover is always a link bug.
    target_link_options(vsql_duckdb PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exported_symbols.map"
        "-Wl,--exclude-libs,ALL"
        "-Wl,--no-undefined")
endif()

VEF_CREATE_VEB(
    NAME ${EXTENSION_NAME}
    LIBRARY_TARGET vsql_duckdb
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
