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

// =============================================================================
// Query Hook Builder - Declare query lifecycle hooks and config variables
// =============================================================================
//
// Usage (in extension registration):
//
//   make_extension("myext", "1.0")
//     .query_hook(make_query_hook<VEF_QUERY_HOOK_POSTEXECUTE, &my_hook>())
//     .config_var(make_config_var_double("threshold_secs",
//                                        "Slow query threshold",
//                                        &g_threshold, 1.0, 0.0, 3600.0))
//
// The hook function must match vef_query_hook_func_t:
//   void my_hook(vef_context_t*, const vef_query_hook_args_t*,
//                vef_query_hook_result_t*);

#include <villagesql/abi/types.h>

namespace villagesql {
namespace query_hook_builder {

// Wraps a single vef_query_hook_desc_t by value so the builder can store it
// in a compile-time tuple.
struct QueryHookDescriptor {
  vef_query_hook_desc_t desc;
};

// Wraps a single vef_config_var_desc_t by value.
struct ConfigVarDescriptor {
  vef_config_var_desc_t desc;
};

// ---------------------------------------------------------------------------
// make_query_hook
// ---------------------------------------------------------------------------

template <vef_query_hook_phase_t Phase, vef_query_hook_func_t Fn>
constexpr QueryHookDescriptor make_query_hook() {
  QueryHookDescriptor d{};
  d.desc.phase = Phase;
  d.desc.hook = Fn;
  return d;
}

// ---------------------------------------------------------------------------
// make_config_var_* helpers
// ---------------------------------------------------------------------------

constexpr ConfigVarDescriptor make_config_var_bool(const char *name,
                                                   const char *comment,
                                                   bool *value_ptr,
                                                   bool def_val) {
  ConfigVarDescriptor d{};
  d.desc.name = name;
  d.desc.comment = comment;
  d.desc.type = VEF_VAR_BOOL;
  d.desc.value_ptr = value_ptr;
  d.desc.boolean.def_val = def_val;
  return d;
}

constexpr ConfigVarDescriptor make_config_var_int(
    const char *name, const char *comment, long long *value_ptr,
    long long def_val, long long min_val, long long max_val) {
  ConfigVarDescriptor d{};
  d.desc.name = name;
  d.desc.comment = comment;
  d.desc.type = VEF_VAR_INT;
  d.desc.value_ptr = value_ptr;
  d.desc.integer.def_val = def_val;
  d.desc.integer.min_val = min_val;
  d.desc.integer.max_val = max_val;
  return d;
}

constexpr ConfigVarDescriptor make_config_var_double(
    const char *name, const char *comment, double *value_ptr, double def_val,
    double min_val, double max_val) {
  ConfigVarDescriptor d{};
  d.desc.name = name;
  d.desc.comment = comment;
  d.desc.type = VEF_VAR_DOUBLE;
  d.desc.value_ptr = value_ptr;
  d.desc.dbl.def_val = def_val;
  d.desc.dbl.min_val = min_val;
  d.desc.dbl.max_val = max_val;
  return d;
}

constexpr ConfigVarDescriptor make_config_var_str(const char *name,
                                                  const char *comment,
                                                  char **value_ptr,
                                                  const char *def_val) {
  ConfigVarDescriptor d{};
  d.desc.name = name;
  d.desc.comment = comment;
  d.desc.type = VEF_VAR_STR;
  d.desc.value_ptr = value_ptr;
  d.desc.str.def_val = def_val;
  return d;
}

}  // namespace query_hook_builder
}  // namespace villagesql

#endif  // VILLAGESQL_SDK_QUERY_HOOK_BUILDER_H
