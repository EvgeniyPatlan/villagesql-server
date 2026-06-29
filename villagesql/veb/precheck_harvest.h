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

#ifndef VILLAGESQL_VEB_PRECHECK_HARVEST_H_
#define VILLAGESQL_VEB_PRECHECK_HARVEST_H_

#include <cstdint>
#include <string>
#include <vector>

namespace villagesql {
namespace veb {

// One type harvested from the target extension's vef_register output.
// Exactly the data the precheck comparison logic needs to see; nothing
// more. This shape doubles as the wire format for the Phase-2 subprocess
// prechecker: the child writes a vector of these, the parent reads them.
struct HarvestedTargetType {
  std::string type_name;
  int64_t persisted_length{0};
};

// Open the target extension's .so, call vef_register, harvest the type
// metadata, and unload. Pure with respect to caller state: no THD, no
// victionary, no logging.
//
// Returns false on success, with `out_types` populated. Returns true on
// failure, with `out_error` set to a short reason and `out_types`
// untouched.
//
// Two implementations:
//
//   HarvestTargetTypes (default):
//     The parent-side entry point. posix_spawns a pre-check subprocess
//     (this same mysqld binary, --vsql-precheck-mode flag), reads a
//     framed result from a pipe, parses, and returns. The candidate .so
//     never enters the live server's address space.
//
//   HarvestTargetTypesInProcess:
//     The in-process harvest. dlopens the .so directly in the calling
//     process. Used by the pre-check subprocess's main() to do the
//     actual work after the parent has spawned us. NOT for use by the
//     live server -- the whole point of the subprocess design is to
//     keep candidate code out of the live server's address space.
//     See Docs/VEF_PRECHECK_SUBPROCESS_DESIGN.md.
bool HarvestTargetTypes(const std::string &target_so_path, int server_protocol,
                        std::vector<HarvestedTargetType> &out_types,
                        std::string &out_error);

bool HarvestTargetTypesInProcess(const std::string &target_so_path,
                                 int server_protocol,
                                 std::vector<HarvestedTargetType> &out_types,
                                 std::string &out_error);

}  // namespace veb
}  // namespace villagesql

#endif  // VILLAGESQL_VEB_PRECHECK_HARVEST_H_
