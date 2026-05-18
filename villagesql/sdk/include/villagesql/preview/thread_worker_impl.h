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
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VILLAGESQL_PREVIEW_THREAD_WORKER_IMPL_H
#define VILLAGESQL_PREVIEW_THREAD_WORKER_IMPL_H

#include <villagesql/preview/thread_worker.h>

namespace vsql::preview_thread_worker {

namespace detail {

// C trampoline: adapts vef_work_fn_t to a typed C++ work function that
// takes Wakeup<T>. The `arg` carried in vef_thread_worker_descriptor_t is
// stored as void* and reinterpreted here.
template <auto WorkFn>
inline vef_next_wakeup_t work_fn_trampoline(vef_wakeup_reason_t reason,
                                            struct vef_thread_handle_t *thread,
                                            void *arg) {
  using ArgType = typename WorkFnTraits<decltype(WorkFn)>::ArgType;
  Wakeup<ArgType> w{static_cast<WakeupReason>(reason), thread, arg};
  return WorkFn(w).raw();
}

}  // namespace detail

template <auto WorkFn>
template <typename U, std::enable_if_t<std::is_void_v<U>, int>>
inline ThreadWorkerCapability<WorkFn>::ThreadWorkerCapability(
    const char *suffix, const char *var_name) noexcept {
  descriptor.work_fn = &detail::work_fn_trampoline<WorkFn>;
  descriptor.arg = nullptr;
  descriptor.sleep_ms = 0;
  descriptor.suffix = suffix;
  descriptor.var_name = var_name;
}

template <auto WorkFn>
template <typename U, std::enable_if_t<!std::is_void_v<U>, int>>
inline ThreadWorkerCapability<WorkFn>::ThreadWorkerCapability(
    const char *suffix, U &state, const char *var_name) noexcept {
  descriptor.work_fn = &detail::work_fn_trampoline<WorkFn>;
  descriptor.arg = static_cast<void *>(&state);
  descriptor.sleep_ms = 0;
  descriptor.suffix = suffix;
  descriptor.var_name = var_name;
}

}  // namespace vsql::preview_thread_worker

#endif  // VILLAGESQL_PREVIEW_THREAD_WORKER_IMPL_H
