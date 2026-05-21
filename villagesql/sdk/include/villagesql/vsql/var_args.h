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

#ifndef VILLAGESQL_VSQL_VAR_ARGS_H
#define VILLAGESQL_VSQL_VAR_ARGS_H

// Typed wrappers for varargs VDFs.
//
// A varargs VDF is declared at registration time with .varargs() on the
// func builder and takes a vsql::VarArgs argument in place of the usual
// fixed-arity IntArg/StringArg/CustomArg parameters. The body inspects
// each argument's runtime type via AnyArg and dispatches accordingly.
//
// Example:
//
//   void concat_all(vsql::VarArgs args, vsql::StringResult out) {
//     auto dst = out.buffer();
//     size_t off = 0;
//     for (auto a : args) {
//       if (a.is_null()) { out.set_null(); return; }
//       auto bytes = a.as_custom();
//       memcpy(dst.data() + off, bytes.data(), bytes.size());
//       off += bytes.size();
//     }
//     out.set_length(off);
//   }
//
//   make_func<&concat_all>("concat_all")
//       .returns(STRING)
//       .varargs()
//       .prerun<&concat_all_prerun>()
//       .build();
//
// The prerun is responsible for validating argument count and types
// (the framework cannot validate either for varargs).
//
// Varargs registration requires VEF_PROTOCOL_2 — older servers do not
// recognise the VEF_PARAM_VARARGS sentinel.

#include <cstddef>
#include <string_view>

#include <villagesql/abi/types.h>
#include <villagesql/vsql/func_types.h>
#include <villagesql/vsql/type_params.h>

namespace vsql {

// AnyArg: view over one vef_invalue_t whose type is not known at compile
// time. Used inside a varargs VDF body where each argument may be a
// different SQL type. Always check type() (or is_int/is_real/is_str/
// is_custom) before reading the typed value.
//
// Parity with classic MySQL UDFs
// ==============================
//
// VEF varargs VDFs are strictly less ergonomic than UDFs on a few axes
// because the prerun hook is observation-only — the server populates the
// fields and never reads them back. Specifically, none of these UDF
// features exist in VEF today:
//
//   1. Input-type coercion. UDF xxx_init can write back to
//      UDF_ARGS::arg_type[i] and the server coerces inputs accordingly
//      (e.g., the body sees a long long when init asked for INT_RESULT,
//      even if the SQL caller wrote the string "123"). VEF prerun's
//      arg_types is read-only; an AnyArg reports whatever natural type
//      the caller passed, and the body must dispatch on type() or the
//      prerun must reject mismatches and require CAST.
//
//   2. Per-argument nullability hints (UDF_ARGS::maybe_null). UDF init
//      sees which args may be NULL at this call site, useful for picking
//      a fast path. VEF prerun has no equivalent.
//
//   3. Output-shape hints surfaced to the planner: UDF_INIT::maybe_null,
//      max_length, decimals. VEF prerun has result_buffer_size (a
//      buffer-allocation request, not a planner hint) but no maybe_null
//      or max_length signal that propagates to query planning.
//
//   4. Per-call-site determinism. UDF_INIT::const_item lets init declare
//      "for this call, my output is constant" (e.g., when all args are
//      constants). VEF has only registration-time .deterministic(); it
//      cannot upgrade or downgrade per call site.
//
//   5. Argument-expression attributes (UDF_ARGS::attributes /
//      attribute_lengths). UDF init can read the textual form of each
//      argument expression — used by a few UDFs that want to know which
//      column was passed. VEF prerun does not expose this.
//
// TODO(villagesql-beta): consider letting prerun rewrite arg_types[i] so
// the server can coerce varargs inputs the way UDF init does. The other
// gaps (2)-(5) are smaller and likely belong in a separate prerun-result
// expansion.
class AnyArg {
 public:
  explicit AnyArg(const vef_invalue_t *v) : v_(v) {}

  bool is_null() const { return v_->is_null; }

  vef_type_id type() const { return v_->type; }

  bool is_int() const { return v_->type == VEF_TYPE_INT; }
  bool is_real() const { return v_->type == VEF_TYPE_REAL; }
  bool is_str() const { return v_->type == VEF_TYPE_STRING; }
  bool is_custom() const { return v_->type == VEF_TYPE_CUSTOM; }

  // Precondition: is_int().
  long long as_int() const { return v_->int_value; }
  // Precondition: is_real().
  double as_real() const { return v_->real_value; }
  // Precondition: is_str().
  std::string_view as_str() const { return {v_->str_value, v_->str_len}; }
  // Precondition: is_custom().
  Span<const unsigned char> as_custom() const {
    return {v_->bin_value, v_->bin_len};
  }

  // Precondition: is_custom() and the type was registered with
  // .params<P, &parse_fn>(). Returns the parsed type parameters from the
  // per-process cache.
  template <typename P>
  const P &custom_params() const {
    return type_params_cache_for<P>().get(v_->type_params);
  }

 private:
  const vef_invalue_t *v_;
};

// VarArgs: view over vef_vdf_args_t for varargs VDFs. Provides size,
// indexed access, and range-for iteration. The values array is always
// the protocol-2 pointer layout (args->values) because varargs registration
// requires VEF_PROTOCOL_2.
class VarArgs {
 public:
  explicit VarArgs(vef_vdf_args_t *a) : a_(a) {}

  size_t size() const { return a_->value_count; }

  AnyArg operator[](size_t i) const { return AnyArg(a_->values[i]); }

  // Read user_data stashed by prerun. Returns nullptr if prerun did not
  // set user_data (or no prerun is registered).
  template <typename T>
  T *state() const {
    return static_cast<T *>(a_->user_data);
  }

  class Iter {
   public:
    Iter(const VarArgs *v, size_t i) : v_(v), i_(i) {}
    AnyArg operator*() const { return (*v_)[i_]; }
    Iter &operator++() {
      ++i_;
      return *this;
    }
    bool operator!=(const Iter &o) const { return i_ != o.i_; }

   private:
    const VarArgs *v_;
    size_t i_;
  };

  Iter begin() const { return Iter(this, 0); }
  Iter end() const { return Iter(this, a_->value_count); }

 private:
  vef_vdf_args_t *a_;
};

}  // namespace vsql

#endif  // VILLAGESQL_VSQL_VAR_ARGS_H
