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

// Trivial VillageSQL auth extension exercising the vsql::preview::auth
// capability end to end -- no JWT, no crypto. It registers an auth method named
// "vsql_auth_test" that authenticates iff the client sends one of the fixed
// tokens below, then resolves the session account as follows:
//   - the connecting account "auth_user" is proxy-mapped to the fixed account
//     "vsql_auth_test_user" (the auth_basic/auth_roles tests set up GRANT PROXY
//     for exactly this);
//   - any other connecting account authenticates AS ITSELF (used by the
//     auto-create flow, where the account is provisioned and then runs as
//     itself, no proxy).
//
// It also opts in to handling UNKNOWN accounts: an unknown-account login with a
// valid token is provisioned on the fly (request_provision) and then runs as
// the newly-created account. See auth_auto_create.test.
//
// Two accepted tokens:
//   kToken        -- accept, stage no roles (the account's default roles
//   apply). kTokenRoles   -- accept and additionally stage a fixed role set via
//                    set_active_roles: one role the test grants to the account
//                    ("vsql_role_granted") and one it does NOT
//                    ("vsql_role_denied"). Lets auth_roles.test assert that a
//                    granted role activates while an ungranted requested role
//                    is silently skipped (the no-escalation guarantee).
//
// Pairs with the built-in mysql_clear_password client plugin so the token
// arrives verbatim in the password slot (client must use
// --enable-cleartext-plugin).

#include <cstring>

#include <villagesql/preview/auth.h>
#include <villagesql/vsql.h>

using namespace vsql;

namespace {

constexpr char kToken[] = "vsql-auth-test-token";
constexpr char kTokenRoles[] = "vsql-auth-test-token-roles";
constexpr char kMappedAccount[] = "vsql_auth_test_user";

// Roles the kTokenRoles path stages. The test grants the first to the account
// and leaves the second ungranted, so activation must pick up the granted one
// and skip the denied one (no escalation).
constexpr char kRoleGranted[] = "vsql_role_granted";
constexpr char kRoleDenied[] = "vsql_role_denied";

bool token_matches(const unsigned char *pkt, size_t token_len,
                   const char *expected) {
  return token_len == std::strlen(expected) &&
         std::memcmp(pkt, expected, token_len) == 0;
}

// Opt in to handling unknown accounts: this test method always wants unknown-
// account logins routed to it, so it can validate the token and provision the
// account (a real extension would back this with a runtime sysvar, e.g. SET
// GLOBAL <ext>.auto_create, and query it here).
int auto_create_enabled() { return 1; }

// The authenticator. Reads one packet (the token), compares it to the fixed
// test tokens, and on success maps to the fixed account. Fail closed otherwise.
vef_auth_result_t authenticate(vef_auth_ctx_t *ctx, const vef_auth_ops_t *ops) {
  const unsigned char *pkt = nullptr;
  const int64_t len = ops->read_packet(ctx, &pkt);
  if (len <= 0 || pkt == nullptr) return VEF_AUTH_ERROR;

  // mysql_clear_password sends a NUL-terminated string; drop the trailing NUL.
  size_t token_len = static_cast<size_t>(len);
  if (pkt[token_len - 1] == '\0') --token_len;

  const bool plain = token_matches(pkt, token_len, kToken);
  const bool with_roles = token_matches(pkt, token_len, kTokenRoles);
  if (!plain && !with_roles) return VEF_AUTH_REJECT;

  const char *const user = ops->user_name(ctx);

  // Auto-create: if the account does not exist (this login was routed here by
  // the unknown-account opt-in), ask the server to provision it as itself,
  // IDENTIFIED WITH this method, granting the mapped roles. The server owns the
  // DDL. On success the session runs AS the (now-real) connecting account -- no
  // proxying.
  if (ops->account_unknown(ctx)) {
    const char *roles[] = {kRoleGranted};
    if (ops->request_provision(ctx, user, roles, 1) != 0) return VEF_AUTH_ERROR;
    ops->set_authenticated_as(ctx, user);
    ops->set_external_user(ctx, user);
    return VEF_AUTH_OK;
  }

  // A pre-existing account. Two shapes exercised by the tests:
  //   - auth_basic/auth_roles connect as "auth_user" and expect a fixed proxy
  //     map to kMappedAccount (they set up GRANT PROXY for exactly this).
  //   - auth_auto_create's account (provisioned above on a prior login)
  //   connects
  //     AS ITSELF; on a later login it is a normal existing account and must
  //     authenticate as itself, not proxy to kMappedAccount.
  // Distinguish by the connecting user: only auth_user proxies to the fixed
  // mapped account; everyone else authenticates as themselves.
  const bool proxy_to_mapped =
      user != nullptr && std::strcmp(user, "auth_user") == 0;
  const char *const account = proxy_to_mapped ? kMappedAccount : user;
  ops->set_authenticated_as(ctx, account);
  ops->set_external_user(ctx, account);

  if (with_roles) {
    // Stage a granted + an ungranted role. The server activates only the
    // granted one (grant-checked); the ungranted one is silently skipped.
    const char *roles[] = {kRoleGranted, kRoleDenied};
    ops->set_active_roles(ctx, roles, 2);
  }
  return VEF_AUTH_OK;
}

constexpr auto AUTH_METHOD =
    vsql::preview_auth::make_auth<&authenticate>("vsql_auth_test")
        .pin("mysql_clear_password")
        .auto_create(&auto_create_enabled)
        .build();
// AuthCapability is non-copyable (it self-registers at a fixed address), so
// construct it in place from the built descriptor.
vsql::preview_auth::AuthCapability g_auth{AUTH_METHOD};

}  // namespace

VEF_GENERATE_ENTRY_POINTS(make_extension().with(g_auth))
