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

#include "villagesql/perfschema/table_victionary_columns.h"

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

THR_LOCK table_victionary_columns::m_table_lock;

Plugin_table table_victionary_columns::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "victionary_columns",
    /* Definition */
    "  SCHEMA_NAME VARCHAR(64) not null,\n"
    "  TABLE_NAME VARCHAR(64) not null,\n"
    "  COLUMN_NAME VARCHAR(64) not null,\n"
    "  EXTENSION_NAME VARCHAR(64) not null,\n"
    "  EXTENSION_VERSION VARCHAR(64) not null,\n"
    "  TYPE_NAME VARCHAR(64) not null\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_victionary_columns::m_share = {
    &pfs_readonly_acl,
    table_victionary_columns::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_victionary_columns::get_row_count,
    sizeof(PFS_simple_index),
    &m_table_lock,
    &m_table_def,
    false, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

PFS_engine_table *table_victionary_columns::create(PFS_engine_table_share *) {
  return new table_victionary_columns();
}

ha_rows table_victionary_columns::get_row_count() {
  // Estimated row count
  return 100;
}

table_victionary_columns::table_victionary_columns()
    : PFS_engine_table(&m_share, &m_pos),
      m_pos(0),
      m_next_pos(0),
      m_entry_index(0) {}

void table_victionary_columns::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
  m_entry_index = 0;
  m_all_columns.clear();
}

int table_victionary_columns::rnd_init(bool) {
  reset_position();
  return 0;
}

int table_victionary_columns::rnd_next() {
  villagesql::VictionaryClient &vc = villagesql::VictionaryClient::instance();

  if (!vc.is_initialized()) {
    return HA_ERR_END_OF_FILE;
  }

  // Load all columns on first call
  if (m_all_columns.empty() && m_entry_index == 0) {
    auto lock = vc.get_read_lock();
    m_all_columns = vc.columns().get_all_committed();
  }

  // Check if we've exhausted all entries
  if (m_entry_index >= m_all_columns.size()) {
    return HA_ERR_END_OF_FILE;
  }

  // Make row from current entry
  if (make_row(m_all_columns[m_entry_index]) == 0) {
    m_entry_index++;
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  // Error in make_row, skip this entry
  m_entry_index++;
  return rnd_next();  // Try next entry
}

int table_victionary_columns::rnd_pos(const void *pos) {
  set_position(pos);
  return HA_ERR_RECORD_DELETED;
}

int table_victionary_columns::make_row(const villagesql::ColumnEntry *entry) {
  if (!entry) {
    return HA_ERR_RECORD_DELETED;
  }

  // Fill row with data from ColumnEntry
  m_row.m_schema_name_length = static_cast<uint>(
      std::min(entry->db_name().length(), sizeof(m_row.m_schema_name) - 1));
  memcpy(m_row.m_schema_name, entry->db_name().c_str(),
         m_row.m_schema_name_length);

  m_row.m_table_name_length = static_cast<uint>(
      std::min(entry->table_name().length(), sizeof(m_row.m_table_name) - 1));
  memcpy(m_row.m_table_name, entry->table_name().c_str(),
         m_row.m_table_name_length);

  m_row.m_column_name_length = static_cast<uint>(
      std::min(entry->column_name().length(), sizeof(m_row.m_column_name) - 1));
  memcpy(m_row.m_column_name, entry->column_name().c_str(),
         m_row.m_column_name_length);

  m_row.m_extension_name_length = static_cast<uint>(std::min(
      entry->extension_name.length(), sizeof(m_row.m_extension_name) - 1));
  memcpy(m_row.m_extension_name, entry->extension_name.c_str(),
         m_row.m_extension_name_length);

  m_row.m_extension_version_length =
      static_cast<uint>(std::min(entry->extension_version.length(),
                                 sizeof(m_row.m_extension_version) - 1));
  memcpy(m_row.m_extension_version, entry->extension_version.c_str(),
         m_row.m_extension_version_length);

  m_row.m_type_name_length = static_cast<uint>(
      std::min(entry->type_name.length(), sizeof(m_row.m_type_name) - 1));
  memcpy(m_row.m_type_name, entry->type_name.c_str(), m_row.m_type_name_length);

  return 0;
}

int table_victionary_columns::read_row_values(TABLE *table, unsigned char *,
                                              Field **fields, bool read_all) {
  Field *f;

  // Set the null bits
  assert(table->s->null_bytes == 0);

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0:  // SCHEMA_NAME
          set_field_varchar_utf8mb4(f, m_row.m_schema_name,
                                    m_row.m_schema_name_length);
          break;
        case 1:  // TABLE_NAME
          set_field_varchar_utf8mb4(f, m_row.m_table_name,
                                    m_row.m_table_name_length);
          break;
        case 2:  // COLUMN_NAME
          set_field_varchar_utf8mb4(f, m_row.m_column_name,
                                    m_row.m_column_name_length);
          break;
        case 3:  // EXTENSION_NAME
          set_field_varchar_utf8mb4(f, m_row.m_extension_name,
                                    m_row.m_extension_name_length);
          break;
        case 4:  // EXTENSION_VERSION
          set_field_varchar_utf8mb4(f, m_row.m_extension_version,
                                    m_row.m_extension_version_length);
          break;
        case 5:  // TYPE_NAME
          set_field_varchar_utf8mb4(f, m_row.m_type_name,
                                    m_row.m_type_name_length);
          break;
        default:
          assert(false);
      }
    }
  }

  return 0;
}
