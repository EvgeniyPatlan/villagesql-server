// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VILLAGESQL_SCHEMA_UTIL_H
#define VILLAGESQL_SCHEMA_UTIL_H

struct TABLE;

namespace villagesql {

// Check if a TABLE is in the 'villagesql' schema.
// All tables in the villagesql schema are system tables that can be accessed
// with no_read_locking from INFORMATION_SCHEMA views.
// This allows INFORMATION_SCHEMA views to query villagesql tables
// without triggering InnoDB locking assertions.
bool is_villagesql_system_table(const TABLE *table);

}  // namespace villagesql

#endif  // VILLAGESQL_SCHEMA_UTIL_H
