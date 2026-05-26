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

#ifndef SQL_RETURNING_INCLUDED
#define SQL_RETURNING_INCLUDED

#include "mem_root_deque.h"
#include "my_inttypes.h"

class Item;
class Query_block;
class Query_result;
class Query_result_send;
class THD;

bool prepare_returning_fields(THD *thd, Query_block *query_block,
                              mem_root_deque<Item *> *returning_fields,
                              Query_result_send **returning_result);

bool send_returning_metadata(THD *thd, Query_result *result,
                             const mem_root_deque<Item *> &returning_fields);

bool send_empty_returning_result(
    THD *thd, Query_result *result,
    const mem_root_deque<Item *> &returning_fields);

bool send_returning_eof(THD *thd, Query_result *result, longlong row_count);

#endif  // SQL_RETURNING_INCLUDED
