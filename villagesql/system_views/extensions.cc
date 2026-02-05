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

#include "villagesql/system_views/extensions.h"

#include <string>

#include "sql/dd/string_type.h"
#include "string_with_len.h"

namespace {
enum { FIELD_EXTENSION_NAME, FIELD_EXTENSION_VERSION };

const dd::String_type s_view_name{STRING_WITH_LEN("EXTENSIONS")};
const villagesql::system_views::Extensions *s_instance =
    new villagesql::system_views::Extensions(s_view_name);

}  // namespace

namespace villagesql {
namespace system_views {

const Extensions &Extensions::instance() { return *s_instance; }

Extensions::Extensions(const dd::String_type &n) {
  m_target_def.set_view_name(n);

  // SELECT fields
  m_target_def.add_field(FIELD_EXTENSION_NAME, "EXTENSION_NAME",
                         "ext.extension_name");
  m_target_def.add_field(FIELD_EXTENSION_VERSION, "EXTENSION_VERSION",
                         "ext.extension_version");

  // FROM
  m_target_def.add_from("villagesql.extensions ext");

  // No WHERE clause - all users can see installed extensions
}

const dd::String_type &Extensions::view_name() { return s_view_name; }
}  // namespace system_views
}  // namespace villagesql
