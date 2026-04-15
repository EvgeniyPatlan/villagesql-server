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

#include "villagesql/services/query_hooks.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "villagesql/include/error.h"
#include "villagesql/sdk/include/villagesql/abi/types.h"

namespace villagesql {
namespace services {

namespace {

// A registered query hook together with the extension it belongs to.
struct RegisteredQueryHook {
  std::string extension_name;
  vef_query_hook_desc_t *desc;
  vef_context_t ctx;
};

using HookList = std::vector<RegisteredQueryHook>;

// Hook list uses RCU-style access: readers atomically load the shared_ptr and
// hold a reference for the duration of invocation — no lock needed. Writers
// copy the list, modify the copy, then atomically swap it in.
std::shared_ptr<HookList> g_hooks{std::make_shared<HookList>()};
std::mutex g_hooks_write_mutex;

}  // namespace

static void on_post_execute(THD *thd);

bool register_query_hooks_from_extension(
    const std::string &extension_name,
    const veb::ExtensionRegistration &ext_reg) {
  const vef_registration_t *reg = ext_reg.registration;
  if (reg == nullptr || ext_reg.negotiated_protocol < VEF_PROTOCOL_2 ||
      reg->query_hook_count == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(g_hooks_write_mutex);
  auto new_hooks = std::make_shared<HookList>(*std::atomic_load(&g_hooks));
  for (unsigned int i = 0; i < reg->query_hook_count; i++) {
    vef_query_hook_desc_t *desc = reg->query_hooks[i];
    if (desc == nullptr || desc->hook == nullptr) {
      LogVSQL(ERROR_LEVEL,
              "Extension '%s' has NULL query hook descriptor at index %u",
              extension_name.c_str(), i);
      return true;
    }
    vef_context_t ctx{ext_reg.negotiated_protocol};
    new_hooks->push_back({extension_name, desc, ctx});
    LogVSQL(INFORMATION_LEVEL,
            "Registered query hook phase=%d from extension '%s'",
            static_cast<int>(desc->phase), extension_name.c_str());
  }
  std::atomic_store(&g_hooks, new_hooks);
  return false;
}

void unregister_query_hooks_from_extension(const std::string &extension_name) {
  std::lock_guard<std::mutex> lock(g_hooks_write_mutex);
  auto new_hooks = std::make_shared<HookList>(*std::atomic_load(&g_hooks));
  auto it = std::remove_if(new_hooks->begin(), new_hooks->end(),
                           [&](const RegisteredQueryHook &h) {
                             return h.extension_name == extension_name;
                           });
  new_hooks->erase(it, new_hooks->end());
  std::atomic_store(&g_hooks, new_hooks);
}

// Dispatch PREPARSE hooks. Called before the parser runs; only query text,
// user, and connection info are available. Returns true if a hook blocked
// the query and set an error on the THD.
bool on_pre_parse(THD *thd) {
  if (thd == nullptr) return false;

  auto hooks = std::atomic_load(&g_hooks);
  if (hooks->empty()) return false;

  vef_query_hook_args_t args{};
  args.phase = VEF_QUERY_HOOK_PREPARSE;

  LEX_CSTRING query = thd->query();
  args.query = query.str;
  args.query_len = query.length;

  const Security_context *sctx = thd->security_context();
  args.user = sctx->priv_user().str;
  args.host = sctx->ip().str;
  args.connection_id = thd->thread_id();
  args.port = thd->peer_port;
  args.in_transaction = thd->in_active_multi_stmt_transaction();
  args.schema = thd->db().str != nullptr && thd->db().length > 0 ? thd->db().str
                                                                 : nullptr;

  for (auto &h : *hooks) {
    if (h.desc->phase != VEF_QUERY_HOOK_PREPARSE) continue;
    vef_query_hook_result_t result{};
    h.desc->hook(&h.ctx, &args, &result);
    if (result.error_msg != nullptr && result.error_msg[0] != '\0') {
      my_printf_error(ER_VILLAGESQL_GENERIC_ERROR, "%s", MYF(0),
                      result.error_msg);
      return true;
    }
  }
  return false;
}

// Dispatch POSTPARSE hooks. Called at QUERY_START, after parsing but before
// execution. Returns true if a hook blocked the query (error set on THD).
static bool on_post_parse(THD *thd) {
  if (thd == nullptr) return false;

  auto hooks = std::atomic_load(&g_hooks);
  if (hooks->empty()) return false;

  vef_query_hook_args_t args{};
  args.phase = VEF_QUERY_HOOK_POSTPARSE;

  LEX_CSTRING query = thd->query();
  args.query = query.str;
  args.query_len = query.length;

  const Security_context *sctx = thd->security_context();
  args.user = sctx->priv_user().str;
  args.host = sctx->ip().str;
  args.connection_id = thd->thread_id();
  args.port = thd->peer_port;
  args.in_transaction = thd->in_active_multi_stmt_transaction();
  args.sql_command = static_cast<int>(thd->lex->sql_command);
  args.schema = thd->db().str != nullptr && thd->db().length > 0 ? thd->db().str
                                                                 : nullptr;

  for (auto &h : *hooks) {
    if (h.desc->phase != VEF_QUERY_HOOK_POSTPARSE) continue;
    vef_query_hook_result_t result{};
    h.desc->hook(&h.ctx, &args, &result);
    if (result.error_msg != nullptr && result.error_msg[0] != '\0') {
      my_printf_error(ER_VILLAGESQL_GENERIC_ERROR, "%s", MYF(0),
                      result.error_msg);
      return true;
    }
  }
  return false;
}

bool on_query_event(THD *thd, mysql_event_tracking_query_subclass_t subclass) {
  if (subclass & EVENT_TRACKING_QUERY_START) {
    return on_post_parse(thd);
  }
  if (subclass & EVENT_TRACKING_QUERY_STATUS_END) {
    on_post_execute(thd);
  }
  return false;
}

void on_connection_event(THD *thd,
                         mysql_event_tracking_connection_subclass_t subclass,
                         int errcode) {
  if (thd == nullptr) return;

  vef_query_hook_phase_t phase;
  if (subclass & EVENT_TRACKING_CONNECTION_CONNECT) {
    phase = VEF_QUERY_HOOK_CONNECT;
  } else if (subclass & EVENT_TRACKING_CONNECTION_DISCONNECT) {
    phase = VEF_QUERY_HOOK_DISCONNECT;
  } else {
    return;
  }

  auto hooks = std::atomic_load(&g_hooks);
  if (hooks->empty()) return;

  vef_query_hook_args_t args{};
  args.phase = phase;

  const Security_context *sctx = thd->security_context();
  args.user = sctx->priv_user().str;
  args.host = sctx->ip().str;
  args.connection_id = thd->thread_id();
  args.port = thd->peer_port;
  args.schema = thd->db().str != nullptr && thd->db().length > 0 ? thd->db().str
                                                                 : nullptr;

  if (phase == VEF_QUERY_HOOK_CONNECT) {
    args.status =
        errcode != 0
            ? (thd->is_error() ? thd->get_stmt_da()->mysql_errno() : errcode)
            : 0;
  }

  for (auto &h : *hooks) {
    if (h.desc->phase != phase) continue;
    vef_query_hook_result_t result{};
    h.desc->hook(&h.ctx, &args, &result);
    if (result.error_msg != nullptr && result.error_msg[0] != '\0') {
      LogVSQL(WARNING_LEVEL, "Connection hook error in extension '%s': %s",
              h.extension_name.c_str(), result.error_msg);
    }
  }
}

static void on_post_execute(THD *thd) {
  if (thd == nullptr) return;

  // Atomically grab a reference to the current hook list. The shared_ptr keeps
  // the list alive for the duration of this invocation even if an extension is
  // uninstalled concurrently — no lock needed during iteration.
  auto hooks = std::atomic_load(&g_hooks);
  if (hooks->empty()) return;

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
  args.port = thd->peer_port;
  args.in_transaction = thd->in_active_multi_stmt_transaction();

  // SQL command type
  args.sql_command = static_cast<int>(thd->lex->sql_command);

  // Execution status
  if (thd->is_error()) {
    const Diagnostics_area *da = thd->get_stmt_da();
    args.status = static_cast<int>(da->mysql_errno());
    args.sqlstate = da->returned_sqlstate();
    args.error_message = da->message_text();
  }

  // Timing
  args.query_start_utime = thd->start_utime;
  if (thd->start_utime != 0) {
    const ulonglong elapsed_us = my_micro_time() - thd->start_utime;
    args.query_time_secs = static_cast<double>(elapsed_us) / 1000000.0;
  }
  args.lock_time_secs = static_cast<double>(thd->get_lock_usec()) / 1000000.0;
  args.rows_sent = thd->get_sent_row_count();
  args.rows_examined = thd->get_examined_row_count();
  args.rows_affected =
      thd->get_stmt_da()->is_ok() ? thd->get_stmt_da()->affected_rows() : 0;
  if (thd->copy_status_var_ptr != nullptr) {
    args.bytes_sent =
        thd->status_var.bytes_sent - thd->copy_status_var_ptr->bytes_sent;
    args.bytes_received = thd->status_var.bytes_received -
                          thd->copy_status_var_ptr->bytes_received;
  }

  // Current schema (may be empty if no database is selected)
  args.schema = thd->db().str != nullptr && thd->db().length > 0 ? thd->db().str
                                                                 : nullptr;

  for (auto &h : *hooks) {
    if (h.desc->phase != VEF_QUERY_HOOK_POSTEXECUTE) continue;
    vef_query_hook_result_t result{};
    h.desc->hook(&h.ctx, &args, &result);
    if (result.error_msg != nullptr && result.error_msg[0] != '\0') {
      LogVSQL(WARNING_LEVEL, "Query hook error in extension '%s': %s",
              h.extension_name.c_str(), result.error_msg);
    }
  }
}

}  // namespace services
}  // namespace villagesql
