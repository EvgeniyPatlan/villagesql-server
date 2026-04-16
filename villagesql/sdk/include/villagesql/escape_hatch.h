// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VILLAGESQL_SDK_ESCAPE_HATCH_H
#define VILLAGESQL_SDK_ESCAPE_HATCH_H

// =============================================================================
// Escape Hatch Gating
// =============================================================================
//
// Some SDK features are "Escape Hatches" -- raw, low-level APIs that bypass the
// Happy Path's type safety and ergonomics. They exist so you're never blocked,
// but if you find yourself reaching for one, we'd love to hear about your use
// case so we can improve the Happy Path.
//
// To use an Escape Hatch, define the corresponding macro before including any
// VillageSQL headers:
//
//   #define VEF_ESCAPE_HATCH_PRERUN
//   #define VEF_ESCAPE_HATCH_POSTRUN
//   #include <villagesql/extension.h>
//
// Available escape hatches:
//   VEF_ESCAPE_HATCH_PRERUN   -- raw prerun callback
//   VEF_ESCAPE_HATCH_POSTRUN  -- raw postrun callback

namespace villagesql {
namespace escape_hatch {

#ifdef VEF_ESCAPE_HATCH_PRERUN
constexpr bool prerun_enabled = true;
#else
constexpr bool prerun_enabled = false;
#endif

#ifdef VEF_ESCAPE_HATCH_POSTRUN
constexpr bool postrun_enabled = true;
#else
constexpr bool postrun_enabled = false;
#endif

}  // namespace escape_hatch
}  // namespace villagesql

#endif  // VILLAGESQL_SDK_ESCAPE_HATCH_H
