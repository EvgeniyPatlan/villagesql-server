// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

#ifndef VILLAGESQL_VSQL_BACKGROUND_THREAD_H
#define VILLAGESQL_VSQL_BACKGROUND_THREAD_H

// background_thread — register/unregister extension background threads
//
// Usage:
//
//   #include <villagesql/vsql/background_thread.h>
//
//   static void worker_main() {
//     auto *handle = villagesql::background_thread::register_background_thread(
//         "myext/worker");
//
//     // ... do work ...
//
//     villagesql::background_thread::unregister_background_thread(handle);
//   }
//
// The thread will appear in INFORMATION_SCHEMA.PROCESSLIST and
// performance_schema.threads while registered.
//
// Must be called from the background thread itself after it starts.
// Requires Protocol 2.

#include <villagesql/abi/types.h>

namespace villagesql {
namespace background_thread {

// Extension-local storage for function pointers, set during vef_register()
// by vef_register_impl() in extension_builder.h.
inline vef_register_background_thread_func_t g_register_background_thread =
    nullptr;
inline vef_unregister_background_thread_func_t g_unregister_background_thread =
    nullptr;

// Register the calling thread with MySQL's process list and Performance Schema.
// Call from inside the background thread after it starts.
// Returns an opaque handle to pass to unregister_background_thread, or nullptr
// on failure.
inline vef_thread_handle_t *register_background_thread(
    const char *thread_name) {
  if (g_register_background_thread == nullptr) return nullptr;
  return g_register_background_thread(thread_name);
}

// Unregister the background thread. Must be called from inside the thread
// before it exits, after register_background_thread succeeded.
inline void unregister_background_thread(vef_thread_handle_t *handle) {
  if (g_unregister_background_thread != nullptr)
    g_unregister_background_thread(handle);
}

}  // namespace background_thread
}  // namespace villagesql

#endif  // VILLAGESQL_VSQL_BACKGROUND_THREAD_H
