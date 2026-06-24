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

#ifndef VILLAGESQL_VEB_SQL_EXTENSION_UPDATE_APPLY_H_
#define VILLAGESQL_VEB_SQL_EXTENSION_UPDATE_APPLY_H_

#include <string>
#include <vector>

class THD;

namespace villagesql {
struct ExtensionEntry;
class VictionaryClient;
}  // namespace villagesql

namespace villagesql::veb {

// One per extension that the apply driver has decided about. `use_target`
// is true when the pending action should take effect this restart;
// `failure_msg` carries the reason it should not.
struct ApplyDecision {
  std::string extension_name;
  std::string current_version;
  std::string current_sha256;

  bool use_target = false;
  bool had_failure = false;  // true when a pending action existed and failed
  std::string target_version;
  std::string target_sha256;
  std::string failure_msg;
};

// Cross-extension failure policy for the apply driver. The shape of the
// algorithm is identical either way; this flag is the single decision
// point that flips between them. Today's choice is `kIndependent`.
enum class CrossExtensionPolicy {
  kIndependent,   // per-extension: A succeeds even if B fails
  kAllOrNothing,  // any failure flips all successes to failures
};

// Phase A: build per-extension decisions for every entry that has a
// pending action. Entries without a pending action are skipped (no
// decision recorded). No catalog mutations; reads `victionary` snapshots.
// Caller must hold the victionary read or write lock.
//
// THD is used to clear the diagnostics area when underlying helpers
// (which were designed for live-session SQL paths) stamp errors via
// villagesql_error. We capture our own failure_msg into the decision.
std::vector<ApplyDecision> DecideAllPending(THD *thd,
                                            const VictionaryClient &victionary);

// Apply the configured cross-extension policy to a set of decisions.
// Operates in place. Pure with respect to caller state.
void ApplyCrossExtensionPolicy(std::vector<ApplyDecision> *decisions,
                               CrossExtensionPolicy policy);

// Phase B: stage in-memory catalog mutations matching each decision.
// On use_target decisions: MarkForUpdate the extension entry to target
// version + sha, clear pending_action, and rewrite custom_columns and
// custom_sp_params rows for this extension to the new version.
// On had_failure decisions: MarkFailed the pending action with
// failure_msg and MarkForUpdate the entry.
// Decisions with neither flag set (no pending action) are skipped.
// Caller must hold the victionary write lock. Returns true on any
// MarkForUpdate failure (caller logs and aborts).
bool StageDecisions(THD *thd, VictionaryClient *victionary,
                    const std::vector<ApplyDecision> &decisions);

}  // namespace villagesql::veb

#endif  // VILLAGESQL_VEB_SQL_EXTENSION_UPDATE_APPLY_H_
