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
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VILLAGESQL_DETAIL_ABI_SIGNATURE_COMPUTE_H
#define VILLAGESQL_DETAIL_ABI_SIGNATURE_COMPUTE_H

// Compile-time structural fingerprint computation for ABI vtable
// structs.  Server-side and gunit-test-side only -- requires
// Boost.PFR and is intentionally NOT shipped to extensions (extensions
// use the lighter abi_signature_literals.h, trusting the SDK
// header's pin literal at face value).
//
// Goal: produce a constexpr fingerprint that identifies the *full*
// shape of an ABI type -- the kinds and sizes of every leaf,
// recursively through pointer pointees, function return + parameter
// types, enum underlying types, and registered struct fields.  A
// protocol-relevant change anywhere in the tree flips the
// fingerprint; protocol-irrelevant differences (typedef name
// changes, struct renames, the compiler vendor) do not.
//
// How it works:
//   1. abi_signature_hash<T>() dispatches T to one of a small set of
//      "kinds" (Void, Bool, Char, SignedChar, ..., Long, LongLong,
//      ..., Pointer, Reference, Function, Enum, Array, Struct,
//      Union) using standard <type_traits>.  The kind tag is mixed
//      into the hash, then recursion proceeds:
//        - Pointer / Reference -> recurse on the pointee, mixing in
//          its top-level cv-qualifiers.
//        - Function R(Args...) -> recurse on R and every Arg, plus
//          arity.
//        - Enum -> recurse on std::underlying_type_t<E>.
//        - Array T[N] -> recurse on T, mix in extent N.
//        - Struct / Union -> mix in sizeof(T) and the
//          PFR-enumerated AbiStructFields<T>::field_hash().
//   2. The resulting std::size_t is rendered into an AbiFingerprint
//      (from abi_signature_literals.h), which is a fixed-width string
//      of the form "hash-XXXXXXXXXXXXXXXX".
//
// Why kind tags instead of sizeof alone: on LP64 platforms `long` and
// `long long` have identical sizeof but are distinct types; same for
// `char` vs `signed char` vs `unsigned char`.  Hashing by kind tag
// (plus underlying type info for derived kinds) preserves these
// distinctions even though the byte layout is identical.
//
// Why no __PRETTY_FUNCTION__: it is a GCC/Clang extension whose
// textual form is implementation-defined ("size_t" may render as
// "long unsigned int" or "unsigned long" depending on compiler and
// platform).  Relying on it produces different fingerprints for
// ABI-identical types when the server and an extension are built
// with different toolchains, causing the loader to refuse compatible
// extensions.  This file uses only <type_traits> and sizeof, which
// produce identical values for a given target ABI regardless of
// which compiler produced the binary.
//
// Struct field enumeration uses Boost.PFR (vendored at
// extra/boost/boost_1_84_0/boost/pfr/).  PFR walks an *aggregate*
// struct's declared fields in declaration order using a C++17
// structured-bindings trick, no macro registration required.  Types
// PFR cannot enumerate (unions, non-aggregate classes, aggregates
// containing anonymous unions) require a manual AbiStructFields
// specialization; the existing ones are colocated with the types
// (e.g. in sys_var_register.h, status_var_register.h).  Opaque
// server-side handles are registered via VEF_ABI_REGISTER_OPAQUE_HANDLE
// below.
//
// TODO(villagesql-windows): function-pointer calling-convention
// attributes (__stdcall, __cdecl, __vectorcall) affect ABI on
// Windows but are not captured by any standard type trait.  Encode
// them explicitly when adding Windows support.

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <boost/pfr/core.hpp>
#include <boost/pfr/tuple_size.hpp>

#include "villagesql/sdk/include/villagesql/detail/abi_signature_literals.h"

namespace villagesql::detail {

// Distinct ABI-relevant type kinds.  Values are part of the hash;
// reordering or removing entries is a scheme-breaking change and
// requires bumping the AbiFingerprint scheme tag.
enum class AbiKind : std::uint8_t {
  Void = 1,
  Bool,
  Char,
  SignedChar,
  UnsignedChar,
  Char16,
  Char32,
  WChar,
  Short,
  UnsignedShort,
  Int,
  UnsignedInt,
  Long,
  UnsignedLong,
  LongLong,
  UnsignedLongLong,
  Float,
  Double,
  LongDouble,
  Pointer,
  LValueReference,
  RValueReference,
  Function,
  Enum,
  Array,
  Struct,
  Union,
  MemberPointer,
  Unknown,
  OpaqueHandle,
};

// FNV-1a 64-bit offset basis (the standard starting value for FNV-1a).
// Used as the seed for every fold so that an empty fold has a
// well-defined, non-zero starting point.  If this changes, also bump
// the scheme tag in AbiFingerprint ("hash-" -> something else).
constexpr std::size_t kAbiSeed = 0xcbf29ce484222325ULL;

// Order-sensitive accumulator -- the same shape boost::hash_combine
// uses for std::size_t pairs.
//
// The magic number 0x9e3779b97f4a7c15 is the 64-bit golden-ratio
// constant (floor(2^64 / phi)) that boost::hash_combine uses to spread
// bits when folding a new value into an accumulator.  Same caveat as
// kAbiSeed: the exact constant is not load-bearing on its own, but
// changing it changes every fingerprint, so bump the scheme tag if
// you ever touch it.
constexpr std::size_t abi_combine(std::size_t a, std::size_t b) {
  return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

template <typename T>
constexpr std::size_t abi_type_hash_raw();

// Marker trait for ABI types that are intentionally opaque -- forward-
// declared on one or both sides of the ABI and never given a layout
// (e.g. vef_thread_handle_t, an opaque server-side handle that
// extensions only ever see through a pointer).
//
// Concrete (layout-bearing) types must NOT be marked opaque, and
// opaque types must be marked explicitly via
// VEF_ABI_REGISTER_OPAQUE_HANDLE(T) next to their forward declaration.
// Going by "is the type complete in the current TU?" instead would
// make the hash TU-dependent: the SDK's TU might see only the forward
// declaration while the server happens to include the definition, and
// the two would compute different hashes for the same type.  Explicit
// registration removes that footgun.
template <typename T>
struct is_opaque_handle : std::false_type {};

// Constexpr fold of a C string into a hash, using the same combine
// rule as the rest of this scheme.  Used by
// VEF_ABI_REGISTER_OPAQUE_HANDLE to turn the stringified type name
// into a discriminating tag.
constexpr std::size_t abi_str_fold(const char *s) {
  std::size_t h = kAbiSeed;
  while (*s) {
    h = abi_combine(h, static_cast<std::size_t>(*s));
    ++s;
  }
  return h;
}

// Fold abi_type_hash_raw<F>() over the field types of an aggregate,
// in declaration order, using Boost.PFR for field enumeration.
template <typename T, std::size_t... Is>
constexpr std::size_t abi_pfr_field_hash(std::index_sequence<Is...>) {
  std::size_t h = kAbiSeed;
  ((h = abi_combine(h,
                    abi_type_hash_raw<boost::pfr::tuple_element_t<Is, T>>())),
   ...);
  return h;
}

// Field-enumeration trait.  The primary template uses Boost.PFR to
// walk an aggregate's declared fields in source order -- no macro
// registration required for aggregate (non-union) structs.  Unions
// and non-aggregate types fire static_asserts with the offending
// case named; manual specializations override this template for
// types PFR cannot walk (anonymous-union-bearing aggregates;
// opaque handles).
template <typename T>
struct AbiStructFields {
  static constexpr std::size_t field_hash() {
    static_assert(
        !std::is_union_v<T>,
        "AbiStructFields<T>::field_hash(): T is a union.  Boost.PFR refuses "
        "to enumerate union alternatives (which one is live is a runtime "
        "property, not a type property).  No current VillageSQL capability "
        "vtable transitively reaches a union -- reorganise the type or "
        "raise this with a maintainer if you have a use case.");
    static_assert(
        std::is_aggregate_v<T>,
        "AbiStructFields<T>::field_hash(): T is not an aggregate, so "
        "Boost.PFR cannot enumerate its fields.  Make T an aggregate (no "
        "user-declared constructors, no private/protected non-static data "
        "members, no virtual functions, no virtual or non-public bases) "
        "or raise this with a maintainer if you need an escape hatch.");
    return abi_pfr_field_hash<T>(
        std::make_index_sequence<boost::pfr::tuple_size_v<T>>{});
  }
};

// Unpacks function types R(Args...) so the return and parameter
// types can be recursed into.  The unspecialized template is never
// instantiated.
template <typename F>
struct AbiFunctionHash;

template <typename R, typename... Args>
struct AbiFunctionHash<R(Args...)> {
  static constexpr std::size_t hash() {
    std::size_t h = static_cast<std::size_t>(AbiKind::Function);
    h = abi_combine(h, abi_type_hash_raw<R>());
    h = abi_combine(h, sizeof...(Args));
    ((h = abi_combine(h, abi_type_hash_raw<Args>())), ...);
    return h;
  }
};

template <typename T>
constexpr std::size_t abi_type_hash_raw() {
  // Strip top-level cv at the outermost level.  A `const SomeScalar`
  // field does not change ABI layout vs `SomeScalar`, so the hash
  // should match.  Without this, the is_same_v<U, int>-style arms
  // below would all miss for `const int` and fall through to the
  // static_assert at the bottom.
  using U = std::remove_cv_t<T>;
  std::size_t h = kAbiSeed;
  if constexpr (std::is_void_v<U>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Void));
  } else if constexpr (std::is_same_v<U, bool>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Bool));
  } else if constexpr (std::is_same_v<U, char>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Char));
  } else if constexpr (std::is_same_v<U, signed char>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::SignedChar));
  } else if constexpr (std::is_same_v<U, unsigned char>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::UnsignedChar));
  } else if constexpr (std::is_same_v<U, char16_t>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Char16));
  } else if constexpr (std::is_same_v<U, char32_t>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Char32));
  } else if constexpr (std::is_same_v<U, wchar_t>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::WChar));
  } else if constexpr (std::is_same_v<U, short>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Short));
  } else if constexpr (std::is_same_v<U, unsigned short>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::UnsignedShort));
  } else if constexpr (std::is_same_v<U, int>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Int));
  } else if constexpr (std::is_same_v<U, unsigned int>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::UnsignedInt));
  } else if constexpr (std::is_same_v<U, long>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Long));
  } else if constexpr (std::is_same_v<U, unsigned long>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::UnsignedLong));
  } else if constexpr (std::is_same_v<U, long long>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::LongLong));
  } else if constexpr (std::is_same_v<U, unsigned long long>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::UnsignedLongLong));
  } else if constexpr (std::is_same_v<U, float>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Float));
  } else if constexpr (std::is_same_v<U, double>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::Double));
  } else if constexpr (std::is_same_v<U, long double>) {
    return abi_combine(h, static_cast<std::size_t>(AbiKind::LongDouble));
  } else if constexpr (std::is_pointer_v<U>) {
    using P = std::remove_pointer_t<U>;
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::Pointer));
    h = abi_combine(h, std::is_const_v<P> ? 1u : 0u);
    h = abi_combine(h, std::is_volatile_v<P> ? 1u : 0u);
    h = abi_combine(h, abi_type_hash_raw<std::remove_cv_t<P>>());
    return h;
  } else if constexpr (std::is_lvalue_reference_v<U>) {
    using P = std::remove_reference_t<U>;
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::LValueReference));
    h = abi_combine(h, std::is_const_v<P> ? 1u : 0u);
    h = abi_combine(h, std::is_volatile_v<P> ? 1u : 0u);
    h = abi_combine(h, abi_type_hash_raw<std::remove_cv_t<P>>());
    return h;
  } else if constexpr (std::is_rvalue_reference_v<U>) {
    using P = std::remove_reference_t<U>;
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::RValueReference));
    h = abi_combine(h, std::is_const_v<P> ? 1u : 0u);
    h = abi_combine(h, std::is_volatile_v<P> ? 1u : 0u);
    h = abi_combine(h, abi_type_hash_raw<std::remove_cv_t<P>>());
    return h;
  } else if constexpr (std::is_function_v<U>) {
    return abi_combine(h, AbiFunctionHash<U>::hash());
  } else if constexpr (std::is_array_v<U>) {
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::Array));
    h = abi_combine(h, std::extent_v<U>);
    h = abi_combine(h, abi_type_hash_raw<std::remove_extent_t<U>>());
    return h;
  } else if constexpr (std::is_enum_v<U>) {
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::Enum));
    h = abi_combine(h, abi_type_hash_raw<std::underlying_type_t<U>>());
    return h;
  } else if constexpr (std::is_union_v<U>) {
    // Unions cannot be auto-enumerated by Boost.PFR.  Calling
    // AbiStructFields<T>::field_hash() here will fire a clear
    // static_assert unless a manual specialization exists.
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::Union));
    h = abi_combine(h, sizeof(U));
    h = abi_combine(h, AbiStructFields<U>::field_hash());
    return h;
  } else if constexpr (is_opaque_handle<U>::value) {
    // Explicitly-registered opaque handle (e.g. vef_thread_handle_t).
    // Hashes by registered tag only -- no sizeof, no field walking.
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::OpaqueHandle));
    h = abi_combine(h, AbiStructFields<U>::field_hash());
    return h;
  } else if constexpr (std::is_class_v<U>) {
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::Struct));
    h = abi_combine(h, sizeof(U));
    h = abi_combine(h, AbiStructFields<U>::field_hash());
    return h;
  } else if constexpr (std::is_member_pointer_v<U>) {
    h = abi_combine(h, static_cast<std::size_t>(AbiKind::MemberPointer));
    h = abi_combine(h, sizeof(U));
    return h;
  } else {
    static_assert(
        sizeof(U) == 0,
        "abi_type_hash_raw<T>: T is not a recognized ABI type kind.  Add "
        "a new AbiKind enumerator and an is_same_v arm in "
        "abi_signature_compute.h if you need to hash this type.");
    return h;
  }
}

constexpr AbiFingerprint abi_fingerprint_from_hash(std::uint32_t version,
                                                   std::size_t h) {
  AbiFingerprint fp{};
  constexpr char kVerPrefix[AbiFingerprint::kVerPrefixLen] = {
      'v', 'e', 'r', 'h', 'a', 's', 'h', '-'};
  for (std::size_t i = 0; i < AbiFingerprint::kVerPrefixLen; ++i) {
    fp.chars[i] = kVerPrefix[i];
  }
  // Version: zero-padded 3-digit decimal.  Version values greater than
  // 999 silently wrap modulo 1000 -- bumped to 4 digits when needed.
  fp.chars[AbiFingerprint::kVerPrefixLen + 0] =
      static_cast<char>('0' + ((version / 100) % 10));
  fp.chars[AbiFingerprint::kVerPrefixLen + 1] =
      static_cast<char>('0' + ((version / 10) % 10));
  fp.chars[AbiFingerprint::kVerPrefixLen + 2] =
      static_cast<char>('0' + (version % 10));
  fp.chars[AbiFingerprint::kVerPrefixLen + AbiFingerprint::kVersionLen] = '-';
  constexpr std::size_t kHexStart = AbiFingerprint::kVerPrefixLen +
                                    AbiFingerprint::kVersionLen +
                                    AbiFingerprint::kSepLen;
  constexpr char kHex[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  for (std::size_t i = 0; i < AbiFingerprint::kHexLen; ++i) {
    const std::size_t shift = 4 * (AbiFingerprint::kHexLen - 1 - i);
    fp.chars[kHexStart + i] = kHex[(h >> shift) & 0xFULL];
  }
  return fp;
}

// Version is a non-type template parameter so the rendered fingerprint
// string is a constexpr (used as the comparand inside pin_matches's
// static_assert).  Defaults to 0 for callers (e.g. abi_signature-t)
// that don't care about the version slot -- both sides of any
// comparison get the same "verhash-000-XXX" prefix and the test
// continues to exercise just the structural hash portion.
template <typename T, std::uint32_t Version = 0>
constexpr AbiFingerprint abi_signature_hash() {
  static_assert(Version <= 999,
                "ABI version must fit in 3 decimal digits (0..999)");
  return abi_fingerprint_from_hash(Version, abi_type_hash_raw<T>());
}

// pin_matches<T, Version>(lit): true iff the literal matches T's
// structurally-computed fingerprint *and* declares the same Version,
// or if the literal is the empty-placeholder sentinel.
//
//   * N <= 1 (empty literal ""): treated as "no pin recorded yet" --
//     returns true so static_assert passes.  An empty pin still
//     reaches the server's wire-level strcmp at extension load, which
//     only matches another empty pin.
//   * N == kBufSize (well-formed "verhash-NNN-XXXXXXXXXXXXXXXX"):
//     compares against abi_signature_hash<T, Version>() and returns
//     the result.  Mismatch may indicate either version drift or
//     struct-shape drift.
//   * Any other N (wrong length): returns false -- a malformed pin
//     literal is treated as a mismatch.
//
// The if-constexpr branching is necessary so that AbiFingerprint::
// from_literal<1>(""), which would trip its own static_assert on N ==
// kBufSize, is never instantiated for the empty-placeholder path.
template <typename T, std::uint32_t Version, std::size_t N>
constexpr bool pin_matches(const char (&lit)[N]) {
  if constexpr (N <= 1) {
    return true;
  } else if constexpr (N != AbiFingerprint::kBufSize) {
    return false;
  } else {
    return abi_signature_hash<T, Version>() ==
           AbiFingerprint::from_literal(lit);
  }
}

}  // namespace villagesql::detail

// Register T as an opaque ABI handle: a struct whose layout one or
// both sides of the ABI deliberately do not see (typically a
// forward-declared server-internal type that extensions only ever
// take addresses of).
//
// Hash machinery treats T as identity-only: no sizeof, no field walk.
// The discriminator is the stringified type name -- two opaque
// handles registered with different identifiers hash differently, so
// swapping them in a vtable still flips its hash.  Both sides of the
// ABI must register the same identifier; that is the contract that
// keeps SDK and server hashes aligned regardless of which TU happens
// to see the full definition.
//
// Place the registration next to the forward declaration:
//
//   struct vef_thread_handle_t;
//   VEF_ABI_REGISTER_OPAQUE_HANDLE(vef_thread_handle_t);
#define VEF_ABI_REGISTER_OPAQUE_HANDLE(Type)            \
  namespace villagesql::detail {                        \
  template <>                                           \
  struct is_opaque_handle<Type> : std::true_type {};    \
  template <>                                           \
  struct AbiStructFields<Type> {                        \
    static constexpr std::size_t field_hash() {         \
      return ::villagesql::detail::abi_str_fold(#Type); \
    }                                                   \
  };                                                    \
  }                                                     \
  static_assert(true, "force trailing semicolon at call site")

// VEF_PIN_VERIFY(T, mac_lit, x86_lit, arm_lit) -- server-side pin
// macro.  Expands to the pin literal for the current build target,
// with a compile-time static_assert that the literal matches T's
// structurally-computed fingerprint.  A mismatch fails the build with
// a clean diagnostic naming T and the target; the empty placeholder
// ("") is accepted without verification (see pin_matches above).
//
// Implemented as an immediately-invoked captureless lambda so the
// static_assert lives in a function body (where the macro is most
// naturally invoked inline as a struct-initializer or call argument)
// while the resulting expression evaluates to the const char * pin
// literal for use on the wire.  Run the abi_pin_literals gunit test
// on the target to obtain the current hash for pasting.
#if defined(__APPLE__)
#define VEF_PIN_VERIFY(T, version, mac_lit, x86_lit, arm_lit)                \
  ([] {                                                                      \
    static_assert(                                                           \
        ::villagesql::detail::pin_matches<T, (version)>(mac_lit),            \
        "ABI pin mismatch for " #T                                           \
        " on mac -- "                                                        \
        "run abi_pin_literals-t for the current verhash");                   \
    (void)(x86_lit);                                                         \
    (void)(arm_lit);                                                         \
    return mac_lit;                                                          \
  }())
#elif defined(__linux__) && defined(__x86_64__)
#define VEF_PIN_VERIFY(T, version, mac_lit, x86_lit, arm_lit)                \
  ([] {                                                                      \
    static_assert(                                                           \
        ::villagesql::detail::pin_matches<T, (version)>(x86_lit),            \
        "ABI pin mismatch for " #T                                           \
        " on linux_x86 -- "                                                  \
        "run abi_pin_literals-t for the current verhash");                   \
    (void)(mac_lit);                                                         \
    (void)(arm_lit);                                                         \
    return x86_lit;                                                          \
  }())
#elif defined(__linux__) && defined(__aarch64__)
#define VEF_PIN_VERIFY(T, version, mac_lit, x86_lit, arm_lit)                \
  ([] {                                                                      \
    static_assert(                                                           \
        ::villagesql::detail::pin_matches<T, (version)>(arm_lit),            \
        "ABI pin mismatch for " #T                                           \
        " on linux_arm -- "                                                  \
        "run abi_pin_literals-t for the current verhash");                   \
    (void)(mac_lit);                                                         \
    (void)(x86_lit);                                                         \
    return arm_lit;                                                          \
  }())
#else
#define VEF_PIN_VERIFY(T, version, mac_lit, x86_lit, arm_lit)                \
  ((void)(version), (void)(mac_lit), (void)(x86_lit), (void)(arm_lit),       \
   (const char *)"")
#endif

#endif  // VILLAGESQL_DETAIL_ABI_SIGNATURE_COMPUTE_H
