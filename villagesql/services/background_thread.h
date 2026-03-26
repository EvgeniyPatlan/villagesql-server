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

#ifndef VILLAGESQL_SERVICES_BACKGROUND_THREAD_H_
#define VILLAGESQL_SERVICES_BACKGROUND_THREAD_H_

#include "villagesql/sdk/include/villagesql/abi/types.h"

// Full definition of the opaque handle type. Defined here (not in the ABI
// header) so that both background_thread.cc and any server code that
// calls unregister_vef_background_thread() see the same complete type,
// while the extension-facing ABI only sees the forward declaration.
struct vef_thread_handle_t {
  class THD *thd;
};

namespace villagesql {
namespace services {

// Register the PSI thread key used for all VEF extension background threads.
// Must be called once at server startup, before any extension is loaded.
void init_vef_background_thread_psi_key();

// Server-side implementations of the background thread registration service
// exposed to extensions via vef_register_arg_t function pointers.
//
// register_vef_background_thread:
//   Call from inside a newly started background thread. Registers the thread
//   with MySQL's Global_THD_manager (visible in INFORMATION_SCHEMA.PROCESSLIST
//   and Performance Schema). thread_name is shown as the thread state.
//   Returns an opaque handle, or NULL on failure.
//
// unregister_vef_background_thread:
//   Call from inside the background thread before it exits. Removes the thread
//   from Global_THD_manager and cleans up the THD.
vef_thread_handle_t *register_vef_background_thread(const char *thread_name);
void unregister_vef_background_thread(vef_thread_handle_t *handle);

}  // namespace services
}  // namespace villagesql

#endif  // VILLAGESQL_SERVICES_BACKGROUND_THREAD_H_
