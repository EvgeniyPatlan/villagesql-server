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

#include "villagesql/services/capability_registry.h"

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "villagesql/sdk/include/villagesql/abi/preview/keyring.h"
#include "villagesql/sdk/include/villagesql/abi/preview/ping.h"
#include "villagesql/sdk/include/villagesql/abi/preview/sql_query.h"
#include "villagesql/sdk/include/villagesql/abi/preview/status_var.h"
#include "villagesql/sdk/include/villagesql/abi/preview/storage.h"
#include "villagesql/sdk/include/villagesql/abi/preview/sys_var.h"
#include "villagesql/sdk/include/villagesql/abi/preview/thread_worker.h"
// abi_specializations.h pulls in abi_signature_compute.h and registers
// all the opaque-handle and anonymous-union specializations the
// server-side VEF_PIN_VERIFY needs to walk the ABI structs at compile
// time.  Must be visible before any VEF_PIN_VERIFY chain below.
#include "villagesql/services/abi_specializations.h"
#include "villagesql/services/preview/column_store.h"
#include "villagesql/services/preview/keyring.h"
#include "villagesql/services/preview/ping.h"
#include "villagesql/services/preview/sql_query.h"
#include "villagesql/services/preview/status_var.h"
#include "villagesql/services/preview/storage.h"
#include "villagesql/services/preview/sys_var.h"
#include "villagesql/services/preview/thread_worker.h"

// Per-capability pins are inlined into each register_capability() call
// below.  They are this server's contract: "this is the struct shape I
// am willing to serve."  Extensions carry their own copy of the same
// pin literal in their CapabilityTraits specializations (using the
// lighter VEF_PIN); the literals are matched at extension load time
// via strcmp on vtable_hash + capability_config_hash on the wire.
//
// Server-side VEF_PIN_VERIFY chains additionally compare each
// per-target literal against the structurally-computed fingerprint of
// the ABI type at compile time (via Boost.PFR field walking), failing
// the build if the literal is stale.  Run the abi_pin_literals gunit
// test on the target to obtain the current hash to paste in.
//
// linux_arm pins are best-effort copies of linux_x86 -- the Linux C
// ABI matches on x86_64 and arm64 for every primitive these structs
// touch, so they should match.  If a future build on Linux arm64
// produces a different hash, the linux_arm verification fails the
// build and we update the literal.

bool vsql_allow_preview_extensions = false;

namespace villagesql::services {

namespace {

// One registered (capability, ABI version) pair.  A capability name may map
// to multiple CapabilityVersion entries -- the server can serve multiple
// ABI shapes of the same capability simultaneously, and the extension
// chooses which by providing vtable_hash (plus capability_config_hash
// if the capability has a descriptor).
struct CapabilityVersion {
  void *vtable;
  // Static-lifetime string of the form "hash-XXXXXXXXXXXXXXXX" identifying
  // the exact ABI struct shape this entry serves.  Comparison with
  // vef_required_capability_t::vtable_hash is by strcmp.
  const char *vtable_hash;
  // Fingerprint of the descriptor struct type for capabilities that take
  // one (e.g. thread_worker).  nullptr for capabilities without a
  // descriptor.  Comparison with vef_required_capability_t::capability_config_hash
  // is by strcmp, treating nullptr as a sentinel that only matches another
  // nullptr.
  const char *capability_config_hash;
  // Optional. Called after the hash match succeeds for each extension that
  // requires this capability. Returns true on error (sets error_message),
  // false on success. NULL for capabilities that need no per-extension
  // setup.
  bool (*on_populate)(const PopulateContext &ctx, std::string &error_message);
  // Optional. Called before unloading an extension that required this
  // capability. Used to stop threads or clean up server-side resources
  // before the extension is removed. NULL for capabilities that need no
  // cleanup.
  void (*on_depopulate)(const DepopulateContext &ctx);
};

// Invariant: g_registry is mutated only from register_builtin_capabilities()
// (server startup) and unregister_capability() (server shutdown), both of
// which run single-threaded.  populate_capabilities() reads it from whatever
// thread runs INSTALL EXTENSION but only after startup registration is done.
// No locking is required as long as this invariant holds.  If a future
// capability ever needs post-startup registration, this map needs a mutex
// -- the inner vector's address-stability would be lost on concurrent
// push_back.
std::unordered_map<std::string, std::vector<CapabilityVersion>> g_registry;

// Compare two descriptor hash strings, with nullptr meaning "no descriptor".
// Two nullptrs match; nullptr vs a real string does not.
bool capability_config_hash_matches(const char *a, const char *b) {
  if (a == nullptr && b == nullptr) return true;
  if (a == nullptr || b == nullptr) return false;
  return std::strcmp(a, b) == 0;
}

// Look up the specific (name, vtable_hash, capability_config_hash)
// triple.  Returns nullptr when no matching version is registered.
const CapabilityVersion *find_capability_version(const char *name,
                                                 const char *vtable_hash,
                                                 const char *capability_config_hash) {
  if (name == nullptr || vtable_hash == nullptr) return nullptr;
  auto it = g_registry.find(name);
  if (it == g_registry.end()) return nullptr;
  for (const auto &v : it->second) {
    if (std::strcmp(v.vtable_hash, vtable_hash) != 0) continue;
    if (!capability_config_hash_matches(v.capability_config_hash, capability_config_hash)) continue;
    return &v;
  }
  return nullptr;
}

}  // namespace

void register_capability(std::string name, CapabilityRegistration reg) {
  if (reg.on_server_startup != nullptr) reg.on_server_startup();
  g_registry[std::move(name)].push_back({reg.vtable, reg.vtable_hash,
                                         reg.capability_config_hash, reg.on_populate,
                                         reg.on_depopulate});
}

void unregister_capability(const std::string &name) { g_registry.erase(name); }

void register_builtin_capabilities() {
  // Each (capability, ABI version) is registered as a separate entry.
  // To add a new ABI version for an existing capability, register an
  // additional entry under the same name with the new vtable and its
  // new pinned hash.
  //
  // VEF_PIN_VERIFY is used here (rather than the lighter VEF_PIN that
  // ships in the SDK) so the per-target literal is verified against
  // the structural fingerprint of the ABI type at compile time on
  // this server build -- a mismatch fails the build with a clean
  // static_assert diagnostic naming the type and target.  The
  // corresponding extension-side pin in each capability's _register.h
  // must use the same literal value; that is what makes the
  // wire-level strcmp at extension load time meaningful.
  //
  // Empty placeholder pins ("") are accepted on both sides without
  // verification -- used for capabilities whose per-target literals
  // have not yet been recorded (column_store, sql_query, status_var,
  // sys_var).  Run the abi_pin_literals gunit test on each target to
  // obtain the current hash to paste in.
  //
  // Each VEF_PIN_VERIFY(...) at the call site expands to a static_assert
  // that the per-target literal matches the structurally-computed hash
  // of the given ABI type, plus an evaluation that yields the literal
  // for the current build target.  A stale literal fails compilation
  // with a diagnostic naming the type and target.
  register_capability(VEF_PREVIEW_PING_NAME,
                      {.vtable = preview_ping_vtable(),
                       .vtable_hash = VEF_PIN_VERIFY(
                           vef_preview_ping_t, VEF_PREVIEW_PING_ABI_VERSION,
                           VEF_PREVIEW_PING_ABI_HASH_MAC,
                           VEF_PREVIEW_PING_ABI_HASH_LINUX_X86,
                           VEF_PREVIEW_PING_ABI_HASH_LINUX_ARM)});
  register_capability(
      VEF_PREVIEW_KEYRING_NAME,
      {.vtable = preview_keyring_vtable(),
       .vtable_hash = VEF_PIN_VERIFY(vef_preview_keyring_t,
                                     VEF_PREVIEW_KEYRING_ABI_VERSION,
                                     VEF_PREVIEW_KEYRING_ABI_HASH_MAC,
                                     VEF_PREVIEW_KEYRING_ABI_HASH_LINUX_X86,
                                     VEF_PREVIEW_KEYRING_ABI_HASH_LINUX_ARM)});
  register_capability(
      VEF_PREVIEW_STORAGE_NAME,
      {.vtable = preview_storage_vtable(),
       .vtable_hash = VEF_PIN_VERIFY(vef_preview_storage_t,
                                     VEF_STORAGE_SE_INTF_VERSION,
                                     VEF_PREVIEW_STORAGE_ABI_HASH_MAC,
                                     VEF_PREVIEW_STORAGE_ABI_HASH_LINUX_X86,
                                     VEF_PREVIEW_STORAGE_ABI_HASH_LINUX_ARM)});
  register_capability(
      VEF_PREVIEW_THREAD_WORKER_NAME,
      {.vtable = preview_thread_worker_vtable(),
       .vtable_hash = VEF_PIN_VERIFY(
           vef_preview_thread_worker_t, VEF_PREVIEW_THREAD_WORKER_ABI_VERSION,
           VEF_PREVIEW_THREAD_WORKER_ABI_HASH_MAC,
           VEF_PREVIEW_THREAD_WORKER_ABI_HASH_LINUX_X86,
           VEF_PREVIEW_THREAD_WORKER_ABI_HASH_LINUX_ARM),
       .capability_config_hash =
           VEF_PIN_VERIFY(vef_thread_worker_descriptor_t,
                          VEF_PREVIEW_THREAD_WORKER_ABI_VERSION,
                          VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_MAC,
                          VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_LINUX_X86,
                          VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_LINUX_ARM),
       .on_server_startup = init_thread_worker_psi_keys,
       .on_populate = on_populate_thread_worker,
       .on_depopulate = on_depopulate_thread_worker});
  register_capability(
      VEF_PREVIEW_COLUMN_STORE_NAME,
      {.vtable = preview_column_store_vtable(),
       .vtable_hash = VEF_PIN_VERIFY(
           vef_preview_column_store_t, VEF_COLUMN_STORE_INTF_VERSION,
           VEF_PREVIEW_COLUMN_STORE_ABI_HASH_MAC,
           VEF_PREVIEW_COLUMN_STORE_ABI_HASH_LINUX_X86,
           VEF_PREVIEW_COLUMN_STORE_ABI_HASH_LINUX_ARM),
       .capability_config_hash = VEF_PIN_VERIFY(
           vef_preview_column_store_ext_desc_t, VEF_COLUMN_STORE_INTF_VERSION,
           VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_MAC,
           VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_LINUX_X86,
           VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_LINUX_ARM)});
  register_capability(
      VEF_PREVIEW_SQL_QUERY_NAME,
      {.vtable = preview_sql_query_vtable(),
       .vtable_hash = VEF_PIN_VERIFY(
           vef_preview_sql_query_t, VEF_PREVIEW_SQL_QUERY_ABI_VERSION,
           VEF_PREVIEW_SQL_QUERY_ABI_HASH_MAC,
           VEF_PREVIEW_SQL_QUERY_ABI_HASH_LINUX_X86,
           VEF_PREVIEW_SQL_QUERY_ABI_HASH_LINUX_ARM)});
  // Status var: on_populate registers the extension's variables with
  // MySQL; on_depopulate unregisters them on extension unload.
  register_capability(
      VEF_PREVIEW_STATUS_VAR_NAME,
      {.vtable = preview_status_var_vtable(),
       .vtable_hash = VEF_PIN_VERIFY(vef_preview_status_var_t,
                                     VEF_PREVIEW_STATUS_VAR_ABI_VERSION,
                                     VEF_PREVIEW_STATUS_VAR_ABI_HASH_MAC,
                                     VEF_PREVIEW_STATUS_VAR_ABI_HASH_LINUX_X86,
                                     VEF_PREVIEW_STATUS_VAR_ABI_HASH_LINUX_ARM),
       .capability_config_hash =
           VEF_PIN_VERIFY(vef_status_var_descriptor_list_t,
                          VEF_PREVIEW_STATUS_VAR_ABI_VERSION,
                          VEF_STATUS_VAR_DESC_LIST_ABI_HASH_MAC,
                          VEF_STATUS_VAR_DESC_LIST_ABI_HASH_LINUX_X86,
                          VEF_STATUS_VAR_DESC_LIST_ABI_HASH_LINUX_ARM),
       .on_populate = on_populate_status_var,
       .on_depopulate = on_depopulate_status_var});
  // Sys var: on_populate registers the extension's system variables
  // with MySQL; on_depopulate unregisters them on extension unload.
  register_capability(
      VEF_PREVIEW_SYS_VAR_NAME,
      {.vtable = preview_sys_var_vtable(),
       .vtable_hash = VEF_PIN_VERIFY(vef_preview_sys_var_t,
                                     VEF_PREVIEW_SYS_VAR_ABI_VERSION,
                                     VEF_PREVIEW_SYS_VAR_ABI_HASH_MAC,
                                     VEF_PREVIEW_SYS_VAR_ABI_HASH_LINUX_X86,
                                     VEF_PREVIEW_SYS_VAR_ABI_HASH_LINUX_ARM),
       .capability_config_hash = VEF_PIN_VERIFY(
           vef_sys_var_descriptor_list_t, VEF_PREVIEW_SYS_VAR_ABI_VERSION,
           VEF_SYS_VAR_DESC_LIST_ABI_HASH_MAC,
           VEF_SYS_VAR_DESC_LIST_ABI_HASH_LINUX_X86,
           VEF_SYS_VAR_DESC_LIST_ABI_HASH_LINUX_ARM),
       .on_populate = on_populate_sys_var,
       .on_depopulate = on_depopulate_sys_var});
}

// TODO(villagesql-preview): Verify that the capabilities declared in
// vef_registration_t match those listed in the extension's manifest.
bool populate_capabilities(const PopulateContext &ctx,
                           const vef_registration_t *reg,
                           std::string &error_message) {
  if (reg == nullptr || reg->protocol < VEF_PROTOCOL_2 ||
      reg->required_capabilities == nullptr ||
      reg->required_capability_count == 0)
    return false;

  if (!vsql_allow_preview_extensions) {
    error_message =
        "extension requires preview capabilities but "
        "vsql_allow_preview_extensions is OFF";
    return true;
  }

  for (uint32_t i = 0; i < reg->required_capability_count; ++i) {
    const vef_required_capability_t &req = reg->required_capabilities[i];
    if (req.name == nullptr || req.vtable_dest == nullptr) continue;

    // vtable_hash is mandatory: every extension built against the current
    // SDK has it populated by vef_register.h.  An extension that arrives
    // without one was built against an SDK that predates the structural
    // fingerprint, which we do not support.
    if (req.vtable_hash == nullptr) {
      error_message =
          std::string("required capability missing ABI fingerprint: ") +
          req.name;
      return true;
    }
    // Distinguish "name unknown" from "name known but no version matches"
    // so the diagnostic points at the right thing.
    auto it = g_registry.find(req.name);
    if (it == g_registry.end()) {
      error_message =
          std::string("required capability not registered: ") + req.name;
      return true;
    }
    const CapabilityVersion *entry =
        find_capability_version(req.name, req.vtable_hash, req.capability_config_hash);
    if (entry == nullptr) {
      error_message = std::string("no matching ABI version for capability '") +
                      req.name + "' (extension requires " + req.vtable_hash +
                      (req.capability_config_hash != nullptr
                           ? std::string(" + descriptor ") + req.capability_config_hash
                           : std::string()) +
                      ")";
      return true;
    }
    *req.vtable_dest = entry->vtable;
    if (entry->on_populate != nullptr) {
      // ctx carries shared fields (reason, thd, extension_name); capability_config
      // is capability-specific and comes from the per-capability req entry.
      PopulateContext cap_ctx = ctx;
      cap_ctx.capability_config = req.capability_config;
      if (entry->on_populate(cap_ctx, error_message)) return true;
    }
  }

  return false;
}

void depopulate_capabilities(const DepopulateContext &ctx,
                             const vef_registration_t *reg) {
  if (reg == nullptr || reg->protocol < VEF_PROTOCOL_2 ||
      reg->required_capabilities == nullptr ||
      reg->required_capability_count == 0)
    return;

  for (uint32_t i = 0; i < reg->required_capability_count; ++i) {
    const vef_required_capability_t &req = reg->required_capabilities[i];
    if (req.name == nullptr || req.vtable_hash == nullptr) continue;

    const CapabilityVersion *entry =
        find_capability_version(req.name, req.vtable_hash, req.capability_config_hash);
    if (entry == nullptr || entry->on_depopulate == nullptr) continue;
    // Same as populate: ctx carries shared fields, capability_config is per-cap.
    DepopulateContext cap_ctx = ctx;
    cap_ctx.capability_config = req.capability_config;
    entry->on_depopulate(cap_ctx);
  }
}

}  // namespace villagesql::services
