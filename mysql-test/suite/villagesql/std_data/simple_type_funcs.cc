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

// Simple test type functions for CREATE TYPE testing.
// Provides minimal FROM_STRING, TO_STRING, and COMPARE functions.
// Registers two types: SIMPLETYPE (fixed 32 bytes) and VARLENTYPE (variable).

#include <villagesql/extension.h>

#include <cstring>

// Simple encode: just copy input bytes to buffer
bool simple_encode(unsigned char *buffer, size_t buffer_size, const char *from,
                   size_t from_len, size_t *length) {
  if (!from || from_len == 0) {
    *length = SIZE_MAX;  // Return SQL NULL
    return false;
  }
  size_t to_copy = (from_len < buffer_size) ? from_len : buffer_size;
  memcpy(buffer, from, to_copy);
  *length = to_copy;
  return false;  // Success
}

// Simple decode: just copy buffer bytes to output
bool simple_decode(const unsigned char *buffer, size_t buffer_size, char *to,
                   size_t to_buffer_size, size_t *to_length) {
  if (!buffer || buffer_size == 0) {
    *to_length = 0;
    return false;
  }
  size_t to_copy =
      (buffer_size < to_buffer_size) ? buffer_size : to_buffer_size;
  memcpy(to, buffer, to_copy);
  *to_length = to_copy;
  return false;  // Success
}

// Simple compare: lexicographic comparison
int simple_cmp(const unsigned char *data1, size_t len1,
               const unsigned char *data2, size_t len2) {
  size_t min_len = (len1 < len2) ? len1 : len2;
  int result = memcmp(data1, data2, min_len);
  if (result != 0) return result;
  if (len1 < len2) return -1;
  if (len1 > len2) return 1;
  return 0;
}

using namespace villagesql::extension_builder;
using namespace villagesql::func_builder;
using namespace villagesql::type_builder;

constexpr const char *SIMPLETYPE = "simpletype";
constexpr const char *VARLENTYPE = "varlentype";

VEF_GENERATE_ENTRY_POINTS(
    make_extension("simple_type_funcs", "0.0.1-devtest")
        .type(make_type(SIMPLETYPE)
                  .persisted_length(32)
                  .max_decode_buffer_length(128)
                  .encode(&simple_encode)
                  .decode(&simple_decode)
                  .compare(&simple_cmp)
                  .build())
        .type(make_type(VARLENTYPE)
                  .persisted_length(-1)  // Variable length
                  .max_decode_buffer_length(65535)
                  .encode(&simple_encode)
                  .decode(&simple_decode)
                  .compare(&simple_cmp)
                  .build())
        .func(make_func("simple_from_string")
                  .from_string<&simple_encode>(SIMPLETYPE))
        .func(
            make_func("simple_to_string").to_string<&simple_decode>(SIMPLETYPE))
        .func(make_func("varlen_from_string")
                  .from_string<&simple_encode>(VARLENTYPE))
        .func(make_func("varlen_to_string")
                  .to_string<&simple_decode>(VARLENTYPE)))
