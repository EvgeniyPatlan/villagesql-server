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

#ifndef VILLAGESQL_VEB_VEB_QUERY_HOOKS_H_
#define VILLAGESQL_VEB_VEB_QUERY_HOOKS_H_

#include "villagesql/veb/veb_file.h"

class THD;

namespace villagesql {
namespace veb {

// Register query lifecycle hooks declared by an extension.
// Appends each hook to the global list; hooks are invoked in registration order
// (i.e., extension load order).
// Does NOT require the victionary write lock — the global hook list has its own
// mutex.
// Returns false on success, true on error.
bool register_query_hooks_from_extension(const std::string &extension_name,
                                         const ExtensionRegistration &ext_reg);

// Unregister all query hooks that belong to the given extension.
// Called when an extension is uninstalled.
void unregister_query_hooks_from_extension(const std::string &extension_name);

// Invoke all registered post-execute hooks for the current query.
// Reads query_time, rows_sent, rows_examined, bytes_sent, bytes_received from
// the mysql_audit_print_service_double_data_source component service.
// Called from sql_parse.cc immediately after
// EVENT_TRACKING_QUERY_STATUS_END.
void invoke_post_execute_query_hooks(THD *thd);

// Register config variables declared by an extension as MySQL component system
// variables. Called after load_vef_extension(), outside the victionary lock.
// Returns false on success, true on error.
bool register_config_vars_from_extension(const std::string &extension_name,
                                         const ExtensionRegistration &ext_reg);

// Unregister config variables that belong to the given extension.
void unregister_config_vars_from_extension(const std::string &extension_name);

}  // namespace veb
}  // namespace villagesql

#endif  // VILLAGESQL_VEB_VEB_QUERY_HOOKS_H_
