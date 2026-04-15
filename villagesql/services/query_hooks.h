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

#ifndef VILLAGESQL_SERVICES_QUERY_HOOKS_H_
#define VILLAGESQL_SERVICES_QUERY_HOOKS_H_

#include <stddef.h>

#include "mysql/components/services/defs/event_tracking_connection_defs.h"
#include "mysql/components/services/defs/event_tracking_query_defs.h"
#include "villagesql/veb/veb_file.h"

class THD;

namespace villagesql {
namespace services {

// Register query lifecycle hooks declared by an extension.
// Appends each hook to the global list; hooks are invoked in registration order
// (i.e., extension load order).
// Does NOT require the victionary write lock — the global hook list has its own
// mutex.
// Returns false on success, true on error.
bool register_query_hooks_from_extension(
    const std::string &extension_name,
    const veb::ExtensionRegistration &ext_reg);

// Unregister all query hooks that belong to the given extension.
// Called when an extension is uninstalled.
void unregister_query_hooks_from_extension(const std::string &extension_name);

// Invoked from sql_parse.cc (dispatch_command, COM_QUERY path) before the
// parser runs. Dispatches VEF_QUERY_HOOK_PREPARSE hooks. This is a separate
// call site from on_query_event because the sql_audit.cc event tracking
// framework has no pre-parse event — its earliest query event (QUERY_START)
// fires after parsing is complete.
// Returns true if a hook blocked the query (error set on THD).
bool on_pre_parse(THD *thd);

// Invoked from mysql_event_tracking_query_notify in sql_audit.cc.
// Dispatches VEF_QUERY_HOOK_POSTEXECUTE hooks (QUERY_STATUS_END).
// Note: VEF_QUERY_HOOK_PREPARSE is NOT dispatched here — see on_pre_parse().
// Returns true if a hook blocked the query (error set on THD).
bool on_query_event(THD *thd, mysql_event_tracking_query_subclass_t subclass);

// Invoked from mysql_event_tracking_connection_notify in sql_audit.cc.
// Dispatches to registered VEF hooks based on the connection subclass.
void on_connection_event(THD *thd,
                         mysql_event_tracking_connection_subclass_t subclass,
                         int errcode);

}  // namespace services
}  // namespace villagesql

#endif  // VILLAGESQL_SERVICES_QUERY_HOOKS_H_
