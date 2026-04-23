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

// WHY THIS FILE EXISTS
//
// We considered using undefined functions as compile-time guards in the SDK
// builder (e.g., calling a never-defined function when the user does something
// invalid like mixing .param() and .varargs()). The idea was that the linker
// would reject the undefined symbol.
//
// The concern: what if the extension is built with
//   target_link_options(ext PRIVATE -undefined dynamic_lookup)
// which tells the macOS linker to skip undefined symbol checks?
//
// This file is TEST CASE 1: the undefined function is called from a constexpr
// context (the VEF_GENERATE_ENTRY_POINTS macro forces constexpr evaluation).
//
// RESULT: This file FAILS TO COMPILE. The constexpr evaluator catches the
// call to a non-constexpr function before the linker is even invoked. The
// -undefined dynamic_lookup flag is irrelevant here.
//
// This is why static_assert works for our builder checks — the builder chain
// is always evaluated in a constexpr context via VEF_GENERATE_ENTRY_POINTS.
//
// See also: dynamic_lookup_runtime_fail.cc for the case where the undefined
// function is called from a non-constexpr context.

#include <villagesql/vsql.h>

#include <cstring>

using namespace vsql;

// Intentionally undefined. Calling this in a constexpr context is a
// compile error because the constexpr evaluator requires all called
// functions to be constexpr-evaluable.
void config_error__should_not_compile();

void some_func(StringResult out) {
  const char *msg = "shouldn't work";
  auto buf = out.buffer();
  memcpy(buf.data(), msg, strlen(msg));
  out.set_length(strlen(msg));
}

// The undefined function is called inside the constexpr builder chain.
// The compiler traces through the constexpr evaluation, hits
// config_error__should_not_compile(), and rejects it because a
// non-constexpr function cannot be called during constant evaluation.
//
// Error: "constexpr variable 'kExt' must be initialized by a constant
// expression ... non-constexpr function 'config_error__should_not_compile'
// cannot be used in a constant expression"
constexpr auto bad_func() {
  config_error__should_not_compile();
  return make_func<&some_func>("some_func")
      .returns(STRING)
      .buffer_size(64)
      .build();
}

VEF_GENERATE_ENTRY_POINTS(
    make_extension(VEF_EXTENSION_NAME, "0.0.1-devtest")
        .func(bad_func()))
