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

# Builds DuckDB as static libraries and exposes them as the `duckdb_static`
# interface target.
#
# Set DUCKDB_PREBUILT_ROOT to reuse a DuckDB tree that was already built with
# the same options; otherwise DuckDB is fetched at DUCKDB_TAG and built here.
#
# DUCKDB_READERS selects which DuckDB extensions are linked in. Every name in
# the list must be either an in-tree DuckDB extension or a file named
# <name>.cmake under duckdb/.github/config/extensions/. Readers that need
# vcpkg packages also need VCPKG_TOOLCHAIN_PATH — see README.

include(ExternalProject)
include(FetchContent)

set(DUCKDB_TAG "v1.5.5" CACHE STRING "DuckDB release tag to build against")
set(DUCKDB_READERS "httpfs;json" CACHE STRING "DuckDB extensions to link in")
set(DUCKDB_PREBUILT_ROOT "" CACHE PATH "Existing DuckDB install prefix to reuse")

# The shared CI action calls cmake with a fixed argument list, so CI passes the
# prebuilt DuckDB tree through the environment instead.
if(NOT DUCKDB_PREBUILT_ROOT AND DEFINED ENV{DUCKDB_PREBUILT_ROOT})
  set(DUCKDB_PREBUILT_ROOT "$ENV{DUCKDB_PREBUILT_ROOT}")
endif()
set(VCPKG_TOOLCHAIN_PATH "" CACHE FILEPATH "vcpkg toolchain, for readers that need it")

# Static archives DuckDB always produces, in link order. The extension
# archives are appended per reader below.
set(_duckdb_core_libs
    duckdb_static
    duckdb_generated_extension_loader
    duckdb_fastpforlib
    duckdb_fmt
    duckdb_fsst
    duckdb_hyperloglog
    duckdb_mbedtls
    duckdb_miniz
    duckdb_pg_query
    duckdb_re2
    duckdb_skiplistlib
    duckdb_utf8proc
    duckdb_yyjson
    duckdb_zstd)

# parquet and core_functions are compiled into every DuckDB build whether or
# not they are named in DUCKDB_READERS.
set(_duckdb_always_linked parquet core_functions)

if(DUCKDB_PREBUILT_ROOT)
  set(_duckdb_prefix "${DUCKDB_PREBUILT_ROOT}")
else()
  set(_duckdb_prefix "${CMAKE_CURRENT_BINARY_DIR}/duckdb-install")
endif()

# Turn the reader names into archive names. A reader whose archive is missing
# at link time is a build error, not a silently absent feature.
set(_duckdb_ext_libs "")
foreach(_r IN LISTS DUCKDB_READERS _duckdb_always_linked)
  list(APPEND _duckdb_ext_libs "${_r}_extension")
endforeach()
list(REMOVE_DUPLICATES _duckdb_ext_libs)

set(_duckdb_link_libs "")
foreach(_l IN LISTS _duckdb_ext_libs _duckdb_core_libs)
  list(APPEND _duckdb_link_libs "${_duckdb_prefix}/lib/lib${_l}.a")
endforeach()

if(DUCKDB_PREBUILT_ROOT)
  message(STATUS "DuckDB: reusing prebuilt tree at ${_duckdb_prefix}")
  add_custom_target(duckdb_build)
else()
  message(STATUS "DuckDB: building ${DUCKDB_TAG} with readers: ${DUCKDB_READERS}")

  string(REPLACE ";" "|" _duckdb_readers_arg "${DUCKDB_READERS}")

  set(_duckdb_cmake_args
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=${_duckdb_prefix}
      -DBUILD_SHELL=0
      -DBUILD_UNITTESTS=0
      -DEXTENSION_STATIC_BUILD=1
      -DENABLE_EXTENSION_AUTOLOADING=0
      -DENABLE_EXTENSION_UPDATING=0
      # Compiles DuckDB's install-and-load path out entirely. Without this a
      # caller's own query text can write "INSTALL <reader>" and make the
      # database fetch a shared library from a third-party host and load it
      # into mysqld. Turning off autoinstall and autoload does NOT cover that;
      # they only stop DuckDB reaching for a reader by itself. The extension
      # refuses to serve queries if it finds this was left off -- see
      # check_extension_load_disabled() in src/engine.cc.
      -DDISABLE_EXTENSION_LOAD=1
      # A second allocator inside mysqld is not acceptable.
      -DENABLE_JEMALLOC=OFF
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      # A shallow clone has no tags, so DuckDB cannot derive its own version.
      -DOVERRIDE_GIT_DESCRIBE=${DUCKDB_TAG}
      # LIST_SEPARATOR below turns each "|" back into a ";" for the sub-build.
      # Passing the ";" form directly would let CMake split the reader list
      # into separate command line arguments, and every reader after the first
      # would be dropped without a word.
      -DBUILD_EXTENSIONS=${_duckdb_readers_arg})

  if(OPENSSL_ROOT_DIR)
    list(APPEND _duckdb_cmake_args -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR})
  endif()
  if(VCPKG_TOOLCHAIN_PATH)
    list(APPEND _duckdb_cmake_args
         -DVCPKG_TOOLCHAIN_PATH=${VCPKG_TOOLCHAIN_PATH}
         -DCMAKE_TOOLCHAIN_FILE=${VCPKG_TOOLCHAIN_PATH})
  endif()

  ExternalProject_Add(duckdb_build
      GIT_REPOSITORY https://github.com/duckdb/duckdb
      GIT_TAG ${DUCKDB_TAG}
      GIT_SHALLOW TRUE
      PREFIX ${CMAKE_CURRENT_BINARY_DIR}/duckdb
      LIST_SEPARATOR |
      CMAKE_ARGS ${_duckdb_cmake_args}
      # Every archive the extension links has to be named here. Without it
      # the generator has no rule to produce a file that does not exist at
      # configure time, and the link step fails before DuckDB has been built.
      BUILD_BYPRODUCTS ${_duckdb_link_libs}
      UPDATE_DISCONNECTED TRUE)
endif()

add_library(duckdb_static INTERFACE)
add_dependencies(duckdb_static duckdb_build)
target_include_directories(duckdb_static SYSTEM INTERFACE "${_duckdb_prefix}/include")

# The loader archive calls into each reader archive and every reader calls back
# into libduckdb_static.a, so no single order satisfies GNU ld's one pass.
# Apple's linker resolves to closure and rejects --start-group.
if(APPLE)
  target_link_libraries(duckdb_static INTERFACE ${_duckdb_link_libs})
else()
  target_link_libraries(duckdb_static INTERFACE
      -Wl,--start-group ${_duckdb_link_libs} -Wl,--end-group)
endif()

# httpfs reaches the network through libcurl and OpenSSL. mysqld already links
# both on every platform we build for.
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
target_link_libraries(duckdb_static INTERFACE OpenSSL::SSL OpenSSL::Crypto CURL::libcurl)

