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

#include "villagesql/schema/systable/extensions.h"

#include "scope_guard.h"
#include "sql/field.h"
#include "sql/table.h"
#include "villagesql/include/error.h"
#include "villagesql/schema/systable/helpers.h"

namespace villagesql {

bool TableTraits<ExtensionEntry>::read_from_table(TABLE &table,
                                                  ExtensionEntry &entry) {
  Field **field = table.field;

  // extension_name (field 0) - read key component
  std::string ext_name;
  read_string_field(field[0], ext_name);
  entry.set_key(ExtensionKey(std::move(ext_name)));

  // extension_version (field 1)
  read_string_field(field[1], entry.extension_version);

  // veb_sha256 (field 2)
  read_string_field(field[2], entry.veb_sha256);

  return false;  // Success
}

bool TableTraits<ExtensionEntry>::write_to_table(TABLE &table,
                                                 const ExtensionEntry &entry) {
  Field **field = table.field;

  // Set all fields for the row
  // extension_name (field 0) - required
  field[0]->store(entry.extension_name().c_str(),
                  entry.extension_name().length(), &my_charset_utf8mb4_bin);

  // extension_version (field 1) - required
  field[1]->store(entry.extension_version.c_str(),
                  entry.extension_version.length(), &my_charset_utf8mb4_bin);

  // veb_sha256 (field 2) - required
  field[2]->store(entry.veb_sha256.c_str(), entry.veb_sha256.length(),
                  &my_charset_utf8mb4_bin);

  // Write the row - use ha_write_row for INSERT
  int error = table.file->ha_write_row(table.record[0]);
  if (should_assert_if_true(error)) {
    LogVSQL(
        ERROR_LEVEL, "Failed to write extension '%s' version '%s': error %d",
        entry.extension_name().c_str(), entry.extension_version.c_str(), error);
    return true;
  }

  return false;  // Success
}

bool TableTraits<ExtensionEntry>::update_in_table(TABLE &table,
                                                  const ExtensionEntry &entry,
                                                  const std::string &old_key) {
  // Determine which key to use for lookup
  std::string lookup_key = old_key.empty() ? entry.key().str() : old_key;

  // Build index scan on primary key (extension_name)
  uchar key_buf[MAX_KEY_LENGTH];

  // Set up the key for index scan
  Field **field = table.field;
  field[0]->store(lookup_key.c_str(), lookup_key.length(),
                  &my_charset_utf8mb4_bin);

  // Copy the key for index read
  key_copy(key_buf, table.record[0], table.key_info,
           table.key_info->key_length);

  // Save old record for update
  store_record(&table, record[1]);

  // Find the row using index read
  int error = table.file->ha_index_init(0, false);
  if (error) {
    LogVSQL(ERROR_LEVEL, "Failed to init index for update: error %d", error);
    return true;
  }

  auto index_end_guard =
      create_scope_guard([&table]() { table.file->ha_index_end(); });

  error = table.file->ha_index_read_map(table.record[0], key_buf, HA_WHOLE_KEY,
                                        HA_READ_KEY_EXACT);
  if (should_assert_if_true(error)) {
    LogVSQL(ERROR_LEVEL, "Failed to find row for update: error %d", error);
    return true;
  }

  // Now update the fields with new values
  field[0]->store(entry.extension_name().c_str(),
                  entry.extension_name().length(), &my_charset_utf8mb4_bin);
  field[1]->store(entry.extension_version.c_str(),
                  entry.extension_version.length(), &my_charset_utf8mb4_bin);
  field[2]->store(entry.veb_sha256.c_str(), entry.veb_sha256.length(),
                  &my_charset_utf8mb4_bin);

  // Update the row
  error = table.file->ha_update_row(table.record[1], table.record[0]);

  if (should_assert_if_true(error && error != HA_ERR_RECORD_IS_THE_SAME)) {
    LogVSQL(ERROR_LEVEL, "Failed to update row: error %d", error);
    return true;
  }

  return false;  // Success
}

bool TableTraits<ExtensionEntry>::delete_from_table(
    TABLE &table, const ExtensionEntry &entry) {
  // Build index scan on primary key (extension_name)
  uchar key_buf[MAX_KEY_LENGTH];

  // Set up the key for index scan
  Field **field = table.field;
  field[0]->store(entry.extension_name().c_str(),
                  entry.extension_name().length(), &my_charset_utf8mb4_bin);

  // Copy the key for index read
  key_copy(key_buf, table.record[0], table.key_info,
           table.key_info->key_length);

  // Find the row using index read
  int error = table.file->ha_index_init(0, false);
  if (error) {
    LogVSQL(ERROR_LEVEL, "Failed to init index for delete: error %d", error);
    return true;
  }

  auto index_end_guard =
      create_scope_guard([&table]() { table.file->ha_index_end(); });

  error = table.file->ha_index_read_map(table.record[0], key_buf, HA_WHOLE_KEY,
                                        HA_READ_KEY_EXACT);
  if (error) {
    if (error == HA_ERR_KEY_NOT_FOUND) {
      // Row doesn't exist - this is OK for delete
      LogVSQL(WARNING_LEVEL, "Extension row not found for delete: %s",
              entry.extension_name().c_str());
      return false;
    }
    // We expect this to not happen if the caller checked for existence first
    if (should_assert_if_true(error)) {
      LogVSQL(ERROR_LEVEL, "Failed to find row for delete: error %d", error);
      return true;
    }
  }

  // Delete the row
  error = table.file->ha_delete_row(table.record[0]);

  if (should_assert_if_true(error)) {
    LogVSQL(ERROR_LEVEL, "Failed to delete row: error %d", error);
    return true;
  }

  return false;  // Success
}

}  // namespace villagesql
