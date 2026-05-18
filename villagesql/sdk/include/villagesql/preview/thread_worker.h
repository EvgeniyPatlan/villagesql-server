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

// =============================================================================
// PREVIEW CAPABILITY — UNSTABLE API
// =============================================================================
// This header is part of the VEF preview surface. Its API and ABI may change
// or be removed without notice. See villagesql/preview/README.md for details.
// =============================================================================

#ifndef VILLAGESQL_PREVIEW_THREAD_WORKER_H
#define VILLAGESQL_PREVIEW_THREAD_WORKER_H

#include <chrono>
#include <limits>
#include <type_traits>

#include <villagesql/abi/preview/thread_worker.h>
#include <villagesql/detail/capability_base.h>
#include <villagesql/detail/capability_traits.h>

namespace vsql::preview_thread_worker {

// Reason the worker is being woken up. Mirrors vef_wakeup_reason_t but as
// an enum class so switches over it are exhaustive and constants are scoped.
enum class WakeupReason {
  Enable = VEF_WAKEUP_ENABLE,
  Periodic = VEF_WAKEUP_PERIODIC,
  PollFd = VEF_WAKEUP_POLL_FD,
  Disable = VEF_WAKEUP_DISABLE,
};

// Opaque handle for the current worker thread. Pass to capability open()
// methods (e.g. SqlQueryCapability::open). Not directly usable by extension
// code.
class ThreadHandle {
 public:
  // True if the handle is valid (non-null). Enable wakeups receive an
  // invalid handle — do not pass it to capability open() methods on Enable.
  explicit operator bool() const noexcept { return raw_ != nullptr; }

  // Internal use — capabilities call this to unwrap. Not for extension code.
  vef_thread_handle_t *raw_handle() const noexcept { return raw_; }

  ThreadHandle() = default;
  explicit ThreadHandle(vef_thread_handle_t *raw) noexcept : raw_(raw) {}

 private:
  vef_thread_handle_t *raw_{nullptr};
};

// Returned by the work function to schedule the next wakeup. Use the named
// constructors below to make the intent obvious at the call site.
class NextWakeup {
 public:
  // Sleep for `d` then wake periodically. Keeps any existing poll_fd unless
  // one is set explicitly via on_fd().
  static NextWakeup in(std::chrono::milliseconds d) noexcept {
    NextWakeup w;
    w.raw_.sleep_ms = duration_to_sleep_ms(d);
    w.raw_.poll_fd = 0;
    return w;
  }

  // Wake when `fd` becomes readable. Clears periodic sleep unless combined
  // with .also_in().
  static NextWakeup on_fd(int fd) noexcept {
    NextWakeup w;
    w.raw_.sleep_ms = 0;
    w.raw_.poll_fd = fd;
    return w;
  }

  // No change — keep the current sleep/poll configuration. Equivalent to
  // returning a default-constructed vef_next_wakeup_t.
  static NextWakeup done() noexcept { return NextWakeup{}; }

  // Combine: also wake periodically every `d` while waiting on the fd.
  NextWakeup &also_in(std::chrono::milliseconds d) noexcept {
    raw_.sleep_ms = duration_to_sleep_ms(d);
    return *this;
  }

  // Internal: hand back the C ABI representation.
  vef_next_wakeup_t raw() const noexcept { return raw_; }

 private:
  static unsigned int duration_to_sleep_ms(
      std::chrono::milliseconds d) noexcept {
    const auto count = d.count();
    if (count <= 0) return 0;
    if (count > std::numeric_limits<unsigned int>::max())
      return std::numeric_limits<unsigned int>::max();
    return static_cast<unsigned int>(count);
  }

  vef_next_wakeup_t raw_{};
};

// A wakeup event passed to the user's work function. Use Wakeup<> for
// stateless workers, and Wakeup<T> for stateful workers registered with an
// arg of type T. In the typed variant, w.arg() returns T& with no casts.
template <typename T = void>
class Wakeup {
 public:
  WakeupReason reason() const noexcept { return reason_; }
  ThreadHandle handle() const noexcept { return ThreadHandle{handle_}; }

  // Typed access to the registered arg. Available only when T != void.
  template <typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
  U &arg() const noexcept {
    return *static_cast<U *>(arg_);
  }

  Wakeup(WakeupReason r, vef_thread_handle_t *h, void *a) noexcept
      : reason_(r), handle_(h), arg_(a) {}

 private:
  WakeupReason reason_;
  vef_thread_handle_t *handle_;
  void *arg_;
};

// ThreadWorkerCapability<WorkFn> is a concrete capability wrapper for the
// "vsql::preview::thread_worker" preview capability. The work function is a
// template parameter so each distinct work function gets its own static
// descriptor.
//
// The work function's signature determines whether the capability is
// stateful or stateless:
//
//   NextWakeup my_work(Wakeup<> w);           // stateless
//   NextWakeup my_work(Wakeup<MyState> w);    // stateful — pass state
//
// Usage (stateless):
//   static vsql::NextWakeup my_work(vsql::Wakeup<> w) { ... }
//   static vsql::preview_thread_worker::ThreadWorkerCapability<&my_work>
//       g_cap{"suffix"};
//
// Usage (stateful):
//   struct MyState { ... };
//   static MyState g_state;
//   static vsql::NextWakeup my_work(vsql::Wakeup<MyState> w) {
//     MyState &s = w.arg();
//     ...
//   }
//   static vsql::preview_thread_worker::ThreadWorkerCapability<&my_work>
//       g_cap{"suffix", g_state};
//
//   VEF_GENERATE_ENTRY_POINTS(make_extension().with(g_cap))

namespace detail {

// Extract the arg type T from a work function of signature
//   NextWakeup (*)(Wakeup<T>)
// (T = void for the stateless overload Wakeup<void>, i.e. plain Wakeup).
template <typename Fn>
struct WorkFnTraits;

template <typename T>
struct WorkFnTraits<NextWakeup (*)(Wakeup<T>)> {
  using ArgType = T;
};

template <typename T>
struct WorkFnTraits<NextWakeup (*)(Wakeup<T>) noexcept> {
  using ArgType = T;
};

}  // namespace detail

template <auto WorkFn>
class ThreadWorkerCapability
    : public ::vsql::detail::CapabilityBase<ThreadWorkerCapability<WorkFn>> {
 public:
  using ArgType = typename detail::WorkFnTraits<decltype(WorkFn)>::ArgType;

  // Constructor for stateless workers (work fn takes Wakeup, not Wakeup<T>).
  template <typename U = ArgType, std::enable_if_t<std::is_void_v<U>, int> = 0>
  ThreadWorkerCapability(const char *suffix,
                         const char *var_name = nullptr) noexcept;

  // Constructor for stateful workers. `state` must remain alive for the
  // lifetime of the extension.
  template <typename U = ArgType, std::enable_if_t<!std::is_void_v<U>, int> = 0>
  ThreadWorkerCapability(const char *suffix, U &state,
                         const char *var_name = nullptr) noexcept;

  // One static descriptor per WorkFn instantiation. The constructor
  // populates it. The trait's capability_config() returns its address so
  // the wire format carries a pointer to it.
  // TODO(villagesql-beta): rename `descriptor` to `cc` to match the
  // capability_config naming.
  static inline vef_thread_worker_descriptor_t descriptor{};

 private:
  template <typename Capability>
  friend struct ::vsql::detail::CapabilityTraits;

  const vef_preview_thread_worker_t *abi_ = nullptr;
};

}  // namespace vsql::preview_thread_worker

// Re-export the user-facing types into the top-level vsql namespace so
// extensions can write `vsql::Wakeup`, `vsql::NextWakeup`, etc.
namespace vsql {
using preview_thread_worker::NextWakeup;
using preview_thread_worker::ThreadHandle;
using preview_thread_worker::Wakeup;
using preview_thread_worker::WakeupReason;
}  // namespace vsql

#include <villagesql/preview/detail/thread_worker_register.h>
#include <villagesql/preview/thread_worker_impl.h>

#endif  // VILLAGESQL_PREVIEW_THREAD_WORKER_H
