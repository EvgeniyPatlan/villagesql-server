// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

#ifndef VILLAGESQL_VSQL_SERVICES_H_
#define VILLAGESQL_VSQL_SERVICES_H_

// Escape-hatch wrappers for directly consuming or providing MySQL component
// services from a VEF extension.
//
// These headers require MySQL component headers in the include path.
// Extensions that use these take on MySQL version compatibility for those APIs.
//
// Usage — consuming a service:
//
//   #include <villagesql/vsql/services.h>
//   #include "mysql/components/services/keyring_reader_with_status.h"
//
//   static vsql::RequiredService<SERVICE_TYPE(keyring_reader_with_status)> kr{
//       "keyring_reader_with_status"};
//
//   static int64_t my_vdf(int64_t x) {
//     if (!kr) return x;
//     // kr->fetch(...);
//     return x;
//   }
//
//   VEF_GENERATE_ENTRY_POINTS(
//       make_extension()
//           .esc_requires_service(kr)
//           .func(make_func("my_vdf", my_vdf)));
//
// Usage — providing a service:
//
//   #include <villagesql/vsql/services.h>
//   #include "mysql/components/util/event_tracking/
//            event_tracking_query_consumer_helper.h"
//
//   namespace Event_tracking_implementation { ... }
//   IMPLEMENTS_SERVICE_EVENT_TRACKING_QUERY(my_ext);
//   extern SERVICE_TYPE(event_tracking_query) imp_my_ext_event_tracking_query;
//
//   static vsql::ProvidedService<SERVICE_TYPE(event_tracking_query)> svc{
//       "event_tracking_query", "my_ext",
//       &imp_my_ext_event_tracking_query};
//
//   VEF_GENERATE_ENTRY_POINTS(
//       make_extension()
//           .esc_provides_service(svc));
//
#include <cstring>

#include "mysql/components/my_service.h"
#include "mysql/components/services/dynamic_loader_service_notification.h"
#include "mysql/components/services/registry.h"

#include <villagesql/vsql/component_registry.h>
#include <villagesql/vsql/services_base.h>

namespace vsql {

// RequiredService<SvcType> acquires a MySQL component service on extension
// load and releases it on unload. Use via operator-> for the extension's
// lifetime. Declare as a static and pass to .esc_requires_service().
template <typename SvcType>
class RequiredService : public ServiceBase {
 public:
  explicit RequiredService(const char *service_name)
      : service_name_(service_name) {}

  bool load(char *error_msg) override {
    void *raw = villagesql::component_registry::acquire();
    if (raw == nullptr) {
      snprintf(error_msg, VEF_MAX_ERROR_LEN, "component registry unavailable");
      return true;
    }
    registry_ = static_cast<SERVICE_TYPE(registry) *>(raw);
    svc_ = new my_service<SvcType>(service_name_, registry_);
    if (!svc_->is_valid()) {
      delete svc_;
      svc_ = nullptr;
      villagesql::component_registry::release(raw);
      registry_ = nullptr;
      snprintf(error_msg, VEF_MAX_ERROR_LEN, "service '%s' unavailable",
               service_name_);
      return true;
    }
    return false;
  }

  void unload() override {
    if (svc_ != nullptr) {
      delete svc_;
      svc_ = nullptr;
    }
    if (registry_ != nullptr) {
      villagesql::component_registry::release(
          const_cast<SERVICE_TYPE_NO_CONST(registry) *>(registry_));
      registry_ = nullptr;
    }
  }

  explicit operator bool() const { return svc_ != nullptr; }
  SvcType *operator->() const { return svc_->operator->(); }

 private:
  const char *service_name_;
  SERVICE_TYPE(registry) * registry_ { nullptr };
  my_service<SvcType> *svc_{nullptr};
};

// ProvidedService<SvcType> registers a service implementation with the MySQL
// component registry on load and unregisters on unload. Declare as a static
// and pass to .esc_provides_service().
template <typename SvcType>
class ProvidedService : public ServiceBase {
 public:
  ProvidedService(const char *service_type, const char *impl_name,
                  const SvcType *vtable)
      : vtable_(vtable) {
    size_t tlen = strlen(service_type);
    size_t nlen = strlen(impl_name);
    char *buf = new char[tlen + 1 + nlen + 1];
    memcpy(buf, service_type, tlen);
    buf[tlen] = '.';
    memcpy(buf + tlen + 1, impl_name, nlen);
    buf[tlen + 1 + nlen] = '\0';
    service_name_ = buf;
  }

  ~ProvidedService() override { delete[] service_name_; }

  bool load(char *error_msg) override {
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

    if (reg_svc->register_service(service_name_,
                                  static_cast<my_h_service>(const_cast<void *>(
                                      static_cast<const void *>(vtable_))))) {
      villagesql::component_registry::release(raw);
      snprintf(error_msg, VEF_MAX_ERROR_LEN, "failed to register service '%s'",
               service_name_);
      return true;
    }

    my_service<SERVICE_TYPE(dynamic_loader_services_loaded_notification)>
        notify_svc("dynamic_loader_services_loaded_notification.mysql_server",
                   reg);
    if (notify_svc.is_valid()) {
      const char *name = service_name_;
      notify_svc->notify(&name, 1);
    }

    villagesql::component_registry::release(raw);
    return false;
  }

  void unload() override {
    void *raw = villagesql::component_registry::acquire();
    if (raw == nullptr) return;
    auto *reg = static_cast<SERVICE_TYPE(registry) *>(raw);

    my_service<SERVICE_TYPE(dynamic_loader_services_unload_notification)>
        notify_svc("dynamic_loader_services_unload_notification.mysql_server",
                   reg);
    if (notify_svc.is_valid()) {
      const char *name = service_name_;
      notify_svc->notify(&name, 1);
    }

    my_service<SERVICE_TYPE(registry_registration)> reg_svc(
        "registry_registration", reg);
    if (reg_svc.is_valid()) {
      reg_svc->unregister(service_name_);
    }

    villagesql::component_registry::release(raw);
  }

 private:
  const SvcType *vtable_;
  char *service_name_;
};

}  // namespace vsql

#endif  // VILLAGESQL_VSQL_SERVICES_H_
