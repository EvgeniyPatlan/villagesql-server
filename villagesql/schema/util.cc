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

#include "villagesql/schema/util.h"

#include "sql/table.h"
#include "villagesql/schema/schema_manager.h"

namespace villagesql {

// TODO(villagesql-beta): Consolidate to one set of functions for checking
// villagesql schema name with proper charset. Also check all of the sites
// that use the constants from SchemaManager to make sure they use this.
bool is_villagesql_system_table(const TABLE *table) {
  if (!table || !table->s) return false;

  // Any table in the 'villagesql' schema is a system table
  return (my_strcasecmp(system_charset_info,
                        SchemaManager::VILLAGESQL_SCHEMA_NAME,
                        table->s->db.str) == 0);
}

}  // namespace villagesql
