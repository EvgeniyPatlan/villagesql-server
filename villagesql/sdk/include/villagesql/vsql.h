// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VILLAGESQL_VSQL_H
#define VILLAGESQL_VSQL_H

// =============================================================================
// VillageSQL Extension SDK
// =============================================================================
//
// This is the recommended header for writing VillageSQL extensions.
//
//   #include <villagesql/vsql.h>
//   using namespace vsql;
//
// The vsql namespace provides everything you need: function definitions,
// type definitions, system variables, and extension registration.
//
//
// ---- QUICK START ------------------------------------------------------------
//
//   #include <villagesql/vsql.h>
//   using namespace vsql;
//
//   // 1. Implement your type operations
//   bool complex_from_string(std::string_view s,
//                            Span<unsigned char> buf, size_t* len);
//   bool complex_to_string(Span<const unsigned char> data,
//                          Span<char> out, size_t* out_len);
//   int  complex_compare(Span<const unsigned char> a,
//                        Span<const unsigned char> b);
//
//   // 2. Define the type
//   static constexpr const char kComplexTypeName[] = "COMPLEX";
//   constexpr auto COMPLEX = make_type<kComplexTypeName>()
//       .persisted_length(16)
//       .max_decode_buffer_length(64)
//       .from_string<&complex_from_string>()
//       .to_string<&complex_to_string>()
//       .compare<&complex_compare>()
//       .build();  // compile error if any required operation is missing
//
//   // 3. Write functions that operate on COMPLEX values
//   void complex_add(CustomArg a, CustomArg b, CustomResult out) { ... }
//
//   // 4. Register
//   VEF_GENERATE_ENTRY_POINTS(
//     make_extension("my_ext", "1.0.0")
//       .type(COMPLEX)
//       .func(make_func<&complex_add>("complex_add")
//           .returns(COMPLEX)
//           .param(COMPLEX)
//           .param(COMPLEX)
//           .build()))
//
//
// ---- FUNCTIONS --------------------------------------------------------------
//
// Extension functions are defined with make_func<&impl>("sql_name") and
// registered with .func() on the extension builder.
//
// Use typed wrappers for parameters and return values — the framework
// detects your function's C++ signature and adapts automatically:
//
//   void add_impl(IntArg a, IntArg b, IntResult out) {
//     if (a.is_null() || b.is_null()) { out.set_null(); return; }
//     out.set(a.value() + b.value());
//   }
//
//   make_func<&add_impl>("add").returns(INT).param(INT).param(INT).build();
//
// Available wrapper types:
//
//   Input:   IntArg, RealArg, StringArg, CustomArg, CustomArgWith<P>
//   Output:  IntResult, RealResult, StringResult, CustomResult,
//            CustomResultWith<P>
//
// For binary (custom) types, write directly into the caller-provided buffer:
//
//   void rot13_impl(CustomArg in, CustomResult out) {
//     if (in.is_null()) { out.set_null(); return; }
//     auto src = in.value();        // Span<const unsigned char>
//     auto dst = out.buffer();      // Span<unsigned char>
//     for (size_t i = 0; i < src.size(); i++) { dst[i] = transform(src[i]); }
//     out.set_length(src.size());
//   }
//
// Builder options:
//
//   make_func<&my_impl>("my_func")
//     .returns(INT)           // Return type
//     .param(INT)             // First parameter
//     .param(STRING)          // Second parameter
//     .buffer_size(256)       // Optional: output buffer size
//     .deterministic()        // Optional: mark as deterministic
//     .build()
//
// Available types for .returns() and .param():
//   INT, STRING, REAL, or any custom type object from make_type.
//
// For aggregate functions and prerun/postrun hooks, see ADVANCED below.
//
//
// ---- TYPES ------------------------------------------------------------------
//
// Custom types are defined with make_type<kName>() at file scope. The type
// name must be a static constexpr char array:
//
//   static constexpr const char kMyTypeName[] = "MYTYPE";
//
//   constexpr auto MYTYPE = make_type<kMyTypeName>()
//       .persisted_length(8)              // Fixed storage size in bytes
//       .max_decode_buffer_length(64)     // Max bytes for string representation
//       .from_string<&my_encode>()        // Required: string -> binary
//       .to_string<&my_decode>()          // Required: binary -> string
//       .compare<&my_compare>()           // Required: comparison
//       .hash<&my_hash>()                 // Optional: hash function
//       .build();                         // Compile error if required ops missing
//
// Calling .build() without from_string, to_string, or compare is a compile
// error. Each builder method also static_asserts that the function signature
// matches the expected type.
//
// The type builder auto-generates VDF names ("MYTYPE::from_string", etc.)
// and embeds the type operations in the type object. When you pass the type
// to .type() on the extension builder, the operations are registered
// automatically — no separate .func() calls needed.
//
// The resulting type object converts implicitly to const char*, so you can
// pass it directly to .returns(MYTYPE) and .param(MYTYPE) on function
// builders.
//
// Type operation signatures:
//
//   // from_string: string -> binary. false=success, true=error.
//   // Set *length = SIZE_MAX to produce SQL NULL.
//   bool my_encode(std::string_view from, Span<unsigned char> buf,
//                  size_t *length);
//
//   // to_string: binary -> string. false=success, true=error.
//   bool my_decode(Span<const unsigned char> data, Span<char> out,
//                  size_t *out_len);
//
//   // compare: returns <0, 0, or >0.
//   int my_compare(Span<const unsigned char> a, Span<const unsigned char> b);
//
//   // hash: returns hash code.
//   size_t my_hash(Span<const unsigned char> data);
//
// To set a default value for NOT NULL columns:
//
//   .intrinsic_default_str("default_string")    // string that will be encoded
//   .intrinsic_default_vdf("vdf_name")          // or a VDF registered separately
//                                               // with make_intrinsic_default()
//
// For parameterized types (e.g. TVECTOR(1536)), see ADVANCED below.
//
//
// ---- REGISTERING THE EXTENSION ----------------------------------------------
//
// VEF_GENERATE_ENTRY_POINTS generates the extern "C" vef_register() and
// vef_unregister() entry points that the server calls to load the extension:
//
//   VEF_GENERATE_ENTRY_POINTS(
//     make_extension("my_ext", "1.0.0")
//       .type(MYTYPE)
//       .func(make_func<&func1_impl>("func1").returns(INT).build())
//       .sys_var(make_sys_var_int("limit", "Max items", &g_limit, 100, 0, 10000))
//   )
//
//
// ---- COMPLETE EXAMPLE -------------------------------------------------------
//
//   #include <villagesql/vsql.h>
//   #include <cstring>
//   using namespace vsql;
//
//   static const size_t kBytearrayLen = 8;
//   static constexpr const char kBytearrayName[] = "BYTEARRAY";
//
//   // BYTEARRAY type: fixed 8-byte value stored as raw bytes
//
//   // Encode: string -> binary (copy up to 8 bytes, zero-pad)
//   bool bytearray_encode(std::string_view from, Span<unsigned char> buf,
//                         size_t* length) {
//     if (buf.size() < kBytearrayLen) return true;  // error
//     memset(buf.data(), 0, kBytearrayLen);
//     size_t n = from.size() < kBytearrayLen ? from.size() : kBytearrayLen;
//     if (n > 0) memcpy(buf.data(), from.data(), n);
//     *length = kBytearrayLen;
//     return false;  // success
//   }
//
//   // Decode: binary -> string (copy 8 bytes)
//   bool bytearray_decode(Span<const unsigned char> data, Span<char> out,
//                         size_t* out_len) {
//     if (out.size() < kBytearrayLen) return true;  // error
//     memcpy(out.data(), data.data(), kBytearrayLen);
//     *out_len = kBytearrayLen;
//     return false;  // success
//   }
//
//   // Compare: lexicographic byte comparison
//   int bytearray_compare(Span<const unsigned char> a,
//                         Span<const unsigned char> b) {
//     return memcmp(a.data(), b.data(), kBytearrayLen);
//   }
//
//   // Define the type
//   constexpr auto BYTEARRAY = make_type<kBytearrayName>()
//       .persisted_length(kBytearrayLen)
//       .max_decode_buffer_length(kBytearrayLen)
//       .from_string<&bytearray_encode>()
//       .to_string<&bytearray_decode>()
//       .compare<&bytearray_compare>()
//       .build();
//
//   // ROT13: apply ROT13 cipher to ASCII letters in a bytearray
//   void rot13_impl(CustomArg in, CustomResult out) {
//     if (in.is_null()) { out.set_null(); return; }
//     auto src = in.value();
//     auto dst = out.buffer();
//     for (size_t i = 0; i < kBytearrayLen; i++) {
//       unsigned char c = src[i];
//       if (c >= 'A' && c <= 'Z') c = 'A' + ((c - 'A' + 13) % 26);
//       else if (c >= 'a' && c <= 'z') c = 'a' + ((c - 'a' + 13) % 26);
//       dst[i] = c;
//     }
//     out.set_length(kBytearrayLen);
//   }
//
//   // Register everything
//   VEF_GENERATE_ENTRY_POINTS(
//     make_extension("bytearray_ext", "1.0.0")
//       .type(BYTEARRAY)
//       .func(make_func<&rot13_impl>("rot13")
//         .returns(BYTEARRAY)
//         .param(BYTEARRAY)
//         .build()))
//
//
// =============================================================================
// ADVANCED
// =============================================================================
//
//
// ---- AGGREGATE FUNCTIONS ----------------------------------------------------
//
// Aggregate VDFs (like SQL SUM, COUNT, etc.) accumulate state across rows
// within each GROUP BY group, then return a final result per group.
//
// The recommended approach uses .state<T>() with typed callbacks. Define a
// state type, then write clear, accumulate, and result functions using C++
// types:
//
//   using SumState = std::optional<long long>;
//
//   void my_clear(SumState &s)       { s = std::nullopt; }
//   void my_acc(SumState &s, IntArg v) {
//     if (!v.is_null()) s = s.value_or(0) + v.value();
//   }
//   std::optional<long long> my_result(const SumState &s) { return s; }
//
//   make_func<&my_result>("my_sum")
//       .returns(INT)
//       .param(INT)
//       .state<SumState>()
//       .clear<&my_clear>()
//       .accumulate<&my_acc>()
//       .build()
//
// How it works:
//   - .state<T>() generates prerun (allocates T via value-initialization) and
//     postrun (deletes T) automatically. T is stored in user_data.
//   - .clear<&fn>() wraps void(State&) -> vef_vdf_clear_func_t
//   - .accumulate<&fn>() wraps void(State&, TypedArgs...) ->
//     vef_vdf_accumulate_func_t. TypedArgs are deduced from the function
//     signature (IntArg, StringArg, etc.).
//   - The result function (make_func template parameter) can return T directly
//     (never NULL) or std::optional<T> (nullopt -> SQL NULL).
//
// For results that are never NULL (e.g., COUNT), use a plain state type:
//
//   using CountState = long long;
//   void count_clear(CountState &s) { s = 0; }
//   void count_acc(CountState &s, IntArg v) { if (!v.is_null()) s++; }
//   long long count_result(const CountState &s) { return s; }
//
// You can also use the raw ABI directly for full control:
//
//   make_func<&raw_result>("my_agg")
//       .returns(INT).param(INT)
//       .prerun<&my_prerun>()      // void(ctx, prerun_args, prerun_result)
//       .postrun<&my_postrun>()    // void(ctx, postrun_args, postrun_result)
//       .clear<&my_clear>()        // void(ctx, vdf_args)
//       .accumulate<&my_acc>()     // void(ctx, vdf_args, vdf_result)
//       .build()
//
// See aggregate_vdf.cc in the test suite for complete examples of both styles.
//
//
// ---- PRERUN/POSTRUN ---------------------------------------------------------
//
// For prerun/postrun functions (per-statement setup/teardown):
//
//   make_func<&my_impl>("my_func")
//     .returns(STRING)
//     .prerun<&my_prerun>()   // Called before first row
//     .postrun<&my_postrun>() // Called after last row
//     .build()
//
// Note: Prerun and postrun functions can be a cumbersome API. The func builder
// already handles simple cases (e.g., type checking for functions with fixed
// args and allocating fixed buffer sizes). We want to cover more cases. If
// you find that you need to use prerun or postrun functions, please come talk
// to us so we can understand your use case.
//
//
// ---- PARAMETERIZED TYPES ----------------------------------------------------
//
// If the type has SQL-level parameters (e.g., TVECTOR(1536)), define a params
// struct and a parse function, then register them with .params<P, &parse>()
// on the type builder. Type operation functions take const P& as their first
// argument; the SDK detects this signature and wires up a memoized parse
// cache automatically.
//
// The parse function is called based on the canonicalized output of the
// resolve_params function; all parameter error checking should be done there.
//
// The parse function can be a static method on the struct (shown below) or
// any free function with the signature:
//   P parse_fn(const std::map<std::string, std::string>& params)
//
//   struct MyParams {
//     int64_t dimension;
//     static MyParams parse(const std::map<std::string, std::string>& p) {
//       return {.dimension = stoll(p.at("dimension"))};
//     }
//   };
//
//   // Type operations take const P& as first argument:
//   bool my_encode(const MyParams& p, std::string_view from,
//                  Span<unsigned char> buf, size_t* length) { ... }
//
//   static constexpr const char kMyTypeName[] = "MYTYPE";
//   constexpr auto MYTYPE = make_type<kMyTypeName>()
//       .persisted_length(...)
//       .params<MyParams, &MyParams::parse>()
//       .from_string<&my_encode>()
//       ...
//       .build();
//
// For types with MYTYPE(N) integer shorthand and explicit parameter
// resolution, also provide:
//
//   .int_to_params<&fn>()     // bool fn(int64_t, map<string,string>&, char*)
//   .resolve_params<&fn>()    // bool fn(const map<string,string>&,
//                             //         ResolvedTypeParams*, char*)
//
// Omitting .params<>() while using const P& signatures will fail at
// registration time.
// TODO(villagesql-beta): make this a compile time error.
//
// Note if a Params type is registered for more than one custom type, each
// custom type MUST register the same parse function.
// TODO(villagesql-beta): remove this restriction.
//
//
// ---- SYSTEM VARIABLES -------------------------------------------------------
//
// Extensions can declare system variables accessible from SQL:
//
//   make_extension("myext", "1.0")
//     .sys_var(make_sys_var_int("threshold_ms",
//                               "Slow query threshold in ms",
//                               &g_threshold_ms, 1000, 0, 3600000))
//     .sys_var(make_sys_var_str("log_file",
//                               "Path to log file",
//                               &g_log_file, "/tmp/myext.log"))
//
// Available builders: make_sys_var_bool, make_sys_var_int, make_sys_var_double,
// make_sys_var_str. Each takes (name, comment, value_ptr, default, ...).
//
// Variables are accessible in SQL as:
//   SELECT @@global.myext.threshold_ms;
//   SET GLOBAL myext.threshold_ms = 500;
//
// From extension code, use sys_var::get() and sys_var::set().
//
//
// ---- LIMITATIONS ------------------------------------------------------------
//
// - intrinsic_default VDFs must be registered separately with
//   make_intrinsic_default(). Reference them by name with
//   .intrinsic_default_vdf("vdf_name") on the type builder.
//
//
// ---- OLDER API --------------------------------------------------------------
//
// You may encounter extensions using <villagesql/extension.h> with a
// different type builder: make_type("name") (string argument) with separate
// .func(make_type_encode<&fn>(...)) registration calls. This still works
// but is more verbose. The make_type<kName>() builder documented above is
// preferred for new code. See villagesql/type_builder.h for details on
// the older API.

#include <villagesql/extension.h>

#include <villagesql/vsql/sys_var_builder.h>
#include <villagesql/vsql/type_builder.h>

// The vsql namespace re-exports symbols from the villagesql namespace so
// that `using namespace vsql` is sufficient for a complete extension.
// make_type<kName>() is defined in vsql/type_builder.h; make_func and
// make_extension come from the shared func_builder / extension_builder.
namespace vsql {

using villagesql::extension_builder::make_extension;
using villagesql::func_builder::make_func;

namespace sys_var = villagesql::sys_var;
using villagesql::sys_var_builder::make_sys_var_bool;
using villagesql::sys_var_builder::make_sys_var_double;
using villagesql::sys_var_builder::make_sys_var_int;
using villagesql::sys_var_builder::make_sys_var_str;

using villagesql::CustomArg;
using villagesql::CustomArgWith;
using villagesql::CustomResult;
using villagesql::CustomResultWith;
using villagesql::IntArg;
using villagesql::IntResult;
using villagesql::RealArg;
using villagesql::RealResult;
using villagesql::Span;
using villagesql::StringArg;
using villagesql::StringResult;

using villagesql::func_builder::INT;
using villagesql::func_builder::REAL;
using villagesql::func_builder::STRING;

}  // namespace vsql

#endif  // VILLAGESQL_VSQL_H
