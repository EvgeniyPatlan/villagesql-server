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
// builder. This file is TEST CASE 2: the undefined function is called from
// a non-constexpr context — a regular function that is never actually invoked.
//
// BUILD BEHAVIOR:
//   Without -undefined dynamic_lookup: the linker rejects the undefined
//   symbol and the build fails. The guard works as intended.
//
//   With -undefined dynamic_lookup: the linker defers symbol resolution to
//   runtime. The build SUCCEEDS. The .so is produced and packaged into a .veb.
//
// LOAD BEHAVIOR:
//   When the server loads the extension via dlopen(), macOS resolves all
//   symbols in the shared library's symbol table — even those in functions
//   that are never called. dlopen() finds that
//   config_error__should_not_link is undefined and fails with:
//     "symbol not found in flat namespace"
//
//   So the guard still catches it, but:
//   1. The error happens at INSTALL EXTENSION time, not at build time.
//   2. The error message is an inscrutable dlopen failure, not a clear
//      "you called .param() and .varargs() together" message.
//
// CONCLUSION: Undefined-function guards are fragile. They depend on linker
// behavior that extension authors can (and do) override. static_assert in
// constexpr contexts is strictly better: it fires at compile time regardless
// of linker flags, and the error message is clear.
//
// See also: dynamic_lookup_constexpr_fail.cc for the constexpr case.

#include <villagesql/vsql.h>

#include <cstring>

using namespace vsql;

// Intentionally undefined function. Without -undefined dynamic_lookup,
// the linker rejects this. With it, the linker lets it through.
void config_error__should_not_link();

void some_func(StringResult out) {
  const char *msg = "shouldn't work";
  auto buf = out.buffer();
  memcpy(buf.data(), msg, strlen(msg));
  out.set_length(strlen(msg));
}

// This function is never called at runtime. But its mere existence puts
// config_error__should_not_link into the symbol table. On macOS, dlopen()
// resolves all symbols eagerly, so the undefined reference is caught at
// load time even though this code path is never executed.
void never_called() { config_error__should_not_link(); }

VEF_GENERATE_ENTRY_POINTS(
    make_extension(VEF_EXTENSION_NAME, "0.0.1-devtest")
        .func(make_func<&some_func>("some_func")
                  .returns(STRING)
                  .buffer_size(64)
                  .build()))
