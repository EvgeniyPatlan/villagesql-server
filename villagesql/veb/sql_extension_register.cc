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

#include "villagesql/veb/sql_extension_register.h"

#include <optional>
#include <string>
#include <utility>

#include "sql/sql_class.h"
#include "villagesql/include/error.h"
#include "villagesql/schema/descriptor/extension_descriptor.h"
#include "villagesql/schema/victionary_client.h"
#include "villagesql/veb/register.h"

namespace villagesql {

bool mark_extension_for_insertion(THD *thd, VictionaryClient &victionary,
                                  const std::string &extension_name,
                                  const std::string &version,
                                  std::string &&sha256_hash,
                                  veb::ExtensionRegistration &&registration) {
  std::string reg_error;
  std::optional<veb::ValidatedRegistration> validated =
      veb::parse_extension_registration(registration, extension_name, version,
                                        reg_error);
  if (!validated) {
    villagesql_error("Failed to validate extension '%s': %s", MYF(0),
                     extension_name.c_str(), reg_error.c_str());
    return true;
  }

  // Collision checks use the THD-aware lookup so the deletions queued by
  // mark_extension_for_deletion earlier in this statement are visible,
  // preventing false collisions on the about-to-be-deleted old version's
  // types.
  if (veb::register_validated_extension(*thd, std::move(*validated),
                                        reg_error)) {
    villagesql_error("Failed to register extension '%s': %s", MYF(0),
                     extension_name.c_str(), reg_error.c_str());
    return true;
  }

  if (victionary.extension_descriptors().MarkForInsertion(
          *thd,
          ExtensionDescriptor(ExtensionDescriptorKey(extension_name, version),
                              std::move(registration)))) {
    villagesql_error("Failed to register descriptor for extension '%s'", MYF(0),
                     extension_name.c_str());
    return true;
  }

  ExtensionEntry new_ext(ExtensionKey(extension_name), version,
                         std::move(sha256_hash));
  if (victionary.extensions().MarkForInsertion(*thd, std::move(new_ext))) {
    villagesql_error("Failed to register extension entry for '%s'", MYF(0),
                     extension_name.c_str());
    return true;
  }

  return false;
}

void mark_extension_for_deletion(
    THD *thd, VictionaryClient &victionary, const ExtensionEntry &ext_entry,
    std::optional<veb::ExtensionRegistration> &to_unregister) {
  const std::string &extension_name = ext_entry.extension_name();
  const std::string &extension_version = ext_entry.extension_version;

  // Delete TypeContexts for this extension (we do it before TypeDescriptors
  // since TypeContext holds a raw pointer to TypeDescriptor, but under the
  // lock, it doesn't really matter)
  const auto &all_type_contexts =
      victionary.type_contexts().get_all_committed();
  for (const auto *type_context : all_type_contexts) {
    if (type_context->extension_name() == extension_name &&
        type_context->extension_version() == extension_version) {
      victionary.type_contexts().MarkForDeletion(*thd, type_context->key());
    }
  }

  // Delete TypeDescriptors for this extension
  const auto &all_type_descs =
      victionary.type_descriptors().get_all_committed();
  for (const auto *type_desc : all_type_descs) {
    if (type_desc->extension_name() == extension_name &&
        type_desc->extension_version() == extension_version) {
      victionary.type_descriptors().MarkForDeletion(*thd, type_desc->key());
    }
  }

  // Delete IndexProfileDescriptors for this extension (before IndexType since
  // profiles reference index types, but ordering under the lock doesn't matter
  // in practice; deletion is transactional).
  const auto &all_index_profiles =
      victionary.index_profile_descriptors().get_all_committed();
  for (const auto *prof : all_index_profiles) {
    if (prof->extension_name() == extension_name &&
        prof->extension_version() == extension_version) {
      victionary.index_profile_descriptors().MarkForDeletion(*thd, prof->key());
    }
  }

  // Delete IndexTypeDescriptors for this extension
  const auto &all_index_types =
      victionary.index_type_descriptors().get_all_committed();
  for (const auto *index_type : all_index_types) {
    if (index_type->extension_name() == extension_name &&
        index_type->extension_version() == extension_version) {
      victionary.index_type_descriptors().MarkForDeletion(*thd,
                                                          index_type->key());
    }
  }

  // Delete VDFs for this extension
  const auto &all_funcs = victionary.funcs().get_all_committed();
  for (const auto *func : all_funcs) {
    if (func->extension_name() == extension_name &&
        func->extension_version() == extension_version) {
      victionary.funcs().MarkForDeletion(*thd, func->key());
    }
  }

  victionary.extensions().MarkForDeletion(*thd, ext_entry.key());
  const auto *ext_desc = victionary.extension_descriptors().get_committed(
      ExtensionDescriptorKey(extension_name, extension_version));
  if (ext_desc != nullptr) {
    to_unregister.emplace(ext_desc->registration());
    victionary.extension_descriptors().MarkForDeletion(*thd, ext_desc->key());
  }
}

}  // namespace villagesql
