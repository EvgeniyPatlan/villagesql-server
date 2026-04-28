# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# Quick start (standalone, requires MySQL source tree for component headers):
#   mkdir build && cd build
#   cmake .. -DCMAKE_PREFIX_PATH=/path/to/villagesql-extension-sdk \
#             -DMYSQL_INCLUDE_DIR=/path/to/mysql-src/include
#   make

cmake_minimum_required(VERSION 3.16)
project(vsql_component_escape_hatch_extension)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXTENSION_NAME "vsql_component_escape_hatch")

find_package(VillageSQLExtensionFramework REQUIRED)

include_directories(${VillageSQLExtensionFramework_INCLUDE_DIR})

# MySQL component headers (event_tracking_query_service.h etc.) are required
# because this extension registers directly as a component service consumer.
# When building standalone, pass -DMYSQL_INCLUDE_DIR=/path/to/mysql-src/include.
# When building in-tree, CMAKE_SOURCE_DIR/include is used automatically.
if(DEFINED MYSQL_INCLUDE_DIR)
  include_directories(${MYSQL_INCLUDE_DIR})
  if(DEFINED MYSQL_BUILD_INCLUDE_DIR)
    # my_config.h and other generated headers live in the build include dir
    include_directories(${MYSQL_BUILD_INCLUDE_DIR})
  endif()
elseif(DEFINED CMAKE_SOURCE_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/include/mysql/components")
  include_directories(${CMAKE_SOURCE_DIR}/include)
else()
  message(FATAL_ERROR
    "MySQL component headers not found. "
    "Pass -DMYSQL_INCLUDE_DIR=/path/to/mysql-src/include")
endif()

add_library(extension SHARED
    src/extension.cc
)

target_compile_options(extension PRIVATE -fPIC)

VEF_CREATE_VEB(
    NAME ${EXTENSION_NAME}
    LIBRARY_TARGET extension
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

install(FILES ${VEB_OUTPUT} DESTINATION ".")
