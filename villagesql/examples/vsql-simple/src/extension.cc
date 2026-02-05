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

// VillageSQL extension demonstrating the extension.h API.
//
// The BYTEARRAY type is a fixed 8-byte value.

#include <cassert>
#include <cstring>

#include <villagesql/extension.h>

static const size_t kBytearrayLen = 8;

// from_string: string -> binary (copy up to 8 bytes, zero-pad)
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

// ROT13: apply ROT13 cipher to ASCII letters
void rot13_impl(vef_context_t *ctx, vef_invalue_t *input,
                vef_vdf_result_t *out) {
  if (input->is_null) {
    out->type = VEF_RESULT_NULL;
    return;
  }

  for (size_t i = 0; i < kBytearrayLen && i < input->bin_len; i++) {
    unsigned char c = input->bin_value[i];
    if (c >= 'A' && c <= 'Z') {
      out->bin_buf[i] = 'A' + ((c - 'A' + 13) % 26);
    } else if (c >= 'a' && c <= 'z') {
      out->bin_buf[i] = 'a' + ((c - 'a' + 13) % 26);
    } else {
      out->bin_buf[i] = c;
    }
  }
  out->type = VEF_RESULT_VALUE;
  out->actual_len = kBytearrayLen;
}

// EVEN_CHARS: extract bytes at positions 0, 2, 4, 6 (returns 4 bytes)
void even_chars_impl(vef_context_t *ctx, vef_invalue_t *input,
                     vef_vdf_result_t *out) {
  if (input->is_null) {
    out->type = VEF_RESULT_NULL;
    return;
  }

  memset(out->bin_buf, ' ', kBytearrayLen);
  if (input->bin_len >= kBytearrayLen) {
    out->bin_buf[0] = input->bin_value[0];
    out->bin_buf[1] = input->bin_value[2];
    out->bin_buf[2] = input->bin_value[4];
    out->bin_buf[3] = input->bin_value[6];
  }
  out->type = VEF_RESULT_VALUE;
  out->actual_len = kBytearrayLen;
}

// ODD_CHARS: extract bytes at positions 1, 3, 5, 7 (returns 4 bytes)
void odd_chars_impl(vef_context_t *ctx, vef_invalue_t *input,
                    vef_vdf_result_t *out) {
  if (input->is_null) {
    out->type = VEF_RESULT_NULL;
    return;
  }

  memset(out->bin_buf, ' ', kBytearrayLen);
  if (input->bin_len >= kBytearrayLen) {
    out->bin_buf[0] = input->bin_value[1];
    out->bin_buf[1] = input->bin_value[3];
    out->bin_buf[2] = input->bin_value[5];
    out->bin_buf[3] = input->bin_value[7];
  }
  out->type = VEF_RESULT_VALUE;
  out->actual_len = kBytearrayLen;
}

// BA_CONCAT: concatenate two bytearrays (returns STRING with 16 bytes)
void ba_concat_impl(vef_context_t *ctx, vef_invalue_t *a, vef_invalue_t *b,
                    vef_vdf_result_t *out) {
  if (a->is_null || b->is_null) {
    out->type = VEF_RESULT_NULL;
    return;
  }

  memset(out->str_buf, ' ', kBytearrayLen * 2);
  memcpy(out->str_buf, a->bin_value, kBytearrayLen);
  memcpy(out->str_buf + kBytearrayLen, b->bin_value, kBytearrayLen);
  out->type = VEF_RESULT_VALUE;
  out->actual_len = kBytearrayLen * 2;
}

using namespace villagesql::extension_builder;
using namespace villagesql::func_builder;
using namespace villagesql::type_builder;

constexpr const char *BYTEARRAY = "bytearray";

VEF_GENERATE_ENTRY_POINTS(
    make_extension("vsql_simpler", "0.0.1")
        .type(make_type(BYTEARRAY)
                  .persisted_length(kBytearrayLen)
                  .max_decode_buffer_length(kBytearrayLen)
                  .encode(&bytearray_from_string)
                  .decode(&bytearray_to_string)
                  .compare(&bytearray_compare)
                  .build())
        .func(make_func("bytearray_from_string")
                  .from_string<&bytearray_from_string>(BYTEARRAY))
        .func(make_func("bytearray_to_string")
                  .to_string<&bytearray_to_string>(BYTEARRAY))
        .func(make_func<&rot13_impl>("rot13")
                  .returns(BYTEARRAY)
                  .param(BYTEARRAY)
                  .build())
        .func(make_func<&even_chars_impl>("even_chars")
                  .returns(BYTEARRAY)
                  .param(BYTEARRAY)
                  .build())
        .func(make_func<&odd_chars_impl>("odd_chars")
                  .returns(BYTEARRAY)
                  .param(BYTEARRAY)
                  .build())
        .func(make_func<&ba_concat_impl>("ba_concat")
                  .returns(STRING)
                  .param(BYTEARRAY)
                  .param(BYTEARRAY)
                  .build()))
