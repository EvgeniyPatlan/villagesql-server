# Extension Update at Restart

This document describes the deferred extension-update model:

```sql
ALTER EXTENSION name VERSION 'x.y.z' AT RESTART
```

The statement validates and records an update request while the live server
continues running the currently installed extension version. The actual catalog
rewrite and extension load happen during the next server startup.

This model intentionally avoids the hard parts of an in-process extension swap:
no capability swap while SQL is running, no affected-table MDL fence, no table
or storage-engine cache invalidation, no old `.so` unload while cached function
pointers may still exist, and no capability-state WAL for mid-update rollback.
Capabilities are populated only after startup has decided which extension
version is authoritative in the catalog.

## Goals

- Keep the live server serving the current extension version until restart.
- Validate the target package before accepting the pending update.
- Make the restart path deterministic: either commit the update and load the
  target version, or fall back to the current catalog version.
- Keep pending-update state operator-visible and durable.
- Preserve the existing compatibility rules for retained, dropped, and new
  types.
- Avoid in-process `on_swap_update` semantics for the restart-based update
  path.

## Non-Goals

- No live replacement of an already-loaded extension.
- No `offline_mode` requirement for the deferred statement.
- No affected-table cache flushing during the live statement.
- No capability rollback API or capability-state WAL for this path.
- No separate compatibility-check `.so` in the initial implementation.

## Extension Author Contract

Restart-based update makes the `.so` lifetime problem tractable, but it does
not make arbitrary type changes safe. Extension authors must preserve the
meaning of persisted data for any type that keeps the same extension name,
version-updated type name, and on-disk representation.

Rules authors must follow:

- Do not change the on-disk representation of an existing type.
- Do not reinterpret bytes already stored for an existing type.
- Do not change comparison, ordering, hashing, or indexing semantics in a way
  that makes existing persisted values or existing indexes inconsistent.
- Do not remove or rename functions, VDFs, or capability-provided behavior that
  existing schema objects depend on unless the update checks can prove no such
  dependency exists.
- If a persisted format change is required, publish it as a new type instead of
  overloading the existing type.

The user-facing migration path for a format-changing update should be a manual,
data-copy procedure:

1. Install or update to a version that provides both the old and new types.
2. Add a new column using the new type.
3. Copy or transform data from the old column to the new column.
4. Update application/schema references.
5. Drop the old column when safe.
6. Move to a later extension version that removes the old type, if desired.

That procedure should be documented separately for users. The important point
for this design is that only some compatibility properties can be checked
efficiently in server code. The rest are part of the extension author contract.

## Validation Checks

The update path runs checks in both phase 1 and phase 2. Phase 1 validates the
requested target before recording pending state. Phase 2 repeats the checks at
startup because the catalog, VEB file, or server configuration may have changed
between the original request and restart.

Checks done in code:

- Target VEB resolution: `{name}-{version}.veb` exists and its manifest version
  matches the requested target.
- Target `.so` load and `vef_register` success.
- Extension identity checks: the target registration must identify the same
  extension name and requested version.
- Retained type storage-size compatibility: for types present in both old and
  new registrations, `persisted_length` must not change.
- Dropped type dependency check: a type present in the old version but absent
  from the new version can be dropped only if no table columns or stored
  procedure parameters use it.
- Capability `on_check_update` hooks for capabilities required by the target
  registration.
- Pending target hash check, if we decide the stored `pending_sha256` is
  enforceable rather than informational.

Checks not currently done in code:

- Semantic compatibility of encode/decode behavior when `persisted_length` is
  unchanged.
- Semantic compatibility of comparison, ordering, hashing, indexing, or
  collation-like behavior for existing values.
- Whether VDFs or extension functions are referenced by generated columns,
  functional indexes, CHECK constraints, DEFAULT expressions, views, triggers,
  stored routines, or application SQL.
- Whether dropping or changing a VDF is safe.
- Whether capability configuration changes are semantically compatible beyond
  what each capability's `on_check_update` hook validates.
- Whether external resources used by the extension, such as files or services,
  are compatible with the target version.

These non-checked items must be covered by extension author rules and release
notes. Over time, specific checks can move from "author contract" to enforced
validation when VillageSQL has the metadata needed to do them cheaply and
reliably.

## Capability State

Restart-based update avoids in-process capability swapping: capabilities are
populated only after startup has selected and committed the catalog version for
an extension. That removes the main rollback problem from the live-update
design.

There is still one known stateful capability today: persisted system variables.
If an extension defines a persisted system variable, the persisted value is
currently stored outside the victionary. Because that state is not represented
in VillageSQL system tables, it cannot be updated transactionally with the
extension catalog rewrite during restart.

The preliminary design decision is that current and future capabilities with
persistent state should store that state in the victionary. For persisted
extension system variables, that likely means introducing a VillageSQL-owned
system table for extension sys-var persisted values and teaching the sys-var
capability to read and write that table instead of relying on MySQL's generic
persisted-variable file for extension-owned variables. This work is tracked as
a standalone GitHub issue — landing it is a prerequisite for an extension
that uses persisted system variables to be safe under restart-based update.

Once capability state is victionary-backed:

- Capability state can participate in the same transaction as the extension
  catalog update.
- Startup can populate capabilities from a single authoritative catalog state.
- Rollback and soft fallback become ordinary catalog rollback problems, not
  ad hoc file or process-state recovery problems.

At this point, no other known capability has persistent state that should cause
special problems during update at restart. New capabilities should document
whether they own persistent state. If they do, they should use victionary-backed
storage from the start, or explicitly document why their state is safe across a
restart update without transaction participation.

## SQL Surface

The new form is explicit about deferred semantics:

```sql
ALTER EXTENSION name VERSION 'x.y.z' AT RESTART;
```

The same shape leaves room for a future live-update command by dropping
`AT RESTART`:

```sql
ALTER EXTENSION name VERSION 'x.y.z';
```

That future form is not part of this design. The restart-only form is explicit
so operators can see that the live server will keep running the current version
until the next restart.

Canceling a pending update should have a dedicated operator-facing syntax:

```sql
ALTER EXTENSION name CANCEL UPDATE;
```

Direct manipulation of pending columns may also be useful for debugging and
administration, but the SQL verb gives users a stable interface.

Before canceling, the operator needs to see what is pending. The pending state
is queryable directly on `villagesql.extensions`:

```sql
SELECT extension_name, extension_version, pending_version, pending_requested_at,
       pending_last_error
FROM villagesql.extensions
WHERE pending_version IS NOT NULL;
```

A `SHOW EXTENSIONS` polish step that exposes the same fields is covered in
"Operator Visibility" below.

## Catalog State

The preferred design is to store pending restart-update state on the existing
extension catalog row rather than adding a separate pending-actions table.

Add nullable columns to `villagesql.extensions`, for example:

```sql
pending_version      VARCHAR(64)  NULL,
pending_sha256       CHAR(64)     NULL,
pending_requested_at TIMESTAMP(6) NULL,
pending_last_error   TEXT         NULL,
pending_last_error_at TIMESTAMP(6) NULL
```

The exact column names can change during implementation. The important property
is that the installed extension row remains the single authoritative place for
both the current version and any restart-pending target version.

`pending_last_error` and `pending_last_error_at` are populated by phase 2 when a
pending update fails: the message that went to the error log is also written to
the row so operators can `SELECT` the failure reason without parsing logs. The
fields are cleared when the operator cancels the pending update or when a
subsequent phase-1 `ALTER ... AT RESTART` replaces the pending target.

Benefits of extending the existing table:

- One row represents the extension's current and pending state.
- `UNINSTALL EXTENSION` naturally removes pending update state with the
  extension row.
- `SHOW EXTENSIONS` and direct catalog inspection can expose pending state
  without joining another table.
- Startup can iterate installed extensions in catalog order and see any pending
  target directly on each row.

Victionary infrastructure needed:

- Extend the extension descriptor/entity with pending target fields.
- Update table schema definitions and schema-manager registration.
- Update serialization, deserialization, and MarkForUpdate paths for extension
  rows.
- Add helper APIs for setting, replacing, clearing, and applying pending update
  fields.
- Ensure install, uninstall, cancel, and startup-update paths preserve or clear
  pending fields deliberately.

### Alternatives Considered

A generic `villagesql.pending_actions` table was considered as a uniform queue
for deferred administrative actions, with `extension_update` as one row type
and an opaque payload column for type-specific data. The shape would have been
roughly:

```sql
villagesql.pending_actions
  action_id      PK
  action_type    ENUM('extension_update', ...)
  target_name    VARCHAR
  action_payload JSON
  requested_at   TIMESTAMP
```

The trade-off:

**For per-extension fields on `villagesql.extensions` (current design):**

- The "one pending update per extension" invariant is enforced by the PK on
  `extension_name` without an extra uniqueness constraint.
- `UNINSTALL EXTENSION` naturally removes pending state with the extension row;
  a separate table would need explicit cascade or application-level cleanup.
- Startup already reads `villagesql.extensions`; pending fields ride that read
  with no extra query or join.
- The failure-reason fields (`pending_last_error`, `pending_last_error_at`) sit
  naturally on the same row.

**For a generic `pending_actions` table:**

- Future restart-time actions (deferred uninstall, deferred type-level
  mutations, deferred capability config changes) slot in without schema
  migrations on `villagesql.extensions`.
- One uniform queue for operators and tooling to inspect, regardless of
  action type.
- Action history (completed/failed rows kept around) is natural in a queue
  shape; awkward as columns on the extension row.

**Open**: reviewers have asked whether the generic shape is preferable even
without a second user lined up. The argument for choosing it now is shape
stability — once SQL surface and tooling are built around per-row fields,
moving to a queue later is a breaking change. The argument against is YAGNI:
there is no second action type queued for implementation today (INSTALL is
online; UNINSTALL's failure modes do not benefit from the AT RESTART pattern
in the same way as UPDATE), and the per-row design has concrete advantages
listed above.

This decision is not final and should be resolved before slice 1 lands.

### Schema Migration for Existing Installations

Existing VillageSQL installations have `villagesql.extensions` rows without the
new `pending_version`, `pending_sha256`, and `pending_requested_at` columns.
The columns must be added to deployed databases on first startup of the
upgraded server.

VillageSQL already has version-stamped schema migrations driven by
`run_villagesql_version_upgrades` in `villagesql/schema/schema_manager.cc`. The
function reads the stored schema version from `villagesql.properties` and
applies upgrade steps from `villagesql/schema/upgrade.{h,cc}` for any version
gap. The existing `upgrade_villagesql_from_0_0_1_to_0_0_3` step demonstrates
the pattern: an `ALTER TABLE villagesql.<table> ADD COLUMN ...` issued via
`execute_statement_ignore_errors` with `ER_DUP_FIELDNAME` ignored so the step
is idempotent.

For this design the migration adds a new upgrade step keyed to the version that
first ships restart-based update support. The step issues:

```sql
ALTER TABLE villagesql.extensions
  ADD COLUMN pending_version VARCHAR(64) NULL,
  ADD COLUMN pending_sha256 CHAR(64) NULL,
  ADD COLUMN pending_requested_at TIMESTAMP(6) NULL;
```

ignoring `ER_DUP_FIELDNAME` for idempotency. After the migration runs,
`run_villagesql_version_upgrades` writes the new build version to
`villagesql.properties` so re-runs are no-ops. The exact target version string
and column names are pinned during implementation.

Notes:

- The schema-manager `TABLE_FIELD_DEF` for `villagesql.extensions` must be
  updated in the same change so freshly initialized databases get the columns
  via `CREATE TABLE` and existing databases get them via the upgrade step.
  Both paths converge on the same final table shape.
- The migration is run before any extension is loaded, so pending fields are
  guaranteed to exist before phase-2 consults them.
- New installations skip the upgrade step entirely; the schema manager
  detects that the stored version is already current and runs no upgrades.

## Phase 1: Live Server

`ALTER EXTENSION foo VERSION '2.0.0' AT RESTART` runs while the current
extension version remains loaded and active.

Steps:

1. Validate extension name and resolve `{name}-{version}.veb`.
2. `dlopen` the target `.so`.
3. Call `vef_register` and populate the target registration structure enough
   to inspect descriptors and required capabilities.
4. Run the same pre-swap compatibility checks used by the current live update:
   retained type `persisted_length` compatibility, dropped-type dependency
   checks, and capability `on_check_update` hooks.
5. `dlclose` the target `.so`.
6. Set or replace the pending fields on the extension's catalog row.
7. Return `OK`; the currently loaded extension version continues serving all
   queries.

If pending fields already exist for the same extension, replace them and return
a warning. This lets operators correct a pending target without first canceling
the old request.

The phase-1 path must not call capability `on_swap_update`, must not rewrite
`villagesql.extensions`, and must not rewrite dependent column or stored
procedure parameter entries.

## Phase 2: Server Startup

Startup applies pending updates from `load_installed_extensions`, before
capabilities are populated for the extension.

For each installed extension in catalog order:

1. Look for pending target fields on the extension row.
2. If no pending target exists, load the current catalog version as today and
   populate capabilities.
3. If a pending target exists:
   - Resolve and `dlopen` the target VEB/`.so`.
   - Call `vef_register`.
   - Re-run pre-checks against the current catalog state.
   - If any check fails, `dlclose` the target, log an error, leave the pending
     fields in place, load the current catalog version, and populate
     capabilities for the current version.
   - If checks pass, rewrite `villagesql.extensions`,
     `villagesql.custom_columns`, and `villagesql.custom_sp_params`, and clear
     the pending fields in one transaction.
   - Keep the target `.so` loaded and populate capabilities for the target
     version.

Several extensions may have pending updates at the same restart. Startup should
process each extension independently in catalog order: one extension's failed
pending update falls back to its current version and must not prevent later
extensions from applying their own pending updates.

Open transaction-scope question: should all pending extension updates at a
restart be applied inside one transaction, or should each extension update use
its own transaction?

Per-extension transactions are the current preferred direction because they
match the soft-fallback policy: a failure for one extension leaves its pending
fields in place, loads its current version, and still allows unrelated pending
updates to apply. A single transaction for all pending updates would give an
all-or-nothing restart batch, but it conflicts with the desired "unrelated
extension not affected" behavior and makes one bad target block every other
pending update. If cross-extension dependency handling becomes necessary later,
we may need a dependency-aware batch transaction, but that is out of scope for
the first design.

The central invariant is:

Capabilities are populated only for the version that the catalog ends up
declaring current. The restart-update path never populates old capabilities and
then swaps them in-process.

## Failure Policy

Phase 1 failures abort the statement and leave the current extension and any
existing pending fields unchanged, except for the deliberate
replace-with-warning case after validation succeeds.

Phase 2 uses soft fallback:

- If the target cannot be loaded or validated, the server logs the failure,
  writes the failure reason to `pending_last_error` / `pending_last_error_at`
  on the extension row, leaves the pending fields in place, and loads the
  current catalog version.
- If the startup rewrite transaction fails, the server logs the failure,
  writes the failure reason to `pending_last_error`, leaves or restores the
  pending fields, and loads the current catalog version.
- A failed pending update for one extension must not prevent unrelated
  extensions from loading.

Operators can inspect the pending fields and the queryable failure reason
without parsing logs:

```sql
SELECT extension_name, extension_version, pending_version, pending_last_error,
       pending_last_error_at
FROM villagesql.extensions
WHERE pending_last_error IS NOT NULL;
```

This policy favors availability. Operators can `SELECT` the failure reason,
fix the target VEB, cancel the pending update, or restart again. The error log
remains the authoritative trace; the row column is a convenience that the SQL
surface provides without log access.

## UNINSTALL Interaction

`UNINSTALL EXTENSION foo` should be allowed when `foo` has a pending update.
Uninstall wins: removing the installed extension row also removes its pending
fields. This avoids orphaned pending work for an extension that no longer
exists.

## Operator Visibility

Minimum visibility is direct SQL:

```sql
SELECT extension_name, extension_version, pending_version, pending_requested_at
FROM villagesql.extensions
WHERE pending_version IS NOT NULL
ORDER BY extension_name;
```

A later polish step can extend `SHOW EXTENSIONS` with pending-update columns,
for example `pending_version` or `pending_update_version`. That surface should
be additive; the extension catalog row remains the source of truth.

Startup and phase-1 log messages should include:

- extension name
- current version
- pending target version
- VEB path or resolved package identity
- whether the pending fields were inserted, replaced, applied, canceled, or
  left in place after failure

## Compatibility-Check `.so`

The initial implementation will use the target extension `.so` for phase-1 and
phase-2 compatibility checks: `dlopen`, call `vef_register`, inspect the
registration, then `dlclose` if the target is not being loaded as the active
version.

### Risk: loading the target `.so` in the live server

Loading the target `.so` in the running phase-1 server process is the main
correctness risk in this design. A `.so` can have static initializers,
constructor side effects, or symbol-interposition behavior that is unsafe to
exercise inside a server already running another version of the same
extension. Even with `dlclose` immediately after `vef_register` returns, the
in-process effects of the load have already happened.

### Mitigation: subprocess pre-check (planned, v2)

The recommended mitigation is to run the pre-check in a **separate process**.
A small helper binary `dlopen`s the target `.so`, runs `vef_register`,
performs the structured compatibility checks, returns a serializable result
to the server, and exits. The server never `dlopen`s the target during
phase 1; nothing about the target can affect the live server beyond a
process-isolated fail/pass verdict.

We are not building the subprocess in v1. V1 runs the pre-check in-process
to keep the implementation simple. The v1 API is shaped so that the v2 lift
is a **mechanical refactor** with no change to the check logic: see
"Pre-Check API Shape: Subprocess-Ready" below for the constraints
(self-contained input struct, structured result, no globals or `THD*`, no
side effects beyond `dlopen`/`dlclose`).

In v2, the in-process call site is replaced with a `fork`/`exec` of the
helper binary plus a pipe-based RPC; the helper binary's `main` is a thin
wrapper that calls the same `run_update_pre_check` function the v1 server
calls directly. Until v2 ships, the v1 in-process model is documented as a
known transient risk.

### Why not push everything to the manifest

A theoretical alternative is to enrich the VEB manifest enough to run all
compatibility checks against declarative metadata only, never `dlopen`ing
the target. This was considered and rejected for v1:

- The manifest today carries only name, version, description, and author.
  All compat-check inputs (provided types and their `persisted_length`,
  required-capability vtable and config hashes, descriptor shapes for
  capability `on_check_update` hooks) come from the registration produced
  by `vef_register`, not from the manifest.
- Lifting all of that into the manifest would require a manifest schema
  large enough to mirror the registration struct, plus build-side tooling
  that emits the manifest from the same source as the `.so` and a
  reviewer-checkable invariant that the two never drift.
- Capability `on_check_update` hooks are programmatic — they may need to
  inspect descriptor fields beyond what a static schema captures. The
  hook entry point cannot run without an executable artifact of some kind.

A future design may split the executable artifact into a smaller
compatibility-check `.so` so the artifact loaded for compat is not the same
binary as the runtime `.so`. That split is orthogonal to the subprocess
mitigation and is not part of v1. The restart model keeps the
compatibility-check entry points isolated enough that either evolution can
replace the target registration loader without changing the SQL or
pending-update catalog semantics.

## Pre-Check API Shape: Subprocess-Ready

Phase 1 runs the pre-check on a live server. Loading the target `.so` in the
running server process is convenient but not risk-free: a malformed or
adversarial target could destabilize the live server via static initializers,
constructor side effects, or symbol interposition the moment it is `dlopen`ed.
The restart model already eliminates the runtime concerns of having two
versions resident at once (capabilities are populated only after restart), but
phase-1 still does a transient `dlopen`/`dlclose` against the live server.

The pre-check API is shaped so that lifting it into a separate process is a
mechanical refactor with no change to the check logic. We are not doing that
lift in v1. The constraint is purely on the v1 API surface so the v2 lift is
cheap.

### Constraints on the v1 pre-check entry point

The pre-check function is **pure with respect to process state**:

- All input is passed by value through a self-contained struct. No `THD*`,
  no victionary handle, no globals beyond what the caller materializes into
  the struct.
- Output is a structured result by value (success flag + structured error
  info). No mutation of caller state, no direct writes to the server error
  log from inside the function, no catalog access.
- Side effects are limited to `dlopen`/`dlclose` of the target `.so` and any
  filesystem reads the dlopen entails. No PSI registration, no static-init
  observable to the server, no logging.

The caller (phase-1 or phase-2 wrapper code) is responsible for:

- Materializing the catalog snapshot under the victionary read lock and
  populating the input struct.
- Translating the structured result into operator-facing log lines, SQL
  errors, or pending-row mutations.

### Sketch

```cpp
namespace villagesql::veb {

struct UpdatePreCheckInput {
  std::string extension_name;
  std::string current_version;
  std::string target_version;
  std::string target_veb_path;
  std::string expected_target_sha256;

  // Snapshot of the current registration's types (persisted_length and any
  // other shape-only fields the checks need).
  struct TypeSnapshot { /* ... */ };
  std::vector<TypeSnapshot> current_types;

  // Columns and SP-params dependent on each current type, so the
  // dropped-type-has-dependents check can run without victionary access.
  struct DependentColumn { /* ... */ };
  std::vector<DependentColumn> dependent_columns;
  struct DependentSpParam { /* ... */ };
  std::vector<DependentSpParam> dependent_sp_params;

  // Capability declarations on the old registration, for capability
  // on_check_update hooks that compare old vs new.
  struct CapabilityRequirement { /* ... */ };
  std::vector<CapabilityRequirement> current_capabilities;
};

struct UpdatePreCheckResult {
  bool ok;
  std::string error_message;
  // Future: per-check breakdown, structured warnings, etc.
};

UpdatePreCheckResult run_update_pre_check(const UpdatePreCheckInput &input);

}  // namespace villagesql::veb
```

Phase-1 and phase-2 both build an `UpdatePreCheckInput` from their respective
catalog access (under the appropriate lock) and call `run_update_pre_check`.
The function does the `dlopen`, validation, `dlclose`, and returns the
verdict. The caller writes log lines and decides next action.

### Capability `on_check_update` contract

`on_check_update` hooks must not depend on `THD*` or any in-process catalog
handle. They receive a serializable view of old and new capability
requirements (the `CapabilityRequirement` struct above and the new
registration's equivalent), and they return a structured verdict. This is an
ABI change for preview capabilities — only the sys-var capability has an
`on_check_update` hook today, so the migration cost is small. The hook
signature change happens in the same slice that introduces
`run_update_pre_check`.

### Refactor cost in v2 (out of scope for this design)

When we decide to move the pre-check to a separate process:

- A small helper binary's `main()` deserializes `UpdatePreCheckInput` from
  stdin, calls `run_update_pre_check`, and writes `UpdatePreCheckResult` to
  stdout.
- The in-process caller is replaced with a `fork`/`exec` plus a pipe-based
  RPC. The pre-check function body does not change.

Out of scope here. The point is only that the v1 API does not force changes
to the pre-check logic when v2 happens. The discipline is in the v1 signature
and the capability hook contract, not in any v2 plumbing.

## Implementation Slices

### Slice 1: Catalog Fields and Parser Stub

- Add pending-update fields to `villagesql.extensions`, the schema manager, and
  victionary.
- Add a `run_villagesql_version_upgrades` step that `ALTER TABLE`s the new
  columns onto existing installations, keyed to the version that first ships
  this feature. See "Schema Migration for Existing Installations" above.
- Add parser support for:
  - `ALTER EXTENSION name VERSION 'x.y.z' AT RESTART`
  - `ALTER EXTENSION name CANCEL UPDATE`
- Route both statements to handlers that return "not implemented" until the
  behavior lands.
- Add basic tests for parsing and system-table shape.

### Slice 2: Phase-1 Pending Update

- Implement the `AT RESTART` handler.
- Reuse or factor current update pre-checks from `sql_extension_update.cc`
  into the `run_update_pre_check` entry point described in "Pre-Check API
  Shape: Subprocess-Ready". Phase 1 builds an `UpdatePreCheckInput` from the
  victionary under the read lock, calls the entry point, and acts on the
  structured result.
- Change preview capability `on_check_update` hooks to take a serializable
  view of old and new capability requirements (no `THD*`, no live catalog
  handle). Only sys-var has an `on_check_update` hook today.
- Set or replace pending fields with a warning on replacement.
- Implement cancel by clearing the pending fields.
- Ensure `UNINSTALL EXTENSION` removes pending state with the extension row.
- Add tests for happy path, pre-check failure, replace-pending, cancel, and
  uninstall-with-pending.

### Slice 3: Startup Application

- Modify `load_installed_extensions` to consult pending fields.
- Apply the pending update transaction before capability population.
- Fall back to the current version on load, validation, or transaction failure.
- Leave failed pending fields in place.
- Add restart-based MTR coverage for successful application and soft fallback.

### Slice 4: Operator Surface

- Add `SHOW EXTENSIONS` pending-update visibility if desired.
- Standardize error-log messages.
- Add tests for unrelated extensions continuing to load when one pending update
  fails.

## Code Areas

Likely touch points:

- `sql/sql_yacc.yy` and parse tree nodes for syntax.
- `villagesql/veb/sql_extension.h` / `.cc` for statement dispatch.
- `villagesql/veb/sql_extension_update.cc` for factoring reusable
  compatibility checks.
- `villagesql/schema` for the extended extension entity and schema registration.
- `villagesql/schema/victionary_client.*` for accessors.
- Extension startup loading code, including `load_installed_extensions`.
- `mysql-test/suite/villagesql/extension` for MTR coverage.

## Open Checks Before Coding

- Confirm whether target hash validation is required at startup or only logged.
- Confirm whether direct `UPDATE villagesql.extensions SET pending_version =
  NULL ...` should be supported as an operator interface or treated as an
  internal escape hatch.
- Confirm whether the future live-update syntax will be
  `ALTER EXTENSION name VERSION 'x.y.z'` without `AT RESTART`.
- Confirm exact `SHOW EXTENSIONS` column names if that surface is included.
- Confirm transaction scope for restart application: per extension, all pending
  updates in one transaction, or a future dependency-aware batch.
