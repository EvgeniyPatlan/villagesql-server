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

#include "villagesql/perfschema/table_victionary_statistics.h"

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

THR_LOCK table_victionary_statistics::m_table_lock;

Plugin_table table_victionary_statistics::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "victionary_statistics",
    /* Definition */
    "  MAP_NAME VARCHAR(64) not null,\n"
    "  COMMITTED_ENTRIES BIGINT unsigned not null,\n"
    "  UNCOMMITTED_ENTRIES BIGINT unsigned not null,\n"
    "  CACHE_HITS BIGINT unsigned not null,\n"
    "  CACHE_MISSES BIGINT unsigned not null\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_victionary_statistics::m_share = {
    &pfs_readonly_acl,
    table_victionary_statistics::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_victionary_statistics::get_row_count,
    sizeof(PFS_simple_index),
    &m_table_lock,
    &m_table_def,
    false, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

PFS_engine_table *table_victionary_statistics::create(
    PFS_engine_table_share *) {
  return new table_victionary_statistics();
}

ha_rows table_victionary_statistics::get_row_count() {
  // We have 7 maps in VictionaryClient
  return 7;
}

table_victionary_statistics::table_victionary_statistics()
    : PFS_engine_table(&m_share, &m_pos), m_pos(0), m_next_pos(0) {}

void table_victionary_statistics::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
}

int table_victionary_statistics::rnd_init(bool) {
  reset_position();
  return 0;
}

int table_victionary_statistics::rnd_next() {
  villagesql::VictionaryClient &vc = villagesql::VictionaryClient::instance();

  if (!vc.is_initialized()) {
    return HA_ERR_END_OF_FILE;
  }

  // Acquire read lock
  auto lock = vc.get_read_lock();

  // Iterate through the 7 maps
  // 0 = properties, 1 = columns, 2 = extensions, 3 = type_descriptors,
  // 4 = extension_descriptors, 5 = type_contexts, 6 = funcs
  if (m_pos.m_index >= 7) {
    return HA_ERR_END_OF_FILE;
  }

  const char *map_name;
  size_t committed = 0;
  size_t uncommitted = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;

  switch (m_pos.m_index) {
    case 0: {
      map_name = "properties";
      auto stats = vc.properties().get_stats();
      committed = stats.committed_entries;
      uncommitted = stats.uncommitted_entries;
      hits = stats.hits;
      misses = stats.misses;
      break;
    }
    case 1: {
      map_name = "columns";
      auto stats = vc.columns().get_stats();
      committed = stats.committed_entries;
      uncommitted = stats.uncommitted_entries;
      hits = stats.hits;
      misses = stats.misses;
      break;
    }
    case 2: {
      map_name = "extensions";
      auto stats = vc.extensions().get_stats();
      committed = stats.committed_entries;
      uncommitted = stats.uncommitted_entries;
      hits = stats.hits;
      misses = stats.misses;
      break;
    }
    case 3: {
      map_name = "type_descriptors";
      auto stats = vc.type_descriptors().get_stats();
      committed = stats.committed_entries;
      uncommitted = stats.uncommitted_entries;
      hits = stats.hits;
      misses = stats.misses;
      break;
    }
    case 4: {
      map_name = "extension_descriptors";
      auto stats = vc.extension_descriptors().get_stats();
      committed = stats.committed_entries;
      uncommitted = stats.uncommitted_entries;
      hits = stats.hits;
      misses = stats.misses;
      break;
    }
    case 5: {
      map_name = "type_contexts";
      auto stats = vc.type_contexts().get_stats();
      committed = stats.committed_entries;
      uncommitted = stats.uncommitted_entries;
      hits = stats.hits;
      misses = stats.misses;
      break;
    }
    case 6: {
      map_name = "funcs";
      auto stats = vc.funcs().get_stats();
      committed = stats.committed_entries;
      uncommitted = stats.uncommitted_entries;
      hits = stats.hits;
      misses = stats.misses;
      break;
    }
    default:
      return HA_ERR_END_OF_FILE;
  }

  if (make_row(map_name, committed, uncommitted, hits, misses) == 0) {
    m_pos.m_index++;
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_RECORD_DELETED;
}

int table_victionary_statistics::rnd_pos(const void *pos) {
  set_position(pos);
  return HA_ERR_RECORD_DELETED;
}

int table_victionary_statistics::make_row(const char *map_name,
                                          size_t committed, size_t uncommitted,
                                          uint64_t hits, uint64_t misses) {
  m_row.m_map_name_length = static_cast<uint>(
      std::min(strlen(map_name), sizeof(m_row.m_map_name) - 1));
  memcpy(m_row.m_map_name, map_name, m_row.m_map_name_length);

  m_row.m_committed_entries = static_cast<ulonglong>(committed);
  m_row.m_uncommitted_entries = static_cast<ulonglong>(uncommitted);
  m_row.m_cache_hits = static_cast<ulonglong>(hits);
  m_row.m_cache_misses = static_cast<ulonglong>(misses);

  return 0;
}

int table_victionary_statistics::read_row_values(TABLE *table, unsigned char *,
                                                 Field **fields,
                                                 bool read_all) {
  Field *f;

  // Set the null bits
  assert(table->s->null_bytes == 0);

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0:  // MAP_NAME
          set_field_varchar_utf8mb4(f, m_row.m_map_name,
                                    m_row.m_map_name_length);
          break;
        case 1:  // COMMITTED_ENTRIES
          set_field_ulonglong(f, m_row.m_committed_entries);
          break;
        case 2:  // UNCOMMITTED_ENTRIES
          set_field_ulonglong(f, m_row.m_uncommitted_entries);
          break;
        case 3:  // CACHE_HITS
          set_field_ulonglong(f, m_row.m_cache_hits);
          break;
        case 4:  // CACHE_MISSES
          set_field_ulonglong(f, m_row.m_cache_misses);
          break;
        default:
          assert(false);
      }
    }
  }

  return 0;
}
