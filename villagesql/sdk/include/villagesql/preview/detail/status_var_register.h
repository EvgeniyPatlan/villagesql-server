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

#ifndef VILLAGESQL_PREVIEW_DETAIL_STATUS_VAR_REGISTER_H
#define VILLAGESQL_PREVIEW_DETAIL_STATUS_VAR_REGISTER_H

#include <villagesql/abi/preview/status_var.h>
#include <villagesql/detail/abi_signature_literals.h>
#include <villagesql/detail/capability_traits.h>
#include <villagesql/preview/status_var.h>

// vef_status_var_desc_t contains an anonymous union over
// `{integer_ptr, double_ptr}` and so cannot be Boost.PFR-decomposed
// by the structural-hash machinery.  Its manual AbiStructFields
// specialization lives in villagesql/services/abi_specializations.h
// on the server side; extensions don't compute hashes, so they don't
// need it here.

namespace vsql::detail {

template <size_t N>
struct CapabilityTraits<::vsql::preview_status_var::StatusVarCapability<N>> {
  static constexpr const char *kName = VEF_PREVIEW_STATUS_VAR_NAME;
  static constexpr const char *kCppTypeName =
      "vsql::preview_status_var::StatusVarCapability";
  using CapabilityConfigType = vef_status_var_descriptor_list_t;
  // Empty placeholders until real per-target literals are recorded
  // (run the abi_pin_literals gunit test on each target to obtain
  // them).  See abi_signature_literals.h for empty-pin semantics.
  static constexpr const char *kVtableHash =
      VEF_PIN(VEF_PREVIEW_STATUS_VAR_ABI_HASH_MAC,
              VEF_PREVIEW_STATUS_VAR_ABI_HASH_LINUX_X86,
              VEF_PREVIEW_STATUS_VAR_ABI_HASH_LINUX_ARM);
  static constexpr const char *kCapabilityConfigHash =
      VEF_PIN(VEF_STATUS_VAR_DESC_LIST_ABI_HASH_MAC,
              VEF_STATUS_VAR_DESC_LIST_ABI_HASH_LINUX_X86,
              VEF_STATUS_VAR_DESC_LIST_ABI_HASH_LINUX_ARM);

  static constexpr void *vtable_destination(
      ::vsql::preview_status_var::StatusVarCapability<N> *p) noexcept {
    return static_cast<void *>(&p->abi_);
  }

  // Returns a pointer to the descriptor list so the server's on_populate
  // callback can reach the variable descriptors. The server receives this
  // as capability_config in the populate context.
  static constexpr const void *capability_config(
      ::vsql::preview_status_var::StatusVarCapability<N> *p) noexcept {
    return static_cast<const void *>(&p->descriptor_list);
  }
};

}  // namespace vsql::detail

#endif  // VILLAGESQL_PREVIEW_DETAIL_STATUS_VAR_REGISTER_H
