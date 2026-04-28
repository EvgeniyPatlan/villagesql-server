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

#ifndef VILLAGESQL_VSQL_EVENT_TRACKING_H_
#define VILLAGESQL_VSQL_EVENT_TRACKING_H_

// Helpers for registering MySQL component event tracking consumers from a
// VillageSQL extension.
//
// Extensions that consume MySQL event tracking services (e.g.
// event_tracking_query) must:
//   1. Register their vtable with the component registry.
//   2. Notify the event tracking infrastructure so the reference caching
//      cache picks up the new consumer.
//
// These helpers encapsulate both steps. Use them in your on_load/on_unload
// callbacks. The vtable pointer may be const (SERVICE_TYPE(x) is always
// const); pass it directly without casting.
//
// Usage:
//
//   #include <villagesql/vsql/event_tracking.h>
//   #include "mysql/components/util/event_tracking/..."  // consumer helper
//
//   IMPLEMENTS_SERVICE_EVENT_TRACKING_QUERY(my_ext);
//
//   static bool on_load(char *error_msg) {
//     return villagesql::event_tracking::register_service(
//         "event_tracking_query.my_ext",
//         &imp_my_ext_event_tracking_query, error_msg);
//   }
//
//   static void on_unload() {
//     villagesql::event_tracking::unregister_service(
//         "event_tracking_query.my_ext");
//   }

#include <cstdio>
#include <cstring>

#include <villagesql/abi/types.h>
#include <villagesql/vsql/component_registry.h>

// MySQL component headers — required by callers of this header.
// Extensions including this file already depend on MySQL headers.
#include "mysql/components/my_service.h"
#include "mysql/components/services/dynamic_loader_service_notification.h"
#include "mysql/components/services/registry.h"

namespace villagesql {
namespace event_tracking {

// Register a service implementation with the component registry and notify
// the event tracking infrastructure. Call from on_load.
//
// service_name: fully-qualified service name, e.g.
//               "event_tracking_query.my_extension"
// vtable:       pointer to the service vtable (the variable produced by
//               IMPLEMENTS_SERVICE_EVENT_TRACKING_QUERY or similar macro).
//               May be const — no cast required at the call site.
// error_msg:    buffer of VEF_MAX_ERROR_LEN bytes for error messages.
//
// Returns true on failure (error_msg populated), false on success.
inline bool register_service(const char *service_name, const void *vtable,
                             char *error_msg) {
  void *raw = villagesql::component_registry::acquire();
  if (raw == nullptr) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN, "component registry unavailable");
    return true;
  }
  auto *reg = static_cast<SERVICE_TYPE(registry) *>(raw);

  my_service<SERVICE_TYPE(registry_registration)> reg_svc(
      "registry_registration", reg);
  if (!reg_svc.is_valid()) {
    villagesql::component_registry::release(raw);
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "registry_registration service unavailable");
    return true;
  }

  if (reg_svc->register_service(
          service_name,
          static_cast<my_h_service>(const_cast<void *>(vtable)))) {
    villagesql::component_registry::release(raw);
    snprintf(error_msg, VEF_MAX_ERROR_LEN, "failed to register service '%s'",
             service_name);
    return true;
  }

  my_service<SERVICE_TYPE(dynamic_loader_services_loaded_notification)>
      notify_svc("dynamic_loader_services_loaded_notification.mysql_server",
                 reg);
  if (notify_svc.is_valid()) {
    notify_svc->notify(&service_name, 1);
  }

  villagesql::component_registry::release(raw);
  return false;
}

// Unregister a service implementation and notify the event tracking
// infrastructure. Call from on_unload.
//
// service_name: the same name passed to register_service.
inline void unregister_service(const char *service_name) {
  void *raw = villagesql::component_registry::acquire();
  if (raw == nullptr) return;
  auto *reg = static_cast<SERVICE_TYPE(registry) *>(raw);

  my_service<SERVICE_TYPE(dynamic_loader_services_unload_notification)>
      notify_svc("dynamic_loader_services_unload_notification.mysql_server",
                 reg);
  if (notify_svc.is_valid()) {
    notify_svc->notify(&service_name, 1);
  }

  my_service<SERVICE_TYPE(registry_registration)> reg_svc(
      "registry_registration", reg);
  if (reg_svc.is_valid()) {
    reg_svc->unregister(service_name);
  }

  villagesql::component_registry::release(raw);
}

}  // namespace event_tracking
}  // namespace villagesql

#endif  // VILLAGESQL_VSQL_EVENT_TRACKING_H_
