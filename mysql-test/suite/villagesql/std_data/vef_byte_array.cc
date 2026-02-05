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

// Test UDF with VEF support for byte array operations.
// Demonstrates statically-typed functions for custom type operations.

#include <villagesql/extension.h>

#include <cstdlib>
#include <cstring>

// Raw statically-typed function: parse string "[1, 2, 3]" to byte array
// Returns false on success, true on error
// Sets *length to number of bytes written, or SIZE_MAX for SQL NULL on invalid
// input
bool encode_byte_array(unsigned char *buffer, size_t buffer_size,
                       const char *from, size_t from_len, size_t *length) {
  // Parse format: "[1, 2, 3, ...]" or "[0x01, 0x02, ...]"
  const char *str = from;
  size_t str_len = from_len;

  // Handle NULL or empty input
  if (!str || str_len == 0) {
    *length = SIZE_MAX;  // Return SQL NULL
    return false;
  }

  // Simple parser: expect "[" ... "]"
  if (str_len < 2 || str[0] != '[' || str[str_len - 1] != ']') {
    *length = SIZE_MAX;  // Invalid format - return SQL NULL
    return false;
  }

  // Parse comma-separated numbers
  size_t bytes_written = 0;
  const char *p = str + 1;              // Skip '['
  const char *end = str + str_len - 1;  // Before ']'

  while (p < end && bytes_written < buffer_size) {
    // Skip whitespace
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end) break;

    // Parse number (decimal or hex)
    long value;
    if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
      // Hex format: 0x01
      value = strtol(p, const_cast<char **>(&p), 16);
    } else {
      // Decimal format: 123
      value = strtol(p, const_cast<char **>(&p), 10);
    }

    // Validate byte range
    if (value < 0 || value > 255) {
      *length = SIZE_MAX;  // Invalid value - return SQL NULL
      return false;
    }

    buffer[bytes_written++] = static_cast<unsigned char>(value);

    // Skip whitespace
    while (p < end && (*p == ' ' || *p == '\t')) p++;

    // Expect comma or end
    if (p < end && *p == ',') {
      p++;  // Skip comma
    }
  }

  *length = bytes_written;
  return false;  // Success
}

// Compare wrapper function for VEF - takes two string inputs
void byte_array_compare_impl(vef_context_t *ctx, vef_invalue_t *a,
                             vef_invalue_t *b, vef_vdf_result_t *out) {
  // Handle NULL cases
  if (a->is_null && b->is_null) {
    out->int_value = 0;  // NULL == NULL
    out->type = VEF_RESULT_VALUE;
    return;
  }
  if (a->is_null) {
    out->int_value = -1;  // NULL < non-NULL
    out->type = VEF_RESULT_VALUE;
    return;
  }
  if (b->is_null) {
    out->int_value = 1;  // non-NULL > NULL
    out->type = VEF_RESULT_VALUE;
    return;
  }

  // Compare byte-by-byte up to the shorter length
  size_t min_len = (a->str_len < b->str_len) ? a->str_len : b->str_len;

  for (size_t i = 0; i < min_len; i++) {
    unsigned char c1 = static_cast<unsigned char>(a->str_value[i]);
    unsigned char c2 = static_cast<unsigned char>(b->str_value[i]);
    if (c1 < c2) {
      out->int_value = -1;
      out->type = VEF_RESULT_VALUE;
      return;
    }
    if (c1 > c2) {
      out->int_value = 1;
      out->type = VEF_RESULT_VALUE;
      return;
    }
  }

  // If all compared bytes are equal, shorter array is "less than"
  if (a->str_len < b->str_len) {
    out->int_value = -1;
  } else if (a->str_len > b->str_len) {
    out->int_value = 1;
  } else {
    out->int_value = 0;
  }
  out->type = VEF_RESULT_VALUE;
}

VEF_GENERATE_ENTRY_POINTS(
    make_extension("vef_byte_array", "0.0.1-devtest")
        .func(make_func("byte_array_from_string")
                  .from_string<&encode_byte_array>(STRING))
        .func(make_func<&byte_array_compare_impl>("byte_array_compare")
                  .returns(INT)
                  .param(STRING)
                  .param(STRING)
                  .build()))
