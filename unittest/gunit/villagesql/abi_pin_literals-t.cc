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

// abi_pin_literals-t: verify that every ABI struct's pin literal for
// the current build target matches the structurally-computed hash, and
// print the computed value so it can be pasted into the SDK ABI header
// when filling in placeholders or updating after an ABI struct change.
//
// Workflow:
//   1. Build the server (and this test) on the target you want to
//      pin (mac / linux_x86 / linux_arm).
//   2. If the server build fails with an "ABI pin mismatch" static
//      _assert (from VEF_PIN_VERIFY in capability_registry.cc), the
//      server-side pin is stale.  Build just this test instead:
//        make villagesql-unit-tests
//      The test compiles independently of capability_registry.cc and
//      will succeed even when the server build doesn't.
//   3. Run this test.  Each TEST checks one capability:
//        * Pass: the pin literal for the current target matches the
//          computed structural hash.
//        * Fail (empty): the pin literal for the current target is "",
//          the placeholder sentinel.  The failure message includes the
//          computed hash to paste in.
//        * Fail (mismatch): the pin literal is set but doesn't match.
//          Same: the failure message includes the computed hash.
//   4. Paste each failing test's reported hash into the matching
//        #define VEF_<...>_ABI_HASH_<TARGET> "hash-..."
//      in the SDK ABI header next to its type.
//   5. Repeat on each target; the per-target literals coexist in the
//      same header.
//
// The server-side VEF_PIN_VERIFY in capability_registry.cc is the
// canonical correctness check (a stale pin fails the *server* build).
// This test exists because (a) empty placeholder pins compile cleanly
// in the server -- so without this test, an empty placeholder would
// silently ship to production -- and (b) when a real mismatch trips
// the server build, this test stays buildable and surfaces the right
// value to paste.

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include "villagesql/sdk/include/villagesql/abi/preview/keyring.h"
#include "villagesql/sdk/include/villagesql/abi/preview/ping.h"
#include "villagesql/sdk/include/villagesql/abi/preview/sql_query.h"
#include "villagesql/sdk/include/villagesql/abi/preview/status_var.h"
#include "villagesql/sdk/include/villagesql/abi/preview/storage.h"
#include "villagesql/sdk/include/villagesql/abi/preview/sys_var.h"
#include "villagesql/sdk/include/villagesql/abi/preview/thread_worker.h"
#include "villagesql/services/abi_specializations.h"

namespace villagesql_unittest {

namespace {

// Returns the symbolic name of the current build target, matching the
// VEF_..._HASH_<TARGET> suffix the failure message tells you to paste
// into.
constexpr const char *current_target_name() {
#if defined(__APPLE__)
  return "mac";
#elif defined(__linux__) && defined(__x86_64__)
  return "linux_x86";
#elif defined(__linux__) && defined(__aarch64__)
  return "linux_arm";
#else
  return "unknown_target";
#endif
}

// Returns the per-target #define name for the current build target,
// matching the same #if cascade as VEF_PIN's literal selection.
constexpr const char *current_target_define_name(const char *mac_name,
                                                 const char *x86_name,
                                                 const char *arm_name) {
#if defined(__APPLE__)
  (void)x86_name;
  (void)arm_name;
  return mac_name;
#elif defined(__linux__) && defined(__x86_64__)
  (void)mac_name;
  (void)arm_name;
  return x86_name;
#elif defined(__linux__) && defined(__aarch64__)
  (void)mac_name;
  (void)x86_name;
  return arm_name;
#else
  (void)mac_name;
  (void)x86_name;
  (void)arm_name;
  return "(unknown target)";
#endif
}

// Verifies that pin_literal (the per-target literal already selected
// by VEF_PIN in the caller) matches the structurally-computed
// fingerprint of T for the given ABI version.  Fires a gtest failure
// with the full #define line to paste if the pin is empty or wrong.
// Silent on success, so when a new type is added (no pin yet for the
// current target), the failure output names exactly that type and the
// value to paste.
template <typename T, std::uint32_t Version>
void check_pin(const char *type_name, const char *define_name,
               const char *pin_literal) {
  const auto computed = villagesql::detail::abi_signature_hash<T, Version>();
  const std::string_view computed_view = computed.view();
  const std::string_view pin_view = pin_literal;

  if (pin_view.empty()) {
    ADD_FAILURE() << type_name << " has an empty pin for "
                  << current_target_name()
                  << ".  Paste this line over the matching empty placeholder:\n"
                  << "    #define " << define_name << " \"" << computed_view
                  << "\"";
  } else if (pin_view != computed_view) {
    ADD_FAILURE() << type_name << " pin for " << current_target_name()
                  << " is stale.  Replace its #define with:\n"
                  << "    #define " << define_name << " \"" << computed_view
                  << "\"\n"
                  << "  (current value: \"" << pin_view << "\")";
  }
}

// The version arg names the ABI version the type is declared for
// (forwarded to abi_signature_hash<T, Version>).  The three target
// macro args are the per-target VEF_<...>_ABI_HASH_<TARGET> macros.
// `#mac` etc. stringify the macro *name* (not its expanded value), so
// we can embed it in the failure message; the bare names in VEF_PIN
// are expanded as usual to feed the literal selector.
#define VEF_CHECK_PIN(Type, version, mac, x86, arm)                        \
  check_pin<Type, (version)>(                                              \
      #Type,                                                               \
      ::villagesql_unittest::current_target_define_name(#mac, #x86, #arm), \
      VEF_PIN(mac, x86, arm))

}  // namespace

// One TEST per capability so the output groups types under their
// owning capability -- easier to scan when you're pinning a single
// one.  Each TEST fails iff the current target's pin literal is
// empty or stale; the printed line + failure message together give
// the value to paste in.

TEST(AbiPinLiterals, Keyring) {
  VEF_CHECK_PIN(vef_preview_keyring_t, VEF_PREVIEW_KEYRING_ABI_VERSION,
                VEF_PREVIEW_KEYRING_ABI_HASH_MAC,
                VEF_PREVIEW_KEYRING_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_KEYRING_ABI_HASH_LINUX_ARM);
}

TEST(AbiPinLiterals, Ping) {
  VEF_CHECK_PIN(vef_preview_ping_t, VEF_PREVIEW_PING_ABI_VERSION,
                VEF_PREVIEW_PING_ABI_HASH_MAC,
                VEF_PREVIEW_PING_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_PING_ABI_HASH_LINUX_ARM);
}

TEST(AbiPinLiterals, Storage) {
  VEF_CHECK_PIN(vef_preview_storage_t, VEF_STORAGE_SE_INTF_VERSION,
                VEF_PREVIEW_STORAGE_ABI_HASH_MAC,
                VEF_PREVIEW_STORAGE_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_STORAGE_ABI_HASH_LINUX_ARM);
}

TEST(AbiPinLiterals, ThreadWorker) {
  VEF_CHECK_PIN(vef_preview_thread_worker_t,
                VEF_PREVIEW_THREAD_WORKER_ABI_VERSION,
                VEF_PREVIEW_THREAD_WORKER_ABI_HASH_MAC,
                VEF_PREVIEW_THREAD_WORKER_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_THREAD_WORKER_ABI_HASH_LINUX_ARM);
  VEF_CHECK_PIN(vef_thread_worker_descriptor_t,
                VEF_PREVIEW_THREAD_WORKER_ABI_VERSION,
                VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_MAC,
                VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_LINUX_X86,
                VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_LINUX_ARM);
}

TEST(AbiPinLiterals, ColumnStore) {
  VEF_CHECK_PIN(vef_preview_column_store_t, VEF_COLUMN_STORE_INTF_VERSION,
                VEF_PREVIEW_COLUMN_STORE_ABI_HASH_MAC,
                VEF_PREVIEW_COLUMN_STORE_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_COLUMN_STORE_ABI_HASH_LINUX_ARM);
  VEF_CHECK_PIN(vef_preview_column_store_ext_desc_t,
                VEF_COLUMN_STORE_INTF_VERSION,
                VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_MAC,
                VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_LINUX_ARM);
}

TEST(AbiPinLiterals, SqlQuery) {
  VEF_CHECK_PIN(vef_preview_sql_query_t, VEF_PREVIEW_SQL_QUERY_ABI_VERSION,
                VEF_PREVIEW_SQL_QUERY_ABI_HASH_MAC,
                VEF_PREVIEW_SQL_QUERY_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_SQL_QUERY_ABI_HASH_LINUX_ARM);
}

TEST(AbiPinLiterals, StatusVar) {
  VEF_CHECK_PIN(vef_preview_status_var_t, VEF_PREVIEW_STATUS_VAR_ABI_VERSION,
                VEF_PREVIEW_STATUS_VAR_ABI_HASH_MAC,
                VEF_PREVIEW_STATUS_VAR_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_STATUS_VAR_ABI_HASH_LINUX_ARM);
  VEF_CHECK_PIN(vef_status_var_descriptor_list_t,
                VEF_PREVIEW_STATUS_VAR_ABI_VERSION,
                VEF_STATUS_VAR_DESC_LIST_ABI_HASH_MAC,
                VEF_STATUS_VAR_DESC_LIST_ABI_HASH_LINUX_X86,
                VEF_STATUS_VAR_DESC_LIST_ABI_HASH_LINUX_ARM);
}

TEST(AbiPinLiterals, SysVar) {
  VEF_CHECK_PIN(vef_preview_sys_var_t, VEF_PREVIEW_SYS_VAR_ABI_VERSION,
                VEF_PREVIEW_SYS_VAR_ABI_HASH_MAC,
                VEF_PREVIEW_SYS_VAR_ABI_HASH_LINUX_X86,
                VEF_PREVIEW_SYS_VAR_ABI_HASH_LINUX_ARM);
  VEF_CHECK_PIN(vef_sys_var_descriptor_list_t, VEF_PREVIEW_SYS_VAR_ABI_VERSION,
                VEF_SYS_VAR_DESC_LIST_ABI_HASH_MAC,
                VEF_SYS_VAR_DESC_LIST_ABI_HASH_LINUX_X86,
                VEF_SYS_VAR_DESC_LIST_ABI_HASH_LINUX_ARM);
}

}  // namespace villagesql_unittest
