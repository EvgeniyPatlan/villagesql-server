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

// VillageSQL: PERFORMANCE_SCHEMA.FILE_IO_HISTOGRAM (implementation).

#include "villagesql/perfschema/table_vsql_file_io_histogram.h"

#include <assert.h>
#include <stddef.h>

#include "my_thread.h"
#include "sql/field.h"
#include "sql/plugin_table.h"
#include "sql/table.h"
#include "storage/perfschema/pfs_column_values.h"
#include "storage/perfschema/pfs_global.h"
#include "storage/perfschema/pfs_instr.h"
#include "storage/perfschema/pfs_instr_class.h"
#include "storage/perfschema/pfs_timer.h"

THR_LOCK table_vsql_file_io_histogram::m_table_lock;

Plugin_table table_vsql_file_io_histogram::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "file_io_histogram",
    /* Definition */
    "  EVENT_NAME VARCHAR(128) not null,\n"
    "  BUCKET_NUMBER INTEGER unsigned not null,\n"
    "  BUCKET_TIMER_LOW BIGINT unsigned not null,\n"
    "  BUCKET_TIMER_HIGH BIGINT unsigned not null,\n"
    "  COUNT_BUCKET BIGINT unsigned not null,\n"
    "  COUNT_BUCKET_AND_LOWER BIGINT unsigned not null,\n"
    "  BUCKET_QUANTILE DOUBLE(7,6) not null,\n"
    "  PRIMARY KEY (EVENT_NAME, BUCKET_NUMBER) USING HASH\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_vsql_file_io_histogram::m_share = {
    &pfs_truncatable_acl,
    table_vsql_file_io_histogram::create,
    nullptr, /* write_row */
    table_vsql_file_io_histogram::delete_all_rows,
    table_vsql_file_io_histogram::get_row_count,
    sizeof(pos_vsql_file_io_histogram),
    &m_table_lock,
    &m_table_def,
    false, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

bool PFS_index_vsql_file_io_histogram::match(PFS_file_class *file_class) {
  if (m_fields >= 1) {
    return m_key_1.match(file_class);
  }
  return true;
}

bool PFS_index_vsql_file_io_histogram::match_bucket(ulong bucket_index) {
  if (m_fields >= 2) {
    return m_key_2.match(bucket_index);
  }
  return true;
}

PFS_engine_table *table_vsql_file_io_histogram::create(
    PFS_engine_table_share *) {
  return new table_vsql_file_io_histogram();
}

int table_vsql_file_io_histogram::delete_all_rows() {
  reset_file_instance_io();
  reset_file_class_io();
  return 0;
}

ha_rows table_vsql_file_io_histogram::get_row_count() {
  return file_class_max * NUMBER_OF_BUCKETS;
}

table_vsql_file_io_histogram::table_vsql_file_io_histogram()
    : PFS_engine_table(&m_share, &m_pos),
      m_materialized_class(nullptr),
      m_pos(),
      m_next_pos(),
      m_opened_index(nullptr) {}

void table_vsql_file_io_histogram::reset_position() {
  m_pos.reset();
  m_next_pos.reset();
}

int table_vsql_file_io_histogram::rnd_next() {
  PFS_file_class *file_class;

  for (m_pos.set_at(&m_next_pos); m_pos.m_index_1 <= file_class_max;
       m_pos.next_file_class()) {
    file_class = find_file_class(m_pos.m_index_1);
    if (file_class != nullptr && m_pos.m_index_2 < NUMBER_OF_BUCKETS) {
      make_row(file_class, m_pos.m_index_2);
      m_next_pos.set_after(&m_pos);
      return 0;
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_vsql_file_io_histogram::rnd_pos(const void *pos) {
  PFS_file_class *file_class;

  set_position(pos);

  file_class = find_file_class(m_pos.m_index_1);
  if (file_class != nullptr && m_pos.m_index_2 < NUMBER_OF_BUCKETS) {
    return make_row(file_class, m_pos.m_index_2);
  }

  return HA_ERR_RECORD_DELETED;
}

int table_vsql_file_io_histogram::index_init(uint idx [[maybe_unused]], bool) {
  assert(idx == 0);
  auto *result = PFS_NEW(PFS_index_vsql_file_io_histogram);
  m_opened_index = result;
  m_index = result;
  return 0;
}

int table_vsql_file_io_histogram::index_next() {
  PFS_file_class *file_class;

  for (m_pos.set_at(&m_next_pos); m_pos.m_index_1 <= file_class_max;
       m_pos.next_file_class()) {
    file_class = find_file_class(m_pos.m_index_1);
    if (file_class == nullptr) {
      continue;
    }
    if (!m_opened_index->match(file_class)) {
      continue;
    }

    while (m_pos.m_index_2 < NUMBER_OF_BUCKETS) {
      if (m_opened_index->match_bucket(m_pos.m_index_2)) {
        if (!make_row(file_class, m_pos.m_index_2)) {
          m_next_pos.set_after(&m_pos);
          return 0;
        }
      }
      m_pos.next_bucket();
    }
  }

  return HA_ERR_END_OF_FILE;
}

void table_vsql_file_io_histogram::materialize(PFS_file_class *file_class) {
  if (file_class == m_materialized_class) {
    return;
  }

  m_snapshot.m_event_name.make_row(file_class);

  PFS_histogram *histogram = &file_class->m_file_stat.m_io_histogram;

  ulonglong count_and_lower = 0;
  for (ulong index = 0; index < NUMBER_OF_BUCKETS; index++) {
    const ulonglong count = histogram->read_bucket(index);
    count_and_lower += count;

    vsql_file_io_histogram_bucket &b = m_snapshot.m_buckets[index];
    b.m_count_bucket = count;
    b.m_count_bucket_and_lower = count_and_lower;
  }

  m_materialized_class = file_class;
}

int table_vsql_file_io_histogram::make_row(PFS_file_class *file_class,
                                           ulong bucket_index) {
  assert(bucket_index < NUMBER_OF_BUCKETS);

  materialize(file_class);

  m_row.m_event_name = m_snapshot.m_event_name;
  m_row.m_bucket_number = bucket_index;
  m_row.m_bucket_timer_low =
      g_histogram_pico_timers.m_bucket_timer[bucket_index];
  m_row.m_bucket_timer_high =
      g_histogram_pico_timers.m_bucket_timer[bucket_index + 1];
  m_row.m_count_bucket = m_snapshot.m_buckets[bucket_index].m_count_bucket;
  m_row.m_count_bucket_and_lower =
      m_snapshot.m_buckets[bucket_index].m_count_bucket_and_lower;

  const ulonglong count_star =
      m_snapshot.m_buckets[NUMBER_OF_BUCKETS - 1].m_count_bucket_and_lower;

  if (count_star > 0) {
    const double dividend = m_row.m_count_bucket_and_lower;
    const double divisor = count_star;
    m_row.m_percentile = dividend / divisor; /* computed with double, not int */
  } else {
    m_row.m_percentile = 0.0;
  }

  return 0;
}

int table_vsql_file_io_histogram::read_row_values(TABLE *table, unsigned char *,
                                                  Field **fields,
                                                  bool read_all) {
  Field *f;

  assert(table->s->null_bytes == 0);

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0: /* EVENT_NAME */
          m_row.m_event_name.set_field(f);
          break;
        case 1: /* BUCKET_NUMBER */
          set_field_ulong(f, m_row.m_bucket_number);
          break;
        case 2: /* BUCKET_TIMER_LOW */
          set_field_ulonglong(f, m_row.m_bucket_timer_low);
          break;
        case 3: /* BUCKET_TIMER_HIGH */
          set_field_ulonglong(f, m_row.m_bucket_timer_high);
          break;
        case 4: /* COUNT_BUCKET */
          set_field_ulonglong(f, m_row.m_count_bucket);
          break;
        case 5: /* COUNT_BUCKET_AND_LOWER */
          set_field_ulonglong(f, m_row.m_count_bucket_and_lower);
          break;
        case 6: /* BUCKET_QUANTILE */
          set_field_double(f, m_row.m_percentile);
          break;
        default:
          assert(false);
          break;
      }
    }
  }

  return 0;
}
