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

#ifndef VSQL_IO_INTENT_H
#define VSQL_IO_INTENT_H

// VillageSQL: caller-declared I/O intent, carried thread-locally.
//
// Performance Schema records a file I/O wait at the point the I/O completes
// (pfs_end_file_wait_vc), where the *reason* for the I/O is no longer known:
// a synchronous single-page buffer-pool read and a background read-ahead are
// both just "a read on a data file". The reason is only known at the caller
// (e.g. buf_read_page vs buf_read_ahead_*).
//
// This carries that reason from the caller to the completion point via a
// thread-local, set with an RAII scope around the intent-bearing call. It does
// NOT depend on current_thd, so it also works on background threads, and it is
// always cleared on scope exit. The histogram feed reads the current intent to
// route the I/O into an intent-specific histogram in addition to the overall
// one.

// The set of I/O intents VillageSQL distinguishes. NONE means "no caller
// declared an intent" (e.g. read-ahead, background writes, non-InnoDB file
// I/O).
enum class VsqlIoIntent {
  NONE = 0,
  // A synchronous single-page buffer-pool read: a thread blocking now, waiting
  // for one page it needs to make progress. Excludes read-ahead.
  SYNC_PAGE_READ,
};

// The intent currently in effect on this thread.
VsqlIoIntent vsql_current_io_intent();

// RAII scope: declares an I/O intent for the duration of the scope, restoring
// the previous intent on exit (so nesting is safe). Use around the call that
// issues the intent-bearing I/O.
class vsql_io_intent_scope {
 public:
  explicit vsql_io_intent_scope(VsqlIoIntent intent);
  ~vsql_io_intent_scope();

  vsql_io_intent_scope(const vsql_io_intent_scope &) = delete;
  vsql_io_intent_scope &operator=(const vsql_io_intent_scope &) = delete;

 private:
  VsqlIoIntent m_previous;
};

#endif  // VSQL_IO_INTENT_H
