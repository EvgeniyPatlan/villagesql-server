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

#include "villagesql/schema/systable/pending_action.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>

#include "sql/field.h"
#include "sql/sql_base.h"
#include "sql/table.h"
#include "villagesql/schema/systable/helpers.h"

#include "my_rapidjson_size_t.h"  // IWYU pragma: keep

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace villagesql {

namespace {

// Wire-stable string for the version_update kind. Changing this string is a
// data-format break; callers and tests should pin it.
constexpr const char kKindVersionUpdate[] = "version_update";

// Single source of truth for the column name carrying a PendingAction.
// Future versions of the table layout would consult schema state here.
constexpr const char kPendingActionColumn[] = "pending_action";

// Format the current UTC time as ISO-8601 with microseconds:
// "YYYY-MM-DDTHH:MM:SS.uuuuuuZ".
std::string CurrentTimestampUtc() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(now - seconds)
          .count();

  std::time_t t = clock::to_time_t(seconds);
  std::tm tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &t);
#else
  gmtime_r(&t, &tm_utc);
#endif

  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
                static_cast<long>(micros));
  return buf;
}

}  // namespace

PendingAction PendingAction::CreateVersionUpdate(
    std::string target_version, std::string target_veb_sha256) {
  PendingAction a;
  a.kind_ = Kind::kVersionUpdate;
  a.target_version_ = std::move(target_version);
  a.target_veb_sha256_ = std::move(target_veb_sha256);
  a.requested_at_ = CurrentTimestampUtc();
  return a;
}

void PendingAction::MarkFailed(std::string error_message) {
  last_error_ = std::move(error_message);
  last_error_at_ = CurrentTimestampUtc();
}

bool PendingAction::is_version_update() const {
  return kind_ == Kind::kVersionUpdate;
}

const std::string &PendingAction::target_version() const {
  return target_version_;
}

const std::string &PendingAction::target_veb_sha256() const {
  return target_veb_sha256_;
}

const std::string &PendingAction::requested_at() const { return requested_at_; }

const std::string &PendingAction::last_error() const { return last_error_; }

const std::string &PendingAction::last_error_at() const {
  return last_error_at_;
}

bool PendingAction::has_failure() const { return !last_error_.empty(); }

std::string PendingAction::Serialize() const {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);

  w.StartObject();
  w.Key("kind");
  switch (kind_) {
    case Kind::kVersionUpdate:
      w.String(kKindVersionUpdate);
      w.Key("target_version");
      w.String(target_version_.c_str(),
               static_cast<rapidjson::SizeType>(target_version_.size()));
      w.Key("target_veb_sha256");
      w.String(target_veb_sha256_.c_str(),
               static_cast<rapidjson::SizeType>(target_veb_sha256_.size()));
      break;
  }
  w.Key("requested_at");
  w.String(requested_at_.c_str(),
           static_cast<rapidjson::SizeType>(requested_at_.size()));
  if (!last_error_.empty()) {
    w.Key("last_error");
    w.String(last_error_.c_str(),
             static_cast<rapidjson::SizeType>(last_error_.size()));
    w.Key("last_error_at");
    w.String(last_error_at_.c_str(),
             static_cast<rapidjson::SizeType>(last_error_at_.size()));
  }
  w.EndObject();

  return sb.GetString();
}

bool PendingAction::ReadFromTable(TABLE &table,
                                  std::optional<PendingAction> &out,
                                  std::string &error_message) {
  Field *f = find_field_in_table_sef(&table, kPendingActionColumn);
  if (f == nullptr) {
    error_message = std::string("column '") + kPendingActionColumn +
                    "' not found in extensions table";
    return true;
  }
  if (f->is_null()) {
    out.reset();
    return false;
  }
  std::string raw;
  read_string_field(f, raw);
  PendingAction parsed;
  if (Deserialize(raw, parsed, error_message)) return true;
  out = std::move(parsed);
  return false;
}

std::string PendingAction::TargetVersionSqlExpr(const char *table_alias) {
  return std::string("JSON_UNQUOTE(JSON_EXTRACT(") + table_alias + "." +
         kPendingActionColumn + ", '$.target_version'))";
}

std::string PendingAction::RequestedAtSqlExpr(const char *table_alias) {
  return std::string("JSON_UNQUOTE(JSON_EXTRACT(") + table_alias + "." +
         kPendingActionColumn + ", '$.requested_at'))";
}

std::string PendingAction::LastErrorSqlExpr(const char *table_alias) {
  return std::string("JSON_UNQUOTE(JSON_EXTRACT(") + table_alias + "." +
         kPendingActionColumn + ", '$.last_error'))";
}

std::string PendingAction::LastErrorAtSqlExpr(const char *table_alias) {
  return std::string("JSON_UNQUOTE(JSON_EXTRACT(") + table_alias + "." +
         kPendingActionColumn + ", '$.last_error_at'))";
}

bool PendingAction::StoreToTable(TABLE &table,
                                 const std::optional<PendingAction> &value,
                                 std::string &error_message) {
  Field *f = find_field_in_table_sef(&table, kPendingActionColumn);
  if (f == nullptr) {
    error_message = std::string("column '") + kPendingActionColumn +
                    "' not found in extensions table";
    return true;
  }
  if (!value.has_value()) {
    f->set_null();
    return false;
  }
  const std::string raw = value->Serialize();
  f->set_notnull();
  f->store(raw.c_str(), raw.length(), &my_charset_utf8mb4_bin);
  return false;
}

bool PendingAction::Deserialize(const std::string &raw, PendingAction &out,
                                std::string &error_message) {
  rapidjson::Document doc;
  if (doc.Parse(raw.c_str(), raw.size()).HasParseError()) {
    error_message = std::string("pending_action JSON parse error: ") +
                    rapidjson::GetParseError_En(doc.GetParseError());
    return true;
  }
  if (!doc.IsObject()) {
    error_message = "pending_action JSON is not an object";
    return true;
  }

  const auto kind_it = doc.FindMember("kind");
  if (kind_it == doc.MemberEnd() || !kind_it->value.IsString()) {
    error_message = "pending_action JSON is missing string field 'kind'";
    return true;
  }
  const std::string kind_str = kind_it->value.GetString();

  PendingAction tmp;
  if (kind_str == kKindVersionUpdate) {
    tmp.kind_ = Kind::kVersionUpdate;

    const auto tv = doc.FindMember("target_version");
    if (tv == doc.MemberEnd() || !tv->value.IsString()) {
      error_message =
          "pending_action JSON kind 'version_update' is missing string field "
          "'target_version'";
      return true;
    }
    tmp.target_version_ = tv->value.GetString();

    const auto ts = doc.FindMember("target_veb_sha256");
    if (ts == doc.MemberEnd() || !ts->value.IsString()) {
      error_message =
          "pending_action JSON kind 'version_update' is missing string field "
          "'target_veb_sha256'";
      return true;
    }
    tmp.target_veb_sha256_ = ts->value.GetString();
  } else {
    error_message =
        std::string("pending_action JSON kind '") + kind_str + "' is unknown";
    return true;
  }

  const auto ra = doc.FindMember("requested_at");
  if (ra == doc.MemberEnd() || !ra->value.IsString()) {
    error_message =
        "pending_action JSON is missing string field 'requested_at'";
    return true;
  }
  tmp.requested_at_ = ra->value.GetString();

  // Optional failure record. Both fields must be present together; either
  // both absent (no failure recorded) or both present strings.
  const auto le = doc.FindMember("last_error");
  const auto lea = doc.FindMember("last_error_at");
  const bool le_present = le != doc.MemberEnd();
  const bool lea_present = lea != doc.MemberEnd();
  if (le_present != lea_present) {
    error_message =
        "pending_action JSON has only one of 'last_error' / 'last_error_at'";
    return true;
  }
  if (le_present) {
    if (!le->value.IsString() || !lea->value.IsString()) {
      error_message =
          "pending_action JSON 'last_error' / 'last_error_at' must be strings";
      return true;
    }
    tmp.last_error_ = le->value.GetString();
    tmp.last_error_at_ = lea->value.GetString();
  }

  out = std::move(tmp);
  return false;
}

}  // namespace villagesql
