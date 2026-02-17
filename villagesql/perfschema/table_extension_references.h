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

#ifndef VILLAGESQL_PERFSCHEMA_TABLE_EXTENSION_REFERENCES_H_
#define VILLAGESQL_PERFSCHEMA_TABLE_EXTENSION_REFERENCES_H_

#include <sys/types.h>
#include <string>
#include <utility>
#include <vector>

#include "my_inttypes.h"
#include "sql/field.h"
#include "storage/perfschema/pfs_column_types.h"
#include "storage/perfschema/pfs_engine_table.h"
#include "villagesql/schema/victionary_client.h"

// Row structure for PERFORMANCE_SCHEMA.EXTENSION_REFERENCES table
struct row_extension_references {
  // Column EXTENSION_NAME
  char m_extension_name[NAME_LEN];
  uint m_extension_name_length;

  // Column EXTENSION_VERSION
  char m_extension_version[64];
  uint m_extension_version_length;

  // Column OBJECT_TYPE (TYPE_CONTEXT, TYPE_DESCRIPTOR, FUNC_DESCRIPTOR, etc.)
  char m_object_type[32];
  uint m_object_type_length;

  // Column OBJECT_KEY (the normalized key string for the object)
  char m_object_key[512];
  uint m_object_key_length;

  // Column REFERENCE_COUNT (use_count from shared_ptr)
  ulonglong m_reference_count;

  // Column COLUMN_COUNT (number of table columns using this type)
  ulonglong m_column_count;
};

// Table PERFORMANCE_SCHEMA.EXTENSION_REFERENCES
class table_extension_references : public PFS_engine_table {
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

  table_extension_references();

 public:
  ~table_extension_references() override = default;

 private:
  int make_row();

  // Position tracking
  PFS_simple_index m_pos;
  PFS_simple_index m_next_pos;

  // Cached list of all extension references
  std::vector<villagesql::VictionaryClient::ExtensionReference>
      m_all_references;
  size_t m_entry_index;

  static THR_LOCK m_table_lock;
  static Plugin_table m_table_def;

  row_extension_references m_row;
};

#endif  // VILLAGESQL_PERFSCHEMA_TABLE_EXTENSION_REFERENCES_H_
