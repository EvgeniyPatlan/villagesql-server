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

#ifndef VILLAGESQL_PREVIEW_DETAIL_STORAGE_BUILDER_REGISTER_H
#define VILLAGESQL_PREVIEW_DETAIL_STORAGE_BUILDER_REGISTER_H

#include <villagesql/abi/preview/storage.h>
#include <villagesql/detail/abi_signature_literals.h>
#include <villagesql/detail/capability_traits.h>
#include <villagesql/preview/storage_builder.h>

namespace vsql::detail {

template <>
struct CapabilityTraits<::vsql::preview_storage_builder::StorageCapability> {
  static constexpr const char *kName = VEF_PREVIEW_STORAGE_NAME;
  static constexpr const char *kCppTypeName =
      "vsql::preview_storage_builder::StorageCapability";
  static constexpr const char *kVtableHash = VEF_PIN(
      VEF_PREVIEW_STORAGE_ABI_HASH_MAC, VEF_PREVIEW_STORAGE_ABI_HASH_LINUX_X86,
      VEF_PREVIEW_STORAGE_ABI_HASH_LINUX_ARM);

  static void *vtable_destination(
      ::vsql::preview_storage_builder::StorageCapability * /*p*/) noexcept {
    return static_cast<void *>(&::vsql::preview_storage::detail::g_abi);
  }
};

template <size_t N>
struct CapabilityTraits<
    ::vsql::preview_storage_builder::ColumnStoreCapability<N>> {
  static constexpr const char *kName = VEF_PREVIEW_COLUMN_STORE_NAME;
  static constexpr const char *kCppTypeName =
      "vsql::preview_storage_builder::ColumnStoreCapability";
  using CapabilityConfigType = vef_preview_column_store_ext_desc_t;
  // Empty placeholders until real per-target literals are recorded
  // (run the abi_pin_literals gunit test on each target to obtain
  // them).  See abi_signature_literals.h for empty-pin semantics.
  static constexpr const char *kVtableHash =
      VEF_PIN(VEF_PREVIEW_COLUMN_STORE_ABI_HASH_MAC,
              VEF_PREVIEW_COLUMN_STORE_ABI_HASH_LINUX_X86,
              VEF_PREVIEW_COLUMN_STORE_ABI_HASH_LINUX_ARM);
  static constexpr const char *kCapabilityConfigHash =
      VEF_PIN(VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_MAC,
              VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_LINUX_X86,
              VEF_PREVIEW_COLUMN_STORE_EXT_DESC_ABI_HASH_LINUX_ARM);

  static void *vtable_destination(
      ::vsql::preview_storage_builder::ColumnStoreCapability<N> *p) noexcept {
    return static_cast<void *>(&p->vtable_);
  }

  static const void *capability_config(
      ::vsql::preview_storage_builder::ColumnStoreCapability<N> *p) noexcept {
    return p->extension_desc();
  }
};

}  // namespace vsql::detail

#endif  // VILLAGESQL_PREVIEW_DETAIL_STORAGE_BUILDER_REGISTER_H
