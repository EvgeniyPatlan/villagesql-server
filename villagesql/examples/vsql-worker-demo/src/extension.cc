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

// VillageSQL worker demo extension.
//
// Demonstrates VEF Protocol 4 background thread registration:
//   - on_register captures the background thread service pointers from
//     vef_register_arg_t
//   - on_install starts a worker thread and registers it with MySQL's
//     process list via register_background_thread
//   - on_uninstall signals stop and joins, then the thread calls
//     unregister_background_thread before exiting
//   - The thread is visible in INFORMATION_SCHEMA.PROCESSLIST and
//     performance_schema.threads while the extension is installed
//
// Install:
//   INSTALL EXTENSION 'vsql_worker_demo';
//
// Verify:
//   SELECT id, user, state FROM information_schema.processlist
//   WHERE user = 'vef_worker';
//
// Uninstall:
//   UNINSTALL EXTENSION 'vsql_worker_demo';

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <villagesql/extension.h>

static vef_register_background_thread_func_t g_register_fn = nullptr;
static vef_unregister_background_thread_func_t g_unregister_fn = nullptr;

static std::thread g_worker_thread;
static std::atomic<bool> g_stop{false};

static void worker_main() {
  vef_thread_handle_t *handle = g_register_fn("vsql_worker_demo/main");

  while (!g_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (handle != nullptr) {
    g_unregister_fn(handle);
  }
}

static void capture_service_ptrs(vef_protocol_t negotiated,
                                 vef_register_arg_t *arg) {
  if (negotiated >= VEF_PROTOCOL_4) {
    g_register_fn = arg->register_background_thread;
    g_unregister_fn = arg->unregister_background_thread;
  }
}

static bool on_install(vef_context_t * /*ctx*/, char *error_msg) {
  if (g_register_fn == nullptr || g_unregister_fn == nullptr) {
    snprintf(error_msg, VEF_MAX_ERROR_LEN,
             "background thread service unavailable (protocol < 4)");
    return true;
  }
  g_stop.store(false, std::memory_order_relaxed);
  g_worker_thread = std::thread(worker_main);
  return false;
}

static void on_uninstall(vef_context_t * /*ctx*/) {
  g_stop.store(true, std::memory_order_relaxed);
  if (g_worker_thread.joinable()) {
    g_worker_thread.join();
  }
}

using namespace villagesql::extension_builder;

VEF_GENERATE_ENTRY_POINTS(make_extension("vsql_worker_demo", "0.0.1")
                              .on_register(&capture_service_ptrs)
                              .on_install(&on_install)
                              .on_uninstall(&on_uninstall))
