# Extension Dependencies

## Status

Design document. Not yet implemented.

## Problem

Today, a VDF can only reference custom types defined in the same extension. The
`vef_type_t.custom_type` field is a bare type name (e.g., `"COMPLEX"`) that is
assumed to belong to the registering extension. This prevents extension authors
from writing VDFs that operate on types defined by other extensions.

**Example:** A `vsql_complex_extras` extension wants to provide
`complex_polar(COMPLEX) -> REAL`, where COMPLEX is defined by `vsql_complex`.
This is currently impossible.

## Scope

This design covers:

- Extensions declaring dependencies on other extensions
- VDFs whose parameters or return types reference non-parameterized custom types
  from a dependency extension
- Dependency-aware extension loading, installation, and uninstallation

Limitations (out of scope for this design):

- Parameterized external types (e.g., `TVECTOR(1536)` from another extension)
- Extension X defining new types that build on or compose types from extension Y
- Type inheritance or subtyping across extensions
- Shared aggregate state across extensions
- Version constraints on dependencies (the manifest format is designed to allow
  adding these later)
- Transitive dependency resolution (if A depends on B and B depends on C, A must
  explicitly declare C if it needs C's types)

## Design

### 1. Manifest Dependencies

Extensions declare dependencies in `manifest.json`:

```json
{
  "name": "vsql_complex_extras",
  "version": "0.0.1",
  "description": "Additional functions for COMPLEX type",
  "author": "VillageSQL Community",
  "license": "GPL-2.0",
  "dependencies": {
    "vsql_complex": {}
  }
}
```

The dependency value is an object, currently empty. This allows future additions
(e.g., `{"min_version": "1.0.0"}`) without changing the format.

### 2. Dependency Info at Registration Time

When the server calls `vef_register()`, it passes information about resolved
dependencies so the extension can access them at runtime:

```c
typedef struct {
  const char *name;       // dependency extension name, e.g. "vsql_complex"
  const char *version;    // installed version of the dependency, e.g. "0.0.1"
  const char *so_path;    // full path to dependency's .so file
  const char *base_path;  // dependency's .veb expansion directory
} vef_dependency_info_t;
```

New fields added to `vef_register_arg_t`:

```c
// protocol >= VEF_PROTOCOL_3 (or added to VEF_PROTOCOL_2 if still unstable)
unsigned int dependency_count;
const vef_dependency_info_t *dependencies;
```

The extension can `dlopen()` the dependency's `.so` to call exported helper
functions. Because the server already loaded the dependency's `.so` via
`dlopen()`, the second `dlopen()` of the same path returns the same handle
(reference count incremented). This means:

- Static variables in the dependency's `.so` are shared, not duplicated
- The extension gets a live reference to the same instance the server uses
- The dependency's code and state are accessible without copying

The `base_path` is provided so extensions can also access non-code files from
the dependency's `.veb` expansion (e.g., data files, configs).

### 3. VDF Signatures Referencing External Types

A VDF in extension X must be able to declare that a parameter is a custom type
from extension Y. The qualified name format `extension.TYPE` is already used
internally (see `make_qualified_base_name()` in `helpers.h`, and the SQL syntax
`ext.func()` for VDF calls).

There are two implementation options; the choice is deferred to implementation
time:

**Option A: New `extension_name` field on `vef_type_t`**

```c
typedef struct {
  vef_type_id id;
  const char *custom_type;    // type name, e.g. "COMPLEX"
  const char *extension_name; // NULL = this extension (backward compat)
} vef_type_t;
```

When `extension_name` is non-NULL, the framework resolves the type from that
extension. When NULL, behavior is unchanged (resolves from the VDF's own
extension).

**Option B: Qualified name in `custom_type` field**

```c
// custom_type = "vsql_complex.COMPLEX"  (external type)
// custom_type = "COMPLEX"               (local type, backward compat)
```

The framework parses the dot-separated format. Extension names cannot contain
dots (enforced by existing name validation), so the parse is unambiguous.

Both options preserve backward compatibility: existing extensions that don't
reference external types continue to work unchanged.

### 4. SDK Builder API

The SDK needs a way to reference external types in VDF signatures. The existing
API passes a compile-time type object:

```cpp
// Existing: local type reference
.param(COMPLEX)
```

For external types, the extension doesn't have the type object at compile time.
Two naming options for the SDK API:

**Option I: Overload `.param()` with a qualified string**

```cpp
make_func<&complex_polar_impl>("complex_polar")
    .returns(REAL)
    .param("vsql_complex.COMPLEX")
    .deterministic()
    .build()
```

Pro: consistent with the `ext.TYPE` convention already used internally and in
SQL. Con: a string that happens to contain a dot silently becomes an external
type reference; easy to miss in code review.

**Option II: Separate `.param_ext()` method**

```cpp
make_func<&complex_polar_impl>("complex_polar")
    .returns(REAL)
    .param_ext("vsql_complex", "COMPLEX")
    .deterministic()
    .build()
```

Pro: explicit — obvious at the call site that this is a cross-extension
reference. Con: a second method to learn; two string arguments instead of one.

Both options need corresponding `.returns()` / `.returns_ext()` variants for
return types. The choice is deferred to implementation (see Open Questions).

### 5. Query-Time Validation

`ValidateAndConvertVDFArguments()` in `types/util.cc` currently builds the
expected qualified base name using the VDF's own extension name:

```cpp
// Current code:
const std::string expected_qbn = make_qualified_base_name(
    std::string(extension_name.str, extension_name.length),
    expected_type.custom_type);
```

For external type references, this must use the type's owning extension name
instead. The change is small: read the extension name from the `vef_type_t`
(either the new field or parsed from the qualified name), and use that when
building the expected QBN.

`SetVDFReturnTypeContext()` needs the same change for return types that are
external custom types.

### 6. Dependency System Table

A new system table `villagesql.extension_dependencies` tracks dependencies
persistently:

```sql
CREATE TABLE villagesql.extension_dependencies (
  extension_name VARCHAR(64) NOT NULL,
  dependency_name VARCHAR(64) NOT NULL,
  PRIMARY KEY (extension_name, dependency_name),
  INDEX idx_dependency (dependency_name)
) ENGINE=InnoDB;
```

The table is populated during `INSTALL EXTENSION` from the manifest's
`dependencies` field, and cleaned up during `UNINSTALL EXTENSION`. The manifest
remains the source of truth; the table is a materialized cache for efficient
queries.

The `idx_dependency` index enables fast reverse lookups: "which extensions depend
on Y?" — needed for both uninstall safety checks and startup loading order.

### 7. Extension Loading Order

`load_installed_extensions()` must load extensions in dependency order.

- Query `villagesql.extension_dependencies` to build the dependency graph
- Topologically sort (dependencies loaded first)
- Circular dependencies are an error (detected during topological sort)

At `INSTALL EXTENSION` time:

- Parse the new extension's manifest for dependencies
- Verify all dependencies are already installed (query `villagesql.extensions`)
- If not, reject with an error naming the missing dependencies
- On success, insert rows into `villagesql.extension_dependencies`

### 8. Uninstall Safety

`UNINSTALL EXTENSION Y` must check whether any installed extension depends on Y.
This is a new check in addition to the existing column-dependency check.

- Query: `SELECT extension_name FROM villagesql.extension_dependencies WHERE dependency_name = 'Y'`
- If any rows exist, reject with RESTRICT behavior
- Error message should name the dependent extension(s)
- On successful uninstall, delete rows: `DELETE FROM villagesql.extension_dependencies WHERE extension_name = 'Y'`

### 9. Data Representation

VDFs receiving external custom types get the binary (internal) representation,
the same as same-extension VDFs. The `vef_invalue_t` struct is unchanged: the
VDF receives `bin_value` / `bin_len` and (for protocol >= 2) `type_params`.

We considered passing the string representation instead, to decouple the
cross-extension VDF from the type's binary format. We chose binary because:

- No encode/decode overhead per row (important for column-scan VDFs)
- Consistent with same-extension VDF behavior (no second-class calling convention)
- The binary format is the type's contract; cross-extension authors need to know
  it regardless
- The `dlopen` mechanism (section 2) provides a clean way for the dependent
  extension to access the type extension's helper functions for reading/writing
  the binary format

## Changes Summary

| Component | File(s) | Change |
|-----------|---------|--------|
| Manifest parsing | `veb_file.cc` | Parse `dependencies` from manifest.json |
| ABI types | `abi/types.h` | Add `vef_dependency_info_t`; extend `vef_register_arg_t` |
| VDF signatures | `abi/types.h` | Extend `vef_type_t` (Option A or B) |
| Registration | `veb_file.cc` | Populate dependency info; pass to `vef_register()` |
| Type registration | `veb_register_type_v2.cc` | Allow external type names in VDF signature validation |
| Func registration | `veb_file.cc` | Allow external type names in VDF signatures |
| Query validation | `types/util.cc` | `ValidateAndConvertVDFArguments` / `SetVDFReturnTypeContext` use type's extension name |
| Loading order | `veb_file.cc` | Topological sort in `load_installed_extensions()` |
| Install/Uninstall | `sql_extension.cc` | Validate deps on install; check reverse deps on uninstall; manage dependency table rows |
| SDK builder | `func_builder.h`, `extension_builder.h` | `.param("ext.TYPE")` overload |
| System tables | `villagesql_schema.sql.in` | Add `villagesql.extension_dependencies` table |

## Open Questions

1. **ABI representation of external type references:** Option A (new
   `extension_name` field on `vef_type_t`) vs Option B (qualified
   `"ext.TYPE"` string in existing `custom_type` field). See section 3.

2. **SDK API naming:** `.param("vsql_complex.COMPLEX")` (overloaded) vs
   `.param_ext("vsql_complex", "COMPLEX")` (explicit). See section 4.

3. **Protocol version:** Should the new ABI fields go into VEF_PROTOCOL_2
   (which is still marked unstable) or a new VEF_PROTOCOL_3?

4. **Transitive dependencies:** If A depends on B and B depends on C, should A
   be able to use C's types without declaring C as a direct dependency? For now
   the answer is no (explicit only), but this may be revisited.

5. **Schema migration:** How do existing installations get the new
   `villagesql.extension_dependencies` table? Need to integrate with the
   existing schema versioning in `villagesql.properties`.

## Future Work

- Version constraints in manifest dependencies (e.g., `{"min_version": "1.0.0"}`)
- `SHOW EXTENSION DEPENDENCIES` or INFORMATION_SCHEMA view for dependency graph
- Extensions defining types that compose or extend types from other extensions
