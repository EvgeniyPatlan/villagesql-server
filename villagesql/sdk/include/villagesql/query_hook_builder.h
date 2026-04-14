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

#ifndef VILLAGESQL_SDK_QUERY_HOOK_BUILDER_H
#define VILLAGESQL_SDK_QUERY_HOOK_BUILDER_H

// Query Hook Builder - Declare query lifecycle hooks
//
// Usage (in extension registration):
//
//   make_extension("myext", "1.0")
//     .query_hook(make_query_hook<VEF_QUERY_HOOK_POSTEXECUTE, &my_hook>())
//
// Available phases (see vef_query_hook_phase_t for full documentation):
//   VEF_QUERY_HOOK_PREPARSE     - before parsing
//   VEF_QUERY_HOOK_POSTPARSE    - after parsing, before execution
//   VEF_QUERY_HOOK_POSTEXECUTE  - after execution
//   VEF_QUERY_HOOK_CONNECT      - new client connection
//   VEF_QUERY_HOOK_DISCONNECT   - client disconnection
//
// The hook function must match vef_query_hook_func_t:
//   void my_hook(vef_context_t*, vef_query_hook_args_t*,
//                vef_query_hook_result_t*);

#include <villagesql/abi/types.h>

namespace villagesql {
namespace query_hook_builder {

// Wraps a single vef_query_hook_desc_t by value so the builder can store it
// in a compile-time tuple.
struct QueryHookDescriptor {
  vef_query_hook_desc_t desc;
};

template <vef_query_hook_phase_t Phase, vef_query_hook_func_t Fn>
constexpr QueryHookDescriptor make_query_hook() {
  QueryHookDescriptor d{};
  d.desc.phase = Phase;
  d.desc.hook = Fn;
  return d;
}

}  // namespace query_hook_builder
}  // namespace villagesql

#endif  // VILLAGESQL_SDK_QUERY_HOOK_BUILDER_H
