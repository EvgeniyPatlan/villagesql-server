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

#ifndef VILLAGESQL_SCHEMA_SYSTABLE_EXTENSIONS_H_
#define VILLAGESQL_SCHEMA_SYSTABLE_EXTENSIONS_H_

#include <string>

#include "villagesql/schema/systable/helpers.h"

// Forward declarations
struct TABLE;

namespace villagesql {

// Forward declaration for TableTraits
template <typename EntryType>
struct TableTraits;

// Key for extensions table entries
// Format: "normalized_extension_name"
// Extensions use extension name normalization rules for case-insensitive
// comparison. Stores original name for display, normalized key for lookups.
struct ExtensionKey {
 public:
  ExtensionKey() = default;

  explicit ExtensionKey(std::string name)
      : extension_name_(std::move(name)),
        normalized_key_(normalize_extension_name(extension_name_)) {}

  const std::string &str() const { return normalized_key_; }

  // Component accessor (returns original value)
  const std::string &extension_name() const { return extension_name_; }

  // Comparison operators for std::map (use normalized key)
  bool operator<(const ExtensionKey &other) const {
    return normalized_key_ < other.normalized_key_;
  }
  bool operator==(const ExtensionKey &other) const {
    return normalized_key_ == other.normalized_key_;
  }

 private:
  std::string extension_name_;
  std::string normalized_key_;
};

// Entry for extensions system table
struct ExtensionEntry {
 public:
  using key_type = ExtensionKey;

  // Non-key data (public)
  std::string extension_version;
  std::string veb_sha256;

  // Full constructor with all fields
  ExtensionEntry(ExtensionKey key, std::string version, std::string sha256)
      : extension_version(std::move(version)),
        veb_sha256(std::move(sha256)),
        key_(std::move(key)) {}

  // Construct with key only, other fields can be set separately (useful for
  // testing)
  explicit ExtensionEntry(ExtensionKey key) : key_(std::move(key)) {}

  ExtensionEntry() = default;

  const ExtensionKey &key() const { return key_; }

  // Accessor for key component (delegate to key)
  const std::string &extension_name() const { return key_.extension_name(); }

 protected:
  void set_key(ExtensionKey key) { key_ = std::move(key); }
  friend struct TableTraits<ExtensionEntry>;

 private:
  ExtensionKey key_;
};

// TableTraits specialization for ExtensionEntry
template <>
struct TableTraits<ExtensionEntry> {
  // ===== Serialization (read from TABLE) =====

  // Read a row from villagesql.extensions table into an ExtensionEntry
  // Returns false on success, true on error
  static bool read_from_table(TABLE &table, ExtensionEntry &entry);

  // ===== Deserialization (write to TABLE) =====

  // Write an ExtensionEntry to villagesql.extensions table
  // Assumes table is already positioned for write (empty_record called, etc.)
  // Returns false on success, true on error
  static bool write_to_table(TABLE &table, const ExtensionEntry &entry);

  // Update an ExtensionEntry in villagesql.extensions table
  // old_key is the key of the row to update (may differ from entry.key() if
  // key columns changed). That is, if the key has changed, the old key must be
  // set in old_key; otherwise, entry.key() is used.
  // Returns false on success, true on error
  static bool update_in_table(TABLE &table, const ExtensionEntry &entry,
                              const std::string &old_key);

  // Delete an ExtensionEntry from villagesql.extensions table
  // Returns false on success, true on error
  static bool delete_from_table(TABLE &table, const ExtensionEntry &entry);
};

}  // namespace villagesql

#endif  // VILLAGESQL_SCHEMA_SYSTABLE_EXTENSIONS_H_
