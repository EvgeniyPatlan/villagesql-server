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

#include "villagesql/veb/precheck_helper_open.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "my_config.h"           // MYSQL_VERSION_*
#include "my_sharedlib.h"        // dlopen/dlsym/dlclose, DLERROR_GENERATE
#include "villagesql/include/version.h"  // VSQL_*_VERSION

namespace villagesql {
namespace veb {

namespace {

std::string format_dlerror() {
  const char *errmsg;
  int error_number = dlopen_errno;
  DLERROR_GENERATE(errmsg, error_number);
  char buf[256];
  std::snprintf(buf, sizeof(buf), "error %d (%s)", error_number,
                errmsg && errmsg[0] ? errmsg : "unknown");
  return buf;
}

template <typename T>
T lookup_symbol(void *handle, const char *symbol_name,
                std::string &error_message) {
  void *sym = dlsym(handle, symbol_name);
  if (sym == nullptr) {
    error_message =
        std::string(symbol_name) + " not found: " + format_dlerror();
    return nullptr;
  }
  return reinterpret_cast<T>(sym);
}

}  // namespace

bool OpenVefExtensionMinimal(const std::string &so_path,
                             vef_protocol_t max_protocol,
                             MinimalExtensionRegistration &out,
                             std::string &error_message) {
  out = MinimalExtensionRegistration{};

  // RTLD_LOCAL keeps each extension's symbols isolated. Without it,
  // macOS defaults to RTLD_GLOBAL, allowing the dynamic linker to
  // coalesce weak symbols across extensions.
  void *handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    error_message = "failed to load so: " + format_dlerror();
    return true;
  }

  auto vef_register = lookup_symbol<vef_register_func_t>(
      handle, VEF_REGISTER_FUNC_NAME, error_message);
  if (vef_register == nullptr) {
    dlclose(handle);
    return true;
  }

  auto vef_unregister = lookup_symbol<vef_unregister_func_t>(
      handle, VEF_UNREGISTER_FUNC_NAME, error_message);
  if (vef_unregister == nullptr) {
    dlclose(handle);
    return true;
  }

  vef_register_arg_t register_arg = {
      max_protocol,
      {MYSQL_VERSION_MAJOR, MYSQL_VERSION_MINOR, MYSQL_VERSION_PATCH, nullptr},
      {VSQL_MAJOR_VERSION, VSQL_MINOR_VERSION, VSQL_PATCH_VERSION, nullptr}};

  vef_registration_t *reg = vef_register(&register_arg);
  if (reg == nullptr) {
    error_message = "vef_register returned NULL";
    dlclose(handle);
    return true;
  }

  const vef_protocol_t negotiated_protocol =
      std::min(max_protocol, reg->protocol);

  if (reg->error_msg != nullptr) {
    error_message =
        std::string("vef_register returned an error: ") + reg->error_msg;
    vef_unregister_arg_t unregister_arg = {negotiated_protocol};
    vef_unregister(&unregister_arg, reg);
    dlclose(handle);
    return true;
  }

  // Reject extensions compiled against an old unstable protocol version.
  // Even-numbered protocol versions are unstable; only the current one
  // (max_protocol) is accepted. Odd versions are stable and always accepted.
  if (reg->protocol % 2 == 0 && reg->protocol != max_protocol) {
    error_message = "extension uses obsolete unstable protocol version " +
                    std::to_string(reg->protocol) +
                    " (current: " + std::to_string(max_protocol) + ")";
    vef_unregister_arg_t unregister_arg = {negotiated_protocol};
    vef_unregister(&unregister_arg, reg);
    dlclose(handle);
    return true;
  }

  out.dlhandle = handle;
  out.registration = reg;
  out.unregister_func = vef_unregister;
  out.negotiated_protocol = negotiated_protocol;
  return false;
}

void CloseVefExtensionMinimal(const MinimalExtensionRegistration &reg) {
  if (reg.dlhandle == nullptr) return;
  if (reg.registration != nullptr && reg.unregister_func != nullptr) {
    vef_unregister_arg_t unregister_arg = {reg.negotiated_protocol};
    reg.unregister_func(&unregister_arg, reg.registration);
  }
  dlclose(reg.dlhandle);
}

}  // namespace veb
}  // namespace villagesql
