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

// sysvar_update_v2.cc - v2 of the sysvar_update_test extension.
// Changes from v1:
//   batch_size  INT    default changed 100->200, range changed
//   [1,1000]->[1,10000] mode        STRING unchanged debug_level INT    dropped
//   timeout_ms  INT    added, default=5000, range [100, 60000]

#include <villagesql/preview/sys_var.h>
#include <villagesql/vsql.h>

using namespace vsql;
namespace sv = vsql::preview_sys_var;

static long long g_batch_size;
static char *g_mode;
static long long g_timeout_ms;

static auto SYS_VARS = sv::make_capability({
    sv::make_int("batch_size", "Number of rows to process per batch",
                 &g_batch_size, 200, 1, 10000),
    sv::make_str("mode", "Processing mode", &g_mode, "fast"),
    sv::make_int("timeout_ms", "Operation timeout in milliseconds",
                 &g_timeout_ms, 5000, 100, 60000),
});

VEF_GENERATE_ENTRY_POINTS(make_extension().with(SYS_VARS))
