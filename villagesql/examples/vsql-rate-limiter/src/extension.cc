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

// VillageSQL rate limiter extension.
//
// Two-tier per-user query rate limiting within a rolling time window:
//
//   Tier 1 — soft limit (max_queries): block at POSTPARSE. The query has
//   already been parsed so sql_command is available, but execution is
//   prevented. Cost: one parse per blocked query.
//
//   Tier 2 — hard limit (max_queries_hard): block at PREPARSE. No parsing
//   occurs at all. Intended for users far exceeding their quota.
//
//   max_queries_hard must be >= max_queries. If set lower it is treated as
//   equal to max_queries (i.e. all blocking happens at PREPARSE).
//
// Install:
//   INSTALL EXTENSION 'vsql_rate_limiter';
//
// Configure:
//   SET GLOBAL vsql_rate_limiter.enabled = ON;                  -- OFF by
//   default SET GLOBAL vsql_rate_limiter.max_queries_post_parse = 100;  -- soft
//   limit SET GLOBAL vsql_rate_limiter.max_queries_pre_parse = 200;   -- hard
//   limit SET GLOBAL vsql_rate_limiter.window_secs = 60;              --
//   rolling window
//
// Reset all per-user counters:
//   SELECT vsql_rate_limiter.reset();

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <villagesql/vsql.h>

using namespace vsql;

static bool g_enabled;
static long long g_max_queries_post_parse;
static long long g_max_queries_pre_parse;
static long long g_window_secs;

// Per-user state: query count in the current window and window start time
// (seconds since epoch).
struct UserBucket {
  long long count;
  long long window_start;
};

static std::mutex g_mutex;
static std::unordered_map<std::string, UserBucket> g_buckets;

// Returns the current bucket count for the user, advancing the window if
// needed and incrementing the counter. Must be called with g_mutex held.
static long long advance_and_count(const char *user, long long now) {
  auto &bucket = g_buckets[user];
  if (now - bucket.window_start >= g_window_secs) {
    bucket.count = 0;
    bucket.window_start = now;
  }
  return ++bucket.count;
}

static long long now_secs() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// PREPARSE hook — blocks users who have exceeded the hard limit.
// Called before parsing; only query text and connection info are available.
static void on_preparse(const QueryHookArgs &args, QueryHookResult &result) {
  if (!g_enabled) return;
  if (args.user() == nullptr) return;

  long long pre_limit = g_max_queries_pre_parse;
  long long post_limit = g_max_queries_post_parse;
  if (pre_limit < post_limit) pre_limit = post_limit;

  std::lock_guard<std::mutex> lock(g_mutex);
  // Peek at the current count without incrementing — the POSTPARSE hook will
  // do the actual increment. If already over the pre-parse limit, block now.
  auto it = g_buckets.find(args.user());
  if (it == g_buckets.end()) return;
  long long count = it->second.count;
  long long age = now_secs() - it->second.window_start;
  if (age >= g_window_secs) return;  // window has expired, let POSTPARSE handle
  if (count >= pre_limit) {
    result.block("Query rate limit exceeded (pre-parse limit)");
  }
}

// POSTPARSE hook — increments the counter and blocks users who have exceeded
// the soft limit. Called after parsing so sql_command is available.
static void on_postparse(const QueryHookArgs &args, QueryHookResult &result) {
  if (!g_enabled) return;
  if (args.user() == nullptr) return;

  std::lock_guard<std::mutex> lock(g_mutex);
  long long count = advance_and_count(args.user(), now_secs());
  if (count > g_max_queries_post_parse) {
    result.block("Query rate limit exceeded");
  }
}

// vsql_rate_limiter.reset() — clears all per-user counters.
// Returns 1 always (INT result required by VDF).
static void reset(IntResult out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_buckets.clear();
  out.set(1);
}

VEF_GENERATE_ENTRY_POINTS(
    make_extension("vsql_rate_limiter", "0.0.1")
        .query_hook(make_query_hook<VEF_QUERY_HOOK_PREPARSE, &on_preparse>())
        .query_hook(make_query_hook<VEF_QUERY_HOOK_POSTPARSE, &on_postparse>())
        .func(make_func<&reset>("reset").returns(INT).build())
        .sys_var(make_sys_var_bool("enabled",
                                   "Enable or disable the rate limiter "
                                   "(default: OFF)",
                                   &g_enabled, false))
        .sys_var(make_sys_var_int("max_queries_post_parse",
                                  "Queries per window before blocking at "
                                  "POSTPARSE (default: 100)",
                                  &g_max_queries_post_parse, 100, 1, 1000000))
        .sys_var(make_sys_var_int("max_queries_pre_parse",
                                  "Queries per window before blocking at "
                                  "PREPARSE (default: 200)",
                                  &g_max_queries_pre_parse, 200, 1, 1000000))
        .sys_var(make_sys_var_int("window_secs",
                                  "Rolling window size in seconds "
                                  "(default: 60)",
                                  &g_window_secs, 60, 1, 86400)))
