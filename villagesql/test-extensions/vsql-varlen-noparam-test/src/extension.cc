// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

// Test fixture for issue #535: a variable-length type whose length is decided
// per value (like a ROARING64 roaring bitmap), not by a type parameter.
//
// VARBITMAP declares persisted_length = -1 with a max_persisted_length upper
// bound and NO params/resolve_params/int_to_params. This is the same shape as
// the community ROARING64 type from the report (plus the now-required
// max_persisted_length). A column of this type is declared without any length
// or parameters:
//   CREATE TABLE t (bitmap vsql_varlen_noparam_test.VARBITMAP);
// and each value keeps its own byte length, like VARBINARY.

#include <villagesql/vsql.h>

#include <cstring>
#include <string_view>

// Upper bound on a stored VARBITMAP value; sizes the backing field and the
// decode buffer.
constexpr int64_t kVarbitmapMaxLen = 1024;

// STRING -> binary: store the input bytes verbatim; length = input length.
void varbitmap_from_string(std::string_view from, vsql::CustomResult out) {
  auto buf = out.buffer();
  if (from.size() > buf.size()) {
    out.warning("VARBITMAP: value exceeds max length");
    return;
  }
  if (!from.empty()) memcpy(buf.data(), from.data(), from.size());
  out.set_length(from.size());
}

// binary -> STRING: copy the stored bytes back out verbatim.
void varbitmap_to_string(vsql::CustomArg in, vsql::StringResult out) {
  auto data = in.value();
  auto buf = out.buffer();
  if (data.size() > buf.size()) return;
  if (!data.empty()) memcpy(buf.data(), data.data(), data.size());
  out.set_length(data.size());
}

// Lexicographic byte comparison; shorter value sorts first on a common prefix.
int varbitmap_compare(vsql::CustomArg a, vsql::CustomArg b) {
  auto va = a.value();
  auto vb = b.value();
  size_t n = va.size() < vb.size() ? va.size() : vb.size();
  int r = memcmp(va.data(), vb.data(), n);
  if (r != 0) return r < 0 ? -1 : 1;
  if (va.size() != vb.size()) return va.size() < vb.size() ? -1 : 1;
  return 0;
}

// Number of stored bytes in this VARBITMAP value -- the "element count" for a
// type that stores raw bytes. The server treats the value as opaque, so the
// extension exposes the count as a SQL-callable VDF (a roaring bitmap would
// instead return its cardinality here).
void varbitmap_length(vsql::CustomArg in, vsql::IntResult out) {
  if (in.is_null()) {
    out.set_null();
    return;
  }
  out.set(static_cast<long long>(in.value().size()));
}

static constexpr const char kVarbitmapTypeName[] = "VARBITMAP";

constexpr auto VARBITMAP = vsql::make_type<kVarbitmapTypeName>()
                               .persisted_length(-1)
                               .max_persisted_length(kVarbitmapMaxLen)
                               .max_decode_buffer_length(kVarbitmapMaxLen)
                               .from_string<&varbitmap_from_string>()
                               .to_string<&varbitmap_to_string>()
                               .compare<&varbitmap_compare>()
                               .build();

using namespace vsql;

VEF_GENERATE_ENTRY_POINTS(make_extension().type(VARBITMAP).func(
    make_func<&varbitmap_length>("varbitmap_length")
        .returns(INT)
        .param(VARBITMAP)
        .deterministic()
        .build()))
