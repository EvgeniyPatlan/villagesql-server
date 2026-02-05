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

SET(VILLAGESQL_COMMON_SOURCES
  semver.cc
)

# Object library for compilation into sql_main (avoids circular dependencies)
ADD_LIBRARY(villagesql_common OBJECT
  ${VILLAGESQL_COMMON_SOURCES})

TARGET_LINK_LIBRARIES(villagesql_common PRIVATE extra::rapidjson)

ADD_DEPENDENCIES(villagesql_common GenError)
ADD_DEPENDENCIES(villagesql_common GenServerSource)
