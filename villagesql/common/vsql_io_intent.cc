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

#include "villagesql/common/vsql_io_intent.h"

namespace {
// Per-thread current I/O intent. Reads and writes are on the same thread (the
// scope is set and the I/O completes on one thread for synchronous I/O), so no
// synchronization is needed.
thread_local VsqlIoIntent t_io_intent = VsqlIoIntent::NONE;
}  // namespace

VsqlIoIntent vsql_current_io_intent() { return t_io_intent; }

vsql_io_intent_scope::vsql_io_intent_scope(VsqlIoIntent intent)
    : m_previous(t_io_intent) {
  t_io_intent = intent;
}

vsql_io_intent_scope::~vsql_io_intent_scope() { t_io_intent = m_previous; }
