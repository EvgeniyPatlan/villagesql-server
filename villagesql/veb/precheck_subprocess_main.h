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

#ifndef VILLAGESQL_VEB_PRECHECK_SUBPROCESS_MAIN_H_
#define VILLAGESQL_VEB_PRECHECK_SUBPROCESS_MAIN_H_

// Pre-check subprocess entry point. The parent mysqld posix_spawns the
// standalone mysqld-vef-precheck helper binary, whose main() is a thin
// wrapper around this function. Argv carries the harvest parameters;
// the framed result goes back over a pipe.
//
// See Docs/VEF_PRECHECK_SUBPROCESS_DESIGN.md for the full plan.

namespace villagesql {
namespace veb {

// Run as the pre-check subprocess. Parses harvest parameters from argv,
// calls HarvestTargetTypesInProcess, writes a framed result to the
// result fd, and returns an exit code.
//
// Returns 0 on success, non-zero on failure (failure is still
// communicated via a kHarvestFailure frame written to the result fd
// where possible).
int precheck_subprocess_main(int argc, char **argv);

}  // namespace veb
}  // namespace villagesql

#endif  // VILLAGESQL_VEB_PRECHECK_SUBPROCESS_MAIN_H_
