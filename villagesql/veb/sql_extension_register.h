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

#ifndef VILLAGESQL_VEB_SQL_EXTENSION_REGISTER_H
#define VILLAGESQL_VEB_SQL_EXTENSION_REGISTER_H

#include <optional>
#include <string>

#include "villagesql/schema/systable/extensions.h"
#include "villagesql/veb/veb_file.h"

class THD;

namespace villagesql {

class VictionaryClient;

// Mark types, VDFs, indexes, descriptor, and extension entry for insertion
// under an already-held victionary write lock. Returns true on error (thd
// error set), false on success.
//
// Currently used by ALTER EXTENSION ... UPDATE TO. INSTALL EXTENSION inlines
// equivalent logic; see TODO(villagesql) in execute_install.
bool mark_extension_for_insertion(THD *thd, VictionaryClient &victionary,
                                  const std::string &extension_name,
                                  const std::string &version,
                                  std::string &&sha256_hash,
                                  veb::ExtensionRegistration &&registration);

// Mark the extension's types, VDFs, indexes, descriptor, and extension entry
// for deletion. The caller must already hold the victionary write lock and
// have verified deletion is safe (no RESTRICT checks happen here).
//
// Currently used by ALTER EXTENSION ... UPDATE TO. UNINSTALL EXTENSION inlines
// equivalent logic; see TODO(villagesql) in remove_extension_from_victionary.
void mark_extension_for_deletion(
    THD *thd, VictionaryClient &victionary, const ExtensionEntry &ext_entry,
    std::optional<veb::ExtensionRegistration> &to_unregister);

}  // namespace villagesql

#endif  // VILLAGESQL_VEB_SQL_EXTENSION_REGISTER_H
