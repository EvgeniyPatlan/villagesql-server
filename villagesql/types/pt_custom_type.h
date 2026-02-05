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

#ifndef VILLAGESQL_TYPES_PT_CUSTOM_TYPE_H_
#define VILLAGESQL_TYPES_PT_CUSTOM_TYPE_H_

#include <string>

#include "lex_string.h"
#include "mysql/components/services/log_builtins.h"
#include "sql/parse_tree_column_attrs.h"
#include "sql/sql_class.h"
#include "villagesql/include/error.h"
#include "villagesql/schema/descriptor/type_context.h"
#include "villagesql/types/util.h"

namespace villagesql {

// Custom types support from VillageSQL.
class PT_custom_type : public PT_type {
  typedef PT_type super;

  // The unvalidated type name. We won't know if it is valid until after
  // we construct the object (and resolve the type).
  const LEX_STRING type_name;
  THD *thd;

  // Represents a particular concrete type - i.e. an abstract type and its
  // options (length, etc.)
  const TypeContext *type_context;

  // Optional length specification from syntax
  const char *length_spec;
  // Buffer for length string (mutable for const method)
  mutable char length_buffer[21];

 private:
  PT_custom_type(const POS &pos, THD *thd, const LEX_STRING &type_name,
                 const char *length = nullptr,
                 const TypeContext *type_context = nullptr)
      : PT_type(pos,
                (type_context
                     ? static_cast<enum_field_types>(
                           type_context->descriptor()->implementation_type())
                     : MYSQL_TYPE_VARCHAR)),
        type_name(type_name),
        thd(thd),
        type_context(type_context),
        length_spec(length) {
    if (nullptr == type_context) {
      // Record the error for now.
      std::string type_key(type_name.str, type_name.length);
      thd->syntax_error_at(
          pos, "Expected a type or a custom type instead of \"%.*s\"",
          static_cast<int>(type_name.length), type_name.str);
      return;
    }

    // Validate length specification against type characteristics
    if (nullptr != length_spec &&
        type_context->descriptor()->persisted_length() != -1) {
      // Fixed-length type with length specification - this is an error
      thd->syntax_error_at(pos,
                           "Type '%.*s' is fixed-length and cannot have a "
                           "length specification",
                           static_cast<int>(type_name.length), type_name.str);
      return;
    }

    // If no length_spec provided, generate it from type_context and point
    // length_spec to it.
    if (nullptr == length_spec) {
      ulonglong len = type_context->descriptor()->persisted_length();
      snprintf(length_buffer, sizeof(length_buffer), "%llu", len);
      length_spec = length_buffer;  // Point to our buffer
    }
  }

 public:
  // During parsing, type_name is not yet validated. However, when we construct
  // this object, we could find out there actually was an error in specifying
  // this as a type.
  // Factory construction is thus required so that we can check the custom type
  // before constructing the object, as PT_type needs to be initialized with
  // the implementation_type of the custom type, and that is a const member in
  // PT_type. Although this isn't the typical pattern for PT nodes, this avoids
  // us creating the TypeContext twice.
  // Factory for custom type names. For qualified names
  // (extension_name.type_name), pass extension_name; for unqualified names,
  // pass empty LEX_STRING {} for extension_name.
  static PT_custom_type *create(MEM_ROOT *pt_mem_root, const POS &pos, THD *thd,
                                const LEX_STRING &extension_name,
                                const LEX_STRING &type_name,
                                const char *length) {
    const TypeContext *type_context = nullptr;
    if (ResolveTypeToContext(extension_name, type_name, *thd->mem_root,
                             type_context)) {
      return nullptr;
    }
    PT_custom_type *ret = new (pt_mem_root)
        PT_custom_type(pos, thd, type_name, length, type_context);
    return ret;
  }

  THD *get_thd() const { return thd; }

  bool is_custom_type() const override { return true; }

  // Return the type context, which should be non-nullptr!
  const TypeContext *get_type_context() const override {
    assert(type_context);
    return type_context;
  }

  const char *get_length() const override { return length_spec; }

  const CHARSET_INFO *get_charset() const override { return &my_charset_bin; }

  // TODO(villagesql-beta): figure out the correct implementation for these.
  const char *get_dec() const override { return nullptr; }
  uint get_uint_geom_type() const override { return 0; }
  List<String> *get_interval_list() const override { return nullptr; }
  bool is_serial_type() const override { return false; }
};

}  // namespace villagesql

#endif  // VILLAGESQL_TYPES_PT_CUSTOM_TYPE_H_
