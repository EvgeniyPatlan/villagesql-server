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

// Test UDF for bad type tests.
// Provides functions for testing type validation scenarios.

#include <villagesql/extension.h>

#include <cstring>

// Generic FROM_STRING function
bool f1_impl(unsigned char *buffer, size_t buffer_size, const char *from,
             size_t from_len, size_t *length) {
  (void)from;
  (void)from_len;

  if (buffer_size < 16 || !buffer) {
    *length = 0;
    return true;
  }

  memset(buffer, 0, 16);
  *length = 16;
  return false;
}

// Generic TO_STRING function
bool f2_impl(const unsigned char *buffer, size_t buffer_size, char *to,
             size_t to_buffer_size, size_t *to_length) {
  (void)buffer;
  (void)buffer_size;

  if (to_buffer_size < 4 || !to) {
    return true;
  }

  strcpy(to, "val");
  *to_length = 3;
  return false;
}

// Generic COMPARE function
int f3_impl(const unsigned char *a, size_t a_len, const unsigned char *b,
            size_t b_len) {
  (void)a_len;
  (void)b_len;
  return memcmp(a, b, 16);
}

using namespace villagesql::extension_builder;
using namespace villagesql::func_builder;
using namespace villagesql::type_builder;

// Register a simple type for testing - tests can use this extension
// to verify basic type operations work
VEF_GENERATE_ENTRY_POINTS(
    make_extension("bad_type_funcs", "0.0.1-devtest")
        .type(make_type("TESTBADTYPE")
                  .persisted_length(16)
                  .max_decode_buffer_length(64)
                  .encode(&f1_impl)
                  .decode(&f2_impl)
                  .compare(&f3_impl)
                  .build())
        .func(make_func("f1").from_string<&f1_impl>("TESTBADTYPE"))
        .func(make_func("f2").to_string<&f2_impl>("TESTBADTYPE")))
