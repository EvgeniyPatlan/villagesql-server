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

#include "villagesql/perfschema/vsql_file_io_histogram.h"

#include "storage/perfschema/pfs_histogram.h"
#include "storage/perfschema/pfs_stat.h"
#include "storage/perfschema/pfs_timer.h"

// The bucket boundaries are the shared statement/histogram scale
// (g_histogram_pico_timers, 450 geometric buckets from 10us). Reusing it keeps
// this consistent with events_statements_histogram_* and needs no new bucket
// math: the wait normalizer already maps a raw timer value to a bucket index.
void vsql_file_io_histogram_add(PFS_file_stat *file_stat, ulonglong wait_time) {
  time_normalizer *normalizer = time_normalizer::get_wait();
  const ulong bucket_index = normalizer->bucket_index(wait_time);
  file_stat->m_io_histogram.increment_bucket(bucket_index);
}
