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

#include "villagesql/perfschema/table_extension_references.h"

#include <stddef.h>

#include "my_compiler.h"

#include "my_inttypes.h"
#include "sql/field.h"
#include "sql/plugin_table.h"
#include "sql/table.h"
#include "storage/perfschema/pfs_column_types.h"
#include "storage/perfschema/pfs_column_values.h"
#include "storage/perfschema/pfs_instr.h"
#include "storage/perfschema/table_helper.h"
#include "villagesql/schema/victionary_client.h"

THR_LOCK table_extension_references::m_table_lock;

Plugin_table table_extension_references::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "extension_references",
    /* Definition */
    "  EXTENSION_NAME VARCHAR(64) not null,\n"
    "  EXTENSION_VERSION VARCHAR(64) not null,\n"
    "  OBJECT_TYPE VARCHAR(32) not null,\n"
    "  OBJECT_KEY VARCHAR(512) not null,\n"
    "  REFERENCE_COUNT BIGINT unsigned not null,\n"
    "  COLUMN_COUNT BIGINT unsigned not null\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_extension_references::m_share = {
    &pfs_readonly_acl,
    table_extension_references::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_extension_references::get_row_count,
    sizeof(PFS_simple_index),
    &m_table_lock,
    &m_table_def,
    false, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

PFS_engine_table *table_extension_references::create(PFS_engine_table_share *) {
  return new table_extension_references();
}

ha_rows table_extension_references::get_row_count() {
  // Rough estimate - will vary based on loaded extensions
  return 100;
}

table_extension_references::table_extension_references()
    : PFS_engine_table(&m_share, &m_pos),
      m_pos(0),
      m_next_pos(0),
      m_entry_index(0) {}

void table_extension_references::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
  m_entry_index = 0;
  m_all_references.clear();
}

int table_extension_references::rnd_init(bool) {
  reset_position();
  return 0;
}

int table_extension_references::rnd_next() {
  villagesql::VictionaryClient &vc = villagesql::VictionaryClient::instance();

  if (!vc.is_initialized()) {
    return HA_ERR_END_OF_FILE;
  }

  // Load all references on first call
  if (m_all_references.empty() && m_entry_index == 0) {
    auto lock = vc.get_read_lock();
    m_all_references = vc.GetAllExtensionReferences();
  }

  // Check if we've exhausted all entries
  if (m_entry_index >= m_all_references.size()) {
    return HA_ERR_END_OF_FILE;
  }

  // Make row from current entry
  if (make_row() == 0) {
    m_entry_index++;
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  // Error in make_row, skip this entry
  m_entry_index++;
  return rnd_next();  // Try next entry
}

int table_extension_references::rnd_pos(const void *pos) {
  // For simplicity, we don't support rnd_pos - would need to cache all rows
  set_position(pos);
  return HA_ERR_RECORD_DELETED;
}

int table_extension_references::make_row() {
  if (m_entry_index >= m_all_references.size()) {
    return HA_ERR_RECORD_DELETED;
  }

  const auto &ref = m_all_references[m_entry_index];

  // Fill row with data from ExtensionReference
  m_row.m_extension_name_length = static_cast<uint>(std::min(
      ref.extension_name.length(), sizeof(m_row.m_extension_name) - 1));
  memcpy(m_row.m_extension_name, ref.extension_name.c_str(),
         m_row.m_extension_name_length);

  m_row.m_extension_version_length = static_cast<uint>(std::min(
      ref.extension_version.length(), sizeof(m_row.m_extension_version) - 1));
  memcpy(m_row.m_extension_version, ref.extension_version.c_str(),
         m_row.m_extension_version_length);

  m_row.m_object_type_length = static_cast<uint>(
      std::min(ref.object_type.length(), sizeof(m_row.m_object_type) - 1));
  memcpy(m_row.m_object_type, ref.object_type.c_str(),
         m_row.m_object_type_length);

  m_row.m_object_key_length = static_cast<uint>(
      std::min(ref.object_key.length(), sizeof(m_row.m_object_key) - 1));
  memcpy(m_row.m_object_key, ref.object_key.c_str(), m_row.m_object_key_length);

  // Subtract 1 from use_count to show only external references.
  // The map itself holds one reference, so we subtract it to show:
  // - 0 = not in use (only stored in map)
  // - 1+ = actively in use (tables, queries, etc. hold references)
  // Note: UNINSTALL EXTENSION checks (use_count > 1) which corresponds to
  // (displayed_count > 0).
  m_row.m_reference_count =
      static_cast<ulonglong>(ref.use_count > 0 ? ref.use_count - 1 : 0);

  // Column count shows how many table columns depend on this type
  m_row.m_column_count = static_cast<ulonglong>(ref.column_count);

  return 0;
}

int table_extension_references::read_row_values(TABLE *table,
                                                unsigned char *buf,
                                                Field **fields, bool read_all) {
  Field *f;

  // Set the null bits
  assert(table->s->null_bytes == 0);

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0:  // EXTENSION_NAME
          set_field_varchar_utf8mb4(f, m_row.m_extension_name,
                                    m_row.m_extension_name_length);
          break;
        case 1:  // EXTENSION_VERSION
          set_field_varchar_utf8mb4(f, m_row.m_extension_version,
                                    m_row.m_extension_version_length);
          break;
        case 2:  // OBJECT_TYPE
          set_field_varchar_utf8mb4(f, m_row.m_object_type,
                                    m_row.m_object_type_length);
          break;
        case 3:  // OBJECT_KEY
          set_field_varchar_utf8mb4(f, m_row.m_object_key,
                                    m_row.m_object_key_length);
          break;
        case 4:  // REFERENCE_COUNT
          set_field_ulonglong(f, m_row.m_reference_count);
          break;
        case 5:  // COLUMN_COUNT
          set_field_ulonglong(f, m_row.m_column_count);
          break;
        default:
          assert(false);
      }
    }
  }

  return 0;
}
