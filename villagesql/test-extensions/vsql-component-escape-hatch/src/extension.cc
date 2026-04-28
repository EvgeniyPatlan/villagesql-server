// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is designed to work with certain software (including
// but not limited to OpenSSL) that is licensed under separate terms,
// as designated in a particular file or component or in included license
// documentation.  The authors of MySQL hereby grant you an additional
// permission to link the program and your derivative works with the
// separately licensed software that they have either included with
// the program or referenced in the documentation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

// VillageSQL component escape hatch extension.
//
// Demonstrates three escape-hatch techniques for direct MySQL component service
// access from a VEF extension:
//
//   ProvidedService  — registers an event_tracking_query consumer with the
//                      component registry via .esc_provides_service(). The
//                      framework handles registration and notification
//                      automatically.
//
//   RequiredService  — acquires mysql_status_variable_string (and helpers) via
//                      .esc_requires_service(). Used to read the global
//                      "Queries" status variable and write it to the log on
//                      each intercepted query.
//
//   Raw on_load/on_unload — registers the "log_enabled" variable directly via
//                      component_sys_variable_register with a custom update
//                      function, using the same MySQL update function pointer
//                      mechanism VEF uses internally. This shows how on_load
//                      and on_unload give access to any component service
//                      not yet covered by a framework wrapper.
//
// Install:
//   INSTALL EXTENSION vsql_component_escape_hatch;
//
// Configure:
//   SET GLOBAL vsql_component_escape_hatch.log_enabled = ON;
//   SET GLOBAL vsql_component_escape_hatch.log_file =
//   '/var/log/vsql_component_escape_hatch.log';

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

// MySQL component headers — requires MySQL source tree in include path.
// Extensions using these take on MySQL version compatibility for these APIs.
#include "mysql/components/my_service.h"
#include "mysql/components/services/component_sys_var_service.h"
#include "mysql/components/services/mysql_current_thread_reader.h"
#include "mysql/components/services/mysql_status_variable_reader.h"
#include "mysql/components/services/mysql_string.h"
#include "mysql/components/services/registry.h"
#include "mysql/components/util/event_tracking/event_tracking_query_consumer_helper.h"

#include <villagesql/vsql.h>
#include <villagesql/vsql/component_registry.h>
#include <villagesql/vsql/services.h>

using namespace vsql;

// ---- RequiredService: read MySQL status variables --------------------------

static RequiredService<SERVICE_TYPE(mysql_current_thread_reader)>
    g_thread_reader{"mysql_current_thread_reader"};
static RequiredService<SERVICE_TYPE(mysql_status_variable_string)>
    g_status_reader{"mysql_status_variable_string"};
static RequiredService<SERVICE_TYPE(mysql_string_factory)> g_string_factory{
    "mysql_string_factory"};
static RequiredService<SERVICE_TYPE(mysql_string_converter)> g_string_converter{
    "mysql_string_converter"};

// Reads a global status variable into buf (null-terminated). Returns false on
// success.
static bool read_status_var(const char *name, char *buf, size_t buf_len) {
  if (!g_thread_reader || !g_status_reader || !g_string_factory ||
      !g_string_converter)
    return true;
  MYSQL_THD thd = nullptr;
  g_thread_reader->get(&thd);
  my_h_string str = nullptr;
  if (g_status_reader->get(thd, name, true, &str) || str == nullptr)
    return true;
  bool failed = g_string_converter->convert_to_buffer(str, buf,
                                                      buf_len - 1, "utf8mb4");
  g_string_factory->destroy(str);
  return failed;
}

// ---- VDF: query_count() ----------------------------------------------------

static void query_count(IntResult out) {
  char buf[32] = "0";
  read_status_var("Queries", buf, sizeof(buf));
  out.set(static_cast<int64_t>(strtoull(buf, nullptr, 10)));
}

// ---- Configuration ---------------------------------------------------------

static bool g_log_enabled = false;
static char *g_log_filename = nullptr;

// ---- Component query interception ------------------------------------------

static std::mutex g_log_mutex;

static void maybe_log(const mysql_event_tracking_query_data *data) {
  if (!g_log_enabled) return;

  // Only act on STATUS_END — that is when query_time is available.
  // The query_data struct does not carry timing; we use it only for the
  // query text and connection info. For a production implementation you
  // would layer in the general event (EVENT_TRACKING_GENERAL_STATUS) which
  // carries query_time_usec. Here we log all STATUS_END events that pass
  // the enabled check as a demonstration.
  if (!(data->event_subclass & (EVENT_TRACKING_QUERY_STATUS_END |
                                EVENT_TRACKING_QUERY_NESTED_STATUS_END)))
    return;

  const char *log_file = (g_log_filename != nullptr && *g_log_filename != '\0')
                             ? g_log_filename
                             : "/tmp/vsql_component_escape_hatch.log";

  time_t now = time(nullptr);
  struct tm tm_buf;
  gmtime_r(&now, &tm_buf);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

  std::lock_guard<std::mutex> lock(g_log_mutex);
  FILE *f = fopen(log_file, "a");
  if (f == nullptr) return;

  char queries_buf[64] = "?";
  read_status_var("Queries", queries_buf, sizeof(queries_buf));

  fprintf(f, "# Time: %s  Queries: %s\n", ts, queries_buf);
  fprintf(f, "# Id: %lu  Status: %d\n", data->connection_id, data->status);
  fprintf(f, "%.*s;\n", static_cast<int>(data->query.length),
          data->query.str ? data->query.str : "");
  fclose(f);
}

// ---- event_tracking_query service implementation ---------------------------

// The vtable implementing SERVICE_TYPE(event_tracking_query).
// Named following MySQL convention: imp_<component>_<service>.
//
// filtered_sub_events = 0 means we receive all subevents and filter in
// the callback. Set bits here to have the framework skip those subclasses
// before calling notify().
namespace Event_tracking_implementation {

mysql_event_tracking_query_subclass_t
    Event_tracking_query_implementation::filtered_sub_events = 0;

bool Event_tracking_query_implementation::callback(
    const mysql_event_tracking_query_data *data) {
  maybe_log(data);
  return false;
}

}  // namespace Event_tracking_implementation

IMPLEMENTS_SERVICE_EVENT_TRACKING_QUERY(vsql_component_escape_hatch);

extern SERVICE_TYPE(event_tracking_query)
    imp_vsql_component_escape_hatch_event_tracking_query;

static ProvidedService<SERVICE_TYPE(event_tracking_query)> g_query_svc{
    "event_tracking_query", "vsql_component_escape_hatch",
    &imp_vsql_component_escape_hatch_event_tracking_query};

// ---- on_change for the "log_enabled" variable ------------------------------
//
// "log_enabled" is registered via component_sys_variable_register in on_load
// rather than through the VEF .sys_var() builder. This uses MySQL's standard
// update function pointer — the same mechanism VEF uses internally — and
// shows how on_load/on_unload give raw access to any component service not
// yet covered by a framework wrapper.

static void on_enabled_update(MYSQL_THD, SYS_VAR *, void *val_ptr,
                              const void *save) {
  memcpy(val_ptr, save, sizeof(bool));

  if (g_log_enabled) {
    const char *log_file =
        (g_log_filename != nullptr && *g_log_filename != '\0')
            ? g_log_filename
            : "/tmp/vsql_component_escape_hatch.log";
    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE *f = fopen(log_file, "a");
    if (f != nullptr) {
      fprintf(f, "# component escape hatch enabled\n");
      fclose(f);
    }
  }
}

static bool on_load(char *error_msg) {
  void *raw = villagesql::component_registry::acquire();
  if (raw == nullptr) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN, "component registry unavailable");
    return true;
  }
  auto *reg = static_cast<SERVICE_TYPE(registry) *>(raw);

  my_service<SERVICE_TYPE(component_sys_variable_register)> reg_svc(
      "component_sys_variable_register", reg);
  if (!reg_svc.is_valid()) {
    villagesql::component_registry::release(raw);
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "component_sys_variable_register unavailable");
    return true;
  }

  BOOL_CHECK_ARG(bool) bool_arg;
  memset(&bool_arg, 0, sizeof(bool_arg));
  bool_arg.def_val = false;

  bool failed = reg_svc->register_variable(
      "vsql_component_escape_hatch", "log_enabled",
      PLUGIN_VAR_BOOL | PLUGIN_VAR_RQCMDARG,
      "Enable or disable the component escape hatch log", nullptr,
      on_enabled_update, &bool_arg, &g_log_enabled);

  villagesql::component_registry::release(raw);
  if (failed) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "failed to register 'log_enabled' variable");
    return true;
  }
  return false;
}

static void on_unload() {
  void *raw = villagesql::component_registry::acquire();
  if (raw == nullptr) return;
  auto *reg = static_cast<SERVICE_TYPE(registry) *>(raw);

  my_service<SERVICE_TYPE(component_sys_variable_unregister)> unreg_svc(
      "component_sys_variable_unregister", reg);
  if (unreg_svc.is_valid()) {
    unreg_svc->unregister_variable("vsql_component_escape_hatch",
                                   "log_enabled");
  }

  villagesql::component_registry::release(raw);
}

// ---- Extension registration ------------------------------------------------

VEF_GENERATE_ENTRY_POINTS(
    make_extension()
        .on_load<&on_load>()
        .on_unload<&on_unload>()
        .esc_provides_service(g_query_svc)
        .esc_requires_service(g_thread_reader)
        .esc_requires_service(g_status_reader)
        .esc_requires_service(g_string_factory)
        .esc_requires_service(g_string_converter)
        .func(make_func<&query_count>("query_count").returns(INT).build())
        .sys_var(make_sys_var_str("log_file",
                                  "Path to the component escape hatch file",
                                  &g_log_filename,
                                  "/tmp/vsql_component_escape_hatch.log")))
