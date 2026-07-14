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

#ifndef VSQL_FILE_IO_HISTOGRAM_H
#define VSQL_FILE_IO_HISTOGRAM_H

// VillageSQL: per-file-class latency histogram for file I/O.
//
// Upstream Performance Schema builds latency histograms only for statements
// (events_statements_histogram_*, WL#5384). File I/O keeps only moments
// (count/sum/min/max) in file_summary. This adds a latency *distribution* for
// file I/O, feeding the existing PFS_histogram bucket machinery from the
// file-wait aggregation point.
//
// This header is the only VillageSQL surface the upstream file-wait path needs.

#include "my_inttypes.h"

struct PFS_file_stat;

// Record one completed, timed file-I/O wait into its file class histogram.
// wait_time is in timer units (same units as PFS_file_io_stat aggregation);
// it is converted to the shared picosecond bucket scale internally.
// Called from the timed branch of the upstream file-wait end path.
void vsql_file_io_histogram_add(PFS_file_stat *file_stat, ulonglong wait_time);

#endif  // VSQL_FILE_IO_HISTOGRAM_H
