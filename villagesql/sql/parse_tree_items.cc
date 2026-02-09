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

#include "villagesql/sql/parse_tree_items.h"

#include "lex_string.h"
#include "sql/item_create.h"
#include "sql/parse_tree_nodes.h"
#include "sql/sql_class.h"
#include "sql/sql_udf.h"
#include "villagesql/schema/descriptor/func_descriptor.h"
#include "villagesql/sql/custom_vdf.h"
#include "villagesql/sql/func_lookup.h"

bool try_itemize_custom_vdf(Parse_context *pc, const LEX_STRING &extension_name,
                            const LEX_STRING &func, PT_item_list *opt_expr_list,
                            Item **res, bool *error) {
  *error = false;

  // Look up function (reference tied to statement mem_root lifetime)
  const villagesql::FuncDescriptor *func_desc = villagesql::find_func(
      to_string_view(extension_name), to_string_view(func), *pc->thd->mem_root);
  if (func_desc == nullptr) {
    return false;  // Not found - let caller try other resolution
  }

  // Add custom function to the list of used custom routines for MDL tracking
  custom_add_used_routine(pc->thd->lex, pc->thd->stmt_arena, extension_name.str,
                          extension_name.length, func.str, func.length);

  // Create udf_func wrapper from FuncDescriptor (allocated on mem_root)
  udf_func *udf =
      villagesql::make_udf_func_from_vdf(func_desc, *pc->thd->mem_root);
  if (!udf) {
    *error = true;
    return true;  // Handled, but with allocation error
  }

  // Create UDF item using the wrapper
  *res = Create_udf_func::s_singleton.create(pc->thd, udf, opt_expr_list);
  if (*res == nullptr || (*res)->itemize(pc, res)) {
    *error = true;
    return true;  // Handled, but with error
  }

  return true;  // Successfully handled as VDF
}
