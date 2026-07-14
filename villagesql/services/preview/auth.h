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

#ifndef VILLAGESQL_SERVICES_PREVIEW_AUTH_H
#define VILLAGESQL_SERVICES_PREVIEW_AUTH_H

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "villagesql/sdk/include/villagesql/abi/preview/auth.h"
#include "villagesql/services/capability_registry.h"

// Forward declarations at GLOBAL scope: THD, MPVIO_EXT, and MYSQL_LEX_CSTRING
// are the server's global types. Declaring them inside the namespace below
// would create distinct villagesql::services::* types and break linkage against
// the caller in sql/auth/, which uses the global ones.
class THD;
class Security_context;
struct MPVIO_EXT;
struct MYSQL_LEX_CSTRING;

namespace villagesql::services {

// The "vsql::preview::auth" capability owns its own in-memory registry of
// extension-provided auth methods (name -> handler config), entirely within
// this capability -- no auth-specific code lives in the core victionary or the
// veb DDL path. An auth method is a runtime callback registration (a handler
// pointer valid only while the .so is loaded), not persisted state, so it is
// registered on extension load via on_populate and removed on unload via
// on_depopulate, exactly like the statement_event capability.

// Server-side vtable for the capability (version tag only).
vef_preview_auth_t *preview_auth_vtable();

// Capability lifecycle hooks, wired into the capability registry. on_populate
// validates the auth method's config and adds it to the registry;
// on_depopulate removes it and drains any in-flight authenticate() call before
// returning, so the extension .so is safe to dlclose.
bool on_populate_auth(const PopulateContext &ctx, std::string &error_message);
void on_depopulate_auth(const DepopulateContext &ctx);

// Look up a registered auth method by its bare name (case-insensitive, matching
// the normalization used for cross-extension uniqueness). Returns the
// capability config, or nullptr if no method matches. While the returned
// pointer is in use, the caller MUST hold an AuthMethodRef (below) so a
// concurrent unload cannot free the config / dlclose the .so.
const vef_auth_cc_t *find_auth_method(std::string_view method_name);

// A scoped in-flight reference. While any AuthMethodRef is alive, on_depopulate
// blocks (drains) before returning, so the extension cannot be unloaded out
// from under an authenticate() call. Construct one for the full duration of a
// find_auth_method() result's use.
class AuthMethodRef {
 public:
  AuthMethodRef();
  ~AuthMethodRef();
  AuthMethodRef(const AuthMethodRef &) = delete;
  AuthMethodRef &operator=(const AuthMethodRef &) = delete;
};

// --- Seam for core MySQL auth (sql/auth/) ---
// The auth capability's core-facing entry points, called by core auth in
// sql/auth/ (sql_user.cc for CREATE USER, sql_authentication.cc for the login
// handshake via try_vef_authenticate below). Core calls these directly rather
// than reaching into the registry internals above -- mirroring how
// sql/sql_audit.cc calls on_statement_postexecute() directly.

// Existence check, used by CREATE USER validation to accept a VEF auth-method
// name the same way an installed plugin name is accepted.
bool auth_method_exists(std::string_view method_name);

// The auth method (if any) that opted in to handling UNKNOWN accounts
// (vef_auth_cc_t.auto_create_unknown_accounts). Called by decoy_user in the
// core auth path to decide whether an unknown-account login should be routed to
// a VEF method (for token validation + on-the-fly provisioning) rather than
// rejected. Returns the method's registered name, or empty if none opted in --
// or if more than one did (an ambiguous configuration; the server declines to
// guess and falls back to the normal unknown-account behavior). decoy_user must
// copy the returned string if it needs to outlive the call.
std::string auth_method_for_unknown_accounts();

// Handle a CREATE USER ... IDENTIFIED WITH <method_name> [BY '...'] whose name
// is not a loaded MySQL auth plugin, deciding whether it names a VEF extension
// auth method.
//
//   std::nullopt - not a VEF method; the caller falls back to its own
//                  "plugin not loaded" error handling.
//   false        - a VEF method; accept the account. A VEF method has no MySQL
//                  plugin, no credential hash, no password history/expiration
//                  -- nothing to validate. Covers plain IDENTIFIED WITH and
//                  IDENTIFIED WITH ... AS '...' (the AS string is the
//                  extension's to interpret).
//   true         - a VEF method used with IDENTIFIED BY '...'. Asking the
//                  (non-existent) plugin to hash a password is meaningless; an
//                  ER_PASSWORD_FORMAT error is raised here and the caller
//                  returns the error.
//
// When a VEF method is recognized (either bool result) the caller must set
// what_to_set to NONE_ATTR and return the bool -- there is nothing more to
// hash.
std::optional<bool> handle_vef_user_bind(std::string_view method_name,
                                         bool uses_identified_by_clause);

// Outcome of driving a VEF authenticator for a connection.
enum class VefAuthOutcome {
  kNotVef,    // method_name is not a VEF auth method (fall back to plugins)
  kAccepted,  // the extension authenticated the connection
  kRejected,  // the extension declined (fail closed)
};

// Called once, under the auth-method reference, with the client-side auth
// plugin name the VEF method pins (e.g. "mysql_clear_password"), or nullptr if
// it pins none. The seam uses it to stash the name on the connection before the
// handler's first read_packet triggers the handshake change-plugin request. The
// pointer is only valid for the duration of this callback's owning
// run_vef_authenticate() call (the extension stays loaded until it returns), so
// the callback must copy it if it needs it to persist.
using VefClientPluginSink = std::function<void(const char *client_plugin)>;

// Drive a VEF extension-provided authenticator by method name. Fail-closed:
// anything other than an explicit accept from the handler maps to kRejected. It
// does NOT touch MySQL internals -- it talks to the extension handler purely
// through the ABI ctx/ops the caller (sql/) supplies.
//
//   ctx / ops        : the server-owned handshake adapter (built in sql/, which
//                      owns MPVIO_EXT); passed opaquely to the handler.
//   on_client_plugin : invoked once, BEFORE the handler runs and while the
//                      auth-method reference is held, with the pinned
//                      client-plugin name.
VefAuthOutcome run_vef_authenticate(
    std::string_view method_name, vef_auth_ctx_t *ctx,
    const vef_auth_ops_t *ops, const VefClientPluginSink &on_client_plugin);

// The MySQL-side handshake adapter. Bridges the server's MPVIO_EXT handshake
// context to the VEF auth ABI (building the vef_auth_ops_t table over
// MPVIO_EXT) and drives run_vef_authenticate. Called from do_auth_once() in
// sql/auth/sql_authentication.cc when the auth method name is not a loaded
// MySQL plugin. Returns true if a VEF method handled the attempt (setting *res
// to CR_OK/CR_ERROR, fail closed); false if no such method is registered, so
// the caller falls back to the plugin-not-loaded error.
bool try_vef_authenticate(THD *thd, const MYSQL_LEX_CSTRING &auth_plugin_name,
                          MPVIO_EXT *mpvio, int *res);

// Opaque per-login state a VEF auth handler stages during the handshake, to be
// applied after account resolution. Its definition and all its fields live in
// auth.cc; core auth (sql/auth/) only holds a pointer to it on MPVIO_EXT and
// forwards it to apply_vef_login_state() below. Today it carries the roles set
// via set_active_roles(); anything else a handler needs to stage post-auth goes
// here too, with no further change to MPVIO_EXT.
struct VefAuthState;

// Apply whatever a VEF handler staged for this login, AFTER the account has
// been resolved. Currently: activate the staged role set on `sctx`, replacing
// default-role activation, using the server's grant-checked activation (a role
// not granted to the account is skipped) so a token can never escalate. The
// caller holds no ACL lock; this takes it as needed, mirroring the default-role
// block it replaces. Does nothing (returns false = "not handled, use default
// roles") when `mpvio` has no staged state. Returns true when it applied staged
// state, so the caller skips its own default-role activation.
//
// `sctx` is the session's Security_context (the same one the default-role path
// activates onto); `acl_user_authid`/`acl_user_host` identify the resolved
// account for warning messages. Roles are activated but access maps are NOT
// checked out here -- the caller does checkout_access_maps() once afterward, as
// it already does for the default-role path.
bool apply_vef_login_state(MPVIO_EXT *mpvio, Security_context *sctx,
                           const char *acl_user_authid,
                           const char *acl_user_host);

// Auto-GRANT the token-staged roles to the resolved account, additively, iff a
// VEF method has opted into token authority (the same opt-in that gates
// auto-create: auth_method_for_unknown_accounts() non-empty). This runs GRANT
// DDL, so it MUST be called on the connection thread at a point where NO ACL
// cache lock is held (it uses a fresh internal THD that takes ACL locks itself)
// -- i.e. BEFORE the role-activation block, not from inside apply_vef_login_state
// (which runs under the ACL read lock). After this grants the claimed roles,
// apply_vef_login_state's grant-checked activation finds them and activates them
// instead of skipping.
//
// Additive only: a role no longer claimed is NOT revoked. Roles must pre-exist
// as DB roles (a claimed role that cannot be granted is skipped, logged). No-op
// when nothing was staged or the method is not opted in (activate-only stays the
// default). Same DDL-on-login caveats as request_provision (replica/read-only,
// concurrency) -- see its TODO(villagesql-beta).
void apply_vef_role_grants(MPVIO_EXT *mpvio,
                           const char *acl_user_authid,
                           const char *acl_user_host);

}  // namespace villagesql::services

#endif  // VILLAGESQL_SERVICES_PREVIEW_AUTH_H
