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

#include "villagesql/sql/util.h"
#include "sql/table.h"
#include "villagesql/schema/schema_manager.h"

namespace villagesql {

std::string make_udf_key(const LEX_STRING &extension_name,
                         const LEX_STRING &function_name) {
  if (extension_name.str && extension_name.length > 0) {
    return std::string(extension_name.str, extension_name.length) + "." +
           std::string(function_name.str, function_name.length);
  }
  return std::string(function_name.str, function_name.length);
}

std::string make_udf_key(const char *extension, size_t ext_len,
                         const char *function, size_t func_len) {
  std::string func_str =
      func_len ? std::string(function, func_len) : std::string(function);
  if (extension && *extension) {
    std::string ext_str =
        ext_len ? std::string(extension, ext_len) : std::string(extension);
    return ext_str + "." + func_str;
  }
  return func_str;
}

}  // namespace villagesql
