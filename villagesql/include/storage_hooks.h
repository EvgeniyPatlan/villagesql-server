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

// Storage-engine hooks exposed to VillageSQL code outside of the storage
// engines themselves.
//
// The pattern: each storage engine that wants to participate sets the
// matching function-pointer hook at engine init. VillageSQL code calls the
// hook (after null-checking) to trigger engine-side action without
// reaching into engine internals.
//
// Today only InnoDB participates (it owns the custom-column dictionary
// cache). If another engine ever stores custom columns the hook can be
// generalized to a list (one per engine); for now a single function
// pointer is sufficient.

#ifndef VILLAGESQL_INCLUDE_STORAGE_HOOKS_H_
#define VILLAGESQL_INCLUDE_STORAGE_HOOKS_H_

#include <string>
#include <utility>
#include <vector>

namespace villagesql {

// (db_name, table_name) identifying a single table in the data dictionary.
using QualifiedTableName = std::pair<std::string, std::string>;

// Invalidate cached storage-engine schema state for a specific list of
// tables. Called by ALTER EXTENSION UPDATE TO between the catalog commit
// and the dlclose of the old .so. The caller supplies the list, derived
// from a single villagesql.custom_columns walk (the SQL-layer flush uses
// the same list).
//
// The engine implementation must:
//   - look up each named table in its dict cache
//   - mark each entry for reload so the next open re-acquires fresh
//     TypeContext references (releasing the shared_ptrs that pin
//     pointers into the about-to-be-unloaded .so)
//
// Set by the engine at init time. May be nullptr (no engine has
// custom-column state) -- callers must null-check.
using storage_invalidate_tables_t =
    void (*)(const std::vector<QualifiedTableName> &tables);

extern storage_invalidate_tables_t g_storage_invalidate_tables;

}  // namespace villagesql

#endif  // VILLAGESQL_INCLUDE_STORAGE_HOOKS_H_
