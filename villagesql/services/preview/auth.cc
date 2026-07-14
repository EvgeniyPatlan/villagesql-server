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

#include "villagesql/services/preview/auth.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "my_sys.h"
#include "mysql/plugin_auth_common.h"
#include "mysqld_error.h"
#include "sql/auth/auth_common.h"
#include "sql/auth/sql_authentication.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/auto_thd.h"     // Auto_THD (auto-create provisioner)
#include "sql/current_thd.h"  // current_thd (restore after Auto_THD)
#include "sql/statement/ed_connection.h"  // Ed_connection (run the DDL)
#include "sql/strfunc.h"                  // lex_string_strmake
#include "strmake.h"
#include "villagesql/include/error.h"
#include "villagesql/schema/systable/helpers.h"
#include "villagesql/sdk/include/villagesql/abi/preview/auth.h"

// The VEF auth ctx is just the connection's MPVIO_EXT. The extension handler
// only ever sees vef_auth_ctx_t (opaque) + the ops table, so it never touches
// MySQL internals directly.
struct vef_auth_ctx_s {
  MPVIO_EXT *mpvio;
};

namespace villagesql::services {

// One staged role, split into name + host ("role" -> host "%"; "role@host" ->
// the given host). Both strings are copied onto the connection MEM_ROOT.
struct StagedRole {
  const char *name;
  const char *host;
};

// The opaque per-login state forward-declared in auth.h. Holds the roles a VEF
// handler staged via set_active_roles(). The array and its strings are all
// allocated on the connection MEM_ROOT (freed with the connection, no manual
// cleanup); MPVIO_EXT holds a pointer to it. count == 0 with vef_auth_state
// non-null means "activate no roles" (SET ROLE NONE), distinct from
// vef_auth_state == nullptr ("handler staged nothing; use the account's default
// roles").
struct VefAuthState {
  StagedRole *roles;
  uint count;
};

namespace {

struct RegisteredMethod {
  std::string method_name;     // normalized (for uniqueness + lookup)
  std::string extension_name;  // for the "already registered by" message
  const vef_auth_cc_t *cc;
};

// The auth-method registry: a small set (names are globally unique across
// extensions, so a handful of entries) mutated only on INSTALL/UNINSTALL
// EXTENSION and read once per login. A plain vector under g_mu suffices; the
// lookup copies out the cc pointer under the lock and releases it before the
// handler runs (which does client I/O and must not hold a global lock).
std::mutex g_mu;
std::vector<RegisteredMethod> g_methods;

// In-flight drain counter. AuthMethodRef bumps it (seq_cst) for the duration of
// an authenticate() call; on_depopulate removes the method from g_methods then
// spins until this reaches zero, so no extension code runs after on_depopulate
// returns and the .so becomes safe to dlclose.
std::atomic<size_t> g_inflight{0};

vef_preview_auth_t g_auth_vtable{VEF_PREVIEW_AUTH_ABI_VERSION};

// --- MySQL-side handshake adapter (the ops table over MPVIO_EXT) ---
// Read/write go through the VIO function pointers already installed on the
// MPVIO_EXT (server_mpvio_read_packet / server_mpvio_write_packet), so this
// needs no access to those file-static functions.
int64_t vef_auth_read_packet(vef_auth_ctx_t *ctx, const unsigned char **data) {
  unsigned char *buf = nullptr;
  const int n = ctx->mpvio->read_packet(ctx->mpvio, &buf);
  if (data != nullptr) *data = buf;
  return n;
}

int64_t vef_auth_write_packet(vef_auth_ctx_t *ctx, const unsigned char *data,
                              uint64_t len) {
  return ctx->mpvio->write_packet(ctx->mpvio, data, static_cast<int>(len));
}

const char *vef_auth_user_name(vef_auth_ctx_t *ctx) {
  const char *u = ctx->mpvio->auth_info.user_name;
  return u != nullptr ? u : "";
}

const char *vef_auth_auth_string(vef_auth_ctx_t *ctx) {
  const char *s = ctx->mpvio->auth_info.auth_string;
  return s != nullptr ? s : "";
}

const char *vef_auth_host_or_ip(vef_auth_ctx_t *ctx) {
  const char *h = ctx->mpvio->auth_info.host_or_ip;
  return h != nullptr ? h : "";
}

const char *vef_auth_client_plugin(vef_auth_ctx_t *ctx) {
  // The client-plugin name the client advertised for this connection, cached on
  // the handshake context. Set before the handler's first read, so it is
  // available throughout the handler call.
  const char *p = ctx->mpvio->cached_client_reply.plugin;
  return p != nullptr ? p : "";
}

void vef_auth_set_authenticated_as(vef_auth_ctx_t *ctx, const char *account) {
  if (account == nullptr) return;
  strmake(ctx->mpvio->auth_info.authenticated_as, account,
          sizeof(ctx->mpvio->auth_info.authenticated_as) - 1);
}

void vef_auth_set_external_user(vef_auth_ctx_t *ctx, const char *identity) {
  if (identity == nullptr) return;
  strmake(ctx->mpvio->auth_info.external_user, identity,
          sizeof(ctx->mpvio->auth_info.external_user) - 1);
}

void vef_auth_set_active_roles(vef_auth_ctx_t *ctx, const char *const *roles,
                               uint64_t n_roles) {
  MPVIO_EXT *mpvio = ctx->mpvio;
  MEM_ROOT *mem_root = mpvio->mem_root;

  // Allocate/replace the state on the connection MEM_ROOT (freed with the
  // connection; no manual cleanup). Re-staging replaces the prior set.
  auto *state = new (mem_root) VefAuthState();
  state->roles = (n_roles > 0) ? new (mem_root)
                                     StagedRole[static_cast<size_t>(n_roles)]
                               : nullptr;
  state->count = 0;

  for (uint64_t i = 0; i < n_roles; ++i) {
    const char *r = roles[i];
    if (r == nullptr || r[0] == '\0') continue;  // skip empties defensively
    // Split "role@host"; a bare "role" resolves to host "%". Copy both parts
    // onto the MEM_ROOT so nothing points into the extension's transient
    // buffer.
    const char *at = strchr(r, '@');
    StagedRole &slot = state->roles[state->count];
    if (at != nullptr) {
      slot.name = strmake_root(mem_root, r, static_cast<size_t>(at - r));
      slot.host = strdup_root(mem_root, at + 1);
    } else {
      slot.name = strdup_root(mem_root, r);
      slot.host = "%";
    }
    ++state->count;
  }

  mpvio->vef_auth_state = state;
}

int vef_auth_account_unknown(vef_auth_ctx_t *ctx) {
  // True when the account being authenticated does not exist -- i.e. this login
  // was routed here as a decoy by the unknown-account opt-in (see decoy_user).
  return ctx->mpvio->acl_user_is_decoy ? 1 : 0;
}

// Run one SQL statement on the given internal (skip-grants) THD. Returns true
// on failure and logs the error. Uses the internal statement executor; the THD
// must already have its security context elevated.
//
// Runs `sql` as one privileged statement on its OWN fresh internal THD:
//   - each statement gets a clean binlog/transaction state (running a SEQUENCE
//     of DDL on one reused Auto_THD trips a binlog-XID assertion, since the
//     per-statement binlog bookkeeping is not reset between execute_direct
//     calls -- so one Auto_THD per statement);
//   - create_internal_thd() hijacks this thread's current_thd via
//     store_globals(), and nothing restores it, so we capture the connection
//     THD first and restore it after the Auto_THD is gone (else the rest of
//     acl_authenticate runs with a bogus current_thd and crashes).
// (Slice 3b's off-thread provisioner avoids both hazards -- it never runs on,
// nor disturbs, the connection thread.)
static bool provision_run(const std::string &sql) {
  THD *const conn_thd = current_thd;
  bool failed = false;
  {
    Auto_THD provisioner;
    provisioner.thd->security_context()->skip_grants();
    Ed_connection conn(provisioner.thd);
    MYSQL_LEX_STRING s;
    lex_string_strmake(provisioner.thd->mem_root, &s, sql.c_str(),
                       sql.length());
    if (conn.execute_direct(s)) {
      failed = true;
      LogVSQL(WARNING_LEVEL, "auto-create: statement failed (errno=%u): %s",
              conn.get_last_errno(),
              conn.get_last_error() ? conn.get_last_error() : "");
    }
  }  // ~Auto_THD -> destroy_internal_thd -> current_thd now dangling
  conn_thd->store_globals();  // restore the connection thread's globals
  return failed;
}

int vef_auth_request_provision(vef_auth_ctx_t *ctx [[maybe_unused]],
                               const char *account, const char *const *roles,
                               uint64_t n_roles) {
  if (account == nullptr || account[0] == '\0') return 1;

  // The account authenticates via the same VEF method that is handling unknown
  // accounts, so once created it runs AS itself (no proxy). Bail if that method
  // is no longer the single opted-in one (config changed mid-login).
  const std::string method = auth_method_for_unknown_accounts();
  if (method.empty()) return 1;

  // TODO(villagesql-beta): running DDL synchronously from the login path is not
  // production-safe. On a replica or a read_only/super_read_only server the
  // CREATE USER will fail, so every unknown-account login attempts (and fails)
  // DDL; and concurrent logins for the same unknown account race with no
  // single-flight guard (CREATE USER IF NOT EXISTS is idempotent, but the
  // GRANTs and binlog events are not coordinated). Slice 3b moves provisioning
  // to a server-internal background thread with block-and-retry + single-flight
  // dedup, which is where read-only/replica handling belongs. Do not enable
  // auto-create in a replicated or read-only deployment until then.
  bool ok = true;
  {
    // TODO(villagesql-beta): SECURITY -- `account` is the client-supplied
    // handshake username (attacker-controlled, reachable pre-auth) and `roles`
    // are interpolated straight into DDL with no escaping. A crafted username
    // (embedded quote/backtick) is a SQL-injection vector. Quote/validate all
    // identifiers before interpolation; do not enable auto-create until closed.
    std::string create = "CREATE USER IF NOT EXISTS '";
    create.append(account);
    create.append("'@'%' IDENTIFIED WITH '");
    create.append(method);
    create.append("'");
    if (provision_run(create)) {
      ok = false;
    } else {
      for (uint64_t i = 0; i < n_roles; ++i) {
        if (roles[i] == nullptr || roles[i][0] == '\0') continue;
        std::string grant = "GRANT '";
        grant.append(roles[i]);
        grant.append("' TO '");
        grant.append(account);
        grant.append("'@'%'");
        // A role that does not exist / cannot be granted is skipped (logged),
        // not fatal -- mirrors set_active_roles' no-escalation stance.
        (void)provision_run(grant);
      }
    }
  }

  return ok ? 0 : 1;
}

const vef_auth_ops_t g_vef_auth_ops = {
    VEF_PREVIEW_AUTH_ABI_VERSION,  vef_auth_read_packet,
    vef_auth_write_packet,         vef_auth_user_name,
    vef_auth_auth_string,          vef_auth_host_or_ip,
    vef_auth_client_plugin,        vef_auth_set_authenticated_as,
    vef_auth_set_external_user,    vef_auth_set_active_roles,
    vef_auth_account_unknown,      vef_auth_request_provision};

}  // namespace

vef_preview_auth_t *preview_auth_vtable() { return &g_auth_vtable; }

bool apply_vef_login_state(MPVIO_EXT *mpvio, Security_context *sctx,
                           const char *acl_user_authid,
                           const char *acl_user_host) {
  const VefAuthState *state = mpvio->vef_auth_state;
  if (state == nullptr) return false;  // handler staged nothing; use defaults

  // The caller holds the ACL cache lock (this replaces the account's
  // default-role activation, which runs under the same lock) and calls
  // checkout_access_maps() afterward. Activate each staged role grant-checked:
  // activate_role(..., validate_access=true) refuses a role not granted to the
  // authenticated account, so a token can only activate what the DBA granted --
  // never grant or escalate. count == 0 activates nothing (SET ROLE NONE).
  for (uint i = 0; i < state->count; ++i) {
    const LEX_CSTRING role{state->roles[i].name, strlen(state->roles[i].name)};
    const LEX_CSTRING host{state->roles[i].host, strlen(state->roles[i].host)};
    if (sctx->activate_role(role, host, /*validate_access=*/true)) {
      LogVSQL(WARNING_LEVEL,
              "VEF auth: role '%s'@'%s' requested for account '%s'@'%s' is not "
              "granted; skipping",
              state->roles[i].name, state->roles[i].host,
              acl_user_authid != nullptr ? acl_user_authid : "",
              acl_user_host != nullptr ? acl_user_host : "");
    }
  }
  return true;  // staged state applied; caller must NOT also activate defaults
}

void apply_vef_role_grants(MPVIO_EXT *mpvio, const char *acl_user_authid,
                           const char *acl_user_host) {
  const VefAuthState *state = mpvio->vef_auth_state;
  if (state == nullptr || state->count == 0) return;  // nothing staged

  // Gate: only auto-grant when a VEF method has opted into token authority --
  // the same opt-in that gates auto-create. When it is off, the token stays
  // activate-only (the DBA owns grants) and we do nothing here.
  if (auth_method_for_unknown_accounts().empty()) return;

  const char *account =
      acl_user_authid != nullptr ? acl_user_authid : "";
  const char *host = acl_user_host != nullptr ? acl_user_host : "%";
  if (account[0] == '\0') return;

  // GRANT each staged role additively. A role that does not exist as a DB role
  // (or otherwise cannot be granted) fails inside provision_run and is skipped
  // (logged), not fatal -- the token carries a group NAME, not the role's
  // definition, so we never CREATE ROLE here. No REVOKE: a role no longer
  // claimed is retained (authoritative reconcile is a separate, deferred task).
  //
  // TODO(villagesql-beta): SECURITY -- `account`/`host`/role names are
  // interpolated into DDL without escaping (same gap as request_provision).
  // Quote/validate identifiers before enabling in a real deployment.
  for (uint i = 0; i < state->count; ++i) {
    const char *role = state->roles[i].name;
    const char *role_host = state->roles[i].host;
    if (role == nullptr || role[0] == '\0') continue;
    std::string grant = "GRANT '";
    grant.append(role);
    grant.append("'@'");
    grant.append(role_host != nullptr ? role_host : "%");
    grant.append("' TO '");
    grant.append(account);
    grant.append("'@'");
    grant.append(host);
    grant.append("'");
    (void)provision_run(grant);
  }
}

AuthMethodRef::AuthMethodRef() {
  g_inflight.fetch_add(1, std::memory_order_seq_cst);
}
AuthMethodRef::~AuthMethodRef() {
  g_inflight.fetch_sub(1, std::memory_order_release);
}

bool on_populate_auth(const PopulateContext &ctx, std::string &error_message) {
  if (ctx.capability_config == nullptr) return false;
  const auto *cc = static_cast<const vef_auth_cc_t *>(ctx.capability_config);

  // Validate the method config. (Moved verbatim from the former validate.cc
  // auth branch -- INSTALL-time misconfiguration fails here.)
  if (cc->name == nullptr || cc->name[0] == '\0') {
    error_message = "auth capability has no method name";
    return true;
  }
  if (strlen(cc->name) > VEF_AUTH_MAX_NAME_LEN) {
    error_message = std::string("auth method '") + cc->name +
                    "': name exceeds the maximum length";
    return true;
  }
  if (cc->handler == nullptr) {
    error_message = std::string("auth method '") + cc->name +
                    "': handler function pointer is not set";
    return true;
  }
  // A VEF auth method must pin a client-side auth plugin: the handshake's
  // change-plugin request needs a name to send the client, and a VEF method has
  // no MySQL plugin to fall back on. Require it here so the misconfiguration
  // fails at INSTALL rather than sending an empty plugin name at login.
  if (cc->client_auth_plugin == nullptr || cc->client_auth_plugin[0] == '\0') {
    error_message = std::string("auth method '") + cc->name +
                    "': client_auth_plugin is not set";
    return true;
  }

  const std::string normalized = normalize_extension_name(cc->name);
  const std::string ext(ctx.extension_name);

  std::lock_guard<std::mutex> lock(g_mu);
  // Auth-method names must be unique across ALL extensions: an account binds to
  // a bare name (IDENTIFIED WITH <name>) and login resolves it by name, so a
  // collision would be ambiguous. Compare on the normalized name so a case-only
  // difference is caught.
  for (const auto &m : g_methods) {
    if (m.method_name == normalized) {
      error_message = std::string("auth method '") + cc->name +
                      "' already registered by extension '" + m.extension_name +
                      "'";
      return true;
    }
  }

  g_methods.push_back({normalized, ext, cc});

  LogVSQL(INFORMATION_LEVEL, "Registered auth method '%s' from extension '%s'",
          cc->name, ext.c_str());
  return false;
}

void on_depopulate_auth(const DepopulateContext &ctx) {
  if (ctx.capability_config == nullptr) return;
  const auto *cc = static_cast<const vef_auth_cc_t *>(ctx.capability_config);

  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_methods.erase(
        std::remove_if(g_methods.begin(), g_methods.end(),
                       [&](const RegisteredMethod &m) { return m.cc == cc; }),
        g_methods.end());
  }

  // Drain: wait for any in-flight authenticate() using this (or any) method to
  // finish before returning, so the extension .so is safe to dlclose.
  while (g_inflight.load(std::memory_order_acquire) > 0)
    std::this_thread::yield();
}

const vef_auth_cc_t *find_auth_method(std::string_view method_name) {
  if (method_name.empty()) return nullptr;
  const std::string normalized =
      normalize_extension_name(std::string(method_name));
  std::lock_guard<std::mutex> lock(g_mu);
  for (const auto &m : g_methods) {
    if (m.method_name == normalized) return m.cc;
  }
  return nullptr;
}

bool auth_method_exists(std::string_view method_name) {
  return find_auth_method(method_name) != nullptr;
}

std::string auth_method_for_unknown_accounts() {
  std::lock_guard<std::mutex> lock(g_mu);
  const vef_auth_cc_t *chosen = nullptr;
  for (const auto &m : g_methods) {
    // Query the opt-in LIVE (so it reflects the extension's runtime sysvar),
    // not a static registration flag. NULL callback == never opts in.
    if (m.cc != nullptr && m.cc->auto_create_unknown_accounts != nullptr &&
        m.cc->auto_create_unknown_accounts() != 0) {
      if (chosen != nullptr) {
        // More than one method opted in: ambiguous. Decline to guess and fall
        // back to normal unknown-account handling.
        LogVSQL(WARNING_LEVEL,
                "Multiple auth methods opted in to handle unknown accounts; "
                "routing unknown accounts is disabled until only one does");
        return std::string();
      }
      chosen = m.cc;
    }
  }
  return (chosen != nullptr && chosen->name != nullptr)
             ? std::string(chosen->name)
             : std::string();
}

std::optional<bool> handle_vef_user_bind(std::string_view method_name,
                                         bool uses_identified_by_clause) {
  if (!auth_method_exists(method_name)) return std::nullopt;
  if (uses_identified_by_clause) {
    // IDENTIFIED BY '...' asks the (non-existent) plugin to hash a password;
    // meaningless for a VEF method. Reject rather than silently ignore.
    my_error(ER_PASSWORD_FORMAT, MYF(0));
    return true;
  }
  return false;
}

VefAuthOutcome run_vef_authenticate(
    std::string_view method_name, vef_auth_ctx_t *ctx,
    const vef_auth_ops_t *ops, const VefClientPluginSink &on_client_plugin) {
  // Hold an in-flight reference for the ENTIRE handler call, so the extension's
  // .so cannot be dlclose'd out from under a login in flight. on_depopulate
  // (extension unload) removes the method then drains outstanding references
  // before returning, so an unload that races this login waits for it to
  // finish. The ref must be taken BEFORE the lookup and held across the handler
  // call and client-plugin resolution below.
  AuthMethodRef ref;
  const vef_auth_cc_t *cc = find_auth_method(method_name);
  if (cc == nullptr) return VefAuthOutcome::kNotVef;

  // Registered but no handler -> fail closed (but it IS a VEF method).
  if (cc->handler == nullptr) return VefAuthOutcome::kRejected;

  // Hand the pinned client-plugin name to the caller under the SAME held
  // reference, before driving the handler. Resolving it here (rather than via a
  // separate ref-less lookup) is what keeps the pointer from dangling if the
  // extension is uninstalled between resolution and use.
  on_client_plugin(cc->client_auth_plugin);

  // Run the extension's authenticator over the caller-supplied ctx/ops. Fail
  // closed: only an explicit VEF_AUTH_OK accepts.
  const vef_auth_result_t r = cc->handler(ctx, ops);
  return (r == VEF_AUTH_OK) ? VefAuthOutcome::kAccepted
                            : VefAuthOutcome::kRejected;
}

bool try_vef_authenticate(THD *thd [[maybe_unused]],
                          const MYSQL_LEX_CSTRING &auth_plugin_name,
                          MPVIO_EXT *mpvio, int *res) {
  const std::string_view method_name(auth_plugin_name.str,
                                     auth_plugin_name.length);

  // A VEF method has no MySQL plugin.
  mpvio->plugin = nullptr;

  // The client-side plugin the handshake should advertise comes from the
  // method's config. run_vef_authenticate resolves it under the auth-method
  // reference and hands it to this sink before driving the handler; we stash it
  // on mpvio so the handler's first read_packet -- which triggers the handshake
  // change-plugin request read via mpvio_client_plugin_name() -- sees it.
  // Resolving under the held reference (not a separate ref-less lookup) avoids
  // a use-after-free of the extension-owned string.
  vef_auth_ctx_t ctx{mpvio};
  const VefAuthOutcome outcome = run_vef_authenticate(
      method_name, &ctx, &g_vef_auth_ops, [mpvio](const char *client_plugin) {
        mpvio->vef_client_auth_plugin = client_plugin;
      });

  if (outcome == VefAuthOutcome::kNotVef) return false;
  *res = (outcome == VefAuthOutcome::kAccepted) ? CR_OK : CR_ERROR;
  return true;
}

}  // namespace villagesql::services
