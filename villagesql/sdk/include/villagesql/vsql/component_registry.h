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

#ifndef VILLAGESQL_VSQL_COMPONENT_REGISTRY_H_
#define VILLAGESQL_VSQL_COMPONENT_REGISTRY_H_

// Direct access to the MySQL component service registry.
//
// Extensions that need services not wrapped by the VEF SDK can acquire them
// directly. This requires including MySQL component headers, which means
// taking on MySQL version compatibility for those services.
//
// Usage:
//
//   #include <villagesql/vsql/component_registry.h>
//   #include <mysql/components/my_service.h>
//   #include <mysql/components/services/component_status_var_service.h>
//
//   auto *reg = villagesql::component_registry::acquire();
//   if (reg) {
//     my_service<SERVICE_TYPE(status_variable_registration)> svc(
//         "status_variable_registration", reg);
//     if (svc.is_valid()) { svc->register_variable(...); }
//     villagesql::component_registry::release(reg);
//   }
//
// The handle returned by acquire() is SERVICE_TYPE(registry)* cast to void*.
// Cast it back before passing to my_service<>:
//
//   auto *reg = static_cast<SERVICE_TYPE(registry) *>(
//       villagesql::component_registry::acquire());

#include <villagesql/abi/types.h>

namespace villagesql {
namespace component_registry {

// Set by vef_register_impl() from vef_register_arg_t. Do not call directly.
inline void *(*g_acquire)() = nullptr;
inline int (*g_release)(void *) = nullptr;

// Acquire a reference to the MySQL component registry.
// Returns nullptr if the registry is unavailable (old server or not set).
// The caller must call release() when done.
inline void *acquire() {
  if (g_acquire == nullptr) return nullptr;
  return g_acquire();
}

// Release a registry reference obtained from acquire().
inline void release(void *registry) {
  if (g_release != nullptr && registry != nullptr) g_release(registry);
}

}  // namespace component_registry
}  // namespace villagesql

#endif  // VILLAGESQL_VSQL_COMPONENT_REGISTRY_H_
