// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is designed to work with certain software (including
// but not limited to OpenSSL) that is licensed under separate terms,
// as designated in a particular file or component or in included license
// documentation.  The authors of MySQL hereby grant you an additional
// permission to link the program and your derivative works with the
// separately licensed software that they have either included with
// the program or referenced in the documentation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

// VillageSQL audit log extension.
//
// Writes audit events to a JSON log file compatible with the MySQL Enterprise
// Audit log format. Hooks into connect, disconnect, and query execution phases.
//
// Install:
//   INSTALL EXTENSION 'vsql_audit_log';
//
// Configure:
//   SET GLOBAL vsql_audit_log.audit_log_file = '/var/log/vsql_audit.log';
//   SET GLOBAL vsql_audit_log.audit_log_policy = 'ALL';  -- ALL, LOGINS,
//   QUERIES, NONE
//
// Log format follows MySQL Enterprise Audit JSON schema so existing audit log
// parsers and tools work with the output.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <villagesql/vsql.h>

using namespace vsql;

// Configuration variables — written by MySQL on SET GLOBAL, read by hooks.
static char *g_log_file;
static char *g_policy;

static std::mutex g_log_mutex;

// Policy values (matches MySQL Enterprise Audit audit_log_policy).
static bool policy_log_logins() {
  return g_policy == nullptr ||
         strcmp(g_policy, "NONE") != 0 &&
             (strcmp(g_policy, "ALL") == 0 || strcmp(g_policy, "LOGINS") == 0);
}

static bool policy_log_queries() {
  return g_policy == nullptr ||
         strcmp(g_policy, "NONE") != 0 &&
             (strcmp(g_policy, "ALL") == 0 || strcmp(g_policy, "QUERIES") == 0);
}

// Write a single JSON audit record to the log file.
// The record is written as a single line followed by a newline, matching the
// MySQL audit log newline-delimited JSON format.
// Returns false on success, true on failure (sets result->error_msg).
static bool write_record(const char *json, vef_query_hook_result_t *result) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  FILE *f = fopen(g_log_file, "a");
  if (f == nullptr) {
    result->error_msg = "vsql_audit_log: failed to open log file";
    return true;
  }
  fprintf(f, "%s\n", json);
  fclose(f);
  return false;
}

// Format an ISO-8601 UTC timestamp into buf (must be at least 32 bytes).
// utime is microseconds since epoch; if 0, falls back to time(nullptr).
static void format_timestamp(char *buf, size_t buf_size, uint64_t utime) {
  time_t now =
      utime != 0 ? static_cast<time_t>(utime / 1000000) : time(nullptr);
  struct tm tm_buf;
  gmtime_r(&now, &tm_buf);
  strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

// Escape a string for JSON — replaces ", \, and control characters.
// Writes into buf (null-terminated). Truncates if necessary.
static void json_escape(const char *in, char *out, size_t out_size) {
  if (in == nullptr) {
    if (out_size > 0) out[0] = '\0';
    return;
  }
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j + 2 < out_size; i++) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == '"' || c == '\\') {
      if (j + 3 >= out_size) break;
      out[j++] = '\\';
      out[j++] = static_cast<char>(c);
    } else if (c < 0x20) {
      // Escape control characters as \uXXXX
      if (j + 7 >= out_size) break;
      snprintf(out + j, out_size - j, "\\u%04x", c);
      j += 6;
    } else {
      out[j++] = static_cast<char>(c);
    }
  }
  out[j] = '\0';
}

static void on_connect(vef_context_t * /*ctx*/, vef_query_hook_args_t *args,
                       vef_query_hook_result_t *result) {
  if (!policy_log_logins()) return;

  char ts[32];
  format_timestamp(ts, sizeof(ts), args->query_start_utime);

  char user[256], host[256], db[256];
  json_escape(args->user, user, sizeof(user));
  json_escape(args->host, host, sizeof(host));
  json_escape(args->schema, db, sizeof(db));

  char record[2048];
  snprintf(record, sizeof(record),
           "{\"timestamp\":\"%s\",\"class\":\"connection\","
           "\"event\":\"%s\","
           "\"connection_id\":%lu,"
           "\"account\":{\"user\":\"%s\",\"host\":\"%s\"},"
           "\"connection_data\":{\"status\":%d,\"db\":\"%s\"}}",
           ts, args->status == 0 ? "connect" : "failed_connect",
           args->connection_id, user, host, args->status, db);

  write_record(record, result);
}

static void on_disconnect(vef_context_t * /*ctx*/, vef_query_hook_args_t *args,
                          vef_query_hook_result_t *result) {
  if (!policy_log_logins()) return;

  char ts[32];
  format_timestamp(ts, sizeof(ts), args->query_start_utime);

  char user[256], host[256], db[256];
  json_escape(args->user, user, sizeof(user));
  json_escape(args->host, host, sizeof(host));
  json_escape(args->schema, db, sizeof(db));

  char record[2048];
  snprintf(record, sizeof(record),
           "{\"timestamp\":\"%s\",\"class\":\"connection\","
           "\"event\":\"disconnect\","
           "\"connection_id\":%lu,"
           "\"account\":{\"user\":\"%s\",\"host\":\"%s\"},"
           "\"connection_data\":{\"db\":\"%s\"}}",
           ts, args->connection_id, user, host, db);

  write_record(record, result);
}

static void on_query(vef_context_t * /*ctx*/, vef_query_hook_args_t *args,
                     vef_query_hook_result_t *result) {
  if (!policy_log_queries()) return;

  char ts[32];
  format_timestamp(ts, sizeof(ts), args->query_start_utime);

  char user[256], host[256], db[256];
  json_escape(args->user, user, sizeof(user));
  json_escape(args->host, host, sizeof(host));
  json_escape(args->schema, db, sizeof(db));

  // Escape query — may be large; truncate at 4096 chars.
  char query[4096];
  char query_trunc[4096];
  if (args->query != nullptr && args->query_len > 0) {
    size_t len = args->query_len < sizeof(query_trunc) - 1
                     ? args->query_len
                     : sizeof(query_trunc) - 1;
    memcpy(query_trunc, args->query, len);
    query_trunc[len] = '\0';
  } else {
    query_trunc[0] = '\0';
  }
  json_escape(query_trunc, query, sizeof(query));

  char record[8192];
  snprintf(record, sizeof(record),
           "{\"timestamp\":\"%s\",\"class\":\"general\","
           "\"event\":\"query\","
           "\"connection_id\":%lu,"
           "\"account\":{\"user\":\"%s\",\"host\":\"%s\"},"
           "\"general_data\":{\"query\":\"%s\",\"status\":%d,"
           "\"query_time\":%.6f,\"db\":\"%s\"}}",
           ts, args->connection_id, user, host, query, args->status,
           args->query_time_secs, db);

  write_record(record, result);
}

VEF_GENERATE_ENTRY_POINTS(
    make_extension("vsql_audit_log", "0.0.1")
        .query_hook(make_query_hook<VEF_QUERY_HOOK_CONNECT, &on_connect>())
        .query_hook(
            make_query_hook<VEF_QUERY_HOOK_DISCONNECT, &on_disconnect>())
        .query_hook(make_query_hook<VEF_QUERY_HOOK_POSTEXECUTE, &on_query>())
        .sys_var(make_sys_var_str("audit_log_file",
                                  "Path to the JSON audit log file",
                                  &g_log_file, "/tmp/vsql_audit.log"))
        .sys_var(make_sys_var_str(
            "audit_log_policy",
            "Events to log: ALL, LOGINS, QUERIES, NONE (default: ALL)",
            &g_policy, "ALL")))
