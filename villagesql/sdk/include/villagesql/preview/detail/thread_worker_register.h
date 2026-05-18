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
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VILLAGESQL_PREVIEW_DETAIL_THREAD_WORKER_REGISTER_H
#define VILLAGESQL_PREVIEW_DETAIL_THREAD_WORKER_REGISTER_H

#include <villagesql/abi/preview/thread_worker.h>
#include <villagesql/detail/abi_signature_literals.h>
#include <villagesql/detail/capability_traits.h>
#include <villagesql/preview/thread_worker.h>

// vef_thread_handle_t is a forward-declared server-side handle that
// extensions only see by pointer.  Its registration as an opaque
// ABI handle (for the structural hash machinery) lives in
// villagesql/services/abi_specializations.h on the server side --
// extensions don't compute hashes, so they don't need it.

namespace vsql::detail {

template <auto WorkFn>
struct CapabilityTraits<
    ::vsql::preview_thread_worker::ThreadWorkerCapability<WorkFn>> {
  using Cap = ::vsql::preview_thread_worker::ThreadWorkerCapability<WorkFn>;
  static constexpr const char *kName = VEF_PREVIEW_THREAD_WORKER_NAME;
  static constexpr const char *kCppTypeName =
      "vsql::preview_thread_worker::ThreadWorkerCapability";
  using CapabilityConfigType = vef_thread_worker_descriptor_t;
  static constexpr const char *kVtableHash =
      VEF_PIN(VEF_PREVIEW_THREAD_WORKER_ABI_HASH_MAC,
              VEF_PREVIEW_THREAD_WORKER_ABI_HASH_LINUX_X86,
              VEF_PREVIEW_THREAD_WORKER_ABI_HASH_LINUX_ARM);
  static constexpr const char *kCapabilityConfigHash =
      VEF_PIN(VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_MAC,
              VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_LINUX_X86,
              VEF_THREAD_WORKER_DESCRIPTOR_ABI_HASH_LINUX_ARM);

  static constexpr void *vtable_destination(Cap *p) noexcept {
    return static_cast<void *>(&p->abi_);
  }

  static const void *capability_config(Cap * /*p*/) noexcept {
    return &Cap::descriptor;
  }
};

}  // namespace vsql::detail

#endif  // VILLAGESQL_PREVIEW_DETAIL_THREAD_WORKER_REGISTER_H
