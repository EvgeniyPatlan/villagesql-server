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

// ABI v1 Test Extension
//
// Compiled against the stable_sdk/v1/ headers to verify that extensions built
// against v1 headers continue to load and run against the current server.
//
// Provides a single function: abi_v1_add(INT, INT) -> INT

#include <villagesql/extension.h>

void abi_v1_add_impl(vef_context_t *ctx, vef_invalue_t *a, vef_invalue_t *b,
                     vef_vdf_result_t *result) {
  if (a->is_null || b->is_null) {
    result->type = VEF_RESULT_NULL;
    return;
  }

  result->int_value = a->int_value + b->int_value;
  result->type = VEF_RESULT_VALUE;
}

VEF_GENERATE_ENTRY_POINTS(make_extension("abi_v1_test", "0.0.1")
                              .func(make_func<&abi_v1_add_impl>("abi_v1_add")
                                        .returns(INT)
                                        .param(INT)
                                        .param(INT)
                                        .build()))
