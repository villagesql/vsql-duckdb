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

# Builds only DuckDB, through the same cmake/DuckDB.cmake an ordinary build
# uses, and none of the extension. It exists for callers who have to build
# DuckDB ahead of time -- continuous integration, a shared build machine, a
# container layer -- so they run the one recipe rather than a copy of it.
#
# A copy is how -DDISABLE_EXTENSION_LOAD=1 came to be missing from the build
# the README documents while continuous integration, which had its own copy,
# kept passing.
#
#   cmake -S cmake/duckdb-standalone -B /tmp/duckdb-stage
#   cmake --build /tmp/duckdb-stage --target duckdb_build -j"$(nproc)"
#
# Then build the extension with
# -DDUCKDB_PREBUILT_ROOT=/tmp/duckdb-stage/duckdb-install.

cmake_minimum_required(VERSION 3.18)
project(vsql_duckdb_dependency CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/..")

# This project's whole job is to produce the tree, so a DUCKDB_PREBUILT_ROOT
# left in the environment for the extension build must not make it skip the
# work and reuse a tree that may not exist yet.
unset(ENV{DUCKDB_PREBUILT_ROOT})
set(DUCKDB_PREBUILT_ROOT "" CACHE PATH "Not used here" FORCE)

include(DuckDB)
