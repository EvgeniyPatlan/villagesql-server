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

// TESTTYPE implementation with short precision (%.6g format)
// Stores two doubles (real and imaginary) as 16 bytes.

#include <villagesql/extension.h>

#include <cstdio>
#include <cstring>

static const size_t kTestTypeLen = 16;

bool encode_testtype(unsigned char *buffer, size_t buffer_size,
                     const char *from, size_t from_len, size_t *length) {
  double real = 0, imag = 0;
  char temp[256];
  size_t copy_len = from_len < sizeof(temp) - 1 ? from_len : sizeof(temp) - 1;
  memcpy(temp, from, copy_len);
  temp[copy_len] = '\0';
  sscanf(temp, " ( %lf , %lf )", &real, &imag);
  memcpy(buffer, &real, 8);
  memcpy(buffer + 8, &imag, 8);
  *length = kTestTypeLen;
  return false;
}

bool decode_testtype_short(const unsigned char *buffer, size_t buffer_size,
                           char *to, size_t to_buffer_size, size_t *to_length) {
  double real, imag;
  memcpy(&real, buffer, 8);
  memcpy(&imag, buffer + 8, 8);
  *to_length = snprintf(to, to_buffer_size, "(%.6g,%.6g)", real, imag);
  return false;
}

int cmp_testtype(const unsigned char *data1, size_t len1,
                 const unsigned char *data2, size_t len2) {
  return memcmp(data1, data2, kTestTypeLen);
}

using namespace villagesql::extension_builder;
using namespace villagesql::func_builder;
using namespace villagesql::type_builder;

constexpr const char *TESTTYPE = "TESTTYPE";

VEF_GENERATE_ENTRY_POINTS(
    make_extension("testtype_short", "0.0.1-devtest")
        .type(make_type(TESTTYPE)
                  .persisted_length(kTestTypeLen)
                  .max_decode_buffer_length(64)
                  .encode(&encode_testtype)
                  .decode(&decode_testtype_short)
                  .compare(&cmp_testtype)
                  .build())
        .func(make_func("testtype_from_string")
                  .from_string<&encode_testtype>(TESTTYPE))
        .func(make_func("testtype_to_string")
                  .to_string<&decode_testtype_short>(TESTTYPE)))
