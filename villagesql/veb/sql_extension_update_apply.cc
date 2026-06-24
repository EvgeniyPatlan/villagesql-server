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

#include "villagesql/veb/sql_extension_update_apply.h"

#include <string>
#include <utility>
#include <vector>

#include "sql/sql_class.h"
#include "villagesql/include/error.h"
#include "villagesql/schema/systable/custom_columns.h"
#include "villagesql/schema/systable/custom_sp_params.h"
#include "villagesql/schema/systable/extensions.h"
#include "villagesql/schema/systable/pending_action.h"
#include "villagesql/schema/victionary_client.h"
#include "villagesql/veb/sql_extension_update_precheck.h"
#include "villagesql/veb/veb_file.h"

namespace villagesql::veb {

namespace {

// Build the precheck input from a snapshot of the victionary for one
// extension's current state plus the target VEB path. Caller holds the
// victionary read or write lock.
UpdatePreCheckInput build_precheck_input(const VictionaryClient &victionary,
                                         const ExtensionEntry &entry,
                                         const std::string &target_version,
                                         std::string target_so_path) {
  UpdatePreCheckInput input;
  input.extension_name = entry.extension_name();
  input.current_version = entry.extension_version;
  input.target_version = target_version;
  input.target_so_path = std::move(target_so_path);
  input.server_protocol = static_cast<int>(vef_server_protocol_version);

  for (const auto *td : victionary.type_descriptors().get_all_committed()) {
    if (td->extension_name() != input.extension_name ||
        td->extension_version() != input.current_version)
      continue;
    CurrentTypeSnapshot s;
    s.type_name = td->type_name();
    s.persisted_length = td->persisted_length();
    input.current_types.push_back(std::move(s));
  }

  for (const auto *col : victionary.columns().get_all_committed()) {
    if (col->extension_name != input.extension_name ||
        col->extension_version != input.current_version)
      continue;
    DependentColumnSnapshot s;
    s.db_name = col->db_name();
    s.table_name = col->table_name();
    s.column_name = col->column_name();
    s.type_name = col->type_name;
    input.dependent_columns.push_back(std::move(s));
  }

  for (const auto *sp : victionary.sp_params().get_all_committed()) {
    if (sp->extension_name != input.extension_name ||
        sp->extension_version != input.current_version)
      continue;
    DependentSpParamSnapshot s;
    s.db_name = sp->db_name();
    s.sp_name = sp->sp_name();
    s.param_name = sp->param_name();
    s.type_name = sp->type_name;
    input.dependent_sp_params.push_back(std::move(s));
  }
  return input;
}

// Resolve the target VEB on disk for an extension. Returns false on
// success and fills in target_so_path / target_sha256. Returns true on
// failure with err set to a short reason; logs nothing.
//
// expand_veb_to_directory unfortunately calls villagesql_error on failure
// (writes to the THD's diagnostics area). We capture our own failure_msg
// here and clear the THD diagnostics so the caller sees a clean state.
bool resolve_target(THD *thd, const std::string &extension_name,
                    const std::string &target_version,
                    std::string *target_so_path, std::string *target_sha256,
                    std::string *err) {
  std::string expanded_path;
  if (expand_veb_to_directory(extension_name, target_version, expanded_path,
                              *target_sha256)) {
    *err = "Failed to resolve target VEB for version '" + target_version + "'";
    if (thd != nullptr && thd->get_stmt_da() != nullptr) {
      thd->get_stmt_da()->reset_diagnostics_area();
      thd->get_stmt_da()->reset_condition_info(thd);
    }
    return true;
  }
  *target_so_path = get_extension_so_path(extension_name, *target_sha256);
  if (target_so_path->empty()) {
    *err = "Failed to construct .so path for target version '" +
           target_version + "'";
    if (thd != nullptr && thd->get_stmt_da() != nullptr) {
      thd->get_stmt_da()->reset_diagnostics_area();
      thd->get_stmt_da()->reset_condition_info(thd);
    }
    return true;
  }
  return false;
}

}  // namespace

std::vector<ApplyDecision> DecideAllPending(
    THD *thd, const VictionaryClient &victionary) {
  std::vector<ApplyDecision> decisions;
  for (const auto *entry : victionary.extensions().get_all_committed()) {
    if (entry == nullptr || !entry->has_pending_action()) continue;
    const PendingAction &pa = *entry->pending_action;
    if (!pa.is_version_update()) {
      // Unknown kind today. Don't apply; mark failure so the row is
      // queryable. Future kinds slot in here.
      ApplyDecision d;
      d.extension_name = entry->extension_name();
      d.current_version = entry->extension_version;
      d.current_sha256 = entry->veb_sha256;
      d.had_failure = true;
      d.failure_msg =
          "Pending action kind is not 'version_update' and cannot be applied "
          "at restart by this server";
      decisions.push_back(std::move(d));
      continue;
    }

    ApplyDecision d;
    d.extension_name = entry->extension_name();
    d.current_version = entry->extension_version;
    d.current_sha256 = entry->veb_sha256;

    std::string target_so_path;
    std::string target_sha256;
    std::string err;
    if (resolve_target(thd, d.extension_name, pa.target_version(),
                       &target_so_path, &target_sha256, &err)) {
      d.had_failure = true;
      d.failure_msg = err;
      decisions.push_back(std::move(d));
      continue;
    }

    UpdatePreCheckInput input = build_precheck_input(
        victionary, *entry, pa.target_version(), target_so_path);
    UpdatePreCheckResult result = RunUpdatePreCheck(input);
    if (!result.ok) {
      d.had_failure = true;
      d.failure_msg = result.error_message;
      // RunUpdatePreCheck's transitive open_vef_extension may have written
      // to the THD diagnostics area on failure; clear it so the caller
      // doesn't see a per-extension precheck error masquerading as a
      // bootstrap-level error.
      if (thd != nullptr && thd->get_stmt_da() != nullptr) {
        thd->get_stmt_da()->reset_diagnostics_area();
        thd->get_stmt_da()->reset_condition_info(thd);
      }
      decisions.push_back(std::move(d));
      continue;
    }

    d.use_target = true;
    d.target_version = pa.target_version();
    d.target_sha256 = std::move(target_sha256);
    decisions.push_back(std::move(d));
  }
  return decisions;
}

void ApplyCrossExtensionPolicy(std::vector<ApplyDecision> *decisions,
                               CrossExtensionPolicy policy) {
  if (policy != CrossExtensionPolicy::kAllOrNothing) return;

  bool any_failure = false;
  std::string offending;
  for (const auto &d : *decisions) {
    if (d.had_failure) {
      any_failure = true;
      if (offending.empty()) offending = d.extension_name;
    }
  }
  if (!any_failure) return;

  for (auto &d : *decisions) {
    if (d.use_target) {
      d.use_target = false;
      d.had_failure = true;
      d.failure_msg =
          "Rolled back: another extension's pending update failed at "
          "restart-time apply (e.g. '" +
          offending + "')";
    }
  }
}

bool StageDecisions(THD *thd, VictionaryClient *victionary,
                    const std::vector<ApplyDecision> &decisions) {
  for (const auto &d : decisions) {
    const ExtensionKey key(d.extension_name);
    const ExtensionEntry *existing =
        victionary->extensions().get_committed(key);
    if (existing == nullptr) {
      // Should not happen — decisions are built from committed entries.
      LogVSQL(ERROR_LEVEL,
              "Cannot stage decision for extension '%s': committed entry "
              "disappeared",
              d.extension_name.c_str());
      return true;
    }

    if (d.use_target) {
      ExtensionEntry updated(key, d.target_version, d.target_sha256);
      updated.pending_action.reset();
      if (victionary->extensions().MarkForUpdate(*thd, std::move(updated),
                                                 key)) {
        return true;
      }

      // Rewrite column entries owned by this extension to the new version.
      // Iterate a snapshot of the keys so we don't mutate while iterating.
      std::vector<const ColumnEntry *> cols =
          victionary->columns().get_all_committed();
      for (const auto *col : cols) {
        if (col->extension_name != d.extension_name ||
            col->extension_version != d.current_version)
          continue;
        ColumnEntry rewritten(col->key(), col->extension_name, d.target_version,
                              col->type_name, col->type_parameters);
        if (victionary->columns().MarkForUpdate(*thd, std::move(rewritten),
                                                col->key())) {
          return true;
        }
      }

      std::vector<const SpParamEntry *> sps =
          victionary->sp_params().get_all_committed();
      for (const auto *sp : sps) {
        if (sp->extension_name != d.extension_name ||
            sp->extension_version != d.current_version)
          continue;
        SpParamEntry rewritten(sp->key(), sp->extension_name, d.target_version,
                               sp->type_name, sp->type_parameters);
        if (victionary->sp_params().MarkForUpdate(*thd, std::move(rewritten),
                                                  sp->key())) {
          return true;
        }
      }
    } else if (d.had_failure) {
      // Pending action exists but failed; mark with the failure reason and
      // keep version/sha as committed.
      PendingAction failed = *existing->pending_action;
      failed.MarkFailed(d.failure_msg);
      ExtensionEntry updated(key, d.current_version, d.current_sha256);
      updated.pending_action = std::move(failed);
      if (victionary->extensions().MarkForUpdate(*thd, std::move(updated),
                                                 key)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace villagesql::veb
