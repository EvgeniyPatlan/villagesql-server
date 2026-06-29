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

#include "villagesql/veb/sql_extension_update_precheck.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "villagesql/veb/precheck_harvest.h"

namespace villagesql {
namespace veb {

namespace {

UpdatePreCheckResult fail(std::string message) {
  UpdatePreCheckResult r;
  r.ok = false;
  r.error_message = std::move(message);
  return r;
}

UpdatePreCheckResult ok() {
  UpdatePreCheckResult r;
  r.ok = true;
  return r;
}

// Reject if a type retained across the update has a persisted_length change.
// A change would cause existing on-disk bytes to be misinterpreted.
//
// `current` is the state of the extension as installed today. The
// `target_persisted_length` map is built from the new version's VEB that
// we're being asked to update to.
UpdatePreCheckResult check_retained_types_persisted_length(
    const UpdatePreCheckInput &current,
    const std::unordered_map<std::string, int64_t> &target_persisted_length) {
  for (const auto &c : current.current_types) {
    auto it = target_persisted_length.find(c.type_name);
    if (it == target_persisted_length.end()) continue;  // dropped
    if (it->second != c.persisted_length) {
      char buf[512];
      std::snprintf(
          buf, sizeof(buf),
          "Cannot update extension '%s': type '%s' persisted_length changed "
          "from %lld to %lld -- existing stored data would be corrupted",
          current.extension_name.c_str(), c.type_name.c_str(),
          static_cast<long long>(c.persisted_length),
          static_cast<long long>(it->second));
      return fail(buf);
    }
  }
  return ok();
}

// Reject if a type present in the current registration is absent from the
// target registration AND still has dependent columns or SP params.
//
// `current` is the state of the extension as installed today (and the
// dependent columns / SP params already on disk that reference its types).
// `target_type_names` is the set of type names declared by the new
// version's VEB.
UpdatePreCheckResult check_dropped_types_have_no_dependents(
    const UpdatePreCheckInput &current,
    const std::unordered_set<std::string> &target_type_names) {
  for (const auto &col : current.dependent_columns) {
    if (target_type_names.find(col.type_name) != target_type_names.end())
      continue;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "Cannot update extension '%s': type '%s' is being dropped "
                  "but column %s.%s.%s depends on it",
                  current.extension_name.c_str(), col.type_name.c_str(),
                  col.db_name.c_str(), col.table_name.c_str(),
                  col.column_name.c_str());
    return fail(buf);
  }
  for (const auto &sp : current.dependent_sp_params) {
    if (target_type_names.find(sp.type_name) != target_type_names.end())
      continue;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "Cannot update extension '%s': type '%s' is being dropped "
                  "but stored procedure parameter %s.%s.%s depends on it",
                  current.extension_name.c_str(), sp.type_name.c_str(),
                  sp.db_name.c_str(), sp.sp_name.c_str(),
                  sp.param_name.c_str());
    return fail(buf);
  }
  return ok();
}

}  // namespace

UpdatePreCheckResult RunUpdatePreCheck(const UpdatePreCheckInput &input) {
  // Harvest target-side type metadata. HarvestTargetTypes owns the
  // dlopen + vef_register + dlclose cycle and returns just the data the
  // checks below need. Today it runs in-process; the seam is set up so
  // Phase 2 can route through a subprocess without changing this call.
  std::vector<HarvestedTargetType> harvested;
  std::string harvest_error;
  if (HarvestTargetTypes(input.target_so_path, input.server_protocol,
                         harvested, harvest_error)) {
    return fail(std::string("Cannot update extension '") +
                input.extension_name + "': " + harvest_error);
  }

  // Index the harvested types for the per-check lookups.
  std::unordered_map<std::string, int64_t> target_persisted_length;
  std::unordered_set<std::string> target_type_names;
  target_persisted_length.reserve(harvested.size());
  target_type_names.reserve(harvested.size());
  for (const auto &h : harvested) {
    target_persisted_length[h.type_name] = h.persisted_length;
    target_type_names.insert(h.type_name);
  }

  UpdatePreCheckResult r;

  r = check_retained_types_persisted_length(input, target_persisted_length);
  if (!r.ok) return r;

  r = check_dropped_types_have_no_dependents(input, target_type_names);
  if (!r.ok) return r;

  return ok();
}

}  // namespace veb
}  // namespace villagesql
