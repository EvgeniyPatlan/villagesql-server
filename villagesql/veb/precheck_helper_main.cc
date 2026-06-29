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

// Entry point for the mysqld-vef-precheck helper binary.
//
// The mysqld parent posix_spawns this helper to harvest type metadata
// from a candidate extension's .so. Keeping it as a separate binary
// (instead of re-execing mysqld with a divert flag) means the helper
// links only the precheck code path -- no InnoDB, no SQL parser, no
// replication, no plugin framework -- so spawn cost is single-digit
// milliseconds rather than the hundreds it would be re-execing mysqld.
//
// See Docs/VEF_PRECHECK_SUBPROCESS_DESIGN.md.

#include "villagesql/veb/precheck_subprocess_main.h"

int main(int argc, char **argv) {
  return villagesql::veb::precheck_subprocess_main(argc, argv);
}
