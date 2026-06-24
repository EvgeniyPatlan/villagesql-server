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

#ifndef VILLAGESQL_SCHEMA_SYSTABLE_PENDING_ACTION_H_
#define VILLAGESQL_SCHEMA_SYSTABLE_PENDING_ACTION_H_

#include <optional>
#include <string>

struct TABLE;

namespace villagesql {

// A deferred action queued against an installed extension, applied at the
// next server restart.
//
// PendingAction is the public surface for callers; the wire format used by
// the storage layer is intentionally hidden behind Serialize / Deserialize.
// Callers construct an action via the factory methods, ask for its kind, and
// read typed fields via the getters. Adding a new kind in a future slice
// requires extending this class only; no caller of has_pending_action(),
// is_version_update(), target_version(), or similar should need to change.
//
// At v1 only one kind exists ("version_update"). The class is structured so
// the wire format and internal layout can evolve without breaking callers.
class PendingAction {
 public:
  // Construct a "version_update" action representing a request to swap the
  // installed extension to target_version at the next restart. requested_at
  // is captured here (server local clock) so callers don't need to know how
  // timestamps are stamped.
  static PendingAction CreateVersionUpdate(std::string target_version,
                                           std::string target_veb_sha256);

  // Annotate this action with a failure reason (and the time it was
  // observed). Used at restart-apply time: when the swap cannot proceed,
  // the caller stamps the action so the row remains queryable as
  // "pending, attempted, failed". Replaces any previous failure record.
  void MarkFailed(std::string error_message);

  // Storage round-trip. Used by the systable I/O layer; not intended for
  // callers. Deserialize returns true on failure with error_message set.
  std::string Serialize() const;
  static bool Deserialize(const std::string &raw, PendingAction &out,
                          std::string &error_message);

  // Optional-aware round-trip for the systable I/O layer. Encapsulates the
  // "empty string means no action / non-empty means parse" convention so
  // callers don't reimplement it per column.
  //
  // FromOptionalJson: empty `raw` sets `out` to std::nullopt (no error).
  // Non-empty `raw` is parsed; on failure returns true with error_message
  // populated and leaves `out` untouched.
  //
  // ToOptionalJson: nullopt yields the empty string; engaged optional
  // returns the serialized JSON.
  static bool FromOptionalJson(const std::string &raw,
                               std::optional<PendingAction> &out,
                               std::string &error_message);
  static std::string ToOptionalJson(const std::optional<PendingAction> &value);

  // Table-level round-trip for the systable I/O layer. The class owns its
  // own column-name(s); callers pass the table and the class finds the
  // right field(s). This is the seam where a future table-version-aware
  // implementation can decide *what* to read based on schema state without
  // any caller change.
  //
  // ReadFromTable: NULL column sets `out` to std::nullopt. Non-NULL is
  // parsed; on failure returns true with error_message populated.
  //
  // StoreToTable: nullopt stores NULL; engaged optional stores the
  // serialized form.
  //
  // Returns true on internal errors (e.g. expected column missing).
  static bool ReadFromTable(TABLE &table, std::optional<PendingAction> &out,
                            std::string &error_message);
  static bool StoreToTable(TABLE &table,
                           const std::optional<PendingAction> &value,
                           std::string &error_message);

  // SQL expressions for I_S view definitions to project individual logical
  // fields of a pending action against a row of the extensions table aliased
  // as `table_alias`. Returned strings are ready to feed to
  // `m_target_def.add_field`'s SQL-expression argument.
  //
  // The view definitions stay free of any knowledge that the underlying
  // storage is JSON; future schema-shape changes affect only the
  // implementations below.
  static std::string TargetVersionSqlExpr(const char *table_alias);
  static std::string RequestedAtSqlExpr(const char *table_alias);
  static std::string LastErrorSqlExpr(const char *table_alias);
  static std::string LastErrorAtSqlExpr(const char *table_alias);

  // Default-constructed action is in an unspecified but valid state. Used
  // by the storage layer as the out-parameter buffer for Deserialize.
  // Callers should construct via CreateVersionUpdate instead.
  PendingAction() = default;

  // Kind discriminator. Today there is exactly one kind.
  bool is_version_update() const;

  // Version-update getters. Valid only when is_version_update() is true.
  const std::string &target_version() const;
  const std::string &target_veb_sha256() const;

  // Common to all kinds.
  const std::string &requested_at() const;

  // Failure record. Empty when the action has not yet been attempted or
  // the latest attempt succeeded. Non-empty when the last attempt
  // recorded an error via MarkFailed.
  const std::string &last_error() const;
  const std::string &last_error_at() const;
  bool has_failure() const;

 private:
  // Internal layout is private. Field names and JSON shape may change
  // without breaking callers as long as the public getters keep returning
  // semantically equivalent values.
  enum class Kind {
    kVersionUpdate,
  };

  Kind kind_{Kind::kVersionUpdate};
  std::string target_version_;
  std::string target_veb_sha256_;
  std::string requested_at_;
  std::string last_error_;
  std::string last_error_at_;
};

}  // namespace villagesql

#endif  // VILLAGESQL_SCHEMA_SYSTABLE_PENDING_ACTION_H_
