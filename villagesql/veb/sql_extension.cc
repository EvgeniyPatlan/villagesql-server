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

#include "villagesql/veb/sql_extension.h"

#include <cctype>
#include <string>
#include <tuple>

#include "my_dbug.h"
#include "my_sys.h"
#include "mysql/components/services/registry.h"
#include "mysql/service_security_context.h"
#include "mysql/service_srv_session.h"
#include "mysql_com.h"
#include "mysqld_error.h"
#include "scope_guard.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/debug_sync.h"
#include "sql/iterators/row_iterator.h"
#include "sql/lock.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/protocol_callback.h"
#include "sql/sql_backup_lock.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#ifndef NDEBUG
#include "sql/item_func.h"
#endif
#include "sql/sql_executor.h"
#include "sql/sql_plugin.h"
#include "sql/sql_udf.h"
#include "sql/srv_session.h"
#include "sql/table.h"
#include "sql/thd_raii.h"
#include "sql_string.h"
#include "villagesql/include/error.h"
#include "villagesql/schema/descriptor/extension_descriptor.h"
#include "villagesql/schema/schema_manager.h"
#include "villagesql/schema/systable/extensions.h"
#include "villagesql/schema/victionary_client.h"
#include "villagesql/services/capability_registry.h"
#include "villagesql/sql/metadata_modifier.h"
#include "villagesql/veb/extension_uninstall_checks.h"
#include "villagesql/veb/register.h"
#include "villagesql/veb/validate.h"
#include "villagesql/veb/veb_file.h"

// Global variables for VEB directory configuration
char *opt_veb_dir_ptr;
char opt_veb_dir[FN_REFLEN];

namespace villagesql {
// Phase 2 of ALTER EXTENSION ... UPDATE TO, defined in
// sql_extension_update.cc. Called after the prologue (locks, preconditions,
// VEB load) has succeeded. Takes ownership of the loaded registration and
// hash; on any error path unloads the new .so before returning.
bool execute_upgrade(THD *thd, const std::string &extension_name,
                     const std::string &new_version,
                     veb::ExtensionRegistration &&new_registration,
                     std::string &&new_sha256_hash);
}  // namespace villagesql

namespace {
// Helpers below execute_install. Forward-declared so the install/update
// paths can call them without reordering this file.
bool validate_extension_name(THD *thd, const std::string &extension_name);
bool resolve_veb_version(THD *thd, const std::string &extension_name,
                         const LEX_CSTRING &m_version, bool require_explicit,
                         std::string &veb_version, std::string &version);
bool load_veb_and_so(THD *thd, const std::string &extension_name,
                     const std::string &veb_version,
                     villagesql::veb::ExtensionRegistration &registration,
                     std::string &sha256_hash,
                     villagesql::services::LoadReason load_reason =
                         villagesql::services::LoadReason::kInstall);
bool check_no_active_connections(THD *thd);
}  // namespace

// EXTENSION MDL locks (defined in sql/mdl.h):
// - An X (exclusive) lock is acquired when installing or uninstalling
//   an extension. This is the lock taken in the install/uninstall path
//   immediately after acquiring the global read lock and backup lock.
// - To protect against concurrent uninstall, DDL operations that add or
//   remove columns of extension-defined types acquire an S (shared) MDL
//   lock on the extension (see Metadata_modifier::lock_extensions_shared).
// - DDL acquires the S lock on extensions after acquiring the table and
//   other required object locks. This ensures the table is not being altered
//   while determining the set of extensions that must be locked.
// - This locking order is deadlock-safe, provided the uninstall command does
//   not itself execute any DDL on dependent objects.
// Shared prologue for INSTALL EXTENSION and ALTER EXTENSION ... UPDATE TO.
// Sets up the DDL guards (binlog, autocommit, dd cache releaser), validates
// the extension name, and acquires the three locks (global shared read,
// shared backup, exclusive extension MDL). RAII guards live in this frame so
// they remain active across the dispatch into execute_install / execute_update
// and are released when execute() returns.
bool Sql_cmd_install_extension::execute(THD *thd) {
  // We do not replicate INSTALL EXTENSION or ALTER EXTENSION ... UPDATE TO.
  const Disable_binlog_guard binlog_guard(thd);

  std::string extension_name(m_name.str, m_name.length);

  // INSTALL EXTENSION is DDL-like (modifies system tables), so we follow the
  // INSTALL PLUGIN pattern: disable autocommit to prevent premature commits
  // when data dictionary tables close (see CF_NEEDS_AUTOCOMMIT_OFF in
  // sql_parse.h).
  const Disable_autocommit_guard autocommit_guard(thd);
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  LogVSQL(INFORMATION_LEVEL, "%s extension: '%s'",
          m_update ? "Updating" : "Installing", extension_name.c_str());

  if (validate_extension_name(thd, extension_name))
    return end_transaction(thd, true);

  // Acquire global shared read lock to check and prevent the operation in
  // "read only mode". Acquire shared backup lock to synchronize with final
  // phase of backup operation.
  if (acquire_shared_global_read_lock(thd, thd->variables.lock_wait_timeout) ||
      acquire_shared_backup_lock(thd, thd->variables.lock_wait_timeout))
    return true;

  // Acquire X MDL lock with statement duration on the normalized extension
  // name to prevent concurrent install/uninstall/update operations on the
  // same extension.
  if (villagesql::Metadata_modifier::lock_extension_exclusive(
          thd, extension_name, MDL_STATEMENT)) {
    return true;
  }

  if (m_update) return execute_update(thd, extension_name);
  return execute_install(thd, extension_name);
}

bool Sql_cmd_install_extension::execute_update(
    THD *thd, const std::string &extension_name) {
  // Shared prologue (binlog guard, autocommit, releaser, name validation,
  // global read lock, backup lock, exclusive extension MDL) ran in execute().
  //
  // UPDATE always requires an explicit version.
  std::string veb_version;
  std::string version;
  if (resolve_veb_version(thd, extension_name, m_version,
                          /*require_explicit=*/true, veb_version, version))
    return end_transaction(thd, true);

  // Require offline_mode = ON. This blocks any *new* non-admin connection
  // from authenticating for the duration of the update, so no new session
  // can pick up a stale TypeContext or VDF reference once we start mutating
  // the victionary. It does not affect connections that are already open
  // (see check_no_active_connections below).
  if (!mysqld_offline_mode()) {
    villagesql_error(
        "Updating an extension requires the server to be in offline mode. "
        "Run SET GLOBAL offline_mode = ON before updating.",
        MYF(0));
    return end_transaction(thd, true);
  }

  // offline_mode only gates *new* connections; existing non-admin sessions
  // continue to run until they finish or are killed. Refuse to proceed
  // until those have drained, so no live session holds a stale TypeContext
  // or VDF reference.
  if (check_no_active_connections(thd)) return end_transaction(thd, true);

  auto &victionary = villagesql::VictionaryClient::instance();

  // Early check: fail fast if extension is not installed.
  // NOTE: We must release the read lock BEFORE calling end_transaction.
  {
    auto read_lock = victionary.get_read_lock();
    if (!victionary.extensions().get_committed(
            villagesql::ExtensionKey(extension_name))) {
      villagesql_error(
          "ALTER EXTENSION ... UPDATE TO failed: extension '%s' is not "
          "installed",
          MYF(0), extension_name.c_str());
    }
  }
  if (thd->is_error()) return end_transaction(thd, true);

  villagesql::veb::ExtensionRegistration registration;
  std::string sha256_hash;
  if (load_veb_and_so(thd, extension_name, veb_version, registration,
                      sha256_hash, villagesql::services::LoadReason::kUpdate))
    return end_transaction(thd, true);

  // Hand off to sql_extension_update.cc for the compatibility checks and
  // the actual victionary swap.
  return villagesql::execute_upgrade(thd, extension_name, version,
                                     std::move(registration),
                                     std::move(sha256_hash));
}

bool Sql_cmd_install_extension::execute_install(
    THD *thd, const std::string &extension_name) {
  auto &victionary = villagesql::VictionaryClient::instance();

  // Early check: fail fast if extension already exists (from in-memory cache).
  // This avoids touching VEB files for an extension that will be rejected.
  // We do a final authoritative check later under table lock to handle races.
  // NOTE: We must release the read lock BEFORE calling end_transaction, because
  // end_transaction -> trans_rollback -> rollback_all_tables needs write lock.
  {
    auto read_lock = victionary.get_read_lock();
    const auto *existing = victionary.extensions().get_committed(
        villagesql::ExtensionKey(extension_name));
    if (existing) {
      villagesql_error("Extension '%s' is already installed", MYF(0),
                       extension_name.c_str());
    }
  }
  if (thd->is_error()) {
    return end_transaction(thd, true);
  }

  std::string veb_version;
  std::string version;
  if (resolve_veb_version(thd, extension_name, m_version,
                          /*require_explicit=*/false, veb_version, version))
    return end_transaction(thd, true);

  villagesql::veb::ExtensionRegistration registration;
  std::string sha256_hash;
  if (load_veb_and_so(thd, extension_name, veb_version, registration,
                      sha256_hash))
    return end_transaction(thd, true);

  std::string reg_error;
  std::optional<villagesql::veb::ValidatedRegistration> validated =
      villagesql::veb::parse_extension_registration(
          registration, extension_name, version, reg_error);
  if (!validated) {
    villagesql_error("Failed to install extension '%s': %s", MYF(0),
                     extension_name.c_str(), reg_error.c_str());
    return end_transaction(thd, true);
  }

  std::optional<villagesql::veb::ValidatedPreviewCapabilities> preview =
      villagesql::veb::parse_preview_capabilities(registration, extension_name,
                                                  version, reg_error);
  if (!preview) {
    villagesql_error("Failed to install extension '%s': %s", MYF(0),
                     extension_name.c_str(), reg_error.c_str());
    return end_transaction(thd, true);
  }

  // TODO(villagesql): the parse + register + MarkForInsertion sequence below
  // duplicates mark_extension_for_insertion in sql_extension_register.cc.
  // Reuse the helper here once INSTALL is restructured to share the code path
  // with UPDATE (preview-capability registration would need to move alongside
  // or remain inline).
  bool mark_success = true;
  {
    auto write_lock = victionary.get_write_lock();

    if (villagesql::veb::register_preview_capabilities(
            *thd, std::move(*preview), *validated, reg_error) ||
        villagesql::veb::register_validated_extension(
            *thd, std::move(*validated), reg_error)) {
      villagesql_error("Failed to install extension '%s': %s", MYF(0),
                       extension_name.c_str(), reg_error.c_str());
      // Rollback should be done after releasing the victionary lock.
      mark_success = false;

    } else if (victionary.extension_descriptors().MarkForInsertion(
                   *thd, villagesql::ExtensionDescriptor(
                             villagesql::ExtensionDescriptorKey(extension_name,
                                                                version),
                             std::move(registration)))) {
      villagesql_error("Failed to register descriptor for extension '%s'",
                       MYF(0), extension_name.c_str());
      mark_success = false;
    }
  }

  if (!mark_success) {
    return end_transaction(thd, true);
  }

  // Open villagesql.extensions table for writing.
  Table_ref ext_table(villagesql::SchemaManager::VILLAGESQL_SCHEMA_NAME,
                      villagesql::SchemaManager::EXTENSIONS_TABLE_NAME,
                      TL_WRITE, MDL_SHARED_WRITE);
  if (open_and_lock_tables(thd, &ext_table, MYSQL_LOCK_IGNORE_TIMEOUT)) {
    villagesql_error("Cannot open extensions table", MYF(0));
    return end_transaction(thd, true);
  }

  // Check if extension already exists and mark for insertion while holding lock
  mark_success = false;
  {
    auto write_lock = victionary.get_write_lock();

    const auto *existing = victionary.extensions().get_committed(
        villagesql::ExtensionKey(extension_name));
    if (existing) {
      villagesql_error("Extension '%s' is already installed", MYF(0),
                       extension_name.c_str());
    } else {
      // Create extension entry and mark for insertion - version is used below
      // and needs to be copied as a result.
      villagesql::ExtensionEntry new_ext(
          villagesql::ExtensionKey(extension_name), version,
          std::move(sha256_hash));
      if (victionary.extensions().MarkForInsertion(*thd, std::move(new_ext))) {
        villagesql_error("Failed to register extension '%s'", MYF(0),
                         extension_name.c_str());
      } else {
        mark_success = true;
      }
    }
  }

  if (!mark_success) {
    return end_transaction(thd, true);
  }

  // Write to table
  if (victionary.write_all_uncommitted_entries(thd)) {
    villagesql_error("Failed to write extension '%s' to table", MYF(0),
                     extension_name.c_str());
    return end_transaction(thd, true);
  }

  LogVSQL(INFORMATION_LEVEL,
          "Extension '%s' (version %s) installed successfully",
          extension_name.c_str(), version.c_str());

  my_ok(thd);
  return end_transaction(thd, false);
}

namespace {

// Validate the extension name and set thd error on failure.
// Returns true on error, false on success.
bool validate_extension_name(THD *thd, const std::string &extension_name) {
  if (extension_name.empty()) {
    villagesql_error("Extension name cannot be empty", MYF(0));
    return true;
  }

  if (extension_name.length() > 64) {
    villagesql_error(
        "Extension name '%s' exceeds maximum length of 64 characters", MYF(0),
        extension_name.c_str());
    return true;
  }

  if (!std::isalpha(static_cast<unsigned char>(extension_name[0]))) {
    villagesql_error("Extension name '%s' must start with a letter", MYF(0),
                     extension_name.c_str());
    return true;
  }

  char last_char = extension_name[extension_name.length() - 1];
  if (!std::isalnum(static_cast<unsigned char>(last_char))) {
    villagesql_error("Extension name '%s' must end with a letter or digit",
                     MYF(0), extension_name.c_str());
    return true;
  }

  for (char c : extension_name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
      villagesql_error(
          "Extension name '%s' contains invalid character '%c' "
          "(only letters, digits, underscore, and hyphen allowed)",
          MYF(0), extension_name.c_str(), c);
      return true;
    }
  }

  (void)thd;
  return false;
}

// Resolve the VEB version sentinel and logical version string.
// veb_version: empty = unversioned ({name}.veb); non-empty = {name}-{ver}.veb.
// version: logical version from manifest (same as veb_version for versioned).
// When m_version is set but {name}-{version}.veb does not exist, falls back
// to {name}.veb and asserts the manifest version matches.
// When require_explicit is true and m_version is unset, fails — used by
// callers (e.g. extension update) where omitting VERSION is not allowed.
// Returns true on error (thd error already set), false on success.
bool resolve_veb_version(THD *thd, const std::string &extension_name,
                         const LEX_CSTRING &m_version, bool require_explicit,
                         std::string &veb_version, std::string &version) {
  std::string requested_version;
  if (m_version.str != nullptr && m_version.length > 0) {
    requested_version.assign(m_version.str, m_version.length);
    // With VERSION specified, prefer {name}-{ver}.veb and fall back to
    // {name}.veb. The manifest is then asserted against requested_version
    // below.
    if (villagesql::veb::veb_file_exists(extension_name, requested_version)) {
      veb_version = requested_version;
    } else {
      veb_version.clear();
    }
  } else {
    if (require_explicit) {
      villagesql_error("Extension '%s' requires an explicit VERSION clause",
                       MYF(0), extension_name.c_str());
      return true;
    }
    if (villagesql::veb::find_veb_version(extension_name, veb_version)) {
      // Error already reported by find_veb_version
      return true;
    }
  }

  // Load manifest; for versioned VEBs this also asserts that the manifest
  // version matches veb_version.
  version = veb_version;
  if (villagesql::veb::load_veb_manifest(extension_name, version)) {
    // Error already reported by load_veb_manifest
    return true;
  }

  if (!requested_version.empty() && version != requested_version) {
    villagesql_error(
        "Cannot install extension '%s': manifest version is '%s' but "
        "VERSION '%s' was specified",
        MYF(0), extension_name.c_str(), version.c_str(),
        requested_version.c_str());
    return true;
  }

  (void)thd;
  return false;
}

// Expand the VEB archive and load the .so into memory.
// On success, registration is populated and sha256_hash contains the hash.
// Returns true on error (thd error set), false on success.
bool load_veb_and_so(THD *thd, const std::string &extension_name,
                     const std::string &veb_version,
                     villagesql::veb::ExtensionRegistration &registration,
                     std::string &sha256_hash,
                     villagesql::services::LoadReason load_reason) {
  std::string expanded_path;
  if (villagesql::veb::expand_veb_to_directory(extension_name, veb_version,
                                               expanded_path, sha256_hash)) {
    return true;
  }

  std::string so_path =
      villagesql::veb::get_extension_so_path(extension_name, sha256_hash);
  if (so_path.empty()) {
    villagesql_error("Failed to construct .so path for extension '%s'", MYF(0),
                     extension_name.c_str());
    return true;
  }

  vef_protocol_t server_protocol =
      static_cast<vef_protocol_t>(villagesql::veb::vef_server_protocol_version);
#ifndef NDEBUG
  {
    auto it = thd->user_vars.find("vef_debug_protocol_override");
    if (it != thd->user_vars.end()) {
      bool null_value = false;
      const longlong val = it->second->val_int(&null_value);
      if (!null_value && val > 0)
        server_protocol = static_cast<vef_protocol_t>(val);
    }
  }
#endif
  std::string load_error;
  if (villagesql::veb::load_vef_extension(
          {.extension_name = extension_name, .reason = load_reason, .thd = thd},
          so_path, server_protocol, registration, load_error)) {
    LogVSQL(ERROR_LEVEL, "Failed to load VEF extension '%s': %s",
            extension_name.c_str(), load_error.c_str());
    villagesql_error("Failed to load VEF extension '%s': %s", MYF(0),
                     extension_name.c_str(), load_error.c_str());
    return true;
  }

  return false;
}

// Counts threads that could hold live extension references during ALTER.
//
// Two separate buckets:
// - Non-admin client connections (no CONNECTION_ADMIN). offline_mode = ON
//   stops *new* ones; existing sessions must drain before ALTER proceeds.
// - Replication applier threads (thd->slave_thread in MySQL core API).
//   Replication bypasses offline_mode, and an applier in the middle of a
//   DML on a custom-typed column holds the same TypeContext / VDF
//   references a client would. The admin must STOP REPLICA before ALTER.
//
// The calling THD itself is excluded.
//
// NOTE: connections already marked KILL_CONNECTION by `offline_mode = ON`
// are still counted. The kill signal is asynchronous; until the session
// processes it on its next I/O, it remains on the THD list with live
// TypeContext / VDF references. Skipping marked-for-kill sessions would
// let ALTER proceed while a stale session is still alive.
class Count_threads : public Do_THD_Impl {
 public:
  explicit Count_threads(THD *calling_thd) : m_calling_thd(calling_thd) {}

  void operator()(THD *thd) override {
    if (thd == m_calling_thd) return;
    mysql_mutex_lock(&thd->LOCK_thd_data);
    if (thd->slave_thread) {
      m_replica_count++;
    } else if (!thd->is_connection_admin()) {
      m_non_admin_count++;
    }
    mysql_mutex_unlock(&thd->LOCK_thd_data);
  }

  uint non_admin_count() const { return m_non_admin_count; }
  uint replica_count() const { return m_replica_count; }

 private:
  THD *m_calling_thd;
  uint m_non_admin_count{0};
  uint m_replica_count{0};
};

// Returns true (with thd error set) if any thread that could hold live
// extension references is still running — either a non-admin client
// connection or a replication applier thread.
bool check_no_active_connections(THD *thd) {
  Count_threads counter(thd);
  Global_THD_manager::get_instance()->do_for_all_thd(&counter);
  if (counter.replica_count() > 0) {
    villagesql_error(
        "Cannot update extension: replication applier thread is running. "
        "STOP REPLICA before running ALTER EXTENSION.",
        MYF(0));
    return true;
  }
  if (counter.non_admin_count() > 0) {
    villagesql_error(
        "Cannot update extension: %u non-admin connection(s) are still active. "
        "Kill them before running ALTER EXTENSION, or wait for them to finish.",
        MYF(0), counter.non_admin_count());
    return true;
  }
  return false;
}

}  // namespace

namespace villagesql {
namespace {

bool check_for_columns_of_extension(
    const villagesql::ExtensionEntry &ext_entry,
    const std::vector<const ColumnEntry *> &all_columns) {
  std::string error_message;
  int count = 0;
  const ColumnEntry *first_col = nullptr;

  for (const auto *col : all_columns) {
    if (col->extension_name == ext_entry.extension_name() &&
        col->extension_version == ext_entry.extension_version) {
      if (count == 0) {
        first_col = col;
      }
      count++;
    }
  }

  if (first_col != nullptr) {
    villagesql_error(
        "Cannot drop extension `%s` as %d column(s) depend on it, "
        "e.g. %s.%s.%s has type %s",
        MYF(0), ext_entry.extension_name().c_str(), count,
        first_col->db_name().c_str(), first_col->table_name().c_str(),
        first_col->column_name().c_str(), first_col->type_name.c_str());
    return true;
  }

  return false;
}

bool check_for_sp_params_of_extension(
    const villagesql::ExtensionEntry &ext_entry,
    const std::vector<const SpParamEntry *> &all_sp_params) {
  const SpParamEntry *first = nullptr;
  int count = 0;

  for (const auto *entry : all_sp_params) {
    if (entry->extension_name == ext_entry.extension_name() &&
        entry->extension_version == ext_entry.extension_version) {
      if (count == 0) first = entry;
      count++;
    }
  }

  if (first != nullptr) {
    villagesql_error(
        "Cannot uninstall extension '%s': stored procedure %s.%s uses "
        "custom type %s",
        MYF(0), ext_entry.extension_name().c_str(), first->db_name().c_str(),
        first->sp_name().c_str(), first->type_name.c_str());
    return true;
  }

  return false;
}

// If the transaction commits, then `to_unregister` is used to unregister the
// .so file.
//
// If `expected_version` is non-empty, the installed extension's version must
// match exactly or uninstall is rejected. Callers pass an empty string when no
// VERSION clause was specified, falling back to whichever version is currently
// installed.
bool remove_extension_from_victionary(
    THD *thd, VictionaryClient &victionary, const std::string &extension_name,
    const std::string &expected_version,
    std::optional<veb::ExtensionRegistration> &to_unregister) {
  auto write_lock = victionary.get_write_lock();

  const auto *ext_entry = victionary.extensions().get_committed(
      villagesql::ExtensionKey(extension_name));
  if (ext_entry == nullptr) {
    villagesql_error("Extension '%s' is not installed", MYF(0),
                     extension_name.c_str());

    return true;
  }

  if (!expected_version.empty() &&
      ext_entry->extension_version != expected_version) {
    villagesql_error(
        "Cannot uninstall extension '%s': installed version is '%s' but "
        "VERSION '%s' was specified",
        MYF(0), extension_name.c_str(), ext_entry->extension_version.c_str(),
        expected_version.c_str());
    return true;
  }

  // Delete all custom types for this extension (RESTRICT behavior - fails
  // if any type has dependent columns or stored procedures)
  const auto &all_columns = victionary.columns().get_all_committed();
  if (!DBUG_EVALUATE_IF("villagesql_skip_uninstall_column_check", true,
                        false) &&
      check_for_columns_of_extension(*ext_entry, all_columns)) {
    return true;
  }

  const auto &all_sp_params = victionary.sp_params().get_all_committed();
  if (check_for_sp_params_of_extension(*ext_entry, all_sp_params)) {
    return true;
  }

  const auto &all_indexes = victionary.custom_indexes().get_all_committed();
  const auto &all_index_columns =
      victionary.custom_index_columns().get_all_committed();
  if (villagesql::check_for_indexes_of_extension(*ext_entry, all_indexes,
                                                 all_index_columns)) {
    return true;
  }

  // Check for active references to VDFs, TypeContexts, and TypeDescriptors.
  // A use_count > 1 means something other than Victionary holds a reference
  // (e.g., an executing query).
  //
  // TODO(villagesql-beta): this only catches in-flight VDF references. There
  // is no check for persisted VDF dependents — deterministic VDFs can appear
  // in generated columns, functional indexes, CHECK constraints, and DEFAULT
  // expressions, and villagesql does not track those references in any system
  // table (VDFs resolve at parse time only). Uninstalling such an extension
  // silently invalidates the persisted expressions. The same gap exists in
  // ALTER EXTENSION ... UPDATE TO (see check_update_compatibility). Fix
  // requires a new custom_vdf_uses-style system table populated at DDL time.
  const auto &all_funcs = victionary.funcs().get_all_committed();
  for (const auto *func : all_funcs) {
    if (func->extension_name() == extension_name &&
        func->extension_version() == ext_entry->extension_version) {
      long use_count = victionary.funcs().get_use_count(func->key().str());
      if (use_count > 1) {
        villagesql_error(
            "Cannot uninstall extension '%s': VDF '%s' is currently in "
            "use",
            MYF(0), extension_name.c_str(), func->function_name().c_str());
        return true;
      }
    }
  }

  const auto &all_type_contexts =
      victionary.type_contexts().get_all_committed();
  for (const auto *type_context : all_type_contexts) {
    if (type_context->extension_name() == extension_name &&
        type_context->extension_version() == ext_entry->extension_version) {
      long use_count =
          victionary.type_contexts().get_use_count(type_context->key().str());
      if (use_count > 1) {
        villagesql_error(
            "Cannot uninstall extension '%s': type '%s' is currently in use",
            MYF(0), extension_name.c_str(), type_context->type_name().c_str());
        return true;
      }
    }
  }

  const auto &all_type_descs =
      victionary.type_descriptors().get_all_committed();
  for (const auto *type_desc : all_type_descs) {
    if (type_desc->extension_name() == extension_name &&
        type_desc->extension_version() == ext_entry->extension_version) {
      long use_count =
          victionary.type_descriptors().get_use_count(type_desc->key().str());
      if (use_count > 1) {
        villagesql_error(
            "Cannot uninstall extension '%s': type '%s' is currently in use",
            MYF(0), extension_name.c_str(), type_desc->type_name().c_str());
        return true;
      }
    }
  }

  // TODO(villagesql): the inline deletion loops below duplicate
  // mark_extension_for_deletion in sql_extension_register.cc. Reuse the helper
  // here once UNINSTALL is restructured to share the code path with UPDATE.

  // Delete TypeContexts for this extension (we do it before TypeDescriptors
  // since TypeContext holds a raw pointer to TypeDescriptor, but under the
  // lock, it doesn't really matter)
  for (const auto *type_context : all_type_contexts) {
    if (type_context->extension_name() == extension_name &&
        type_context->extension_version() == ext_entry->extension_version) {
      victionary.type_contexts().MarkForDeletion(*thd, type_context->key());
    }
  }

  // Delete TypeDescriptors for this extension
  for (const auto *type_desc : all_type_descs) {
    if (type_desc->extension_name() == extension_name &&
        type_desc->extension_version() == ext_entry->extension_version) {
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
        prof->extension_version() == ext_entry->extension_version) {
      victionary.index_profile_descriptors().MarkForDeletion(*thd, prof->key());
    }
  }

  // Delete IndexTypeDescriptors for this extension
  const auto &all_index_types =
      victionary.index_type_descriptors().get_all_committed();
  for (const auto *index_type : all_index_types) {
    if (index_type->extension_name() == extension_name &&
        index_type->extension_version() == ext_entry->extension_version) {
      victionary.index_type_descriptors().MarkForDeletion(*thd,
                                                          index_type->key());
    }
  }

  // Delete VDFs for this extension
  for (const auto *func : all_funcs) {
    if (func->extension_name() == extension_name &&
        func->extension_version() == ext_entry->extension_version) {
      victionary.funcs().MarkForDeletion(*thd, func->key());
    }
  }

  victionary.extensions().MarkForDeletion(*thd, ext_entry->key());
  const auto *ext_desc = victionary.extension_descriptors().get_committed(
      ExtensionDescriptorKey(extension_name, ext_entry->extension_version));
  if (ext_desc != nullptr) {
    to_unregister.emplace(ext_desc->registration());
    victionary.extension_descriptors().MarkForDeletion(*thd, ext_desc->key());
  }

  return false;
}

}  // namespace
}  // namespace villagesql

bool Sql_cmd_uninstall_extension::execute(THD *thd) {
  // We do not replicate the UNINSTALL EXTENSION statement
  const Disable_binlog_guard binlog_guard(thd);

  // Acquire global shared read lock to check and prevent installation in
  // "read only mode". Acquire shared backup lock to synchronize with final
  // phase of backup operation.
  if (acquire_shared_global_read_lock(thd, thd->variables.lock_wait_timeout) ||
      acquire_shared_backup_lock(thd, thd->variables.lock_wait_timeout))
    return true;

  std::string extension_name(m_name.str, m_name.length);
  std::string expected_version =
      m_version.str ? to_string(m_version) : std::string();

  // Acquire X MDL lock with statement duration on the normalized extension
  // name to synchronize with following operations. All such operations must
  // acquire IX lock on the extension name.
  // 1. Concurrent install/uninstall operations with same extension name.
  // 2. Concurrent DDL creating columns with types defined by the extension.
  // 3. Concurrent statement running custom functions defined by the extension.
  if (villagesql::Metadata_modifier::lock_extension_exclusive(
          thd, extension_name, MDL_STATEMENT)) {
    return true;
  }
  DEBUG_SYNC_C("uninstall_after_extension_lock");

  // Start transaction
  const Disable_autocommit_guard autocommit_guard(thd);
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  LogVSQL(INFORMATION_LEVEL, "Uninstalling extension: '%s'",
          extension_name.c_str());

  // Open all required tables in one call:
  // - extensions (WRITE) - to delete extension record
  Table_ref extensions_table(villagesql::SchemaManager::VILLAGESQL_SCHEMA_NAME,
                             villagesql::SchemaManager::EXTENSIONS_TABLE_NAME,
                             TL_WRITE, MDL_SHARED_WRITE);

  // Set the links for open_and_lock_tables
  extensions_table.next_global = extensions_table.next_local = nullptr;

  if (open_and_lock_tables(thd, &extensions_table, MYSQL_LOCK_IGNORE_TIMEOUT)) {
    villagesql_error("Cannot open extension tables", MYF(0));
    return end_transaction(thd, true);
  }

  // Get victionary client
  auto &victionary = villagesql::VictionaryClient::instance();

  // State tracking for three-phase operation:
  // Phase 1 (under lock): lookups and mark operations
  // Phase 2 (lock released): write to table and commit
  // Phase 3 (lock still released): commit

  std::optional<villagesql::veb::ExtensionRegistration> to_unregister;
  // Phase 1: Do all lookups and mark operations while holding lock
  if (villagesql::remove_extension_from_victionary(
          thd, victionary, extension_name, expected_version, to_unregister)) {
    return end_transaction(thd, true);
  }

  // Phase 2: write tables
  if (victionary.write_all_uncommitted_entries(thd)) {
    villagesql_error("Failed to delete extension '%s'", MYF(0),
                     extension_name.c_str());
    return end_transaction(thd, true);
  }

  if (to_unregister.has_value()) {
    villagesql::veb::unload_vef_extension(
        {.reason = villagesql::services::UnloadReason::kUninstall, .thd = thd},
        *to_unregister);
  }

  LogVSQL(INFORMATION_LEVEL, "Extension '%s' uninstalled successfully",
          extension_name.c_str());
  my_ok(thd);

  // Phase 3: perform the commit
  return end_transaction(thd, false);
}
