// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

#ifndef VILLAGESQL_VSQL_SERVICES_BASE_H_
#define VILLAGESQL_VSQL_SERVICES_BASE_H_

// Base interface for escape-hatch service wrappers.
//
// This header is included by extension_builder.h and has no MySQL component
// header dependencies. RequiredService<>, ProvidedService<>, and SysVar<> are
// defined in services.h, which requires MySQL component headers.

#include <villagesql/abi/types.h>

namespace vsql {

// Abstract base for escape-hatch wrappers (RequiredService, ProvidedService,
// SysVar). Stored as pointers in the ExtensionBuilder services tuple so the
// framework can call load()/unload() without knowing the concrete type.
struct ServiceBase {
  virtual bool load(char *error_msg) = 0;
  virtual void unload() = 0;
  virtual ~ServiceBase() = default;
};

// Forward declarations so extension_builder.h can reference these in method
// signatures without pulling in MySQL component headers from services.h.
template <typename SvcType>
class RequiredService;

template <typename SvcType>
class ProvidedService;

}  // namespace vsql

#endif  // VILLAGESQL_VSQL_SERVICES_BASE_H_
