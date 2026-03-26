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

#ifndef VILLAGESQL_SDK_EXTENSION_BUILDER_H
#define VILLAGESQL_SDK_EXTENSION_BUILDER_H

// =============================================================================
// Extension Builder - Registration via Fluent Builder API
// =============================================================================
//
// This file provides the ExtensionBuilder for registering functions and types.
// For the main extension authoring header with full documentation, see
// extension.h instead.
//

#include <cstddef>
#include <cstdio>
#include <string_view>
#include <tuple>
#include <utility>

#include <villagesql/func_builder.h>
#include <villagesql/query_hook_builder.h>
#include <villagesql/sdk_version.h>
#include <villagesql/storage_builder.h>
#include <villagesql/type_builder.h>

namespace villagesql {
namespace extension_builder {

using namespace func_builder;
using namespace query_hook_builder;
using namespace storage_builder;
using namespace type_builder;

// =============================================================================
// ExtensionBuilder
// =============================================================================
//
// Stores functions, types, query hooks, and config vars by value using tuples,
// allowing inline definition without separate variable declarations.

template <typename FuncTuple, typename TypeTuple, typename HookTuple,
          typename ConfigVarTuple>
struct ExtensionBuilder {
  std::string_view name_;
  std::string_view version_;
  FuncTuple funcs_;
  TypeTuple types_;
  HookTuple hooks_;
  ConfigVarTuple config_vars_;
  vef_protocol_t min_protocol_;
  vef_on_install_func_t on_install_{nullptr};
  vef_on_uninstall_func_t on_uninstall_{nullptr};

  // Optional callback invoked inside vef_register(), before returning the
  // registration struct. Use this to capture service pointers from
  // vef_register_arg_t (e.g. register_background_thread). The callback
  // receives the negotiated protocol and the full arg struct.
  // Signature: void fn(vef_protocol_t negotiated, vef_register_arg_t *arg)
  using on_register_func_t = void (*)(vef_protocol_t, vef_register_arg_t *);
  on_register_func_t on_register_{nullptr};

  // Add a function (returns new builder with function appended)
  template <typename F>
  constexpr auto func(F f) const {
    auto new_funcs = std::tuple_cat(funcs_, std::make_tuple(f));
    return ExtensionBuilder<decltype(new_funcs), TypeTuple, HookTuple,
                            ConfigVarTuple>{
        name_,        version_,      new_funcs,   types_,        hooks_,
        config_vars_, min_protocol_, on_install_, on_uninstall_, on_register_};
  }

  // Add a type (returns new builder with type appended).
  // If the type requires a higher protocol than min_protocol_, min_protocol_
  // is raised automatically.
  constexpr auto type(const TypeDescriptor &td) const {
    auto new_types = std::tuple_cat(types_, std::make_tuple(td));
    const auto &t = td.vef_desc;
    const vef_protocol_t new_min =
        t.protocol > min_protocol_ ? t.protocol : min_protocol_;
    return ExtensionBuilder<FuncTuple, decltype(new_types), HookTuple,
                            ConfigVarTuple>{
        name_,        version_, funcs_,      new_types,     hooks_,
        config_vars_, new_min,  on_install_, on_uninstall_, on_register_};
  }

  // Add a query hook. Automatically requires VEF_PROTOCOL_3.
  constexpr auto query_hook(const QueryHookDescriptor &hook) const {
    auto new_hooks = std::tuple_cat(hooks_, std::make_tuple(hook));
    const vef_protocol_t new_min =
        min_protocol_ < VEF_PROTOCOL_3 ? VEF_PROTOCOL_3 : min_protocol_;
    return ExtensionBuilder<FuncTuple, TypeTuple, decltype(new_hooks),
                            ConfigVarTuple>{
        name_,        version_, funcs_,      types_,        new_hooks,
        config_vars_, new_min,  on_install_, on_uninstall_, on_register_};
  }

  // Add a config variable. Automatically requires VEF_PROTOCOL_3.
  constexpr auto config_var(const ConfigVarDescriptor &cv) const {
    auto new_cvs = std::tuple_cat(config_vars_, std::make_tuple(cv));
    const vef_protocol_t new_min =
        min_protocol_ < VEF_PROTOCOL_3 ? VEF_PROTOCOL_3 : min_protocol_;
    return ExtensionBuilder<FuncTuple, TypeTuple, HookTuple, decltype(new_cvs)>{
        name_,   version_, funcs_,      types_,        hooks_,
        new_cvs, new_min,  on_install_, on_uninstall_, on_register_};
  }

  // Set the on_install callback. Automatically requires VEF_PROTOCOL_4.
  // Called after all hooks and config vars are registered.
  // Return false on success; return true and write to error_msg to abort.
  constexpr auto on_install(vef_on_install_func_t fn) const {
    const vef_protocol_t new_min =
        min_protocol_ < VEF_PROTOCOL_4 ? VEF_PROTOCOL_4 : min_protocol_;
    return ExtensionBuilder<FuncTuple, TypeTuple, HookTuple, ConfigVarTuple>{
        name_,        version_, funcs_, types_,        hooks_,
        config_vars_, new_min,  fn,     on_uninstall_, on_register_};
  }

  // Set the on_uninstall callback. Automatically requires VEF_PROTOCOL_4.
  // Called before hooks and config vars are unregistered.
  constexpr auto on_uninstall(vef_on_uninstall_func_t fn) const {
    const vef_protocol_t new_min =
        min_protocol_ < VEF_PROTOCOL_4 ? VEF_PROTOCOL_4 : min_protocol_;
    return ExtensionBuilder<FuncTuple, TypeTuple, HookTuple, ConfigVarTuple>{
        name_,        version_, funcs_,      types_, hooks_,
        config_vars_, new_min,  on_install_, fn,     on_register_};
  }

  // Set the on_register callback. Called inside vef_register() with the
  // negotiated protocol and the full vef_register_arg_t*. Use this to capture
  // Protocol 4 service pointers (e.g. register_background_thread).
  constexpr auto on_register(on_register_func_t fn) const {
    return ExtensionBuilder<FuncTuple, TypeTuple, HookTuple, ConfigVarTuple>{
        name_,        version_,      funcs_,      types_,        hooks_,
        config_vars_, min_protocol_, on_install_, on_uninstall_, fn};
  }

  // This is here only for testing, please don't depend on it.
  // Require a minimum VEF protocol version from the server.
  // If the server offers a lower protocol, registration will fail with an
  // error message explaining the version requirement.
  constexpr auto test_only_require_protocol(vef_protocol_t p) const {
    return ExtensionBuilder<FuncTuple, TypeTuple, HookTuple, ConfigVarTuple>{
        name_,        version_, funcs_,      types_,        hooks_,
        config_vars_, p,        on_install_, on_uninstall_, on_register_};
  }

  // Compile-time counts accessible via the type (no instance needed).
  static constexpr size_t kFuncCount = std::tuple_size_v<FuncTuple>;
  static constexpr size_t kTypeCount = std::tuple_size_v<TypeTuple>;
  static constexpr size_t kHookCount = std::tuple_size_v<HookTuple>;
  static constexpr size_t kConfigVarCount = std::tuple_size_v<ConfigVarTuple>;

  // Accessors
  constexpr std::string_view name() const { return name_; }
  constexpr std::string_view version() const { return version_; }
  constexpr size_t func_count() const { return kFuncCount; }
  constexpr size_t type_count() const { return kTypeCount; }
  constexpr size_t hook_count() const { return kHookCount; }
  constexpr size_t config_var_count() const { return kConfigVarCount; }
  constexpr vef_protocol_t min_protocol() const { return min_protocol_; }

  template <size_t I>
  constexpr const auto &func_at() const {
    return std::get<I>(funcs_);
  }

  template <size_t I>
  constexpr const auto &type_at() const {
    return std::get<I>(types_);
  }

  template <size_t I>
  constexpr const auto &hook_at() const {
    return std::get<I>(hooks_);
  }

  template <size_t I>
  constexpr const auto &config_var_at() const {
    return std::get<I>(config_vars_);
  }
};

// Entry point to create an extension builder
constexpr auto make_extension(std::string_view name, std::string_view version) {
  return ExtensionBuilder<std::tuple<>, std::tuple<>, std::tuple<>,
                          std::tuple<>>{
      name, version, {}, {}, {}, {}, VEF_PROTOCOL_1, nullptr, nullptr, nullptr};
}

}  // namespace extension_builder

namespace detail {

// Implementation helpers used by VEF_GENERATE_ENTRY_POINTS. Not part of the
// public API.

// Fills arr[I] with the materialized vef_func_desc_t* for each function.
template <typename Ext, size_t... Is>
void vef_fill_func_ptrs(vef_func_desc_t **arr, const Ext &e,
                        std::index_sequence<Is...>) {
  using villagesql::func_builder::materialize_func_desc;
  ((arr[Is] = materialize_func_desc<decltype(e.template func_at<Is>()), Is>(
        e.template func_at<Is>())),
   ...);
}

// Fills arr[I] with the vef_type_desc_t* for each type.
template <typename Ext, size_t... Is>
void vef_fill_type_ptrs(vef_type_desc_t **arr, const Ext &e,
                        std::index_sequence<Is...>) {
  ((arr[Is] =
        const_cast<vef_type_desc_t *>(&e.template type_at<Is>().vef_desc)),
   ...);
}

// Fills arr[I] with a pointer to the materialized vef_query_hook_desc_t for
// each hook.
template <typename Ext, size_t... Is>
void vef_fill_hook_ptrs(vef_query_hook_desc_t **arr, const Ext &e,
                        std::index_sequence<Is...>) {
  ((arr[Is] =
        const_cast<vef_query_hook_desc_t *>(&e.template hook_at<Is>().desc)),
   ...);
}

// Fills arr[I] with a pointer to the materialized vef_config_var_desc_t for
// each config var.
template <typename Ext, size_t... Is>
void vef_fill_config_var_ptrs(vef_config_var_desc_t **arr, const Ext &e,
                              std::index_sequence<Is...>) {
  ((arr[Is] = const_cast<vef_config_var_desc_t *>(
        &e.template config_var_at<Is>().desc)),
   ...);
}

// Calls params_init_fn() for each type that has one.
template <typename Ext, size_t... Is>
void vef_init_type_params(const Ext &e, std::index_sequence<Is...>) {
  ((e.template type_at<Is>().params_init_fn
        ? e.template type_at<Is>().params_init_fn()
        : void()),
   ...);
}

// Returns the name of the first VDF that requires a bound params cache but
// whose cache was not bound (i.e., .params<P, &parse_fn>() was omitted from
// the type builder). Must be called after vef_init_type_params().
template <typename Ext, size_t... Is>
const char *vef_check_params_cache(const Ext &e, std::index_sequence<Is...>) {
  const char *unbound = nullptr;
  auto check_one = [&unbound](const auto &func) {
    if (unbound) return;
    auto check_fn = func.check_params_cache_bound();
    if (check_fn && !check_fn()) unbound = func.name();
  };
  (check_one(e.template func_at<Is>()), ...);
  return unbound;
}

// Core registration logic called by VEF_GENERATE_ENTRY_POINTS.
// All Count parameters are explicit template parameters so that array sizes
// are compile-time constants without relying on VLAs.
template <typename Ext, size_t FuncCount, size_t TypeCount, size_t HookCount,
          size_t ConfigVarCount>
vef_registration_t *vef_register_impl(vef_registration_t &reg,
                                      bool &initialized,
                                      vef_register_arg_t *arg, const Ext &ext) {
  if (initialized) return &reg;

  if (arg->protocol < ext.min_protocol()) {
    static char error_buf[128];
    snprintf(error_buf, sizeof(error_buf),
             "requires VEF protocol %u, server offered %u",
             static_cast<unsigned>(ext.min_protocol()),
             static_cast<unsigned>(arg->protocol));
    reg.protocol = arg->protocol;
    reg.error_msg = error_buf;
    return &reg;
  }

  static vef_func_desc_t *func_ptrs[FuncCount > 0 ? FuncCount : 1];
  static vef_type_desc_t *type_ptrs[TypeCount > 0 ? TypeCount : 1];
  static vef_query_hook_desc_t *hook_ptrs[HookCount > 0 ? HookCount : 1];
  static vef_config_var_desc_t
      *cv_ptrs[ConfigVarCount > 0 ? ConfigVarCount : 1];

  if constexpr (FuncCount > 0) {
    vef_fill_func_ptrs(func_ptrs, ext, std::make_index_sequence<FuncCount>{});
  }
  if constexpr (TypeCount > 0) {
    vef_fill_type_ptrs(type_ptrs, ext, std::make_index_sequence<TypeCount>{});
    vef_init_type_params(ext, std::make_index_sequence<TypeCount>{});
  }
  if constexpr (HookCount > 0) {
    vef_fill_hook_ptrs(hook_ptrs, ext, std::make_index_sequence<HookCount>{});
  }
  if constexpr (ConfigVarCount > 0) {
    vef_fill_config_var_ptrs(cv_ptrs, ext,
                             std::make_index_sequence<ConfigVarCount>{});
  }

  if constexpr (FuncCount > 0) {
    const char *unbound_vdf =
        vef_check_params_cache(ext, std::make_index_sequence<FuncCount>{});
    if (unbound_vdf) {
      static char error_buf[256];
      snprintf(error_buf, sizeof(error_buf),
               "VDF '%s' uses a parameterized type cache but no "
               ".params<P, &parse_fn>() was registered for that params type; "
               "add .params<P, &parse_fn>() to the type builder",
               unbound_vdf);
      reg.protocol = arg->protocol;
      reg.error_msg = error_buf;
      return &reg;
    }
  }

  reg.protocol = VEF_PROTOCOL_4;
  reg.error_msg = nullptr;
  reg.extension_name = ext.name().data();
  reg.extension_version = ext.version().data();
  reg.sdk_version = kSdkVersion;
  reg.func_count = FuncCount;
  reg.funcs = FuncCount > 0 ? func_ptrs : nullptr;
  reg.type_count = TypeCount;
  reg.types = TypeCount > 0 ? type_ptrs : nullptr;
  reg.query_hook_count = HookCount;
  reg.query_hooks = HookCount > 0 ? hook_ptrs : nullptr;
  reg.config_var_count = ConfigVarCount;
  reg.config_vars = ConfigVarCount > 0 ? cv_ptrs : nullptr;
  reg.on_install = ext.on_install_;
  reg.on_uninstall = ext.on_uninstall_;

  // Fire the on_register callback so extensions can capture service pointers
  // from vef_register_arg_t (e.g. register_background_thread).
  const vef_protocol_t negotiated =
      std::min(arg->protocol, static_cast<vef_protocol_t>(VEF_PROTOCOL_4));
  if (ext.on_register_ != nullptr) {
    ext.on_register_(negotiated, arg);
  }

  initialized = true;
  return &reg;
}

}  // namespace detail
}  // namespace villagesql

// VEF_GENERATE_ENTRY_POINTS
//
// Generates the extern "C" vef_register and vef_unregister functions.
// Must be called in a .cc file, not a header (defines functions/variables).
// Delegates registration logic to villagesql::detail::vef_register_impl.

#define VEF_GENERATE_ENTRY_POINTS(ext)                                   \
  namespace {                                                            \
  vef_registration_t vef_reg_;                                           \
  bool vef_reg_initialized_ = false;                                     \
  }                                                                      \
                                                                         \
  extern "C" vef_registration_t *vef_register(vef_register_arg_t *arg) { \
    using namespace villagesql::extension_builder;                       \
    using ExtType = decltype(ext);                                       \
    static auto kExt = (ext);                                            \
    return villagesql::detail::vef_register_impl<                        \
        ExtType, ExtType::kFuncCount, ExtType::kTypeCount,               \
        ExtType::kHookCount, ExtType::kConfigVarCount>(                  \
        vef_reg_, vef_reg_initialized_, arg, kExt);                      \
  }                                                                      \
                                                                         \
  extern "C" void vef_unregister(vef_unregister_arg_t *arg,              \
                                 vef_registration_t *reg) {              \
    (void)arg;                                                           \
    (void)reg;                                                           \
  }

#endif  // VILLAGESQL_SDK_EXTENSION_BUILDER_H
