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

#include "villagesql/veb/extension_uninstall_checks.h"

#include "my_sys.h"

#include "villagesql/include/error.h"

namespace villagesql {

bool check_for_indexes_of_extension(
    const ExtensionEntry &ext_entry,
    const std::vector<const IndexEntry *> &all_indexes) {
  const IndexEntry *first = nullptr;
  int count = 0;

  for (const auto *entry : all_indexes) {
    if (entry->extension_name == ext_entry.extension_name() &&
        entry->extension_version == ext_entry.extension_version) {
      if (count == 0) first = entry;
      count++;
    }
  }

  if (first != nullptr) {
    villagesql_error(
        "Cannot drop extension `%s` as %d custom index(es) depend on it, "
        "e.g. %s.%s.%s uses index type %s",
        MYF(0), ext_entry.extension_name().c_str(), count,
        first->db_name().c_str(), first->table_name().c_str(),
        first->index_name().c_str(), first->index_type_name.c_str());
    return true;
  }

  return false;
}

}  // namespace villagesql
