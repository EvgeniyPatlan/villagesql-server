// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

// sysvar_update_v1.cc - v1 of the sysvar_update_test extension.
// Declares three system variables to test update behavior:
//   batch_size  INT    default=100, range [1, 1000]
//   mode        STRING default="fast"
//   debug_level INT    default=0,   range [0, 5]    (dropped in v2)

#include <villagesql/preview/sys_var.h>
#include <villagesql/vsql.h>

using namespace vsql;
namespace sv = vsql::preview_sys_var;

static long long g_batch_size;
static char *g_mode;
static long long g_debug_level;

static auto SYS_VARS = sv::make_capability({
    sv::make_int("batch_size", "Number of rows to process per batch",
                 &g_batch_size, 100, 1, 1000),
    sv::make_str("mode", "Processing mode", &g_mode, "fast"),
    sv::make_int("debug_level", "Verbosity of debug output", &g_debug_level, 0,
                 0, 5),
});

VEF_GENERATE_ENTRY_POINTS(make_extension().with(SYS_VARS))
