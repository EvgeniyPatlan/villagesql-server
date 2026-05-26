/*
   Copyright (c) 2026 VillageSQL Contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "sql/sql_returning.h"

#include "mem_root_deque.h"
#include "sql/auth/auth_acls.h"
#include "sql/item.h"
#include "sql/protocol.h"
#include "sql/query_result.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"

bool prepare_returning_fields(THD *thd, Query_block *query_block,
                              mem_root_deque<Item *> *returning_fields,
                              Query_result_send **returning_result) {
  mem_root_deque<Item *> saved_fields = query_block->fields;
  query_block->fields = *returning_fields;
  query_block->resolve_place = Query_block::RESOLVE_SELECT_LIST;
  if (query_block->with_wild && query_block->setup_wild(thd)) return true;
  *returning_fields = query_block->fields;
  if (query_block->setup_base_ref_items(thd))
    return true; /* purecov: inspected */
  if (setup_fields(thd, SELECT_ACL, /*allow_sum_func=*/true,
                   /*split_sum_funcs=*/true, /*column_update=*/false,
                   /*typed_items=*/nullptr, returning_fields,
                   query_block->base_ref_items))
    return true;
  query_block->resolve_place = Query_block::RESOLVE_NONE;
  query_block->fields = saved_fields;

  *returning_result = new (thd->mem_root) Query_result_send;
  return *returning_result == nullptr; /* purecov: inspected */
}

bool send_returning_metadata(THD *thd, Query_result *result,
                             const mem_root_deque<Item *> &returning_fields) {
  return result->send_result_set_metadata(
      thd, returning_fields, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF);
}

bool send_empty_returning_result(
    THD *thd, Query_result *result,
    const mem_root_deque<Item *> &returning_fields) {
  return send_returning_metadata(thd, result, returning_fields) ||
         result->send_eof(thd);
}

bool send_returning_eof(THD *thd, Query_result *result, longlong row_count) {
  thd->set_row_count_func(row_count);
  return result->send_eof(thd);
}
