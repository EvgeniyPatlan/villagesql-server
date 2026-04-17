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

// ALTER EXTENSION ... UPDATE TO 'x.y.z' phase 2: type/VDF compatibility
// checks, capability hooks, and the victionary swap. The prologue (DDL
// guards, locks, preconditions, VEB load) is in sql_extension.cc.

#include "villagesql/veb/sql_extension.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "villagesql/include/error.h"
#include "villagesql/include/storage_hooks.h"
#include "villagesql/schema/descriptor/type_descriptor.h"
#include "villagesql/schema/schema_manager.h"
#include "villagesql/schema/victionary_client.h"
#include "villagesql/services/capability_registry.h"
#include "villagesql/veb/sql_extension_register.h"
#include "villagesql/veb/veb_file.h"

namespace {

// Check that no columns or SP params use types that are being dropped by an
// UPDATE (i.e. types present in old_entry's version but absent from new_reg).
// Types that survive into new_reg are fine — their columns remain valid.
// Returns true (with thd error set) if any dropped type has dependents.
bool check_dropped_types_have_no_dependents(
    const villagesql::ExtensionEntry &old_entry,
    const villagesql::VictionaryClient &victionary,
    const villagesql::veb::ExtensionRegistration &new_reg) {
  const vef_registration_t *reg = new_reg.registration;

  // Build set of type names present in the new registration.
  std::unordered_set<std::string> new_type_names;
  if (reg != nullptr) {
    for (unsigned int i = 0; i < reg->type_count; i++) {
      if (reg->types[i] && reg->types[i]->name)
        new_type_names.insert(reg->types[i]->name);
    }
  }

  const std::string &ext_name = old_entry.extension_name();
  const std::string &ext_ver = old_entry.extension_version;

  // Check columns — only fail if the column's type is being dropped.
  const auto &all_columns = victionary.columns().get_all_committed();
  const villagesql::ColumnEntry *first_col = nullptr;
  int col_count = 0;
  for (const auto *col : all_columns) {
    if (col->extension_name == ext_name && col->extension_version == ext_ver &&
        new_type_names.find(col->type_name) == new_type_names.end()) {
      if (col_count == 0) first_col = col;
      col_count++;
    }
  }
  if (first_col != nullptr) {
    villagesql_error(
        "Cannot update extension '%s': type '%s' is being dropped but "
        "%d column(s) depend on it, e.g. %s.%s.%s",
        MYF(0), ext_name.c_str(), first_col->type_name.c_str(), col_count,
        first_col->db_name().c_str(), first_col->table_name().c_str(),
        first_col->column_name().c_str());
    return true;
  }

  // Check SP params — same logic.
  const auto &all_sp_params = victionary.sp_params().get_all_committed();
  const villagesql::SpParamEntry *first_sp = nullptr;
  int sp_count = 0;
  for (const auto *entry : all_sp_params) {
    if (entry->extension_name == ext_name &&
        entry->extension_version == ext_ver &&
        new_type_names.find(entry->type_name) == new_type_names.end()) {
      if (sp_count == 0) first_sp = entry;
      sp_count++;
    }
  }
  if (first_sp != nullptr) {
    villagesql_error(
        "Cannot update extension '%s': type '%s' is being dropped but "
        "stored procedure %s.%s depends on it",
        MYF(0), ext_name.c_str(), first_sp->type_name.c_str(),
        first_sp->db_name().c_str(), first_sp->sp_name().c_str());
    return true;
  }

  return false;
}

// Rewrite column and SP param entries that belong to the old extension version
// so they point at the new version. Called under the victionary write lock
// during UPDATE, after mark_extension_for_deletion has marked the old
// type/VDF/extension entries for deletion and before
// mark_extension_for_insertion adds the new ones.
//
// Without this, the column entries in villagesql.custom_columns retain the old
// version string. UNINSTALL's column-dependency check matches on
// (extension_name, extension_version), so it would find no columns for the new
// version and incorrectly allow UNINSTALL while tables with custom-typed
// columns still exist.
bool rewrite_column_and_sp_param_versions(
    THD *thd, villagesql::VictionaryClient &victionary,
    const std::string &extension_name, const std::string &old_version,
    const std::string &new_version) {
  const auto &all_columns = victionary.columns().get_all_committed();
  for (const auto *col : all_columns) {
    if (col->extension_name != extension_name ||
        col->extension_version != old_version)
      continue;

    villagesql::ColumnEntry updated(col->key(), col->extension_name,
                                    new_version, col->type_name,
                                    col->type_parameters);
    if (victionary.columns().MarkForUpdate(*thd, std::move(updated),
                                           col->key())) {
      villagesql_error(
          "Failed to rewrite column entry for '%s.%s.%s' during UPDATE of "
          "extension '%s'",
          MYF(0), col->db_name().c_str(), col->table_name().c_str(),
          col->column_name().c_str(), extension_name.c_str());
      return true;
    }
  }

  const auto &all_sp_params = victionary.sp_params().get_all_committed();
  for (const auto *sp : all_sp_params) {
    if (sp->extension_name != extension_name ||
        sp->extension_version != old_version)
      continue;

    villagesql::SpParamEntry updated(sp->key(), sp->extension_name, new_version,
                                     sp->type_name, sp->type_parameters);
    if (victionary.sp_params().MarkForUpdate(*thd, std::move(updated),
                                             sp->key())) {
      villagesql_error(
          "Failed to rewrite SP param entry for '%s.%s.%s' during UPDATE of "
          "extension '%s'",
          MYF(0), sp->db_name().c_str(), sp->sp_name().c_str(),
          sp->param_name().c_str(), extension_name.c_str());
      return true;
    }
  }

  return false;
}

// Check whether a retained type (present in both old and new registrations)
// is compatible for an in-place UPDATE.
//
// Currently checks persisted_length only — a change would cause existing
// binary data on disk to be misinterpreted.
//
// TODO(villagesql-beta): evaluate expanding this check. Candidates:
//   - Encoding format versioning (detect semantic encode/decode changes)
//   - Max decode buffer length shrinkage (could truncate existing values)
//   - Compare function signature changes (could break index ordering)
bool is_retained_type_compatible(const std::string &extension_name,
                                 const std::string &type_name,
                                 const villagesql::TypeDescriptor &old_td,
                                 const vef_type_desc_t &new_td) {
  if (new_td.persisted_length != old_td.persisted_length()) {
    villagesql_error(
        "Cannot update extension '%s': type '%s' persisted_length changed "
        "from %lld to %lld -- existing stored data would be corrupted",
        MYF(0), extension_name.c_str(), type_name.c_str(),
        (long long)old_td.persisted_length(),
        (long long)new_td.persisted_length);
    return false;
  }
  return true;
}

// Check that the new extension version is storage-compatible with the old one.
// For every type present in both old and new registrations, verify it is
// compatible for in-place replacement (see is_retained_type_compatible).
// Types absent from the new registration are allowed to be dropped; the caller
// is responsible for ensuring no columns or SP params still use them.
// Returns false on success (compatible), true on error (thd error set).
//
// TODO(villagesql-beta): VDF dependents are not validated. Deterministic VDFs
// can be referenced from generated columns, functional indexes, CHECK
// constraints, and DEFAULT expressions, but villagesql does not track those
// references in any system table — VDFs resolve only at parse time. As a
// result, dropping a VDF or changing its signature/determinism across an
// UPDATE can silently invalidate persisted expressions. The same gap exists
// in UNINSTALL today (see remove_extension_from_victionary). Fix requires a
// new custom_vdf_uses-style system table populated at DDL time.
bool check_update_compatibility(
    const villagesql::ExtensionEntry &old_entry,
    const villagesql::VictionaryClient &victionary,
    const villagesql::veb::ExtensionRegistration &new_reg) {
  const vef_registration_t *reg = new_reg.registration;

  // Build a map of type_name -> descriptor from the new registration.
  std::unordered_map<std::string, const vef_type_desc_t *> new_types;
  for (unsigned int i = 0; i < reg->type_count; ++i) {
    const vef_type_desc_t *t = reg->types[i];
    if (t && t->name) new_types[t->name] = t;
  }

  // For every type the old version registered that is also present in the new
  // version, verify it is compatible for an in-place replacement. Types absent
  // from the new registration are allowed to be dropped; check_dropped_types_-
  // have_no_dependents() enforces that no columns or SP params still use them.
  const auto &all_type_descs =
      victionary.type_descriptors().get_all_committed();
  for (const auto *td : all_type_descs) {
    if (td->extension_name() != old_entry.extension_name() ||
        td->extension_version() != old_entry.extension_version) {
      continue;
    }

    const std::string &type_name = td->type_name();
    auto it = new_types.find(type_name);
    if (it == new_types.end()) continue;  // dropped — handled separately

    if (!is_retained_type_compatible(old_entry.extension_name(), type_name, *td,
                                     *it->second)) {
      return true;
    }
  }

  return false;
}

}  // namespace

namespace villagesql {

// Phase 2 of ALTER EXTENSION ... UPDATE TO. Called from
// Sql_cmd_install_extension::execute_update after the prologue (locks,
// preconditions, VEB load) has succeeded. Takes ownership of the loaded
// registration and hash; on any error path unloads the new .so before
// returning.
bool execute_upgrade(THD *thd, const std::string &extension_name,
                     const std::string &new_version,
                     veb::ExtensionRegistration &&new_registration,
                     std::string &&new_sha256_hash) {
  auto &victionary = VictionaryClient::instance();

  // Phase 1: pre-commit safety checks. Type-system checks run inline against
  // the victionary under a read lock; capability hooks run afterwards via the
  // generic registry. NOTE: read lock must be released before end_transaction
  // — end_transaction -> rollback_all_tables takes the write lock and
  // rwlocks are not reentrant.
  std::string old_version;
  const vef_registration_t *old_reg = nullptr;
  std::vector<villagesql::QualifiedTableName> affected_tables;
  {
    bool pre_check_error = false;
    {
      auto read_lock = victionary.get_read_lock();
      const auto *old_entry =
          victionary.extensions().get_committed(ExtensionKey(extension_name));
      if (old_entry == nullptr) {
        // Guard against a race between the early check and now.
        villagesql_error(
            "ALTER EXTENSION ... UPDATE TO failed: extension '%s' is not "
            "installed",
            MYF(0), extension_name.c_str());
        pre_check_error = true;
      } else {
        old_version = old_entry->extension_version;
        const auto *ext_desc = victionary.extension_descriptors().get_committed(
            ExtensionDescriptorKey(extension_name, old_version));
        if (ext_desc != nullptr) {
          old_reg = ext_desc->registration().registration;
        }
        // Check storage-layout compatibility for retained types.
        pre_check_error = check_update_compatibility(*old_entry, victionary,
                                                     new_registration);
        // Check that no dropped types have dependent columns or SP params.
        if (!pre_check_error)
          pre_check_error = check_dropped_types_have_no_dependents(
              *old_entry, victionary, new_registration);

        // Snapshot the set of user tables that reference this extension.
        // We acquire MDL_EXCLUSIVE and flush these tables below, before any
        // catalog mutation, to fence them from access for the duration of
        // the swap. Done here while we still hold the read lock so the snapshot
        // is consistent with the compat checks above. Note: column entries
        // currently say extension_version == old_version; the swap rewrites
        // them. Filtering by extension_name alone is the right key.
        if (!pre_check_error) {
          std::unordered_set<std::string> seen;
          const auto &all_columns = victionary.columns().get_all_committed();
          for (const auto *col : all_columns) {
            if (col->extension_name != extension_name) continue;
            std::string key = col->db_name() + "/" + col->table_name();
            if (!seen.insert(key).second) continue;
            affected_tables.emplace_back(col->db_name(), col->table_name());
          }
          // TODO(villagesql-preview): also walk villagesql.custom_indexes (and
          // custom_index_columns for profile_extension_name) and add their
          // tables here. A table with a regular column but an extension-
          // provided index type/profile has no entry in custom_columns, so it
          // is missed by the walk above. Its TABLE_SHARE caches
          // IndexContext::descriptor_ (raw ptr to IndexTypeDescriptor) and
          // KEY_PART_INFO::custom_index_profile (raw ptr to
          // IndexProfileDescriptor); both dangle after the swap dlclose's the
          // old .so, same hazard as TypeContext::descriptor_.
        }
      }
    }
    if (pre_check_error) {
      veb::unload_vef_extension(
          {.reason = services::UnloadReason::kUninstall, .thd = thd},
          new_registration);
      return end_transaction(thd, true);
    }
  }

  // Capability-driven Phase 1 hooks. Hooks set THD errors via villagesql_error
  // for rich diagnostics, in which case error_message is left empty; we only
  // forward error_message ourselves if it carries text.
  {
    std::string error_message;
    if (services::check_upgrade_compatibility(
            extension_name, old_version, new_version, old_reg,
            new_registration.registration, thd, error_message)) {
      if (!error_message.empty())
        villagesql_error("%s", MYF(0), error_message.c_str());
      veb::unload_vef_extension(
          {.reason = services::UnloadReason::kUninstall, .thd = thd},
          new_registration);
      return end_transaction(thd, true);
    }
  }

  // Fence the affected user tables for the duration of the swap. The pattern:
  //
  //  1. Acquire MDL_EXCLUSIVE on each affected table — blocks any other
  //     session from opening or accessing them.
  //  2. close_cached_tables(thd, list, true, LONG_TIMEOUT) — flush every
  //     TABLE_SHARE for these tables. Since we hold MDL_EXCLUSIVE the wait
  //     is uncontested. After this, no TABLE_SHARE pins a TypeContext for
  //     these tables; share->mem_root's shared_ptr<TypeContext> references
  //     have been released.
  //  3. Storage-engine hook (g_storage_invalidate_tables) — set
  //     dict_table_t::discard_after_ddl on each affected table so InnoDB
  //     reloads from the data dictionary on next open (releasing
  //     dict_col_t::custom_column's shared_ptr<TypeContext>).
  //
  // After this, the swap proceeds with no concurrent access to the affected
  // tables. The MDL_EXCLUSIVE is held for statement duration; on commit it
  // is released and other sessions can open the tables fresh against the
  // new extension version.
  std::vector<Table_ref> user_table_refs;
  if (!affected_tables.empty()) {
    user_table_refs.reserve(affected_tables.size());
    for (const auto &[db, name] : affected_tables) {
      user_table_refs.emplace_back(db.c_str(), db.size(), name.c_str(),
                                   name.size(), TL_WRITE, MDL_EXCLUSIVE);
    }
    for (size_t i = 0; i + 1 < user_table_refs.size(); i++) {
      user_table_refs[i].next_local = &user_table_refs[i + 1];
      user_table_refs[i].next_global = &user_table_refs[i + 1];
    }
    if (lock_table_names(thd, &user_table_refs[0], nullptr,
                         thd->variables.lock_wait_timeout, 0)) {
      // MDL acquisition failed (timeout, killed, etc.). thd error is set.
      veb::unload_vef_extension(
          {.reason = services::UnloadReason::kUninstall, .thd = thd},
          new_registration);
      return end_transaction(thd, true);
    }
    if (close_cached_tables(thd, &user_table_refs[0],
                            /*wait_for_refresh=*/true, LONG_TIMEOUT)) {
      villagesql_error(
          "ALTER EXTENSION ... UPDATE TO failed: could not flush table "
          "cache for extension '%s'",
          MYF(0), extension_name.c_str());
      veb::unload_vef_extension(
          {.reason = services::UnloadReason::kUninstall, .thd = thd},
          new_registration);
      return end_transaction(thd, true);
    }
    if (villagesql::g_storage_invalidate_tables != nullptr) {
      villagesql::g_storage_invalidate_tables(affected_tables);
    }
  }

  // Open all villagesql system tables that UPDATE may write to.
  // custom_columns and custom_sp_params are needed to rewrite the version
  // on dependent entries; extensions is needed for the extension row itself.
  Table_ref cols_table(SchemaManager::VILLAGESQL_SCHEMA_NAME,
                       SchemaManager::COLUMNS_TABLE_NAME, TL_WRITE,
                       MDL_SHARED_WRITE);
  Table_ref sp_params_table(SchemaManager::VILLAGESQL_SCHEMA_NAME,
                            SchemaManager::SP_PARAMS_TABLE_NAME, TL_WRITE,
                            MDL_SHARED_WRITE);
  Table_ref ext_table(SchemaManager::VILLAGESQL_SCHEMA_NAME,
                      SchemaManager::EXTENSIONS_TABLE_NAME, TL_WRITE,
                      MDL_SHARED_WRITE);
  cols_table.next_global = cols_table.next_local = &sp_params_table;
  sp_params_table.next_global = sp_params_table.next_local = &ext_table;
  ext_table.next_global = ext_table.next_local = nullptr;
  if (open_and_lock_tables(thd, &cols_table, MYSQL_LOCK_IGNORE_TIMEOUT)) {
    villagesql_error("Cannot open villagesql system tables for UPDATE", MYF(0));
    veb::unload_vef_extension(
        {.reason = services::UnloadReason::kUninstall, .thd = thd},
        new_registration);
    return end_transaction(thd, true);
  }

  // Mark the old extension's catalog entries for deletion. Skipping the
  // RESTRICT checks here is safe: check_dropped_types_have_no_dependents and
  // check_update_compatibility above already verified the deletion is safe,
  // and offline_mode + no-active-connections ensure no live references.
  std::optional<veb::ExtensionRegistration> old_registration;
  {
    auto write_lock = victionary.get_write_lock();
    const auto *old_entry =
        victionary.extensions().get_committed(ExtensionKey(extension_name));
    if (old_entry == nullptr) {
      villagesql_error(
          "ALTER EXTENSION ... UPDATE TO failed: extension '%s' is not "
          "installed",
          MYF(0), extension_name.c_str());
      veb::unload_vef_extension(
          {.reason = services::UnloadReason::kUninstall, .thd = thd},
          new_registration);
      return end_transaction(thd, true);
    }
    mark_extension_for_deletion(thd, victionary, *old_entry, old_registration);
  }

  // Phase 2: capability-driven atomic swap. Hooks may set THD errors via
  // villagesql_error; we only forward error_message ourselves when non-empty.
  //
  // TODO(villagesql-beta): the swap is NOT atomic across capabilities. If
  // execute_upgrade_swap iterates capabilities and a later capability's
  // on_swap_update fails, earlier capabilities have already swapped their
  // in-memory state and we have no rollback hook to undo them. From this
  // point on, on any failure path the in-memory capability state is
  // inconsistent with the catalog. Plan: set a global "extension upgrade
  // failed, restart required" flag and reject `SET GLOBAL offline_mode = OFF`
  // until the server has restarted (so capabilities reload from the catalog).
  // Same concern applies to all error paths below
  // (mark_extension_for_insertion, write_all_uncommitted_entries, post-commit
  // failure).
  {
    std::string error_message;
    if (services::execute_upgrade_swap(extension_name, old_version, new_version,
                                       old_reg, new_registration.registration,
                                       thd, error_message)) {
      if (!error_message.empty())
        villagesql_error("%s", MYF(0), error_message.c_str());
      veb::unload_vef_extension(
          {.reason = services::UnloadReason::kUninstall, .thd = thd},
          new_registration);
      return end_transaction(thd, true);
    }
  }

  bool mark_success = false;
  {
    auto write_lock = victionary.get_write_lock();
    // Rewrite column and SP param entries to point at the new version so that
    // UNINSTALL's column-dependency check remains effective after UPDATE.
    mark_success =
        !rewrite_column_and_sp_param_versions(thd, victionary, extension_name,
                                              old_version, new_version) &&
        !DBUG_EVALUATE_IF("villagesql_inject_update_mark_insert_fail", true,
                          false) &&
        !mark_extension_for_insertion(thd, victionary, extension_name,
                                      new_version, std::move(new_sha256_hash),
                                      std::move(new_registration));
  }

  if (!mark_success) {
    if (!thd->is_error())
      villagesql_error("Injected mark_extension_for_insertion failure for '%s'",
                       MYF(0), extension_name.c_str());
    return end_transaction(thd, true);
  }

  if (victionary.write_all_uncommitted_entries(thd) ||
      DBUG_EVALUATE_IF("villagesql_inject_update_write_fail", true, false)) {
    villagesql_error("Failed to write extension '%s' to table", MYF(0),
                     extension_name.c_str());
    return end_transaction(thd, true);
  }

  bool error = end_transaction(thd, false);

  // After commit: unload the old .so. The MDL_EXCLUSIVE acquired before the
  // swap is still held (statement duration), so no other session can have
  // opened the affected tables — no TABLE_SHARE or dict_table_t pins a
  // TypeContext into the old .so. The MDL is released when the statement
  // finishes, at which point new opens see the post-commit catalog state and
  // acquire fresh TypeContexts against the new extension version.
  if (!error && old_registration.has_value()) {
    veb::unload_vef_extension(
        {.reason = services::UnloadReason::kUninstall, .thd = thd},
        *old_registration);
  }

  // Only acknowledge success to the client after everything has succeeded —
  // commit and old-.so unload. If commit failed we've already set the thd
  // error; fall through to return true so the caller propagates the error.
  if (!error) {
    LogVSQL(INFORMATION_LEVEL, "Extension '%s' updated to version %s",
            extension_name.c_str(), new_version.c_str());
    my_ok(thd);
  }
  return error;
}

}  // namespace villagesql
