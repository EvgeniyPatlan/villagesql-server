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

#include "villagesql/schema/upgrade.h"

#include "sql/sql_class.h"

namespace villagesql {
namespace upgrade {

bool upgrade_villagesql_from_1_to_2(THD * /*thd*/) {
  // No schema changes needed for this version
  // This is a placeholder for future version-specific migrations
  return false;
}

}  // namespace upgrade
}  // namespace villagesql
