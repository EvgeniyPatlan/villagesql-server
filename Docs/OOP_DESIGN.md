# Out-of-Process Extensions

## Overview

Today, VillageSQL extensions are loaded directly into the mysqld process via
`dlopen`. A bug in extension code (null pointer dereference, buffer overflow)
crashes the entire server. Out-of-process (OOP) extensions fix this by running
each extension in its own handler process. The server communicates with the
handler over a Unix socket, sending JSON messages for every operation: function
calls, type encode/decode, prerun/postrun lifecycle events.

The cost is latency: every VDF call crosses a process boundary and goes through
JSON serialization. This is acceptable for the initial implementation. We can
optimize later (shared memory, binary protocol) once the architecture is proven.

### How it works

When the server starts with `--outproc-extensions`:

1. On `INSTALL EXTENSION foo`, the server spawns a `vef-outproc-handler` process
2. The handler `dlopen`s the extension's `.so` and calls `vef_register`
3. The handler sends back metadata (function names, signatures, type info)
4. The server builds synthetic descriptor structs that look like normal
   in-process descriptors, but with a trampoline function as the VDF pointer
5. When a query calls an extension function, the trampoline serializes the
   arguments as JSON and sends them to the handler over the socket
6. The handler calls the real VDF, serializes the result, and sends it back
7. On `UNINSTALL EXTENSION foo`, the server sends an unregister message;
   the handler calls `vef_unregister`, unloads the `.so`, and exits

Initially, each extension gets its own handler process. However, the handler
and protocol are designed to support multiple extensions in a single process --
the `register` message can be sent multiple times to load additional `.so`
files, and all operation messages include the function/type name to identify
which extension they target. This matters because we plan to add dependencies
between extensions, which will likely require them to share a process so they
can call into each other. The server-side `OutprocManager` can be extended to
route multiple extensions through a single `OutprocConnection` when needed.

### Implementation plan

The implementation is a layered series of commits. Each builds on the previous
and corresponds to one PR. The feature is not functional until all layers are
in place, but the earlier commits are independently reviewable.

1. **Message protocol** -- JSON-over-socket helpers and base64 codec
2. **Handler process** -- standalone executable that loads a `.so` and serves requests
3. **Client connection manager** -- server-side process spawning, RPC methods, VDF trampoline
4. **Extension load path** -- alternate load path (both `INSTALL EXTENSION` and server startup) that spawns a handler instead of `dlopen`ing
5. **VDF/type call routing** -- wire `vdf_handler` and type operations through the outproc connection
6. **System variable and build** -- `--outproc-extensions` flag and CMake integration
7. **Test** -- run existing extension test suite with `--outproc-extensions` enabled

The sections below describe each in detail.

## 1. Message Protocol

`villagesql/oop/oop_common.h`, `villagesql/oop/oop_common.cc`

Shared library for newline-delimited JSON messaging over a file descriptor,
plus base64 encoding for binary data.

```cpp
namespace villagesql::oop {

bool read_json_message(int fd, rapidjson::Document &doc);
bool write_json_message(int fd, const rapidjson::Document &doc);

std::string base64_encode(const unsigned char *data, size_t len);
bool base64_decode(const std::string &input, std::string &output);

}
```

All messages are single-line JSON terminated by `\n`. Structured fields (op
name, function name, types, flags) are plain JSON. Binary payloads (column
values, encoded type data) are base64-encoded strings within the JSON.

Example -- server requests a VDF call:
```json
{"op":"vdf","func_name":"rot13","handle":1,"values":[{"type":4,"is_null":false,"bin_value":"aGVsbG8="}]}
```

Example -- handler responds:
```json
{"ok":true,"type":0,"actual_len":5,"data":"dXJsbHk="}
```

Example -- server requests a type encode:
```json
{"op":"encode","type_name":"bytearray","value":"aGVsbG8="}
```

Example -- handler responds:
```json
{"ok":true,"buffer":"aGVsbG8gICA=","length":8}
```

## 2. Handler Process

`villagesql/oop/outproc_handler.cc`

A standalone executable (`vef-outproc-handler`) that loads one extension's `.so`
via `dlopen` and serves requests over a socket fd passed as a command-line argument.

```
Usage: vef-outproc-handler <fd>
```

The handler runs an event loop dispatching on the `"op"` field:
`register`, `prerun`, `vdf`, `postrun`, `encode`, `decode`, `compare`, `hash`,
`unregister`.

Here's what a type encode operation looks like in the handler. The server sends
the value as base64; the handler decodes it, calls the extension's encode
function, and returns the result as base64:

```cpp
void handle_encode(const Document &req) {
  std::string type_name = req["type_name"].GetString();
  const vef_type_desc_t *td = g_types[type_name];

  std::string decoded_value;
  base64_decode(req["value"].GetString(), decoded_value);

  auto *buffer = new unsigned char[td->persisted_length];
  size_t length = 0;
  td->encode_func(buffer, td->persisted_length,
                   decoded_value.data(), decoded_value.size(), &length);

  Document resp;
  resp.SetObject();
  auto &a = resp.GetAllocator();
  resp.AddMember("ok", true, a);
  resp.AddMember("buffer", Value(base64_encode(buffer, length).c_str(), a), a);
  resp.AddMember("length", static_cast<uint64_t>(length), a);
  write_json_message(g_fd, resp);
  delete[] buffer;
}
```

The handler also maintains a handle map for `prerun` user_data pointers. These
can't be serialized (they're opaque C pointers), so they stay in the handler
process and are referenced by integer handle in subsequent VDF/postrun calls.

## 3. Client Connection Manager

`villagesql/oop/outproc_client.h`, `villagesql/oop/outproc_client.cc`

Server-side management of handler processes.

```cpp
struct OutprocCallInfo {
  const char *extension_name;
  const char *func_name;
  uint64_t handle;  // from prerun, 0 if none
};

class OutprocConnection {
  bool start(std::string &error_msg);
  bool do_register(const std::string &so_path, ...);
  bool do_vdf(const std::string &func_name, vef_vdf_args_t *args, ...);
  bool do_prerun(...);
  bool do_postrun(...);
  bool do_encode(...);
  bool do_decode(...);
  bool do_unregister(std::string &error_msg);
};

class OutprocManager {
  static OutprocManager &instance();
  OutprocConnection *create_connection(const std::string &name, ...);
  OutprocConnection *get_connection(const std::string &name);
  void remove_connection(const std::string &name);
};
```

Process spawning uses `socketpair(AF_UNIX)` + `fork` + `exec`. The child
becomes the handler; the parent keeps one end of the socket.

The **VDF trampoline** is a single function pointer set on all synthetic outproc
func_descs. When called, it reads `OutprocCallInfo` from `args->user_data`
and routes the call through `OutprocConnection::do_vdf()`.

**Lifecycle:** Handler processes are shut down when the extension is uninstalled
(`UNINSTALL EXTENSION`). The server sends an `unregister` message, the handler
calls `vef_unregister` and `dlclose`, then exits. The server `waitpid`s on the
child and closes the socket. Handler processes are also cleaned up on server
shutdown via `OutprocManager` destructor.

## 4. Extension Load Path

`villagesql/veb/veb_file.cc`, `villagesql/veb/veb_file.h`,
`villagesql/veb/sql_extension.cc`, `villagesql/veb/sql_extension.h`

When `opt_outproc_extensions` is true, the server takes an alternate load path:

```cpp
if (opt_outproc_extensions) {
  load_failed = load_vef_extension_outproc(
      so_path, extension_name, registration, VEF_PROTOCOL_2, load_error);
} else {
  load_failed = load_vef_extension(
      so_path, extension_name, registration, VEF_PROTOCOL_2, load_error);
}
```

`load_vef_extension_outproc()` does NOT `dlopen` the `.so` in the server.
Instead it:

1. Creates an `OutprocConnection` and spawns the handler process
2. Sends a `register` RPC to the handler, which loads the `.so` there
3. Receives metadata (function names, signatures, type info) back
4. Builds **synthetic** `vef_registration_t`, `vef_func_desc_t`, and
   `vef_type_desc_t` structs that mirror the real ones but with the VDF
   trampoline as the function pointer

The unload path (`UNINSTALL EXTENSION`) sends an `unregister` RPC to the
handler, frees the synthetic structs, and removes the connection.

## 5. VDF/Type Call Routing

`villagesql/vdf/vdf_handler.cc`, `villagesql/vdf/vdf_handler.h`,
`villagesql/types/special_vdf_call.h`, `villagesql/sql/func_lookup.cc`,
`sql/sql_udf.h`

The VDF handler's `setup()` method detects outproc functions and populates
`OutprocCallInfo` in `args->user_data`:

```cpp
if (m_udf->is_outproc) {
  m_outproc_info.extension_name = m_outproc_ext_name.c_str();
  m_outproc_info.func_name = m_outproc_func_name.c_str();
  m_outproc_info.handle = 0;
  m_vdf_args.user_data = &m_outproc_info;
}
```

Prerun and postrun are also routed through the outproc connection when
`is_outproc` is set. Prerun returns a handle that gets stored in
`OutprocCallInfo` for subsequent VDF calls in the same query.

Type operations (encode, decode, compare, hash) are implemented as
synthetic VDFs with well-known names (e.g., `__outproc_encode_<typename>`)
that the handler recognizes and dispatches to the type's actual functions.

## 6. System Variable and Top-Level Build

`sql/sys_vars.cc`, `sql/CMakeLists.txt`

A new read-only system variable gates the feature:

```cpp
static Sys_var_bool Sys_outproc_extensions(
    "outproc_extensions",
    "Run VillageSQL extensions in a separate process",
    READ_ONLY NON_PERSIST GLOBAL_VAR(opt_outproc_extensions),
    CMD_LINE(OPT_ARG), DEFAULT(false));
```

The `villagesql_oop` object library is added to `sql/CMakeLists.txt` alongside
the existing villagesql libraries.

## 7. Test

`mysql-test/suite/villagesql/extension/t/extension_simple_type_usage_outproc.test`

Reuses the existing `extension_simple_type_usage` test with `--outproc-extensions`
enabled via a `.opt` file. This validates the full end-to-end flow: install
extension, create table with custom type, insert/query data through out-of-process
VDF calls and type encode/decode, then uninstall.

```
--outproc-extensions
```

```sql
--source suite/villagesql/extension/t/extension_simple_type_usage.test
```

The test is added early in the commit stack (it could even go first) but
won't pass until all layers are in place.
