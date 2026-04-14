// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VILLAGESQL_SDK_QUERY_HOOK_BUILDER_H
#define VILLAGESQL_SDK_QUERY_HOOK_BUILDER_H

// Query Hook Builder - Declare query lifecycle hooks
//
// Usage (in extension registration):
//
//   make_extension("myext", "1.0")
//     .query_hook(make_query_hook<VEF_QUERY_HOOK_POSTEXECUTE, &my_hook>())
//
// Available phases (see vef_query_hook_phase_t for full documentation):
//   VEF_QUERY_HOOK_PREPARSE     - before parsing
//   VEF_QUERY_HOOK_POSTPARSE    - after parsing, before execution
//   VEF_QUERY_HOOK_POSTEXECUTE  - after execution
//   VEF_QUERY_HOOK_CONNECT      - new client connection
//   VEF_QUERY_HOOK_DISCONNECT   - client disconnection
//
// Hook functions use the typed C++ signature:
//   void my_hook(const vsql::QueryHookArgs &args, vsql::QueryHookResult
//   &result)

#include <cstddef>
#include <cstring>
#include <string_view>

#include <villagesql/abi/types.h>

namespace villagesql {
namespace query_hook_builder {

// =============================================================================
// Typed wrappers for hook arguments and result
// =============================================================================

// Read-only view of all fields available in a query hook invocation.
// Which fields are populated depends on the phase — see vef_query_hook_args_t.
class QueryHookArgs {
 public:
  explicit QueryHookArgs(const vef_query_hook_args_t *a) : a_(a) {}

  vef_query_hook_phase_t phase() const { return a_->phase; }

  // Available for PREPARSE, POSTPARSE, POSTEXECUTE. Empty for
  // CONNECT/DISCONNECT.
  std::string_view query() const {
    return a_->query ? std::string_view(a_->query, a_->query_len)
                     : std::string_view{};
  }

  // Available for all phases.
  const char *user() const { return a_->user; }
  const char *host() const { return a_->host; }
  unsigned long connection_id() const { return a_->connection_id; }
  uint16_t port() const { return a_->port; }
  bool in_transaction() const { return a_->in_transaction; }
  const char *schema() const { return a_->schema; }

  // POSTPARSE and POSTEXECUTE only. 0 otherwise.
  int sql_command() const { return a_->sql_command; }

  // POSTPARSE only.
  bool is_prepared() const { return a_->is_prepared; }
  // SHA-256 digest of the normalized query, or nullptr.
  const unsigned char *digest() const { return a_->digest; }

  // POSTEXECUTE only.
  int status() const { return a_->status; }
  const char *sqlstate() const { return a_->sqlstate; }
  const char *error_message() const { return a_->error_message; }
  uint64_t query_start_utime() const { return a_->query_start_utime; }
  double query_time_secs() const { return a_->query_time_secs; }
  double lock_time_secs() const { return a_->lock_time_secs; }
  uint64_t rows_sent() const { return a_->rows_sent; }
  uint64_t rows_examined() const { return a_->rows_examined; }
  uint64_t rows_affected() const { return a_->rows_affected; }
  uint64_t bytes_sent() const { return a_->bytes_sent; }
  uint64_t bytes_received() const { return a_->bytes_received; }

 private:
  const vef_query_hook_args_t *a_;
};

// Writable result for a hook invocation.
// For POSTEXECUTE and DISCONNECT all fields are ignored by the server.
class QueryHookResult {
 public:
  explicit QueryHookResult(vef_query_hook_result_t *r) : r_(r) {}

  // Rewrite the query text (PREPARSE / POSTPARSE only). The server copies the
  // string before the hook returns, so the pointed-to memory need not outlive
  // the call.
  void rewrite(const char *query, size_t len) {
    r_->rewritten_query = query;
    r_->rewritten_len = len;
  }
  void rewrite(std::string_view sv) { rewrite(sv.data(), sv.size()); }

  // Block the query / refuse the connection with an error message.
  // Ignored for POSTEXECUTE and DISCONNECT.
  void block(const char *msg) { r_->error_msg = msg; }
  void block(std::string_view sv) { r_->error_msg = sv.data(); }

 private:
  vef_query_hook_result_t *r_;
};

// =============================================================================
// ABI shim for typed hook functions
// =============================================================================

// Generates an ABI-compatible vef_query_hook_func_t that calls a typed hook
// void Fn(const QueryHookArgs&, QueryHookResult&).
template <auto Fn>
void typed_hook_shim(vef_context_t * /*ctx*/, vef_query_hook_args_t *args,
                     vef_query_hook_result_t *result) {
  QueryHookArgs typed_args(args);
  QueryHookResult typed_result(result);
  Fn(typed_args, typed_result);
}

// =============================================================================
// QueryHookDescriptor and make_query_hook
// =============================================================================

// Wraps a single vef_query_hook_desc_t by value so the builder can store it
// in a compile-time tuple.
struct QueryHookDescriptor {
  vef_query_hook_desc_t desc;
};

// Typed variant (preferred): hook signature is
//   void Fn(const QueryHookArgs&, QueryHookResult&)
template <vef_query_hook_phase_t Phase,
          void (*Fn)(const QueryHookArgs &, QueryHookResult &)>
constexpr QueryHookDescriptor make_query_hook() {
  QueryHookDescriptor d{};
  d.desc.phase = Phase;
  d.desc.hook = &typed_hook_shim<Fn>;
  return d;
}

// Raw ABI variant (backwards compatibility): hook signature is
//   void Fn(vef_context_t*, vef_query_hook_args_t*, vef_query_hook_result_t*)
template <vef_query_hook_phase_t Phase, vef_query_hook_func_t Fn>
constexpr QueryHookDescriptor make_query_hook() {
  QueryHookDescriptor d{};
  d.desc.phase = Phase;
  d.desc.hook = Fn;
  return d;
}

}  // namespace query_hook_builder
}  // namespace villagesql

#endif  // VILLAGESQL_SDK_QUERY_HOOK_BUILDER_H
