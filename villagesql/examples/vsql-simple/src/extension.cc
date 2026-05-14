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

// VillageSQL extension demonstrating the vsql API.
//
// The BYTEARRAY type is a fixed 8-byte value.

#include <villagesql/vsql.h>

using namespace vsql;

static const size_t kBytearrayLen = 8;

// from_string: string -> binary (copy up to 8 bytes, space-pad)
void bytearray_from_string(std::string_view from, CustomResult out) {
  auto buf = out.buffer();
  if (buf.size() < kBytearrayLen) return;  // wrapper default warning
  memset(buf.data(), ' ', kBytearrayLen);
  size_t copy_len = from.size() < kBytearrayLen ? from.size() : kBytearrayLen;
  if (copy_len > 0) memcpy(buf.data(), from.data(), copy_len);
  out.set_length(kBytearrayLen);
}

// to_string: binary -> string (copy 8 bytes)
bool bytearray_to_string(Span<const unsigned char> data, Span<char> out,
                         size_t *out_len) {
  if (data.size() < kBytearrayLen || out.size() < kBytearrayLen) return true;
  memcpy(out.data(), data.data(), kBytearrayLen);
  *out_len = kBytearrayLen;
  return false;  // success
}

// Compare: lexicographic byte comparison
int bytearray_compare(Span<const unsigned char> a,
                      Span<const unsigned char> b) {
  return memcmp(a.data(), b.data(), kBytearrayLen);
}

// ROT13: apply ROT13 cipher to ASCII letters
void rot13(CustomArg in, CustomResult out) {
  if (in.is_null()) {
    out.set_null();
    return;
  }
  auto src = in.value();
  auto dst = out.buffer();
  for (size_t i = 0; i < kBytearrayLen && i < src.size(); i++) {
    unsigned char c = src[i];
    if (c >= 'A' && c <= 'Z') {
      dst[i] = 'A' + ((c - 'A' + 13) % 26);
    } else if (c >= 'a' && c <= 'z') {
      dst[i] = 'a' + ((c - 'a' + 13) % 26);
    } else {
      dst[i] = c;
    }
  }
  out.set_length(kBytearrayLen);
}

// EVEN_CHARS: extract bytes at positions 0, 2, 4, 6 (returns 4 bytes)
void even_chars(CustomArg in, CustomResult out) {
  if (in.is_null()) {
    out.set_null();
    return;
  }
  auto src = in.value();
  auto dst = out.buffer();
  memset(dst.data(), ' ', kBytearrayLen);
  if (src.size() >= kBytearrayLen) {
    dst[0] = src[0];
    dst[1] = src[2];
    dst[2] = src[4];
    dst[3] = src[6];
  }
  out.set_length(kBytearrayLen);
}

// ODD_CHARS: extract bytes at positions 1, 3, 5, 7 (returns 4 bytes)
void odd_chars(CustomArg in, CustomResult out) {
  if (in.is_null()) {
    out.set_null();
    return;
  }
  auto src = in.value();
  auto dst = out.buffer();
  memset(dst.data(), ' ', kBytearrayLen);
  if (src.size() >= kBytearrayLen) {
    dst[0] = src[1];
    dst[1] = src[3];
    dst[2] = src[5];
    dst[3] = src[7];
  }
  out.set_length(kBytearrayLen);
}

// BA_CONCAT: concatenate two bytearrays (returns STRING with 16 bytes)
void ba_concat(CustomArg a, CustomArg b, StringResult out) {
  if (a.is_null() || b.is_null()) {
    out.set_null();
    return;
  }
  auto dst = out.buffer();
  memset(dst.data(), ' ', kBytearrayLen * 2);
  memcpy(dst.data(), a.value().data(), kBytearrayLen);
  memcpy(dst.data() + kBytearrayLen, b.value().data(), kBytearrayLen);
  out.set_length(kBytearrayLen * 2);
}

// BA_LEN: return the fixed length of a BYTEARRAY (zero-arity constant function)
void ba_len(IntResult out) { out.set(static_cast<long long>(kBytearrayLen)); }

// BA_CONCAT_ALL: concatenate any number of bytearrays (returns STRING).
//
// This demonstrates the "Escape Hatch" pattern: ba_concat above uses the
// typed Happy Path API (CustomArg, StringResult), but only supports exactly
// two arguments. ba_concat_all drops down to the raw ABI to accept varargs,
// using prerun to validate argument types and size the result buffer.

// Prerun: validate that all arguments are BYTEARRAY and request a buffer
// large enough to hold them all concatenated.
void ba_concat_all_prerun(vef_context_t *, vef_prerun_args_t *args,
                          vef_prerun_result_t *result) {
  if (args->arg_count == 0) {
    result->type = VEF_RESULT_ERROR;
    snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
             "ba_concat_all requires at least one argument");
    return;
  }
  for (unsigned int i = 0; i < args->arg_count; i++) {
    // NULL literals appear as VEF_TYPE_STRING in prerun; skip type check for
    // those and handle them at runtime.
    vef_type_id id = args->arg_types[i].id;
    if (id != VEF_TYPE_CUSTOM && id != VEF_TYPE_STRING) {
      result->type = VEF_RESULT_ERROR;
      snprintf(result->error_msg, VEF_MAX_ERROR_LEN,
               "ba_concat_all: argument %u must be BYTEARRAY", i);
      return;
    }
  }
  result->result_buffer_size = args->arg_count * kBytearrayLen;
}

// Main function: raw ABI signature since we iterate over a variable number
// of arguments.
void ba_concat_all(vef_context_t *ctx, vef_vdf_args_t *args,
                   vef_vdf_result_t *result) {
  size_t total_len = args->value_count * kBytearrayLen;
  for (unsigned int i = 0; i < args->value_count; i++) {
    vef_invalue_t val = vsql::func_builder::get_invalue(ctx, args, i);
    if (val.is_null) {
      result->type = VEF_RESULT_NULL;
      return;
    }
    memcpy(result->str_buf + i * kBytearrayLen, val.bin_value, kBytearrayLen);
  }
  result->type = VEF_RESULT_VALUE;
  result->actual_len = total_len;
}

static constexpr const char kBytearrayTypeName[] = "bytearray";

constexpr auto BYTEARRAY = vsql::make_type<kBytearrayTypeName>()
                               .persisted_length(kBytearrayLen)
                               .max_decode_buffer_length(kBytearrayLen)
                               .from_string<&bytearray_from_string>()
                               .to_string<&bytearray_to_string>()
                               .compare<&bytearray_compare>()
                               .build();

VEF_GENERATE_ENTRY_POINTS(
    make_extension()
        .type(BYTEARRAY)
        .func(make_func<&rot13>("rot13")
                  .returns(BYTEARRAY)
                  .param(BYTEARRAY)
                  .build())
        .func(make_func<&even_chars>("even_chars")
                  .returns(BYTEARRAY)
                  .param(BYTEARRAY)
                  .build())
        .func(make_func<&odd_chars>("odd_chars")
                  .returns(BYTEARRAY)
                  .param(BYTEARRAY)
                  .build())
        .func(make_func<&ba_concat>("ba_concat")
                  .returns(STRING)
                  .param(BYTEARRAY)
                  .param(BYTEARRAY)
                  .build())
        .func(make_func<&ba_len>("ba_len").returns(INT).param().build())
        .func(make_func<&ba_concat_all>("ba_concat_all")
                  .returns(STRING)
                  .varargs()
                  .prerun<&ba_concat_all_prerun>()
                  .build()))
