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

#ifndef VILLAGESQL_PERFSCHEMA_TABLE_VICTIONARY_STATISTICS_H_
#define VILLAGESQL_PERFSCHEMA_TABLE_VICTIONARY_STATISTICS_H_

#include <sys/types.h>
#include <string>
#include <vector>

#include "my_inttypes.h"
#include "sql/field.h"
#include "storage/perfschema/pfs_column_types.h"
#include "storage/perfschema/pfs_engine_table.h"

// Row structure for PERFORMANCE_SCHEMA.VICTIONARY_STATISTICS table
struct row_victionary_statistics {
  // Column MAP_NAME
  char m_map_name[64];
  uint m_map_name_length;

  // Column COMMITTED_ENTRIES
  ulonglong m_committed_entries;

  // Column UNCOMMITTED_ENTRIES
  ulonglong m_uncommitted_entries;

  // Column CACHE_HITS
  ulonglong m_cache_hits;

  // Column CACHE_MISSES
  ulonglong m_cache_misses;
};

// Table PERFORMANCE_SCHEMA.VICTIONARY_STATISTICS
class table_victionary_statistics : public PFS_engine_table {
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

  table_victionary_statistics();

 public:
  ~table_victionary_statistics() override = default;

 private:
  int make_row(const char *map_name, size_t committed, size_t uncommitted,
               uint64_t hits, uint64_t misses);

  // Position tracking
  PFS_simple_index m_pos;
  PFS_simple_index m_next_pos;

  static THR_LOCK m_table_lock;
  static Plugin_table m_table_def;

  row_victionary_statistics m_row;
};

#endif  // VILLAGESQL_PERFSCHEMA_TABLE_VICTIONARY_STATISTICS_H_
