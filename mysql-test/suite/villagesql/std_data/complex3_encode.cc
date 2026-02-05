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

// Test UDFs for COMPLEX3 type - provides FROM_STRING, TO_STRING, and COMPARE.
// Used to verify VEF function lookup from custom type.

#include <villagesql/extension.h>

#include <cstring>

static const size_t kComplex3Len = 16;

// FROM_STRING: Raw statically-typed function - always returns 16 zero bytes
// This is used to verify the VEF lookup mechanism works
bool complex3_from_string(unsigned char *buffer, size_t buffer_size,
                          const char *from, size_t from_len, size_t *length) {
  (void)from;      // Unused - we always return zeros
  (void)from_len;  // Unused

  // Need at least 16 bytes
  if (buffer_size < kComplex3Len || !buffer) {
    *length = 0;
    return true;  // Error
  }

  // Zero out 16 bytes
  memset(buffer, 0, kComplex3Len);
  *length = kComplex3Len;
  return false;  // Success
}

// TO_STRING: Convert binary to string representation
bool complex3_to_string(const unsigned char *buffer, size_t buffer_size,
                        char *to, size_t to_buffer_size, size_t *to_length) {
  (void)buffer;       // Unused - always return "(0,0)"
  (void)buffer_size;  // Unused

  const char *result = "(0,0)";
  size_t len = strlen(result);

  if (to_buffer_size < len + 1 || !to) {
    return true;  // Error
  }

  memcpy(to, result, len);
  *to_length = len;
  return false;  // Success
}

// COMPARE: Compare two values (always returns 0 for equal)
int complex3_compare(const unsigned char *data1, size_t len1,
                     const unsigned char *data2, size_t len2) {
  (void)data1;
  (void)data2;
  (void)len1;
  (void)len2;
  return 0;  // Always equal
}

using namespace villagesql::extension_builder;
using namespace villagesql::func_builder;
using namespace villagesql::type_builder;

constexpr const char *COMPLEX3 = "COMPLEX3";

VEF_GENERATE_ENTRY_POINTS(
    make_extension("complex3_ext", "0.0.1-devtest")
        .type(make_type(COMPLEX3)
                  .persisted_length(kComplex3Len)
                  .max_decode_buffer_length(64)
                  .encode(&complex3_from_string)
                  .decode(&complex3_to_string)
                  .compare(&complex3_compare)
                  .build())
        .func(make_func("COMPLEX_FROM_STRING")
                  .from_string<&complex3_from_string>(COMPLEX3))
        .func(make_func("COMPLEX_TO_STRING")
                  .to_string<&complex3_to_string>(COMPLEX3)))
