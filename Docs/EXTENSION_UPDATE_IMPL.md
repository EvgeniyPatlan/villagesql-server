# Extension Update — Implementation

`ALTER EXTENSION` is implemented as five phases:

## 1. External Conditions

- `offline_mode = ON` — ensures no new non-admin connections can start using the
  extension.
- No active non-admin connections — ensures no session is currently using the
  extension. Admin connections are trusted and excluded.

## 2. Compatibility Checks

The new version is validated against the old before any changes are made:

- **Dropped types**: a type present in the old version but absent from the new is
  only allowed if no table columns or stored procedure parameters use it.
- **Retained types**: a type present in both versions must not change
  `persisted_length` — existing binary data on disk would be misinterpreted.
- **New types**: always allowed.

Capabilities the new registration requires (declared in
`vef_required_capabilities`) can also veto the update at this point via their
`on_check_update` hook. The capability registry runs every such hook before any
catalog changes are made.

## 3. Affected-Table Fence

Before any catalog mutation, the affected user tables — every table that
references the target extension's types — are fenced from concurrent
access for the duration of the swap. This treats `ALTER EXTENSION` like a
controlled restart of those tables: no session can read, write, or open
them until the swap completes.

Why fencing is required: extension-defined types pin function pointers
into the extension's `.so`. Those pointers are cached in two places:

1. **SQL-layer TABLE_SHARE caches.** `share->mem_root` holds a
   `shared_ptr<TypeContext>` per custom-typed column (registered by
   `MaybeInjectCustomType`). The TypeContext caches function pointers
   (`CompareOp/EncodeOp/DecodeOp::fn_`) into the old `.so`.

2. **InnoDB's `dict_sys` cache.** `dict_col_t::custom_column` holds a
   `shared_ptr<TypeContext>` independently, with the same dangling-pointer
   hazard. This survives TABLE_SHARE eviction.

If we `dlclose` the old `.so` while either cache still holds a TypeContext,
the next query that exercises the column dereferences a dangling function
pointer and crashes (deterministic on macOS, where `dlclose` actually
unmaps; latent on glibc due to weak-symbol retention).

The fence acquires `MDL_EXCLUSIVE` on every affected table, then flushes
both caches under that lock. After fencing, no surviving cached pointer
can reach the old `.so`, and no concurrent session can install a new one.
The MDL is held for the rest of the statement, so the swap (phases 4–5),
the commit, and the `dlclose` all happen with the tables still fenced.

### Single source of truth

The affected-table list is derived from one walk of
`villagesql.custom_columns` filtered by the target extension name:

```
For each ColumnEntry in victionary.columns().get_all_committed():
  if entry.extension_name == target:
    add (entry.db, entry.table) to the list (deduped)
```

The walk happens under the victionary read lock, in the same scope as the
compatibility checks (phase 2), so the snapshot is consistent.

**Known gap (TODO villagesql-preview).** The walk currently considers only
`villagesql.custom_columns`. A table with a *regular* column but an
*extension-provided* index type/profile has no entry in `custom_columns`
and would be missed. Its `TABLE_SHARE` caches
`IndexContext::descriptor_` (raw pointer to `IndexTypeDescriptor`) and
`KEY_PART_INFO::custom_index_profile` (raw pointer to
`IndexProfileDescriptor`); both dangle after the swap `dlclose`s the
old `.so`, the same hazard as `TypeContext::descriptor_`. The fix is to
extend the walk to also iterate `villagesql.custom_indexes` (and
`custom_index_columns` for the profile-providing extension) and add
their tables to the affected list. Not blocking for the current slice
because no extension currently ships a custom index type; tracked in
`sql_extension_update.cc` for the preview milestone.

### MDL acquisition

`lock_table_names(thd, table_list, nullptr,
thd->variables.lock_wait_timeout, 0)` acquires `MDL_EXCLUSIVE` on every
table in the list. MDL_EXCLUSIVE blocks any other session from opening
the table — they must wait until our statement releases. The MDL is held
for statement duration; the implicit release at statement end is when
other sessions can resume access.

### SQL-layer TABLE_SHARE flush

`close_cached_tables(thd, table_list, /*wait_for_refresh=*/true,
LONG_TIMEOUT)` evicts every TABLE_SHARE in the supplied list and blocks
until every TABLE handle on those shares has been released. Because we
hold MDL_EXCLUSIVE, no other session can acquire new TABLE handles —
the wait is over any handles already open, which under `offline_mode =
ON` is bounded to admin sessions. Each release runs `share->mem_root`
cleanup, dropping the `shared_ptr<TypeContext>` registered at table
open. When this returns, no TABLE_SHARE pins a TypeContext.

The flush is **targeted, not global** — only the affected tables are
flushed. Other tables in the cache are untouched.

### Storage-engine hook

`villagesql::g_storage_invalidate_tables` is a global function pointer set
at storage-engine init. InnoDB writes it in `innodb_init` (see
`storage/innobase/handler/ha_innodb.cc`). The implementation
(`villagesql::innodb::mark_dict_tables_for_discard` in
`storage/innobase/villagesql/custom_column.cc`) looks up each named table
in `dict_sys` and sets `discard_after_ddl = true`. The next
`ha_innobase::open` on the table evicts the stale entry and reloads from
the data dictionary, acquiring a fresh TypeContext shared_ptr against the
new extension version. This mirrors the `innobase_discard_table` primitive
used by in-place ALTER TABLE.

The hook is null-checked at the call site; if no storage engine has
declared custom-column dict caching, the call is skipped.

### Trade-offs

- **Synchronization cost.** MDL_EXCLUSIVE acquisition can block if
  another session holds the tables open. Under `offline_mode = ON` this
  is bounded to admin sessions — rare in practice. `lock_wait_timeout`
  governs how long we wait.
- **Cost on the happy path.** One read-locked walk of
  `villagesql.custom_columns`, one `lock_table_names` call, one targeted
  `close_cached_tables`, and one dict-cache lookup per affected table.
  All proportional to the number of tables using the target extension.
- **Failure modes.** If MDL acquisition fails (timeout, killed
  connection), or `close_cached_tables` fails, we abort *before any
  catalog mutation*. The transaction rolls back via `end_transaction(thd,
  true)`, the new `.so` is unloaded, and the operator sees the error.
  No partial-mutation state is possible.

## 4. Uninstall Old

The old extension is removed. The normal `UNINSTALL EXTENSION` checks that refuse
removal when a type is used in a table column or stored procedure are bypassed here
— the compatibility checks in phase 2 already ensure that any type still in use is
present in the new version with a compatible storage layout.

## 5. Install New

The new extension is installed. Phases 4 and 5 execute within a single transaction,
so the switch is atomic — either both succeed or neither takes effect.

Between uninstall and install, the capability registry invokes the
`on_swap_update` hook on every required capability. Each capability owns its own
state transition — for example, the sys-var capability unregisters the old
variables, registers the new ones, and re-persists compatible values.

After the transaction commits, the old `.so` is `dlclose`d. Because phase 3
fenced the affected tables and flushed their caches under MDL_EXCLUSIVE, no
TABLE_SHARE or `dict_table_t` still holds a function pointer into the old
text segment — the `dlclose` is safe. When the statement ends and the MDL
releases, fresh opens on the affected tables acquire TypeContexts against
the new extension version.

## Failure Handling

The phases above have very different rollback properties. The dividing line is
the moment `execute_upgrade_swap` starts invoking `on_swap_update` hooks. Before
that, every step is fully reversible by the standard transaction rollback. After
that, in-memory capability state has already started changing and there is no
mechanism to undo it.

### Fully reversible (phases 1, 2, 3 pre-mutation)

A failure in any of these leaves the server exactly as it was:

- Name validation, lock acquisition, offline-mode check, active-connection
  check.
- VEB file resolution and `.so` load (the new `.so` is unloaded on the error
  path).
- Type-system compatibility checks (`check_update_compatibility`,
  `check_dropped_types_have_no_dependents`).
- Capability `on_check_update` hooks (these are contract-bound to be
  read-only — they may not mutate state).
- `mark_extension_for_deletion` victionary mutations (rolled back by
  `end_transaction` → `trans_rollback` → `VictionaryClient::rollback_all_tables`).
- Affected-table fence acquisition (MDL_EXCLUSIVE, `close_cached_tables`,
  storage hook). All of these happen before any catalog mutation; the
  MDL is statement-scoped and released on rollback.

### Partial-swap window (phases 4 / 5)

Once `execute_upgrade_swap` begins iterating capabilities, each capability's
`on_swap_update` mutates that capability's in-memory state (e.g. sys-var
registrations swap, thread-worker registrations swap). A failure partway
through the iteration leaves:

- Earlier capabilities: already swapped to the new version.
- Later capabilities: still on the old version.
- Catalog: rolled back to the old version (via `end_transaction`).

The in-memory capability state is now **inconsistent with the catalog and
with itself**. There is no `rollback_swap_update` hook, and no plan to add
one — capability swaps may have observable side effects (e.g. a sys-var
that was visible to a query mid-update) that cannot be cleanly undone.

The same window applies to subsequent failures:

- `mark_for_insertion` / `rewrite_column_and_sp_param_versions` failure.
- `victionary.write_all_uncommitted_entries` failure (catalog write fails
  after the swap has already happened).
- Final commit (`end_transaction(thd, false)`) failure.

In any of these cases the catalog is rolled back but capabilities are not.

### Specific issue: persisted system variables

The sys-var capability's `on_swap_update`
(`villagesql/services/preview/sys_var.cc::update_sys_vars_for_extension`)
has a persistent side effect that survives the in-memory-only "restart to
recover" story:

1. Snapshot the persisted values of retained vars into an in-memory vector.
2. Unregister all old vars — this calls
   `Persisted_variables_cache::reset_persisted_variables` for each one,
   which **rewrites `mysqld-auto.cnf` on disk** to remove those entries.
   Dropped vars are gone from the file at this point; retained vars are
   also gone, pending step 4.
3. Register new vars (in-memory; no disk write).
4. For each retained var that had a persisted value, call `sys_var_set(...,
   PERSIST, ...)` — this **rewrites `mysqld-auto.cnf` again** to add the
   new entries.

If anything fails after step 2 begins (later capability hook, catalog write,
final commit), `mysqld-auto.cnf` has already been rewritten and there is no
undo. A server restart re-loads the catalog (v1) but the persisted values
recorded against v1 are gone — those settings revert to defaults on the
next startup.

Three observable losses:

- **Dropped vars persisted under v1 lose their persisted values
  permanently.** Even if the catalog rolls back, the persisted entries are
  gone from `mysqld-auto.cnf`.
- **Retained vars lose their persisted values if step 4 fails after step
  2.** Same mechanism, narrower window.
- **Retained vars with a type change that breaks the persisted value get
  silently reset** with only a `WARNING`-level log line (line 589-595 of
  `sys_var.cc`). This is a success-path issue, not a rollback issue, but
  it's the same class of "persisted setting silently lost during ALTER"
  concern.

The planned fix is detailed in `EXTENSION_UPDATE_CAPABILITY_STATE.md`.
Two shapes were considered:

- **In-process undo registry.** The capability snapshots state and registers
  an undo callback on the swap context; the framework runs undos in reverse
  on in-process failure. Covers the dominant failure mode but the callbacks
  die with the process — a server crash mid-update leaves the on-disk side
  effects orphaned with no recovery hint.
- **Capability-state WAL (recommended).** A new victionary table holds undo
  blobs written *outside* the main ALTER transaction. Before mutating
  non-catalog state, the capability writes an undo blob and commits it.
  On success, the WAL rows are deleted after the main ALTER commits. On
  any failure — in-process *or* server crash — the WAL rows linger as the
  operator-facing record of inconsistent state, with a capability-supplied
  recovery hint. See `EXTENSION_UPDATE_CAPABILITY_STATE.md` §"Stage 1
  alternative: capability-state WAL (recommended)" for the full design,
  including the WAL table schema, recovery modes, and preview-era
  detect-and-report framing.

Both shapes are out of scope for the current slice. Until the WAL lands,
admins relying on persisted extension sys vars should snapshot
`mysqld-auto.cnf` before running `ALTER EXTENSION`.

### Planned mitigation: lock-out exit from `offline_mode`

When the partial-swap window opens, the server should mark itself as
"upgrade failed, restart required" and refuse `SET GLOBAL offline_mode = OFF`
until the server has been restarted. The flag is in-memory only, so a
restart clears it; on restart, capabilities re-initialize from the
catalog (which is the authoritative source of what's installed), bringing
in-memory state back into agreement.

This is not yet implemented; the TODO is marked at the partial-swap point
in `sql_extension_update.cc::execute_upgrade`. Until then, an admin whose
ALTER fails mid-swap should restart the server before exiting offline mode,
even if the SQL error doesn't say so. The error log will identify the
capability that failed.
