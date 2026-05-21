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

// Compile-time signature fingerprinting checks.
//
// Verifies that abi_signature_hash<T>() recurses through the function-pointer
// signatures inside an ABI vtable struct using only <type_traits> and sizeof
// (no compiler-string introspection), so the fingerprint is stable across
// GCC and Clang on the same target ABI.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "villagesql/sdk/include/villagesql/abi/preview/keyring.h"
#include "villagesql/sdk/include/villagesql/abi/preview/ping.h"
#include "villagesql/sdk/include/villagesql/abi/preview/storage.h"
#include "villagesql/services/abi_specializations.h"

// Boost.PFR enumerates fields of any aggregate ABI struct automatically;
// nothing in the test surface requires per-type registration.

namespace villagesql_unittest {

using villagesql::detail::abi_signature_hash;

// A locally-defined struct with the same field types and order as
// vef_preview_keyring_t.  Under the structural fingerprint scheme this is
// expected to hash *identically* -- the fingerprint answers "does the
// vtable shape match", not "is this the right vtable".  Capability identity
// is tracked separately by capability name.
struct local_keyring_lookalike {
  uint32_t version;
  vef_read_keyring_fn read;
  vef_write_keyring_fn write;
};

// A function-pointer typedef whose signature exactly matches
// vef_read_keyring_fn.  Same underlying type -> same fingerprint, since the
// structural recursion sees through typedefs.
typedef vef_keyring_result_t (*alias_read_fn)(const char *, const char *,
                                              unsigned char *, std::size_t,
                                              std::size_t *);

// Same as vef_read_keyring_fn but with one parameter type changed
// (size_t -> int).  Different underlying parameter type -> different
// fingerprint.  This is the property the existing name+sizeof hash misses,
// since both function pointers have identical sizeof.
typedef vef_keyring_result_t (*read_fn_with_int_buflen)(const char *,
                                                        const char *,
                                                        unsigned char *, int,
                                                        std::size_t *);

// Same as vef_read_keyring_fn but the buffer parameter is const-qualified.
// const pointee is a meaningful interface change even though pointer size
// and underlying byte ABI are identical.
typedef vef_keyring_result_t (*read_fn_with_const_buf)(const char *,
                                                       const char *,
                                                       const unsigned char *,
                                                       std::size_t,
                                                       std::size_t *);

class AbiSignatureTest : public ::testing::Test {};

TEST_F(AbiSignatureTest, FingerprintIsConstexprAndHashPrefixed) {
  constexpr auto fp = abi_signature_hash<vef_preview_keyring_t>();
  static_assert(fp.chars[0] == 'v' && fp.chars[7] == '-',
                "fingerprint should begin with the 'verhash-' scheme tag");
  EXPECT_EQ(fp.view().substr(0, 8), std::string_view("verhash-"));
  EXPECT_EQ(fp.view().size(), 28u);
}

TEST_F(AbiSignatureTest, StructHashDiffersFromLeafHash) {
  EXPECT_NE(abi_signature_hash<vef_preview_keyring_t>(),
            abi_signature_hash<vef_read_keyring_fn>());
  EXPECT_NE(abi_signature_hash<vef_preview_keyring_t>(),
            abi_signature_hash<vef_write_keyring_fn>());
}

TEST_F(AbiSignatureTest, ReadAndWriteHashDifferently) {
  EXPECT_NE(abi_signature_hash<vef_read_keyring_fn>(),
            abi_signature_hash<vef_write_keyring_fn>());
}

TEST_F(AbiSignatureTest, EquivalentFunctionPointerTypedefsHashEqually) {
  // alias_read_fn and vef_read_keyring_fn name the same underlying function
  // type; the structural recursion is name-insensitive, so the fingerprints
  // must agree.
  EXPECT_EQ(abi_signature_hash<vef_read_keyring_fn>(),
            abi_signature_hash<alias_read_fn>());
}

TEST_F(AbiSignatureTest, ParameterTypeChangeFlipsHash) {
  // Headline property: changing one parameter type in a function-pointer
  // typedef changes the recursive fingerprint, even though the typedef has
  // the same sizeof and the surrounding struct has the same name.
  EXPECT_NE(abi_signature_hash<vef_read_keyring_fn>(),
            abi_signature_hash<read_fn_with_int_buflen>());
}

TEST_F(AbiSignatureTest, ConstQualifiedPointeeFlipsHash) {
  // const-qualifying a pointee is an interface-meaningful difference.
  EXPECT_NE(abi_signature_hash<vef_read_keyring_fn>(),
            abi_signature_hash<read_fn_with_const_buf>());
}

TEST_F(AbiSignatureTest, IdenticalStructFieldsHashEqually) {
  // The structural fingerprint is name-insensitive at the struct level by
  // design.  Two structs with the same field types hash equally; the
  // capability registry distinguishes them by capability name, not by
  // fingerprint.  (Contrast with the previous __PRETTY_FUNCTION__-based
  // scheme, which baked the struct name into the hash and was therefore
  // unstable across compiler vendors.)
  EXPECT_EQ(abi_signature_hash<vef_preview_keyring_t>(),
            abi_signature_hash<local_keyring_lookalike>());
}

// Pinning absolute fingerprint values lives in capability_registry.cc's
// VEF_DECLARE_CAPABILITY_VERSION chains -- those are compile-time checks
// per platform, with the diagnostic templated-error trick that prints the
// computed hash on drift.  Any scheme change (kAbiSeed, abi_combine,
// AbiKind enum) trips those at build time and dumps the new value, so a
// duplicate runtime assertion here would only re-cover the same ground.
//
// The tests below (Adv*, Neg*) pin *relationships* between hashes, not
// absolute values, so they remain orthogonally useful.

TEST_F(AbiSignatureTest, DistinctIntegerKindsWithIdenticalSizeofDiffer) {
  // On LP64 platforms `long` and `long long` are both 8 bytes but are
  // distinct types.  A hash based on sizeof alone would conflate them; the
  // kind-tag dispatch must preserve the distinction.
  EXPECT_NE(abi_signature_hash<long>(), abi_signature_hash<long long>());
  EXPECT_NE(abi_signature_hash<unsigned long>(),
            abi_signature_hash<unsigned long long>());
  // Similarly char / signed char / unsigned char are three distinct types.
  EXPECT_NE(abi_signature_hash<char>(), abi_signature_hash<signed char>());
  EXPECT_NE(abi_signature_hash<char>(), abi_signature_hash<unsigned char>());
  EXPECT_NE(abi_signature_hash<signed char>(),
            abi_signature_hash<unsigned char>());
}

// ===========================================================================
// Adversarial test suite
//
// The tests above establish that the basic recursion works.  These tests aim
// for high-confidence coverage of *similar but distinct* types -- the cases
// most likely to slip through a structural hash if any single recursion path
// is incomplete.  Each test names exactly one mutation; failures point
// directly at which kind of change went undetected.
//
// Test naming: AdvNN_Description where NN is the equivalence-class index
// from the design plan.  Classes 1-25 should hash differently; the trailing
// NegM tests assert pairs that should hash equally despite syntactic
// differences.
// ===========================================================================

// Pairwise-distinct assertion helper.  Folds over a parameter pack and
// asserts every pair hashes differently.  Use for mechanical N x N sweeps;
// hand-write the assertion for any case where a failure message should name
// the specific mutation.
template <typename T1>
inline void expect_distinct_impl() {}
template <typename T1, typename T2, typename... Ts>
inline void expect_distinct_impl() {
  EXPECT_NE(abi_signature_hash<T1>(), abi_signature_hash<T2>());
  if constexpr (sizeof...(Ts) > 0) {
    expect_distinct_impl<T1, Ts...>();
    expect_distinct_impl<T2, Ts...>();
  }
}
#define EXPECT_DISTINCT_HASHES(...) \
  ::villagesql_unittest::expect_distinct_impl<__VA_ARGS__>()

// --- Base types ---------------------------------------------------------

// B1: scalar adversary -- mix of same-sizeof distinct kinds.
struct B1_Leaf {
  int a;
  long b;
  long long c;
  unsigned long d;
  float e;
  double f;
};

// B2: pointer adversary -- distinct cv-qualifications and indirection.
struct B2_PtrZoo {
  int *p;
  const int *cp;
  volatile int *vp;
  int **pp;
};

// B3: array adversary -- extent, element type, rank.
struct B3_Arrays {
  int x[4];
  int y[5];
  unsigned int z[4];
  int w[4][2];
};

// B4: enum adversary -- different underlying types.
enum class E_Int : int { A };
enum class E_Long : long { A };

// B5_FnPtrNest: the headline deep-recursion case.  An outer struct of
// function pointers, one of which takes an inner struct that itself
// contains a function pointer and is referenced by other fields.  The
// hash must flip when *anything* anywhere in this tree changes.
struct B5_Inner {
  int (*cb)(double, const char *);
  long tag;
};
struct B5_FnPtrNest {
  int (*read)(const B5_Inner *, std::size_t, unsigned char *);
  int (*(*make)(B5_Inner))(double,
                           const char *);  // takes Inner, returns fn ptr
  void (*write)(B5_Inner, int);
};

}  // namespace villagesql_unittest

namespace villagesql_unittest {

// --- Class 1: distinct integer kinds with identical sizeof --------------
TEST_F(AbiSignatureTest, Adv01_LongVsLongLong) {
  EXPECT_DISTINCT_HASHES(long, long long, unsigned long, unsigned long long);
}

// --- Class 2: distinct float kinds --------------------------------------
TEST_F(AbiSignatureTest, Adv02_FloatKinds) {
  EXPECT_DISTINCT_HASHES(float, double, long double);
}

// --- Class 3: char-family kinds -----------------------------------------
TEST_F(AbiSignatureTest, Adv03_CharFamilyDistinct) {
  EXPECT_DISTINCT_HASHES(char, signed char, unsigned char, char16_t, char32_t,
                         wchar_t);
}

// --- Class 4: signed vs unsigned same width -----------------------------
TEST_F(AbiSignatureTest, Adv04_SignedVsUnsigned) {
  EXPECT_DISTINCT_HASHES(int, unsigned int);
  EXPECT_DISTINCT_HASHES(short, unsigned short);
  EXPECT_DISTINCT_HASHES(long, unsigned long);
}

// --- Class 5: const-qualified pointee -----------------------------------
TEST_F(AbiSignatureTest, Adv05_ConstPointee) {
  EXPECT_NE(abi_signature_hash<int *>(), abi_signature_hash<const int *>());
}

// --- Class 6: volatile-qualified pointee --------------------------------
TEST_F(AbiSignatureTest, Adv06_VolatilePointee) {
  EXPECT_NE(abi_signature_hash<int *>(), abi_signature_hash<volatile int *>());
}

// --- Class 7: pointer vs lvalue ref -------------------------------------
TEST_F(AbiSignatureTest, Adv07_PointerVsLvalueRef) {
  EXPECT_NE(abi_signature_hash<int *>(), abi_signature_hash<int &>());
}

// --- Class 8: lvalue vs rvalue ref --------------------------------------
TEST_F(AbiSignatureTest, Adv08_LvalueVsRvalueRef) {
  EXPECT_NE(abi_signature_hash<int &>(), abi_signature_hash<int &&>());
}

// --- Class 9: indirection depth -----------------------------------------
TEST_F(AbiSignatureTest, Adv09_IndirectionDepth) {
  EXPECT_DISTINCT_HASHES(int *, int **, int ***);
}

// --- Class 10: different pointee type -----------------------------------
TEST_F(AbiSignatureTest, Adv10_DifferentPointeeType) {
  EXPECT_NE(abi_signature_hash<int *>(), abi_signature_hash<unsigned int *>());
}

// --- Class 11: array extent ---------------------------------------------
TEST_F(AbiSignatureTest, Adv11_ArrayExtent) {
  EXPECT_DISTINCT_HASHES(int[4], int[5], int[6]);
}

// --- Class 12: array element type ---------------------------------------
TEST_F(AbiSignatureTest, Adv12_ArrayElementType) {
  EXPECT_NE((abi_signature_hash<int[4]>()),
            (abi_signature_hash<unsigned int[4]>()));
}

// --- Class 13: array rank -----------------------------------------------
TEST_F(AbiSignatureTest, Adv13_ArrayRank) {
  EXPECT_NE((abi_signature_hash<int[4]>()), (abi_signature_hash<int[4][1]>()));
}

// --- Class 14: enum underlying type -------------------------------------
TEST_F(AbiSignatureTest, Adv14_EnumUnderlying) {
  EXPECT_NE(abi_signature_hash<E_Int>(), abi_signature_hash<E_Long>());
}

// --- Class 15: change one parameter type --------------------------------
using F15_base = int (*)(double, const char *);
using F15_mut = int (*)(int, const char *);  // param 0 double -> int
TEST_F(AbiSignatureTest, Adv15_OneParamTypeChange) {
  EXPECT_NE(abi_signature_hash<F15_base>(), abi_signature_hash<F15_mut>());
}

// --- Class 16: reorder return type vs first parameter -------------------
using F16_RA_BC = int (*)(double, const char *);
using F16_DA_IC = double (*)(int, const char *);  // R<->arg0 swapped types
TEST_F(AbiSignatureTest, Adv16_ReturnSwappedWithFirstParam) {
  EXPECT_NE(abi_signature_hash<F16_RA_BC>(), abi_signature_hash<F16_DA_IC>());
}

// --- Class 17: swap two adjacent parameters of different types ----------
using F17_base = void (*)(int, double, const char *);
using F17_swap = void (*)(double, int, const char *);
TEST_F(AbiSignatureTest, Adv17_AdjacentParamSwap) {
  EXPECT_NE(abi_signature_hash<F17_base>(), abi_signature_hash<F17_swap>());
}

// --- Class 18: arity change ---------------------------------------------
using F18_3args = void (*)(int, double, const char *);
using F18_2args = void (*)(int, double);
TEST_F(AbiSignatureTest, Adv18_Arity) {
  EXPECT_NE(abi_signature_hash<F18_3args>(), abi_signature_hash<F18_2args>());
}

// --- Class 19: return type change ---------------------------------------
using F19_int = int (*)(double);
using F19_long = long (*)(double);
TEST_F(AbiSignatureTest, Adv19_ReturnType) {
  EXPECT_NE(abi_signature_hash<F19_int>(), abi_signature_hash<F19_long>());
}

// --- Class 20: const-qualify a pointer parameter's pointee --------------
using F20_base = void (*)(unsigned char *);
using F20_const = void (*)(const unsigned char *);
TEST_F(AbiSignatureTest, Adv20_ConstifyPointerParam) {
  EXPECT_NE(abi_signature_hash<F20_base>(), abi_signature_hash<F20_const>());
}

// --- Class 21: swap two adjacent struct fields --------------------------
struct S21_swap {
  long b;
  int a;
  long long c;
  unsigned long d;
  float e;
  double f;
};
TEST_F(AbiSignatureTest, Adv21_StructFieldOrderSwap) {
  EXPECT_NE(abi_signature_hash<B1_Leaf>(), abi_signature_hash<S21_swap>());
}

// --- Class 22: replace a field type with same-sizeof other --------------
struct S22_mut {
  unsigned int a;  // was: int
  long b;
  long long c;
  unsigned long d;
  float e;
  double f;
};
TEST_F(AbiSignatureTest, Adv22_SameSizeofFieldTypeFlip) {
  EXPECT_NE(abi_signature_hash<B1_Leaf>(), abi_signature_hash<S22_mut>());
}

// --- Class 23: append a field -------------------------------------------
struct S23_extra {
  int a;
  long b;
  long long c;
  unsigned long d;
  float e;
  double f;
  int g;  // appended
};
TEST_F(AbiSignatureTest, Adv23_AppendField) {
  EXPECT_NE(abi_signature_hash<B1_Leaf>(), abi_signature_hash<S23_extra>());
}

// --- Class 25: deep nested mutation (the headline test) -----------------
// Inner's callback's second parameter changes from `const char*` to
// `const wchar_t*`.  Outer B5 must reflect this change in its hash.
struct B5_Inner_v2 {
  int (*cb)(double, const wchar_t *);  // was: const char *
  long tag;
};
struct B5_FnPtrNest_v2 {
  int (*read)(const B5_Inner_v2 *, std::size_t, unsigned char *);
  int (*(*make)(B5_Inner_v2))(double, const wchar_t *);
  void (*write)(B5_Inner_v2, int);
};
TEST_F(AbiSignatureTest, Adv25_DeepNestedMutationPropagates) {
  // Inner change must flip Inner's hash...
  EXPECT_NE(abi_signature_hash<B5_Inner>(), abi_signature_hash<B5_Inner_v2>());
  // ...and that change must propagate all the way to the outer struct's hash.
  EXPECT_NE(abi_signature_hash<B5_FnPtrNest>(),
            abi_signature_hash<B5_FnPtrNest_v2>());
}

// --- Negative test 1: typedef rename hashes equally ---------------------
using B1_Alias = B1_Leaf;
TEST_F(AbiSignatureTest, Neg01_TypedefRenameEqual) {
  EXPECT_EQ(abi_signature_hash<B1_Leaf>(), abi_signature_hash<B1_Alias>());
}

// --- Negative test 2: same-shape twin struct hashes equally -------------
// Two distinct struct names with identical field declarations must produce
// the same fingerprint -- the structural hash is name-insensitive by design.
struct B5_Inner_twin {
  int (*cb)(double, const char *);
  long tag;
};
struct B5_FnPtrNest_twin {
  int (*read)(const B5_Inner_twin *, std::size_t, unsigned char *);
  int (*(*make)(B5_Inner_twin))(double, const char *);
  void (*write)(B5_Inner_twin, int);
};
TEST_F(AbiSignatureTest, Neg02_SameShapeDifferentNameEqual) {
  EXPECT_EQ(abi_signature_hash<B5_Inner>(),
            abi_signature_hash<B5_Inner_twin>());
  EXPECT_EQ(abi_signature_hash<B5_FnPtrNest>(),
            abi_signature_hash<B5_FnPtrNest_twin>());
}

// --- Negative test 3: function-pointer typedef alias equals direct type -
using ReadAlias = int (*)(const B5_Inner *, std::size_t, unsigned char *);
TEST_F(AbiSignatureTest, Neg03_FnPtrTypedefAlias) {
  EXPECT_EQ(abi_signature_hash<ReadAlias>(),
            abi_signature_hash<int (*)(const B5_Inner *, std::size_t,
                                       unsigned char *)>());
}

// --- Negative test 4: array-of-typedef-int equals array-of-int ----------
using IntAlias = int;
TEST_F(AbiSignatureTest, Neg04_ArrayOfAliasEquals) {
  EXPECT_EQ((abi_signature_hash<int[4]>()),
            (abi_signature_hash<IntAlias[4]>()));
}

// --- Negative test 5: pointer-to-typedef equals pointer-to-original -----
TEST_F(AbiSignatureTest, Neg05_PointerToAliasEquals) {
  EXPECT_EQ(abi_signature_hash<int *>(), abi_signature_hash<IntAlias *>());
}

}  // namespace villagesql_unittest
