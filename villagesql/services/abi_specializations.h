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

#ifndef VILLAGESQL_SERVICES_ABI_SPECIALIZATIONS_H
#define VILLAGESQL_SERVICES_ABI_SPECIALIZATIONS_H

// Server-side specializations needed by the ABI structural-hash
// machinery in abi_signature_compute.h.  Pulled out of the
// extension-facing SDK trait headers so that the SDK can ship
// without dragging Boost.PFR (and its transitive dependencies) along.
//
// Two kinds of specializations live here:
//
//   1. VEF_ABI_REGISTER_OPAQUE_HANDLE(T): mark forward-declared
//      server-internal handle types as opaque so the hash machinery
//      hashes them by name only -- no sizeof, no PFR-walk.
//      Extensions never see the layout of these types; both sides of
//      the ABI must register the same name for the hashes to match.
//
//   2. Manual AbiStructFields<T> specializations for ABI struct types
//      that Boost.PFR cannot decompose -- typically aggregates
//      containing anonymous unions.  Hand-written field walks that
//      enumerate every named field (including each union arm) so a
//      change anywhere flips the hash.
//
// Both server's capability_registry.cc and the abi_signature gunit
// test include this header; the SDK trait headers do not.

#include <cstddef>
#include <type_traits>
#include <utility>

#include "villagesql/sdk/include/villagesql/abi/preview/sql_query.h"
#include "villagesql/sdk/include/villagesql/abi/preview/status_var.h"
#include "villagesql/sdk/include/villagesql/abi/preview/storage.h"
#include "villagesql/sdk/include/villagesql/abi/preview/sys_var.h"
#include "villagesql/sdk/include/villagesql/abi/preview/thread_worker.h"
#include "villagesql/services/abi_signature_compute.h"

// Opaque handle registrations.
//
// Each of these is a forward-declared server-side type that
// extensions only see by pointer:
//   - vef_thread_handle_t  (in abi/preview/thread_worker.h)
//     Reached through:
//       * vef_thread_worker_descriptor_t function-pointer fields
//       * vef_preview_sql_query_t::open_session's first parameter
//   - vef_storage_arena    (in abi/preview/storage.h)
//     Reached through vef_type_storage_intf_t function-pointer fields
//     when fingerprinting vef_preview_column_store_ext_desc_t.
//   - vef_sql_session_t    (in abi/preview/sql_query.h)
//   - vef_sql_result_t     (in abi/preview/sql_query.h)
//     Reached through vef_preview_sql_query_t's function-pointer
//     fields (session and result handles).
VEF_ABI_REGISTER_OPAQUE_HANDLE(vef_thread_handle_t);
VEF_ABI_REGISTER_OPAQUE_HANDLE(vef_storage_arena);
VEF_ABI_REGISTER_OPAQUE_HANDLE(vef_sql_session_t);
VEF_ABI_REGISTER_OPAQUE_HANDLE(vef_sql_result_t);

// Manual AbiStructFields specializations for ABI types that contain
// anonymous unions.  Boost.PFR cannot decompose aggregates whose
// fields include an anonymous union (decomposition uses structured
// bindings, which the language disallows for such aggregates), so we
// list each named field's type and each union arm's type explicitly.
//
// The hash still flips on any change to a field's type, any
// reordering, any addition or removal of a union arm, and any change
// inside an arm.  What we lose vs full PFR walking is the implicit
// invariant "the field list here exactly matches the declared
// struct" -- if a field is added to the C struct but not added to
// the manual spec below, the hash will no longer reflect the new
// field.  Comment on the C struct should point here to keep the two
// in sync.
//
// Anonymous-union members are scoped at the enclosing struct so we
// can reach them via `std::declval<T &>().member`; std::remove_cv +
// remove_reference strips the lvalue-reference that decltype yields.
namespace villagesql::detail {

namespace {
template <typename T>
using vef_member_type_t = std::remove_cv_t<std::remove_reference_t<T>>;
}  // namespace

template <>
struct AbiStructFields<vef_sys_var_change_t> {
  static constexpr std::size_t field_hash() {
    using D = vef_sys_var_change_t;
    std::size_t h = kAbiSeed;
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().var_name)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().type)>>());
    // Anonymous union arms (scalar types):
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().bool_val)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().int_val)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().dbl_val)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().str_val)>>());
    return h;
  }
};

template <>
struct AbiStructFields<vef_sys_var_desc_t> {
  static constexpr std::size_t field_hash() {
    using D = vef_sys_var_desc_t;
    std::size_t h = kAbiSeed;
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().name)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().comment)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().type)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().on_change)>>());
    // Anonymous union arms (each is itself an anonymous aggregate
    // struct -- PFR walks those fine since they have no inner union):
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().boolean)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().integer)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().dbl)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().str)>>());
    return h;
  }
};

template <>
struct AbiStructFields<vef_status_var_desc_t> {
  static constexpr std::size_t field_hash() {
    using D = vef_status_var_desc_t;
    std::size_t h = kAbiSeed;
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().name)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().type)>>());
    // Anonymous union arms (scalar pointer types):
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().integer_ptr)>>());
    h = abi_combine(
        h, abi_type_hash_raw<
               vef_member_type_t<decltype(std::declval<D &>().double_ptr)>>());
    return h;
  }
};

}  // namespace villagesql::detail

#endif  // VILLAGESQL_SERVICES_ABI_SPECIALIZATIONS_H
