// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is designed to work with certain software (including
// but not limited to OpenSSL) that is licensed under separate terms,
// as designated in a particular file or component or in included license
// documentation.  The authors of MySQL hereby grant you an additional
// permission to link the program and your derivative works with the
// separately licensed software that they have either included with
// the program or referenced in the documentation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

// VillageSQL extension demonstrating a VDF with mixed parameter types.
//
// The BYTEARRAY type is a fixed 8-byte value.
// The mask(ba, n) function takes a BYTEARRAY and an INT, returning a BYTEARRAY
// with the byte at offset 'n' replaced with '*'.

#include <cassert>
#include <cstring>

#include <villagesql/extension.h>

static const size_t kBytearrayLen = 8;

// from_string: string -> binary (copy up to 8 bytes, space-pad)
bool bytearray_from_string(unsigned char *buf, size_t buf_size,
                           const char *from, size_t from_len, size_t *length) {
  if (buf_size < kBytearrayLen) return true;  // error
  memset(buf, ' ', kBytearrayLen);
  size_t copy_len = from_len < kBytearrayLen ? from_len : kBytearrayLen;
  if (from && copy_len > 0) memcpy(buf, from, copy_len);
  *length = kBytearrayLen;
  return false;  // success
}

// to_string: binary -> string (copy 8 bytes)
bool bytearray_to_string(const unsigned char *buf, size_t buf_size, char *to,
                         size_t to_size, size_t *to_length) {
  if (to_size < kBytearrayLen) return true;  // error
  memcpy(to, buf, kBytearrayLen);
  *to_length = kBytearrayLen;
  return false;  // success
}

// Compare: lexicographic byte comparison
int bytearray_compare(const unsigned char *a, size_t a_len,
                      const unsigned char *b, size_t b_len) {
  assert(a_len == kBytearrayLen && b_len == kBytearrayLen);
  return memcmp(a, b, kBytearrayLen);
}

// mask: Replace the byte at offset 'n' with '*'
// Takes: BYTEARRAY, INT
// Returns: BYTEARRAY with byte at position n masked
void mask_impl(vef_context_t *ctx, vef_invalue_t *input, vef_invalue_t *offset,
               vef_vdf_result_t *out) {
  // Handle NULL inputs
  if (input->is_null || offset->is_null) {
    out->type = VEF_RESULT_NULL;
    return;
  }

  // Copy input to output
  memcpy(out->bin_buf, input->bin_value, kBytearrayLen);

  // Get offset value and validate
  long long n = offset->int_value;
  if (n >= 0 && static_cast<size_t>(n) < kBytearrayLen) {
    out->bin_buf[n] = '*';
  }
  // If offset is out of range, just return the original unchanged

  out->type = VEF_RESULT_VALUE;
  out->actual_len = kBytearrayLen;
}

constexpr const char *BYTEARRAY = "bytearray";

VEF_GENERATE_ENTRY_POINTS(
    make_extension("bytearray", "0.0.1")
        .type(make_type(BYTEARRAY)
                  .persisted_length(kBytearrayLen)
                  .max_decode_buffer_length(kBytearrayLen)
                  .encode(&bytearray_from_string)
                  .decode(&bytearray_to_string)
                  .compare(&bytearray_compare)
                  .build())
        .func(make_func("from_string")
                  .from_string<&bytearray_from_string>(BYTEARRAY))
        .func(make_func("to_string").to_string<&bytearray_to_string>(BYTEARRAY))
        .func(make_func<&mask_impl>("mask")
                  .returns(BYTEARRAY)
                  .param(BYTEARRAY)
                  .param(INT)
                  .build()))
