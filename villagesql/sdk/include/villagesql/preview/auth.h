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
// along with this program; if not, see <https://www.gnu.org/licenses/>.

// =============================================================================
// PREVIEW CAPABILITY -- UNSTABLE API
// =============================================================================
// This header is part of the VEF preview surface. Its API and ABI may change
// or be removed without notice. See villagesql/preview/README.md for details.
// =============================================================================

#ifndef VILLAGESQL_PREVIEW_AUTH_H
#define VILLAGESQL_PREVIEW_AUTH_H

#include <type_traits>

#include <villagesql/abi/preview/auth.h>
#include <villagesql/detail/capability_base.h>
#include <villagesql/detail/capability_traits.h>

namespace vsql::preview_auth {

// Result an authenticator returns. Mirrors vef_auth_result_t.
using AuthResult = vef_auth_result_t;

// The server-provided operations on the auth context (read the token, set the
// effective account, etc.). Passed to the handler. See abi/preview/auth.h.
using AuthOps = vef_auth_ops_t;
using AuthCtx = vef_auth_ctx_t;

// AuthDescriptor is the passive, fluent builder for one authentication method.
// It follows the same shape as the stable make_func / make_type builders and
// the preview index_builder: the authenticate handler is a compile-time template
// argument (make_auth<&handler>), config is set with chained setters, and
// build() yields the descriptor. It does NOT self-register -- it is a value you
// hand to an AuthCapability token via .method() (see below), mirroring how
// make_index_profile(...).build() feeds IndexProfileCapability().index_profile().
//
// Splitting the fluent builder (this) from the self-registering token
// (AuthCapability) is what lets auth use chained setters safely: the enrolling
// object stays single-config, so a chained temporary can't double-enroll.
//
// Usage:
//   static vef_auth_result_t my_authenticate(vef_auth_ctx_t *ctx,
//                                             const vef_auth_ops_t *ops) {
//     const unsigned char *tok = nullptr;
//     if (ops->read_packet(ctx, &tok) < 0) return VEF_AUTH_ERROR;
//     ... validate ...
//     ops->set_authenticated_as(ctx, "alice");
//     return VEF_AUTH_OK;
//   }
//
//   constexpr auto MY_AUTH = vsql::preview_auth::make_auth<&my_authenticate>(
//                                "my_auth")
//                                .pin("mysql_clear_password")
//                                .auto_create(&my_auto_create_enabled)
//                                .build();
//   static auto AUTH = vsql::preview_auth::AuthCapability().method(MY_AUTH);
//   VEF_GENERATE_ENTRY_POINTS(make_extension().with(AUTH))
class AuthDescriptor {
 public:
  // `pin`: the client-side auth plugin the method advertises (e.g.
  // "mysql_clear_password" to receive a bearer token verbatim). Optional.
  constexpr AuthDescriptor &pin(const char *client_auth_plugin) {
    cc_.client_auth_plugin = client_auth_plugin;
    return *this;
  }

  // `auto_create`: a callback returning non-zero when this method should
  // currently handle logins for UNKNOWN accounts (on-the-fly provisioning).
  // Queried live per unknown-account login, so back it with your runtime sysvar
  // (e.g. return whether SET GLOBAL <ext>.auto_create is ON). Optional; omitted
  // = handle only pre-existing accounts.
  constexpr AuthDescriptor &auto_create(int (*auto_create_enabled)(void)) {
    cc_.auto_create_unknown_accounts = auto_create_enabled;
    return *this;
  }

  constexpr vef_auth_cc_t build() const { return cc_; }

 private:
  template <auto Handler>
  friend constexpr AuthDescriptor make_auth(const char *name);

  vef_auth_cc_t cc_{};
};

// make_auth<&handler>("name") -- entry point for the fluent auth builder. The
// handler is a compile-time template argument (like make_func<&impl>), so a null
// or wrong-signature handler is a compile error, not a runtime failure.
template <auto Handler>
constexpr AuthDescriptor make_auth(const char *name) {
  static_assert(std::is_same_v<decltype(Handler), vef_auth_authenticate_func_t>,
                "make_auth<&handler>: handler must have signature "
                "vef_auth_result_t(vef_auth_ctx_t*, const vef_auth_ops_t*)");
  AuthDescriptor d;
  d.cc_.name = name;
  d.cc_.handler = Handler;
  return d;
}

// AuthCapability is the self-registering token consumed by
// make_extension().with(...). It wraps one built AuthDescriptor via .method();
// its cc member is what CapabilityTraits hands to the server (by address), so it
// must outlive registration (declare the token static).
class AuthCapability : public ::vsql::detail::CapabilityBase<AuthCapability> {
 public:
  AuthCapability() = default;

  // Construct the token directly from a built descriptor. This is the in-place
  // form -- AuthCapability is non-copyable (it self-registers at a fixed
  // address), so a token must be constructed, not copy-initialized from a
  // temporary. Usage: static AuthCapability g_auth{MY_AUTH};
  explicit AuthCapability(const vef_auth_cc_t &desc) { cc = desc; }

  // Attach the built descriptor to an already-constructed token. Single config
  // point (not a multi-setter chain) so the self-registering token is never
  // copied mid-configuration. Returns *this for `g_auth.method(MY_AUTH);`
  // statement use (NOT for copy-init -- the token is non-copyable).
  AuthCapability &method(const vef_auth_cc_t &desc) {
    cc = desc;
    return *this;
  }

  // Deprecated positional form, retained until callers migrate to
  // make_auth<&handler>(...).pin(...).auto_create(...).build() + .method().
  // TODO(villagesql): remove once vsql_oauth2 and tests use the fluent builder.
  AuthCapability(const char *name, vef_auth_authenticate_func_t handler,
                 const char *client_auth_plugin = nullptr,
                 int (*auto_create_enabled)(void) = nullptr) {
    cc.name = name;
    cc.handler = handler;
    cc.client_auth_plugin = client_auth_plugin;
    cc.auto_create_unknown_accounts = auto_create_enabled;
  }

  // Capability config read by the server's validate step. CapabilityTraits
  // returns its address so the wire format carries a pointer to it.
  vef_auth_cc_t cc{};

 private:
  template <typename Capability>
  friend struct ::vsql::detail::CapabilityTraits;

  const vef_preview_auth_t *abi_ = nullptr;
};

}  // namespace vsql::preview_auth

#include <villagesql/preview/detail/auth_register.h>

#endif  // VILLAGESQL_PREVIEW_AUTH_H
