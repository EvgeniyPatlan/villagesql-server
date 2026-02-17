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

SET(VILLAGESQL_PERFSCHEMA_SOURCES
  table_extension_references.cc
  table_extension_references.h
  table_victionary_statistics.cc
  table_victionary_statistics.h
  table_victionary_columns.cc
  table_victionary_columns.h
)

ADD_LIBRARY(villagesql_perfschema OBJECT ${VILLAGESQL_PERFSCHEMA_SOURCES})
TARGET_LINK_LIBRARIES(villagesql_perfschema PUBLIC extra::rapidjson)
ADD_DEPENDENCIES(villagesql_perfschema GenError)
ADD_DEPENDENCIES(villagesql_perfschema GenServerSource)

# Include directories for generated headers like mysqld_error.h
TARGET_INCLUDE_DIRECTORIES(villagesql_perfschema PRIVATE
  ${CMAKE_BINARY_DIR}/include
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/sql
)
