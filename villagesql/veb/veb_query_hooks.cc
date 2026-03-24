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

#include "villagesql/veb/veb_query_hooks.h"

#include <mutex>
#include <string>
#include <vector>

#include "my_sys.h"
#include "mysql/components/my_service.h"
#include "mysql/components/services/component_sys_var_service.h"
#include "mysql/service_plugin_registry.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/sql_class.h"
#include "villagesql/include/error.h"
#include "villagesql/sdk/include/villagesql/abi/types.h"

namespace villagesql {
namespace veb {

namespace {

// A registered query hook together with the extension it belongs to.
struct RegisteredQueryHook {
  std::string extension_name;
  vef_query_hook_desc_t *desc;
  vef_context_t ctx;
};

// A registered config variable together with the extension it belongs to, so
// we can unregister it on extension uninstall.
struct RegisteredConfigVar {
  std::string extension_name;
  std::string var_name;
};

// Global lists — protected by their own mutex so that hook invocation does not
// need to hold the victionary lock.
std::mutex g_hooks_mutex;
std::vector<RegisteredQueryHook> g_hooks;

std::mutex g_config_vars_mutex;
std::vector<RegisteredConfigVar> g_config_vars;

}  // namespace

bool register_query_hooks_from_extension(const std::string &extension_name,
                                         const ExtensionRegistration &ext_reg) {
  const vef_registration_t *reg = ext_reg.registration;
  if (reg == nullptr || ext_reg.negotiated_protocol < VEF_PROTOCOL_3 ||
      reg->query_hook_count == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(g_hooks_mutex);
  for (unsigned int i = 0; i < reg->query_hook_count; i++) {
    vef_query_hook_desc_t *desc = reg->query_hooks[i];
    if (desc == nullptr || desc->hook == nullptr) {
      LogVSQL(ERROR_LEVEL,
              "Extension '%s' has NULL query hook descriptor at index %u",
              extension_name.c_str(), i);
      return true;
    }
    vef_context_t ctx{ext_reg.negotiated_protocol};
    g_hooks.push_back({extension_name, desc, ctx});
    LogVSQL(INFORMATION_LEVEL,
            "Registered query hook phase=%d from extension '%s'",
            static_cast<int>(desc->phase), extension_name.c_str());
  }
  return false;
}

void unregister_query_hooks_from_extension(const std::string &extension_name) {
  std::lock_guard<std::mutex> lock(g_hooks_mutex);
  auto it = std::remove_if(g_hooks.begin(), g_hooks.end(),
                           [&](const RegisteredQueryHook &h) {
                             return h.extension_name == extension_name;
                           });
  g_hooks.erase(it, g_hooks.end());
}

void invoke_post_execute_query_hooks(THD *thd) {
  if (thd == nullptr) return;

  // Snapshot the hook list under the lock, then invoke outside the lock so
  // that a hook cannot deadlock by uninstalling an extension.
  std::vector<RegisteredQueryHook> hooks_snapshot;
  {
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    for (const auto &h : g_hooks) {
      if (h.desc->phase == VEF_QUERY_HOOK_POSTEXECUTE) {
        hooks_snapshot.push_back(h);
      }
    }
  }

  if (hooks_snapshot.empty()) return;

  vef_query_hook_args_t args{};
  args.phase = VEF_QUERY_HOOK_POSTEXECUTE;

  // Query string
  LEX_CSTRING query = thd->query();
  args.query = query.str;
  args.query_len = query.length;

  // User / connection info
  const Security_context *sctx = thd->security_context();
  args.user = sctx->priv_user().str;
  args.host = sctx->ip().str;
  args.connection_id = thd->thread_id();

  // Execution status
  args.status = thd->is_error() ? thd->get_stmt_da()->mysql_errno() : 0;

  // Timing: compute from thd->start_utime (same as slow query log does)
  if (thd->start_utime != 0) {
    const ulonglong elapsed_us = my_micro_time() - thd->start_utime;
    args.query_time_secs = static_cast<double>(elapsed_us) / 1000000.0;
  }
  args.lock_time_secs = static_cast<double>(thd->get_lock_usec()) / 1000000.0;
  args.rows_sent = thd->get_sent_row_count();
  args.rows_examined = thd->get_examined_row_count();
  if (thd->copy_status_var_ptr != nullptr) {
    args.bytes_sent =
        thd->status_var.bytes_sent - thd->copy_status_var_ptr->bytes_sent;
    args.bytes_received = thd->status_var.bytes_received -
                          thd->copy_status_var_ptr->bytes_received;
  }

  // Current schema (may be empty if no database is selected)
  args.schema =
      thd->db().str != nullptr && thd->db().length > 0 ? thd->db().str
                                                        : nullptr;

  // Invoke each post-execute hook. Result fields are ignored for this phase.
  for (auto &h : hooks_snapshot) {
    vef_query_hook_result_t result{};
    h.desc->hook(&h.ctx, &args, &result);
  }
}

bool register_config_vars_from_extension(const std::string &extension_name,
                                         const ExtensionRegistration &ext_reg) {
  const vef_registration_t *reg = ext_reg.registration;
  if (reg == nullptr || ext_reg.negotiated_protocol < VEF_PROTOCOL_3 ||
      reg->config_var_count == 0) {
    return false;
  }

  SERVICE_TYPE(registry) *registry = mysql_plugin_registry_acquire();
  if (registry == nullptr) {
    LogVSQL(ERROR_LEVEL,
            "register_config_vars_from_extension: failed to acquire registry");
    return true;
  }

  my_service<SERVICE_TYPE(component_sys_variable_register)> reg_svc(
      "component_sys_variable_register", registry);
  if (!reg_svc.is_valid()) {
    LogVSQL(ERROR_LEVEL,
            "register_config_vars_from_extension: "
            "component_sys_variable_register service unavailable");
    mysql_plugin_registry_release(registry);
    return true;
  }

  bool error = false;
  for (unsigned int i = 0; i < reg->config_var_count; i++) {
    vef_config_var_desc_t *v = reg->config_vars[i];
    if (v == nullptr || v->name == nullptr) {
      LogVSQL(ERROR_LEVEL,
              "Extension '%s' has NULL config var descriptor at index %u",
              extension_name.c_str(), i);
      error = true;
      break;
    }

    int flags = PLUGIN_VAR_RQCMDARG;
    void *check_arg = nullptr;

    // Build the type-specific check_arg struct on the stack. The service
    // copies the values it needs before register_variable returns.
    // These macros define a struct type (e.g. INTEGRAL_CHECK_ARG(longlong)
    // defines struct longlong_check_arg_s), then we declare a variable of it.
    BOOL_CHECK_ARG(bool) bool_arg;
    INTEGRAL_CHECK_ARG(longlong) int_arg;
    INTEGRAL_CHECK_ARG(double) dbl_arg;
    STR_CHECK_ARG(str) str_arg;
    memset(&bool_arg, 0, sizeof(bool_arg));
    memset(&int_arg, 0, sizeof(int_arg));
    memset(&dbl_arg, 0, sizeof(dbl_arg));
    memset(&str_arg, 0, sizeof(str_arg));

    switch (v->type) {
      case VEF_VAR_BOOL:
        flags |= PLUGIN_VAR_BOOL;
        bool_arg.def_val = v->boolean.def_val;
        check_arg = &bool_arg;
        break;
      case VEF_VAR_INT:
        flags |= PLUGIN_VAR_LONGLONG;
        int_arg.def_val = static_cast<longlong>(v->integer.def_val);
        int_arg.min_val = static_cast<longlong>(v->integer.min_val);
        int_arg.max_val = static_cast<longlong>(v->integer.max_val);
        int_arg.blk_sz = 0;
        check_arg = &int_arg;
        break;
      case VEF_VAR_DOUBLE:
        // TODO(villagesql-beta): component_sys_variable_register does not
        // support PLUGIN_VAR_DOUBLE; register_variable will fail with
        // "Unknown variable type code 0x8". Until MySQL adds support,
        // extensions should use VEF_VAR_INT (milliseconds) instead.
        flags |= PLUGIN_VAR_DOUBLE;
        dbl_arg.def_val = v->dbl.def_val;
        dbl_arg.min_val = v->dbl.min_val;
        dbl_arg.max_val = v->dbl.max_val;
        dbl_arg.blk_sz = 0;
        check_arg = &dbl_arg;
        break;
      case VEF_VAR_STR:
        // PLUGIN_VAR_MEMALLOC tells the server to copy the string on SET,
        // which is required for the variable to be writable at runtime.
        flags |= PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC;
        str_arg.def_val = const_cast<char *>(v->str.def_val);
        check_arg = &str_arg;
        break;
    }

    if (reg_svc->register_variable(extension_name.c_str(), v->name, flags,
                                   v->comment ? v->comment : "", nullptr,
                                   nullptr, check_arg, v->value_ptr)) {
      LogVSQL(ERROR_LEVEL,
              "Failed to register config var '%s' for extension '%s'", v->name,
              extension_name.c_str());
      error = true;
      break;
    }

    {
      std::lock_guard<std::mutex> lock(g_config_vars_mutex);
      g_config_vars.push_back({extension_name, std::string(v->name)});
    }
    LogVSQL(INFORMATION_LEVEL, "Registered config var '%s.%s'",
            extension_name.c_str(), v->name);
  }

  mysql_plugin_registry_release(registry);
  return error;
}

void unregister_config_vars_from_extension(const std::string &extension_name) {
  std::vector<std::string> var_names;
  {
    std::lock_guard<std::mutex> lock(g_config_vars_mutex);
    auto it = std::remove_if(g_config_vars.begin(), g_config_vars.end(),
                             [&](const RegisteredConfigVar &v) {
                               if (v.extension_name == extension_name) {
                                 var_names.push_back(v.var_name);
                                 return true;
                               }
                               return false;
                             });
    g_config_vars.erase(it, g_config_vars.end());
  }

  if (var_names.empty()) return;

  SERVICE_TYPE(registry) *registry = mysql_plugin_registry_acquire();
  if (registry == nullptr) return;

  my_service<SERVICE_TYPE(component_sys_variable_unregister)> unreg_svc(
      "component_sys_variable_unregister", registry);
  if (unreg_svc.is_valid()) {
    for (const auto &name : var_names) {
      unreg_svc->unregister_variable(extension_name.c_str(), name.c_str());
    }
  }

  mysql_plugin_registry_release(registry);
}

}  // namespace veb
}  // namespace villagesql
