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

#include "villagesql/include/semver.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>

namespace villagesql {

Semver::Semver() : major_(0), minor_(0), patch_(0), valid_(false) {}

bool Semver::is_numeric(std::string_view str) {
  if (str.empty()) return false;
  return std::all_of(str.begin(), str.end(),
                     [](unsigned char c) { return std::isdigit(c); });
}

bool Semver::is_valid_identifier(std::string_view id) {
  if (id.empty()) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-';
  });
}

bool Semver::parse(std::string_view s, std::string *error) {
  // Reset state
  *this = Semver();

  if (s.empty()) {
    if (error) *error = "Empty version string";
    return false;
  }

  // Separate core version (MAJOR.MINOR.PATCH)
  std::string_view core_version =
      s.substr(0, std::min(s.find("+"), s.find("-")));
  s.remove_prefix(core_version.size());

  // Parse MAJOR.MINOR.PATCH
  if (std::count(core_version.begin(), core_version.end(), '.') != 2) {
    if (error)
      *error = "Invalid core version format, expected MAJOR.MINOR.PATCH";
    return false;
  }

  size_t minor_pos = core_version.find('.');
  size_t patch_pos = core_version.find('.', minor_pos + 1);
  std::string major_str = std::string(core_version.substr(0, minor_pos));
  std::string minor_str = std::string(
      core_version.substr(minor_pos + 1, patch_pos - minor_pos - 1));
  std::string patch_str = std::string(core_version.substr(patch_pos + 1));

  // Validate and parse major, minor, patch
  if (!is_numeric(major_str) || !is_numeric(minor_str) ||
      !is_numeric(patch_str)) {
    if (error) *error = "MAJOR, MINOR, and PATCH must be numeric";
    return false;
  }

  // Check for leading zeros
  if ((major_str.length() > 1 && major_str[0] == '0') ||
      (minor_str.length() > 1 && minor_str[0] == '0') ||
      (patch_str.length() > 1 && patch_str[0] == '0')) {
    if (error) *error = "Version numbers must not have leading zeros";
    return false;
  }

  // Parse the numbers. Use the C routines to avoid exceptions
  unsigned long major_tmp = strtoul(major_str.data(), nullptr, 10);
  unsigned long minor_tmp = strtoul(minor_str.data(), nullptr, 10);
  unsigned long patch_tmp = strtoul(patch_str.data(), nullptr, 10);
  if (major_tmp == ULONG_MAX || minor_tmp == ULONG_MAX ||
      patch_tmp == ULONG_MAX) {
    if (error) *error = "Version number out of range";
    return false;
  }

  std::vector<std::string> prerelease_tmp;
  // Parse pre-release if present
  if (!s.empty() && s[0] == '-') {
    std::string_view prerelease_str = s.substr(0, s.find('+'));
    s.remove_prefix(prerelease_str.size());

    // Split by dots
    while (!prerelease_str.empty()) {
      // Remove '-' or '.'
      prerelease_str.remove_prefix(1);

      size_t dot = prerelease_str.find('.');
      std::string_view identifier = prerelease_str.substr(0, dot);

      if (!is_valid_identifier(identifier)) {
        if (error) *error = "Invalid pre-release identifier";
        return false;
      }
      // Check for leading zeros in numeric identifiers
      if (is_numeric(identifier) && identifier.length() > 1 &&
          identifier[0] == '0') {
        if (error)
          *error =
              "Numeric pre-release identifiers must not have leading zeros";
        return false;
      }

      prerelease_tmp.push_back(std::string(identifier));
      prerelease_str.remove_prefix(identifier.size());
    }

    if (prerelease_tmp.empty()) {
      if (error) *error = "Pre-release section cannot be empty";
      return false;
    }
  }

  std::vector<std::string> build_metadata_tmp;
  // Parse build metadata if present
  if (!s.empty() && s[0] == '+') {
    std::string_view build_str = s;

    // Split by dots
    while (!build_str.empty()) {
      // Remove '+' or '.'
      build_str.remove_prefix(1);

      size_t dot = build_str.find('.');
      std::string_view identifier = build_str.substr(0, dot);

      if (!is_valid_identifier(identifier)) {
        if (error) *error = "Invalid build metadata identifier";
        return false;
      }

      build_metadata_tmp.push_back(std::string(identifier));
      build_str.remove_prefix(identifier.size());
    }

    if (build_metadata_tmp.empty()) {
      if (error) *error = "Build metadata section cannot be empty";
      return false;
    }
  }

  major_ = major_tmp;
  minor_ = minor_tmp;
  patch_ = patch_tmp;
  prerelease_.swap(prerelease_tmp);
  build_metadata_.swap(build_metadata_tmp);
  valid_ = true;
  return true;
}

Semver Semver::from_string(std::string_view version_str, std::string *error) {
  Semver ver;
  ver.parse(version_str, error);
  return ver;
}

Semver Semver::from_components(unsigned long major, unsigned long minor,
                               unsigned long patch,
                               const std::vector<std::string> &prerelease,
                               const std::vector<std::string> &build_metadata) {
  Semver ver;

  // Validate identifiers if provided
  for (const auto &id : prerelease) {
    if (!is_valid_identifier(id)) {
      ver.valid_ = false;
      return ver;
    }
    // Check for leading zeros in numeric identifiers
    if (is_numeric(id) && id.length() > 1 && id[0] == '0') {
      ver.valid_ = false;
      return ver;
    }
  }

  for (const auto &id : build_metadata) {
    if (!is_valid_identifier(id)) {
      ver.valid_ = false;
      return ver;
    }
  }

  ver.major_ = major;
  ver.minor_ = minor;
  ver.patch_ = patch;
  ver.prerelease_ = prerelease;
  ver.build_metadata_ = build_metadata;
  ver.valid_ = true;

  return ver;
}

std::string Semver::to_string() const {
  if (!valid_) return "";

  const int sz = 33;  // 3x ten digits + 2 dots + 1 null
  char buf[sz];
  int n = snprintf(buf, sz, "%lu.%lu.%lu", major_, minor_, patch_);
  std::string r(buf, n);

  int extra = 0;
  for (size_t i = 0; i < prerelease_.size(); ++i) {
    extra += 1 + prerelease_[i].size();
  }
  for (size_t i = 0; i < build_metadata_.size(); ++i) {
    extra += 1 + build_metadata_[i].size();
  }

  r.reserve(r.size() + 1 + extra);

  for (size_t i = 0; i < prerelease_.size(); ++i) {
    r.push_back(i == 0 ? '-' : '.');
    r.append(prerelease_[i]);
  }

  for (size_t i = 0; i < build_metadata_.size(); ++i) {
    r.push_back(i == 0 ? '+' : '.');
    r.append(build_metadata_[i]);
  }

  return r;
}

int Semver::compare_prerelease(const Semver &other) const {
  // No pre-release > has pre-release
  if (prerelease_.empty() && !other.prerelease_.empty()) return 1;
  if (!prerelease_.empty() && other.prerelease_.empty()) return -1;
  if (prerelease_.empty() && other.prerelease_.empty()) return 0;

  // Compare identifier by identifier
  size_t min_size = std::min(prerelease_.size(), other.prerelease_.size());
  for (size_t i = 0; i < min_size; ++i) {
    const std::string &l = prerelease_[i];
    const std::string &r = other.prerelease_[i];

    bool l_numeric = is_numeric(l);
    bool r_numeric = is_numeric(r);

    if (l_numeric && r_numeric) {
      // Both numeric - compare numerically
      unsigned long l_val = std::stoul(l);
      unsigned long r_val = std::stoul(r);
      if (l_val < r_val) return -1;
      if (l_val > r_val) return 1;
    } else if (l_numeric && !r_numeric) {
      // Numeric < alphanumeric
      return -1;
    } else if (!l_numeric && r_numeric) {
      // Alphanumeric > numeric
      return 1;
    } else {
      // Both alphanumeric - compare lexically
      if (l < r) return -1;
      if (l > r) return 1;
    }
  }

  // All compared identifiers are equal, check length
  if (prerelease_.size() < other.prerelease_.size()) return -1;
  if (prerelease_.size() > other.prerelease_.size()) return 1;
  return 0;
}

bool Semver::operator==(const Semver &other) const {
  if (!valid_ || !other.valid_) return false;
  return major_ == other.major_ && minor_ == other.minor_ &&
         patch_ == other.patch_ && prerelease_ == other.prerelease_;
  // Build metadata is ignored per semver spec
}

bool Semver::operator!=(const Semver &other) const { return !(*this == other); }

bool Semver::operator<(const Semver &other) const {
  if (!valid_ || !other.valid_) return false;

  // Compare major.minor.patch
  if (major_ != other.major_) return major_ < other.major_;
  if (minor_ != other.minor_) return minor_ < other.minor_;
  if (patch_ != other.patch_) return patch_ < other.patch_;

  // Core versions are equal, compare pre-release
  return compare_prerelease(other) < 0;
}

bool Semver::operator<=(const Semver &other) const {
  return *this < other || *this == other;
}

bool Semver::operator>(const Semver &other) const { return !(*this <= other); }

bool Semver::operator>=(const Semver &other) const { return !(*this < other); }

}  // namespace villagesql
