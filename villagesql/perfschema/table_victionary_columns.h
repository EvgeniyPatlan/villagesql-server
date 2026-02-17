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

#ifndef VILLAGESQL_PERFSCHEMA_TABLE_VICTIONARY_COLUMNS_H_
#define VILLAGESQL_PERFSCHEMA_TABLE_VICTIONARY_COLUMNS_H_

#include <sys/types.h>
#include <string>
#include <vector>

#include "my_inttypes.h"
#include "sql/field.h"
#include "storage/perfschema/pfs_column_types.h"
#include "storage/perfschema/pfs_engine_table.h"
#include "villagesql/schema/systable/custom_columns.h"

// Row structure for PERFORMANCE_SCHEMA.VICTIONARY_COLUMNS table
struct row_victionary_columns {
  // Column SCHEMA_NAME
  char m_schema_name[NAME_LEN];
  uint m_schema_name_length;

  // Column TABLE_NAME
  char m_table_name[NAME_LEN];
  uint m_table_name_length;

  // Column COLUMN_NAME
  char m_column_name[NAME_LEN];
  uint m_column_name_length;

  // Column EXTENSION_NAME
  char m_extension_name[64];
  uint m_extension_name_length;

  // Column EXTENSION_VERSION
  char m_extension_version[64];
  uint m_extension_version_length;

  // Column TYPE_NAME
  char m_type_name[64];
  uint m_type_name_length;
};

// Table PERFORMANCE_SCHEMA.VICTIONARY_COLUMNS
class table_victionary_columns : public PFS_engine_table {
 public:
  static PFS_engine_table_share m_share;
  static PFS_engine_table *create(PFS_engine_table_share *);
  static ha_rows get_row_count();

  int rnd_init(bool scan) override;
  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position() override;

 protected:
  int read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                      bool read_all) override;

  table_victionary_columns();

 public:
  ~table_victionary_columns() override = default;

 private:
  int make_row(const villagesql::ColumnEntry *entry);

  // Position tracking
  PFS_simple_index m_pos;
  PFS_simple_index m_next_pos;

  // Cached list of all column entries
  std::vector<const villagesql::ColumnEntry *> m_all_columns;
  size_t m_entry_index;

  static THR_LOCK m_table_lock;
  static Plugin_table m_table_def;

  row_victionary_columns m_row;
};

#endif  // VILLAGESQL_PERFSCHEMA_TABLE_VICTIONARY_COLUMNS_H_
