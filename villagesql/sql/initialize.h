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

#include <sys/types.h>

#include "sql/sys_vars.h"

namespace villagesql {

// Initialize the villagesql extension framework.
bool init_extension_infrastructure();

// Deinitialize the villagesql extension framework.
// Unloads all loaded extensions and cleans up the VictionaryClient.
void deinit_extension_infrastructure();

/**
 * Initializes VillageSQL before running the user's --init-file.  This runs on
 * a bootstrap thread as SYSTEM_THREAD_SERVER_INITIALIZE.
 */
bool bootstrap_for_init_file(THD *thd);

}  // namespace villagesql
