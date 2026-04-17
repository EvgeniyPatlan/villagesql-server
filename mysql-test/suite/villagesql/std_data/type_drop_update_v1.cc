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

// type_drop_update_v1.cc - v1 with two types: COUNTER and GAUGE.
// Used to test that UPDATE can drop a type when no columns/SP params depend
// on it, and is blocked when they do.

#include <villagesql/extension.h>

#include <cstdio>
#include <cstring>

static const size_t kLen = 4;

static bool encode_int(unsigned char *buffer, size_t buffer_size,
                       const char *from, size_t from_len, size_t *length) {
  char temp[64];
  size_t copy_len = from_len < sizeof(temp) - 1 ? from_len : sizeof(temp) - 1;
  memcpy(temp, from, copy_len);
  temp[copy_len] = '\0';
  int val = 0;
  sscanf(temp, "%d", &val);
  memcpy(buffer, &val, 4);
  *length = kLen;
  return false;
}

static bool decode_counter(const unsigned char *buffer, size_t buffer_size,
                           char *to, size_t to_buffer_size, size_t *to_length) {
  int val = 0;
  memcpy(&val, buffer, 4);
  *to_length = snprintf(to, to_buffer_size, "counter:%d", val);
  return false;
}

static bool decode_gauge(const unsigned char *buffer, size_t buffer_size,
                         char *to, size_t to_buffer_size, size_t *to_length) {
  int val = 0;
  memcpy(&val, buffer, 4);
  *to_length = snprintf(to, to_buffer_size, "gauge:%d", val);
  return false;
}

static int compare_int(const unsigned char *a, size_t la,
                       const unsigned char *b, size_t lb) {
  int va = 0, vb = 0;
  memcpy(&va, a, 4);
  memcpy(&vb, b, 4);
  return (va > vb) - (va < vb);
}

using namespace villagesql::extension_builder;
using namespace villagesql::func_builder;
using namespace villagesql::type_builder;

constexpr const char *COUNTER = "COUNTER";
constexpr const char *GAUGE = "GAUGE";

VEF_GENERATE_ENTRY_POINTS(
    make_extension()
        .type(make_type(COUNTER)
                  .persisted_length(kLen)
                  .max_decode_buffer_length(32)
                  .encode(&encode_int)
                  .decode(&decode_counter)
                  .compare(&compare_int)
                  .build())
        .type(make_type(GAUGE)
                  .persisted_length(kLen)
                  .max_decode_buffer_length(32)
                  .encode(&encode_int)
                  .decode(&decode_gauge)
                  .compare(&compare_int)
                  .build())
        .func(make_func("counter_val").to_string<&decode_counter>(COUNTER))
        .func(make_func("gauge_val").to_string<&decode_gauge>(GAUGE)))
