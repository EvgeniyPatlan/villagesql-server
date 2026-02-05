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

#include "sql/item_create.h"
#include "sql/mysqld.h"
#include "sql/parse_tree_nodes.h"
#include "sql/sql_udf.h"
#include "villagesql/sql/custom_vdf.h"

bool try_itemize_custom_vdf(Parse_context *pc, const LEX_STRING &extension_name,
                            const LEX_STRING &func, PT_item_list *opt_expr_list,
                            Item **res, bool *error) {
  *error = false;

  // Check if UDF functions are enabled
  if (!using_udf_functions) {
    return false;  // Not handled - let caller try other resolution
  }

  // Try to find a custom VDF (extension.function)
  udf_func *udf = find_udf_qualified(extension_name.str, extension_name.length,
                                     func.str, func.length, false);
  if (!udf) {
    return false;  // Not found - let caller try other resolution
  }

  // Add custom VDF to the list of used custom routines
  custom_add_used_routine(pc->thd->lex, pc->thd->stmt_arena, extension_name.str,
                          extension_name.length, func.str, func.length);

  // Found custom VDF - create UDF item
  *res = Create_udf_func::s_singleton.create(pc->thd, udf, opt_expr_list);
  if (*res == nullptr || (*res)->itemize(pc, res)) {
    *error = true;
    return true;  // Handled, but with error
  }

  return true;  // Successfully handled as VDF
}
