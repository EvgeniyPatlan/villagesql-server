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

// VillageSQL slow query log extension.
//
// Logs queries exceeding a configurable threshold to a log file using an
// async ring buffer: query threads format and enqueue entries without doing
// any file I/O; a dedicated writer thread drains the buffer to disk.
//
// Install:
//   INSTALL EXTENSION 'vsql_slow_query_log';
//
// Configure (all variables are effective immediately except buffer_slots,
// which is fixed for the lifetime of the installed extension):
//   SET GLOBAL vsql_slow_query_log.threshold_ms = 1000;
//   SET GLOBAL vsql_slow_query_log.log_file = '/var/log/vsql_slow.log';
//   SET GLOBAL vsql_slow_query_log.buffer_slots = 1024;  -- before INSTALL

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <villagesql/extension.h>

// =============================================================================
// Configuration variables — written by MySQL on SET GLOBAL, read by hook/worker
// =============================================================================

static long long g_threshold_ms = 1000;
static char *g_log_file = nullptr;
// Number of ring buffer slots. Read once at on_install; changing it at runtime
// has no effect until the extension is reinstalled.
static long long g_buffer_slots = 1024;

static const char *kDefaultLogFile = "/tmp/vsql_slow_query.log";

// Maximum length of a single formatted log entry (bytes).
static constexpr size_t kSlotTextSize = 1024;

// =============================================================================
// Ring buffer
// =============================================================================

enum class SlotState : uint8_t { Empty = 0, Ready = 1 };

struct LogSlot {
  std::atomic<SlotState> state{SlotState::Empty};
  char text[kSlotTextSize];
  size_t len{0};
};

// Allocated once in on_install. Raw array because std::atomic is not
// move-constructible, which prevents std::vector from resizing.
static LogSlot *g_slots = nullptr;
static size_t g_num_slots = 0;

// head: next slot index for query threads to claim (wraps mod g_num_slots).
static std::atomic<uint64_t> g_head{0};
// tail: next slot index for the writer thread to drain.
static uint64_t g_tail = 0;  // only written by writer thread

static std::atomic<uint64_t> g_dropped_count{0};

// Wake the writer when new entries are available.
static std::mutex g_wake_mutex;
static std::condition_variable g_wake_cv;

// =============================================================================
// Config snapshot for the worker (protects access to g_log_file copy)
// =============================================================================

static std::mutex g_config_mutex;

static std::string snapshot_log_path() {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  const char *p = (g_log_file && g_log_file[0]) ? g_log_file : kDefaultLogFile;
  return std::string(p);
}

// =============================================================================
// Background thread
// =============================================================================

static vef_register_background_thread_func_t g_register_fn = nullptr;
static vef_unregister_background_thread_func_t g_unregister_fn = nullptr;

static std::thread g_writer_thread;
static std::atomic<bool> g_stop{false};

static void writer_main() {
  vef_thread_handle_t *handle = g_register_fn("vsql_slow_query_log/writer");

  std::string open_path;
  FILE *f = nullptr;

  auto ensure_file_open = [&](const std::string &path) {
    if (path == open_path && f != nullptr) return;
    if (f != nullptr) fclose(f);
    f = fopen(path.c_str(), "a");
    open_path = path;
  };

  while (true) {
    // Wait for work or stop signal.
    {
      std::unique_lock<std::mutex> lk(g_wake_mutex);
      g_wake_cv.wait_for(lk, std::chrono::milliseconds(100), [] {
        return g_stop.load(std::memory_order_relaxed) ||
               g_head.load(std::memory_order_relaxed) != g_tail;
      });
    }

    // Snapshot config once per drain cycle.
    std::string current_path = snapshot_log_path();
    ensure_file_open(current_path);

    // Drain all ready slots.
    while (true) {
      uint64_t head = g_head.load(std::memory_order_acquire);
      if (g_tail == head) break;

      LogSlot &slot = g_slots[g_tail % g_num_slots];
      if (slot.state.load(std::memory_order_acquire) != SlotState::Ready) {
        // Query thread claimed the slot but hasn't finished writing yet.
        break;
      }

      if (f != nullptr) {
        fwrite(slot.text, 1, slot.len, f);
      }

      slot.state.store(SlotState::Empty, std::memory_order_release);
      g_tail++;
    }

    // Report any dropped entries.
    uint64_t dropped = g_dropped_count.exchange(0, std::memory_order_relaxed);
    if (dropped > 0 && f != nullptr) {
      fprintf(f, "# Dropped: %llu entries (buffer full)\n",
              (unsigned long long)dropped);
    }

    if (f != nullptr) fflush(f);

    if (g_stop.load(std::memory_order_relaxed) &&
        g_head.load(std::memory_order_relaxed) == g_tail) {
      break;
    }
  }

  if (f != nullptr) fclose(f);
  if (handle != nullptr) g_unregister_fn(handle);
}

// =============================================================================
// Hook — runs on query thread, no file I/O
// =============================================================================

static void slow_query_hook(vef_context_t * /*ctx*/,
                            vef_query_hook_args_t *args,
                            vef_query_hook_result_t * /*result*/) {
  if (args->query_time_secs * 1000.0 < static_cast<double>(g_threshold_ms))
    return;

  // Check if the buffer is full before claiming a slot. g_tail is only written
  // by the writer thread so reading it here may be slightly stale, but that
  // only means we occasionally drop an entry that could have fit — acceptable.
  uint64_t head = g_head.load(std::memory_order_relaxed);
  if (head - g_tail >= g_num_slots) {
    g_dropped_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Claim a slot atomically.
  uint64_t idx = g_head.fetch_add(1, std::memory_order_relaxed);
  LogSlot &slot = g_slots[idx % g_num_slots];

  // Format the entry directly into the slot while it is in Empty state.
  // Mark Ready only after the text is fully written.
  time_t now = time(nullptr);
  struct tm tm_buf;
  gmtime_r(&now, &tm_buf);
  char ts[48];
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S.000000Z", &tm_buf);

  int n =
      snprintf(slot.text, kSlotTextSize,
               "# Time: %s\n"
               "# User@Host: %s @ %s  Id: %lu\n"
               "# Schema: %s  Query_time: %.6f  Lock_time: %.6f"
               "  Rows_sent: %llu  Rows_examined: %llu\n"
               "SET timestamp=%llu;\n"
               "%.*s;\n",
               ts, args->user ? args->user : "", args->host ? args->host : "",
               args->connection_id, args->schema ? args->schema : "",
               args->query_time_secs, args->lock_time_secs,
               (unsigned long long)args->rows_sent,
               (unsigned long long)args->rows_examined, (unsigned long long)now,
               (int)args->query_len, args->query ? args->query : "");

  slot.len =
      (n > 0 && (size_t)n < kSlotTextSize) ? (size_t)n : kSlotTextSize - 1;
  slot.state.store(SlotState::Ready, std::memory_order_release);

  // Wake the writer.
  g_wake_cv.notify_one();
}

// =============================================================================
// Lifecycle
// =============================================================================

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

  // Buffer size is fixed for the lifetime of this installation.
  g_num_slots = static_cast<size_t>(g_buffer_slots > 0 ? g_buffer_slots : 1024);
  g_slots = new LogSlot[g_num_slots];
  for (size_t i = 0; i < g_num_slots; ++i)
    g_slots[i].state.store(SlotState::Empty, std::memory_order_relaxed);
  g_head.store(0, std::memory_order_relaxed);
  g_tail = 0;
  g_dropped_count.store(0, std::memory_order_relaxed);

  g_stop.store(false, std::memory_order_relaxed);
  g_writer_thread = std::thread(writer_main);
  return false;
}

static void on_uninstall(vef_context_t * /*ctx*/) {
  g_stop.store(true, std::memory_order_relaxed);
  g_wake_cv.notify_one();
  if (g_writer_thread.joinable()) g_writer_thread.join();
  delete[] g_slots;
  g_slots = nullptr;
  g_num_slots = 0;
}

// =============================================================================
// Registration
// =============================================================================

using namespace villagesql::extension_builder;
using namespace villagesql::query_hook_builder;

VEF_GENERATE_ENTRY_POINTS(
    make_extension("vsql_slow_query_log", "0.0.1")
        .on_register(&capture_service_ptrs)
        .on_install(&on_install)
        .on_uninstall(&on_uninstall)
        .query_hook(
            make_query_hook<VEF_QUERY_HOOK_POSTEXECUTE, &slow_query_hook>())
        .config_var(make_config_var_int("threshold_ms",
                                        "Minimum query execution time in "
                                        "milliseconds to log (default: 1000)",
                                        &g_threshold_ms, 1000, 0, 3600000))
        .config_var(make_config_var_str("log_file",
                                        "Path to the slow query log file "
                                        "(default: /tmp/vsql_slow_query.log)",
                                        &g_log_file, kDefaultLogFile))
        .config_var(make_config_var_int(
            "buffer_slots",
            "Number of ring buffer slots for async log writes (default: 1024,"
            " fixed at install time)",
            &g_buffer_slots, 1024, 64, 65536)))
