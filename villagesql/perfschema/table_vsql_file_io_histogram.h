// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef TABLE_VSQL_FILE_IO_HISTOGRAM_H
#define TABLE_VSQL_FILE_IO_HISTOGRAM_H

// VillageSQL: PERFORMANCE_SCHEMA.FILE_IO_HISTOGRAM (declarations).
//
// One row per (file event name, latency bucket): the latency distribution of
// file I/O waits, per file class, over the shared 450-bucket histogram scale.
// Modeled on table_esmh_global (bucket emission) and
// table_file_summary_by_event_name (file-class iteration).

#include <sys/types.h>

#include "my_base.h"
#include "storage/perfschema/pfs_column_types.h"
#include "storage/perfschema/pfs_engine_table.h"
#include "storage/perfschema/pfs_histogram.h"
#include "storage/perfschema/table_helper.h"

class Field;
class Plugin_table;
struct PFS_file_class;
struct TABLE;
struct THR_LOCK;

// A materialized bucket of one file class histogram.
struct vsql_file_io_histogram_bucket {
  // Column COUNT_BUCKET.
  ulonglong m_count_bucket;
  // Column COUNT_BUCKET_AND_LOWER.
  ulonglong m_count_bucket_and_lower;
};

// A snapshot of one file class histogram, computed once per file class per scan
// so all its bucket rows share a consistent running total.
struct vsql_file_io_histogram_snapshot {
  PFS_event_name_row m_event_name;
  vsql_file_io_histogram_bucket m_buckets[NUMBER_OF_BUCKETS];
};

// A row of PERFORMANCE_SCHEMA.FILE_IO_HISTOGRAM.
struct row_vsql_file_io_histogram {
  // Column EVENT_NAME.
  PFS_event_name_row m_event_name;
  // Column BUCKET_NUMBER.
  ulong m_bucket_number;
  // Column BUCKET_TIMER_LOW.
  ulonglong m_bucket_timer_low;
  // Column BUCKET_TIMER_HIGH.
  ulonglong m_bucket_timer_high;
  // Column COUNT_BUCKET.
  ulonglong m_count_bucket;
  // Column COUNT_BUCKET_AND_LOWER.
  ulonglong m_count_bucket_and_lower;
  // Column BUCKET_QUANTILE.
  double m_percentile;
};

// Position: a file class index paired with a bucket index. The scan walks all
// buckets of one file class, then advances to the next file class.
struct pos_vsql_file_io_histogram : public PFS_double_index {
  pos_vsql_file_io_histogram() : PFS_double_index(1, 0) {}

  void reset() {
    m_index_1 = 1;
    m_index_2 = 0;
  }

  void next_file_class() {
    m_index_1++;
    m_index_2 = 0;
  }

  void next_bucket() { m_index_2++; }
};

class PFS_index_vsql_file_io_histogram : public PFS_engine_index {
 public:
  PFS_index_vsql_file_io_histogram()
      : PFS_engine_index(&m_key_1, &m_key_2),
        m_key_1("EVENT_NAME"),
        m_key_2("BUCKET_NUMBER") {}

  ~PFS_index_vsql_file_io_histogram() override = default;

  bool match(PFS_file_class *file_class);
  bool match_bucket(ulong bucket_index);

 private:
  PFS_key_event_name m_key_1;
  PFS_key_bucket_number m_key_2;
};

// Table PERFORMANCE_SCHEMA.FILE_IO_HISTOGRAM.
class table_vsql_file_io_histogram : public PFS_engine_table {
 public:
  // Table share.
  static PFS_engine_table_share m_share;
  static PFS_engine_table *create(PFS_engine_table_share *);
  static int delete_all_rows();
  static ha_rows get_row_count();

  void reset_position() override;

  int rnd_next() override;
  int rnd_pos(const void *pos) override;

  int index_init(uint idx, bool sorted) override;
  int index_next() override;

 protected:
  int read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                      bool read_all) override;

  table_vsql_file_io_histogram();

 public:
  ~table_vsql_file_io_histogram() override = default;

 protected:
  void materialize(PFS_file_class *file_class);
  int make_row(PFS_file_class *file_class, ulong bucket_index);

 private:
  // Table share lock.
  static THR_LOCK m_table_lock;
  // Table definition.
  static Plugin_table m_table_def;

  // File class whose histogram is currently snapshotted, or nullptr.
  PFS_file_class *m_materialized_class;
  // Snapshot of that file class histogram.
  vsql_file_io_histogram_snapshot m_snapshot;
  // Current row.
  row_vsql_file_io_histogram m_row;
  // Current position.
  pos_vsql_file_io_histogram m_pos;
  // Next position.
  pos_vsql_file_io_histogram m_next_pos;

  PFS_index_vsql_file_io_histogram *m_opened_index;
};

#endif  // TABLE_VSQL_FILE_IO_HISTOGRAM_H
