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

// v2 of the update_test extension. Paired with update-test-v1.
// COUNTER type: same 4-byte storage as v1 (binary compatible).
// to_string produces "v2:N" (changed from v1).
// counter_double(x) now returns x * 3 (changed from v1's x * 2).

#include <villagesql/extension.h>

#include <cstdio>
#include <cstring>

static const size_t kCounterLen = 4;

// Encode: parse integer from string, store as 4-byte little-endian.
bool counter_encode(unsigned char *buffer, size_t buffer_size, const char *from,
                    size_t from_len, size_t *length) {
  char temp[64];
  size_t copy_len = from_len < sizeof(temp) - 1 ? from_len : sizeof(temp) - 1;
  memcpy(temp, from, copy_len);
  temp[copy_len] = '\0';
  int val = 0;
  sscanf(temp, "%d", &val);
  memcpy(buffer, &val, 4);
  *length = kCounterLen;
  return false;
}

// Decode: read 4-byte little-endian int, produce "v2:N".
bool counter_decode_v2(const unsigned char *buffer, size_t buffer_size,
                       char *to, size_t to_buffer_size, size_t *to_length) {
  int val = 0;
  memcpy(&val, buffer, 4);
  *to_length = snprintf(to, to_buffer_size, "v2:%d", val);
  return false;
}

int counter_compare(const unsigned char *data1, size_t len1,
                    const unsigned char *data2, size_t len2) {
  int v1 = 0, v2 = 0;
  memcpy(&v1, data1, 4);
  memcpy(&v2, data2, 4);
  return (v1 > v2) - (v1 < v2);
}

// counter_double(COUNTER) -> INT: returns value * 3 (changed from v1's * 2).
void counter_double_impl(vef_context_t *ctx, vef_invalue_t *input,
                         vef_vdf_result_t *out) {
  if (input->is_null) {
    out->type = VEF_RESULT_NULL;
    return;
  }
  int val = 0;
  memcpy(&val, input->bin_value, 4);
  out->int_value = (long long)val * 3;
  out->type = VEF_RESULT_VALUE;
}

using namespace villagesql::extension_builder;
using namespace villagesql::func_builder;
using namespace villagesql::type_builder;

constexpr const char *COUNTER = "COUNTER";

VEF_GENERATE_ENTRY_POINTS(
    make_extension()
        .type(make_type(COUNTER)
                  .persisted_length(kCounterLen)
                  .max_decode_buffer_length(32)
                  .encode(&counter_encode)
                  .decode(&counter_decode_v2)
                  .compare(&counter_compare)
                  .build())
        .func(make_func("counter_from_string")
                  .from_string<&counter_encode>(COUNTER))
        .func(make_func("counter_to_string")
                  .to_string<&counter_decode_v2>(COUNTER))
        .func(make_func<&counter_double_impl>("counter_double")
                  .returns(INT)
                  .param(COUNTER)
                  .build()))
