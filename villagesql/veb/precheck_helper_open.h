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

#ifndef VILLAGESQL_VEB_PRECHECK_HELPER_OPEN_H_
#define VILLAGESQL_VEB_PRECHECK_HELPER_OPEN_H_

// Minimal-dependency dlopen + vef_register helper for the pre-check
// subprocess. Pulls in libdl and the VEF ABI headers only; no logging,
// no MySQL services, no LogVSQL. This is what lets the mysqld-vef-precheck
// helper binary stay small (no transitive mysqld dependencies).
//
// The full server-side open_vef_extension in veb_file.cc is a thin wrapper
// around this with LogVSQL calls layered on top -- both paths share the
// dlopen + symbol lookup + vef_register + protocol-validation logic.

#include <string>

#include "villagesql/sdk/include/villagesql/abi/types.h"

namespace villagesql {
namespace veb {

// Output of OpenVefExtensionMinimal. Caller is responsible for calling
// CloseVefExtensionMinimal on success.
struct MinimalExtensionRegistration {
  void *dlhandle{nullptr};
  vef_registration_t *registration{nullptr};
  vef_unregister_func_t unregister_func{nullptr};
  vef_protocol_t negotiated_protocol{};
};

// dlopen + dlsym(vef_register, vef_unregister) + call vef_register, then
// validate the protocol. Returns false on success with `out` populated.
// Returns true on failure with `error_message` set.
//
// On failure, `out` is in a defined-empty state and the .so has been
// dlclose'd; no further cleanup is required.
bool OpenVefExtensionMinimal(const std::string &so_path,
                             vef_protocol_t max_protocol,
                             MinimalExtensionRegistration &out,
                             std::string &error_message);

// vef_unregister + dlclose. Safe to call on a default-constructed
// MinimalExtensionRegistration (no-op).
void CloseVefExtensionMinimal(const MinimalExtensionRegistration &reg);

}  // namespace veb
}  // namespace villagesql

#endif  // VILLAGESQL_VEB_PRECHECK_HELPER_OPEN_H_
