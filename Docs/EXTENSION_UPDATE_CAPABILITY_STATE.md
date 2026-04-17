# Future: capability state in the victionary

This is a forward-looking design note, not a description of current code.
Status: not implemented. The capability-state-on-disk problem it addresses
is documented in the "Failure Handling" section of
`EXTENSION_UPDATE_IMPL.md`.

## Problem

`ALTER EXTENSION ... UPDATE TO` runs capability `on_swap_update` hooks
mid-transaction. The catalog rows participate in the transaction and roll
back cleanly on failure (via `end_transaction` →
`Metadata_modifier::rollback` → `VictionaryClient::rollback_all_tables`).

But a capability may mutate state outside the catalog — files on disk, OS
resources, external services. Those mutations have no transactional tie
to the catalog. When the catalog rolls back, those changes don't, leaving
the server in a state that survives a restart and cannot be cleanly
recovered.

Today the only capability with this problem is the **sys-var capability**:
its `on_swap_update` rewrites `mysqld-auto.cnf` to update persisted values
for the new extension version. A failure later in `execute_upgrade`
(another capability's hook, the catalog commit, etc.) leaves
`mysqld-auto.cnf` rewritten with no automatic recovery.

## Which capabilities need the fix?

A capability needs the mechanism only if its hooks during ALTER mutate
state that BOTH:

1. Persists beyond the catalog (file, OS resource, external service).
2. Has no clean inverse the caller can invoke on failure.

Walking the capabilities that exist today and the ones we've planned:

| Capability | State during ALTER | Needs mechanism? |
|---|---|---|
| Sys vars | Rewrites `mysqld-auto.cnf` (persistent, no undo) | **Yes** |
| Thread workers (Protocol 4) | Function-pointer swap on a live thread, plus the controlling sys var | **Yes — see Stage 0** |
| Process-list registration | PSI registrations (runtime only) | No |
| Async writer / ring buffer (Protocol 4) | Flush+swap buffer (runtime only) | No |
| Custom indexes | Hidden-table changes ride MySQL's DDL transaction | No — already covered |
| Query hooks (Protocol 3) | Function-pointer registrations (runtime only) | No |

So this is **not** a generic "every capability gets a table" change. It's
"give the sys-var capability a table." The mechanism is reusable if a
future capability has durable side effects, but until then it doesn't need
to be designed as a generic framework — adding a victionary entity type
is straightforward and each capability that needs it can follow the
sys-var pattern.

## Three stages

There are three distinct concerns to address, in increasing order of cost:

0. **Live runtime state during the swap**: a capability with running
   machinery (a worker thread executing the old `.so`'s code, an active
   ring buffer, etc.) cannot be safely mutated mid-execution. Even with
   a perfect rollback mechanism, hot-swapping a function pointer under a
   live thread is undefined behavior. Stage 0 sidesteps this by requiring
   capabilities to quiesce when `offline_mode = ON`, so the swap operates
   on stopped machinery.
1. **In-process failure mid-swap**: a hook fails or a downstream step
   (catalog commit, mark_for_insertion) fails. The process is still
   alive. Even with Stage 0 in place, earlier capabilities have already
   completed their swap and there's no undo. Stage 1 adds an in-process
   undo registry.
2. **Crash mid-update**: the server dies between the file rewrite and the
   catalog commit. The catalog is rolled back on restart by the storage
   engine's crash recovery; the file has been rewritten and stays
   rewritten. Neither Stage 0 nor Stage 1 fixes this — there's no
   surviving process to run undo callbacks. Stage 2
   (catalog-as-source-of-truth) does.

The three stages are complementary. Stage 0 makes the swap safe in the
first place. Stage 1 covers the dominant failure case once the swap is
running. Stage 2 closes the crash window for the one capability whose
state lives outside the catalog.

## Stage 0: the quiesce contract

ALTER EXTENSION's safety rests on a single invariant: **while the
victionary is being mutated, no execution context that can resolve an
extension reference is active.** Without that invariant, a function
pointer swap can be observed mid-execution (torn reads, jumps into an
unloaded `.so`), a `TypeContext` can be held by a thread that outlives
its descriptor, a custom index can be traversed against a stale
profile.

Today `offline_mode = ON` is a *connection-level* fence: it blocks new
non-admin client connections and kills existing ones. That's
necessary but not sufficient. Several execution contexts that can
resolve extension references keep running:

- Extension-owned worker threads (Protocol 4 thread workers, async
  writers).
- The MySQL events scheduler and its event-worker threads.
- Replica applier threads (SQL thread, applier coordinator,
  worker threads).
- Plugin / component background threads.

Stage 0 turns `offline_mode = ON` into a *server-wide quiesce
barrier*: when it returns success, none of the above can be executing
extension code. ALTER's precondition becomes a single check
(`offline_mode is on`) and everything else falls into place.

The work splits into three independent pieces that can land and ship
separately:

- **Stage 0a** — capability quiesce hooks for extension-owned runtime
  state (thread workers, async writers).
- **Stage 0b** — events scheduler pause on offline_mode = ON.
- **Stage 0c** — replica stop on offline_mode = ON (today the ALTER
  precondition only one-shot-checks it).

Plugin/component threads are *not* addressed by Stage 0. The set of
plugins that resolve extension references is empty today, and there's
no general mechanism to identify the ones that would. We document this
as a known gap and revisit if a real case emerges.

### Why all three matter

A worker thread, an event-worker, and a replica applier are
*indistinguishable* from the perspective of the victionary: each is
an admin-context THD running arbitrary SQL that may parse and execute
`extension.x(...)` expressions. None of them is caught by the existing
`non_admin_count` precondition. Quiescing one without the others
doesn't make ALTER safe — it just shuffles which execution context
trips it.

The simplest mental model: **offline_mode = ON means "no SQL is
executing on this server except the session that turned offline_mode
on."** Stage 0 makes that mental model true.

## Stage 0a: capability quiesce hooks

A capability that has *live runtime state* — code currently executing,
threads currently running, buffers currently being written to — cannot
be safely swapped while that state is in use. The function pointer
swap that the rollback discussion centers on isn't even the hard part:
once the old worker is mid-callback, replacing the pointer underneath
it is at best a torn read, at worst a jump into an unloaded `.so`.

The contract: any capability with live runtime state must enter a
*quiesced* state when `offline_mode = ON`, and must not resume runtime
state until `offline_mode = OFF`. While quiesced, the capability is
free to swap any in-memory state safely — nothing is running it.

### Which capabilities must implement it?

Following the same walk as the table above, but now from the angle of
"has live runtime state during normal operation":

| Capability | Live state? | Quiesce action |
|---|---|---|
| Sys vars | No — the in-memory descriptor table is read by `SET`/`SHOW` but is not mutated by the extension itself | No-op |
| Thread workers (Protocol 4) | **Yes** — extension-owned OS threads | Signal stop, join threads |
| Process-list registration | No — registration is one-shot at install | No-op |
| Async writer / ring buffer (Protocol 4) | **Yes** — writer thread, in-flight ring entries | Flush ring, stop writer, join |
| Custom indexes | No live state of its own — DDL takes table MDL through MySQL | No-op (already covered by MDL) |
| Query hooks (Protocol 3) | Function-pointer registration; the actual execution is inside the query thread holding extension MDL S-lock, blocked by ALTER's X lock | No-op (already covered by MDL) |

Only thread workers and async writers need to implement the contract.
Other capabilities are either inherently stateless at the runtime layer
or already serialize through MySQL's lock infrastructure.

### Every extension, not just the target

The contract quiesces **every** extension's runtime state when
`offline_mode = ON`, not only the extension that's about to be updated
(or installed/uninstalled). A natural-sounding alternative — "only stop
the threads of the extension we're touching" — is unsound, for four
reasons:

1. **An extension worker can be running arbitrary SQL.** The
   `vsql-thread-worker` example uses its own internal `THD` to execute
   queries on the server. That `THD` is invisible to ALTER's existing
   precondition checks: it isn't a client connection (so
   `non_admin_count` doesn't see it), and it isn't a replica thread (so
   `replica_count` doesn't see it). While ALTER mutates the victionary,
   that worker may be in the middle of `SELECT
   <ext>.<vdf>(...)` against ANY extension — including the target —
   holding a live `TypeContext` reference, executing a VDF, traversing
   a custom index. The victionary mutation we're trying to protect new
   sessions from is exactly the mutation an existing worker can
   already be racing against.
2. **Cross-extension references are not statically prevented.** A
   worker in extension A can issue queries that walk types or call
   VDFs from extension B. Today nothing forbids this. "Only quiesce
   the target extension" assumes inter-extension isolation we don't
   have.
3. **There is no way to prove a worker is harmless.** A worker doing
   pure CPU work that holds no extension references is, in principle,
   safe to leave running during another extension's swap. But we have
   no way to verify that from the outside — the worker's callback is
   opaque extension code. The conservative position is: assume nothing
   about what any worker does.
4. **Symmetry is enforceable, granularity is not.** "No extension
   threads running, period" is a rule that can be mechanically
   verified (iterate registered workers, check each is joined).
   "Threads of other extensions are OK if they don't touch the target"
   is unverifiable. The simpler rule is also the only one we can
   enforce.

The cost of this conservatism is that `SET GLOBAL offline_mode = ON`
quiesces every extension worker on the server before returning, even
when only one extension is about to be updated. That is the right
trade: offline_mode is already a heavyweight operation (blocks new
non-admin connections, kills existing ones), and quiescing extension
workers fits the same posture.

### Runtime enforcement

What `offline_mode = ON` would need to do, additionally to its
existing connection-kill behavior:

1. For each installed extension, iterate its capabilities.
2. For each capability that declares a `quiesce` hook (i.e. registered
   one at install time), call it. The hook is synchronous; it returns
   only when the capability's runtime state is fully stopped.
3. Record the fact that the capability is quiesced, so `offline_mode = OFF`
   can call the matching `resume` hook.

What `offline_mode = OFF` would do, additionally:

1. For each capability that was quiesced, call `resume`. The capability
   restarts its runtime state from current catalog state (which by then
   has either been updated by a successful ALTER or untouched by a
   failed ALTER).

A capability that fails to quiesce (e.g. a worker thread refuses to
join within timeout) must report failure synchronously, blocking
`SET GLOBAL offline_mode = ON` from succeeding. ALTER then cannot
proceed past the offline_mode precondition.

### Scope of Stage 0a

- Add `quiesce` / `resume` hooks to the capability registration struct.
  Optional fields; only capabilities with live runtime state set them.
- Wire `SET GLOBAL offline_mode = ON` to call `quiesce` on every
  registered hook before returning. Same for `OFF` → `resume`.
- Document the contract: capabilities with live state MUST implement
  the hooks; ALTER assumes quiesce has happened.
- Implement `quiesce` / `resume` for the thread-worker capability when
  Protocol 4 lands. Sys-var is no-op.
- Test that ALTER on a thread-worker extension swaps the callback
  cleanly with no observed v1-after-swap execution.

This is a prerequisite for the thread-worker capability shipping at
all — its swap is unsound without it.

## Stage 0b: events scheduler pause

The MySQL events scheduler is a background thread that wakes when an
event is due and dispatches the event to an admin-context worker
THD which runs the event's stored-program body. The body is arbitrary
SQL — it can resolve any extension's types, call any VDF, traverse
any custom index.

`offline_mode = ON` today has no effect on the scheduler. Events keep
firing. An event that fires during the ALTER window can execute
extension code while the victionary is being mutated.

### What's needed

`SET GLOBAL offline_mode = ON` must, additionally:

1. Signal the events scheduler to stop dispatching new events.
2. Wait for any in-flight event workers to complete (synchronous
   join). The wait is bounded by individual event runtimes; an event
   doing long-running work extends the time spent in the offline_mode
   transition. That's the correct behavior, not a bug — the operator
   asked for a quiesce barrier.
3. Record that the scheduler was paused, so `offline_mode = OFF` can
   resume it.

`SET GLOBAL offline_mode = OFF` resumes the scheduler. Events that
were scheduled to fire during the offline window are silently
skipped (matching current MySQL behavior for events whose window has
passed).

### Implementation hooks

The events scheduler already has a state machine for `SET GLOBAL
event_scheduler = OFF` (`Events::stop()` in `sql/events/events.cc`).
Stage 0b reuses it: `handle_offline_mode` in `sys_vars.cc` calls
`Events::stop()` after killing non-admin connections, and stores a
flag so the resume path can re-start.

### Wrinkles

- **Events that themselves run `SET GLOBAL offline_mode = ON`.** An
  admin event can call this. Re-entry must be handled or documented as
  unsupported. Simplest: detect re-entry from inside an event worker
  and error.
- **Missed events.** Operators choosing offline_mode accept event
  slippage. Document.
- **Interaction with `event_scheduler = ON`.** If an admin explicitly
  re-enables the scheduler while offline_mode is on, that should be
  rejected (or queued until offline_mode = OFF). Simplest: reject with
  a clear error.

### Scope of Stage 0b

Smaller than 0a — no new API, just hooks into an existing
stop/start state machine. Tests:

- Schedule an event for "now + 2s", set offline_mode = ON, observe
  that the event has not fired by "now + 5s", set offline_mode = OFF,
  observe (or accept) that the missed event is skipped.
- An in-flight event continues to completion before offline_mode = ON
  returns (use DEBUG_SYNC to pin the event mid-execution).

## Stage 0c: replica stop on offline_mode = ON

Today `Sql_cmd_install_extension::execute_update` has a precondition
that rejects ALTER if any replica thread is running
(`replica_count > 0` in `check_no_active_connections`). That check is
a *one-shot snapshot*: at the moment ALTER runs, no replicas. It
doesn't prevent `START REPLICA` from being issued from another admin
session *during* the ALTER, which would start applying events
mid-swap.

`offline_mode = ON` today does nothing to replicas — they keep
running until ALTER's precondition rejects.

### Two failure shapes today

1. **Active replicas at ALTER time**: precondition rejects ALTER.
   Correct; clear error to the operator.
2. **Replica started mid-ALTER**: not protected. The MDL X lock on
   the extension blocks DDL but does not block a replica applier
   thread that's about to execute a stored program referencing the
   extension's types. The window is small but real.

### What's needed

`SET GLOBAL offline_mode = ON` must, additionally:

1. Synchronously stop all configured replication channels (IO thread,
   SQL thread / applier coordinator and workers). Equivalent to
   `STOP REPLICA` on every channel.
2. While offline_mode is ON, reject `START REPLICA` with a clear
   error.
3. On `offline_mode = OFF`, do NOT automatically restart replicas —
   the operator decides when to resume replication.

With this in place, ALTER's `replica_count == 0` precondition becomes
a tautology under offline_mode = ON. The check stays as defense in
depth (we still want a clear error if the invariant is violated for
any reason).

### Trade-off

Stopping replicas is operationally significant: a server in
`offline_mode = ON` is now also detached from its replication topology.
For operators using `offline_mode` for things adjacent to its
documented purpose (e.g. as a soft maintenance mode), this is a
behavior change. We accept it because:

- The existing `Sql_cmd_install_extension::execute_update` precondition
  already requires `replica_count == 0`, so any operator running ALTER
  today already has to stop replicas first. Stage 0c automates the
  step they were already doing.
- The "soft maintenance mode" use of offline_mode is not what it's
  documented for; group_replication already turns offline_mode on for
  fencing, which is closer to the spirit.

### Scope of Stage 0c

- Extend `handle_offline_mode` in `sys_vars.cc` to call
  `stop_slave_threads` (or the modern equivalent) for each channel
  before returning success.
- Add a check in `START REPLICA` that rejects when offline_mode is on.
- Document the behavior change.
- Tests:
  - Configure a replica, set offline_mode = ON, observe replicas
    stopped, observe START REPLICA fails.
  - The existing ALTER + replica-running precondition test still
    triggers (defense-in-depth coverage).

## Interaction with Stages 1 and 2

Stage 0 (all three sub-stages) narrows the surface. Stage 1 still
applies but only to a smaller set of failure modes:

- Pre-Stage-0: "what if capability N's swap fails after capability
  N-1 already swapped, and N-1's runtime is still running?" Two
  problems entangled.
- Post-Stage-0: "what if capability N's swap fails after capability
  N-1 already swapped?" Just the in-memory undo problem. Stage 1
  solves it cleanly because nothing is running.

Stage 2 (sys-var-specific catalog-state-of-truth) is orthogonal to
Stage 0 — sys vars have no live state to quiesce. Stage 2 still needs
to happen for the crash-mid-update window.

## What Stage 0 does NOT cover

- A capability whose `quiesce` itself takes a long time (e.g. waiting
  for in-flight async work to flush) extends the time the server spends
  in offline_mode visibly to operators. We surface this as `offline_mode
  = ON` blocking until quiesce completes, which is correct but slow.
- Capabilities that have no quiesce-able boundary — e.g. data on disk
  written by extension code that survives offline_mode — are not
  addressed here. None exist today; if one appears, Stage 0 alone is
  insufficient for it.
- Plugin/component background threads. None today resolve extension
  references. If one ever does, we'll need a comparable contract for
  plugins/components or a documented prohibition.
- Failures *inside* a capability's swap on quiesced state. These are
  Stage 1's problem.

## Stage 1: in-process undo registry

Each capability hook that mutates non-catalog state registers a
matching undo action with the swap context. If any subsequent step
fails — another capability's `on_swap_update`, mark_for_insertion, the
catalog commit — the framework walks the registered undo actions in
reverse and runs them before propagating the error.

API sketch:

```cpp
struct UpdateSwapContext {
  // ... existing fields ...
  // Append an action to run if any subsequent step (including this
  // capability's later steps) fails. Runs in reverse registration order.
  void add_undo(std::function<void()> fn);
};
```

The sys-var capability would use it like:

```cpp
bool sys_var_on_swap_update(UpdateSwapContext &ctx, std::string &error) {
  // Snapshot the file content we're about to overwrite.
  std::string saved = read_file_to_string("mysqld-auto.cnf");

  // Apply the change.
  if (rewrite_mysqld_auto_cnf(...)) return true;

  // Register the undo. Runs only if a later step fails.
  ctx.add_undo([saved] { write_string_to_file("mysqld-auto.cnf", saved); });

  // Continue with more mutations, registering undos for each as we go.
  // In-memory sys-var unregister/register also gets paired undos.
  ...
  return false;
}
```

`execute_upgrade_swap` (in the capability registry) collects undos as
each capability's `on_swap_update` succeeds. If any returns failure, it
runs the accumulated undos in reverse and returns the error. The
downstream failure paths in `execute_upgrade` (mark_for_insertion,
write_all_uncommitted_entries, end_transaction commit failure) likewise
trigger the undo walk before returning.

### What it covers

- A later capability's hook fails after an earlier one swapped.
- `mark_for_insertion` / `rewrite_column_and_sp_param_versions` fails.
- `write_all_uncommitted_entries` fails.
- Catalog commit fails (`end_transaction(thd, false)` returns true).

In every case, the file rewrite is undone, the in-memory swap is undone,
and the server is left consistent with the rolled-back catalog. No
restart needed.

### What it does NOT cover

- **Server crash mid-update**. Undo callbacks live in process memory and
  die with the process. On restart, the catalog has been rolled back by
  crash recovery but the file is whatever the last `fsync` left.
- **Undo failures**. An undo callback may itself fail (disk full,
  permission change). Policy: log at ERROR level, continue running
  remaining undos (best effort), and escalate to the
  "lockout-offline_mode" mechanism so the admin sees something is wrong.

### Other subtleties

- **Lifetime of captured data**: `std::function` captures snapshots by
  value. For `mysqld-auto.cnf` (typically a few KB) that's fine. If a
  future capability needs to snapshot something large, it can write to a
  temp file at capture time and capture only the path.
- **Hook ordering**: undos run in strict reverse order of registration.
  Within a capability, the capability owns its order; across
  capabilities, the registration order of `on_swap_update` calls.
- **Clearing on success**: on a successful commit, the undo list is
  dropped — nothing to roll back.

### Scope

Stage 1 is small:

- `add_undo` method on `UpdateSwapContext`.
- Undo-list plumbing in `execute_upgrade_swap` / `execute_upgrade`.
- Sys-var capability uses it to snapshot `mysqld-auto.cnf` and pair its
  in-memory registrations with undos.
- Tests that force a failure after the sys-var swap and verify the file
  + in-memory state are restored.

Could be a single PR after the current ALTER stack lands.

## Stage 1 alternative: uninstall-new + install-old

Instead of every capability author writing paired `add_undo()` callbacks,
treat capability rollback as "uninstall the new registration, install the
old registration." Each capability already implements `on_install` and
`on_unload` for its normal lifecycle; rollback reuses them.

When `execute_upgrade_swap` is iterating capabilities and capability N
fails, the registry walks capabilities `N-1 .. 0` in reverse and, for
each, calls:

```
on_unload(new_reg)   // tear down what swap-update just set up
on_install(old_reg)  // rebuild from the old registration
```

No new API. No per-capability undo state. The same code paths the
server runs at INSTALL EXTENSION / UNINSTALL EXTENSION time also serve
as the rollback machinery.

### Why this can work

`on_install` and `on_unload` are already exhaustive: they have to be,
because INSTALL and UNINSTALL must leave the capability in a fully
correct in-memory state. If they already work for "fresh install of v1"
and "tear down v2," composing them at rollback time gives "fresh
install of v1 after v2 was already torn down" — which is exactly what
we want post-rollback.

For the catalog itself, rollback isn't needed — `end_transaction(thd,
true)` calls `trans_rollback` and InnoDB undoes the row changes in the
victionary tables. This alternative addresses only the
*capability-side* state that lives outside the transaction.

### What it covers

- A later capability's `on_swap_update` fails. Earlier capabilities are
  walked in reverse: each gets `on_unload(new_reg)` to tear down the v2
  state, then `on_install(old_reg)` to rebuild v1. Catalog rolls back
  via `end_transaction(thd, true)`.
- Downstream failures (`mark_extension_for_insertion`,
  `write_all_uncommitted_entries`, catalog commit) trigger the same
  reverse walk before propagating the error.

### What it does NOT cover

- **Persistent side-effects that `on_install(old_reg)` can't restore.**
  The clearest case is `mysqld-auto.cnf`: v2's swap rewrote it, and
  `sys_var_on_install(old_reg)` will read whatever is in the file *now*
  (the rewritten content), not what was there before the UPDATE.
  Stage 2 (catalog source of truth) is still required for this.
- **Persistent values dropped during the swap.** If v1 had a sys var
  `foo.bar = 5` persisted and v2 removed `foo.bar` entirely, the swap
  deleted that persisted value. `on_install(old_reg)` doesn't have the
  old value to restore. Same Stage-2 problem.
- **Atomicity of the rollback itself.** `on_unload(new_reg)` succeeds,
  then `on_install(old_reg)` fails (out of memory, OS resource
  exhaustion, etc.). We're now stuck with neither version's
  capability state present — strictly worse than starting state. This
  is the same failure mode as the undo-registry approach hitting an
  undo that itself fails (see "Undo failures" above), so neither
  alternative wins here. Policy is the same: log, set the lockout
  flag, force a restart.
- **Cost.** A late rollback (e.g. `write_all_uncommitted_entries`
  failed) does a full unload+reinstall cycle per capability. Cheap if
  there are few capabilities; potentially expensive once thread workers
  with slow shutdown or async writers with large drains exist. The
  undo-registry approach can be more selective — only the actual diffs
  get undone. We accept the cost as the price of not maintaining a
  parallel undo API.

### Tradeoff vs. the undo registry

| Concern | Undo registry | Uninstall-new + install-old |
|---|---|---|
| Capability author burden | Every mutation paired with `add_undo()` | Zero — reuse existing lifecycle hooks |
| API surface | New `add_undo` on `UpdateSwapContext` | None |
| Selectivity / cost | Only un-do what was actually done | Full teardown + rebuild per capability |
| Persistent side-effects | Same coverage as alternative (Stage 2 still required) | Same |
| Failure of the rollback itself | Same — logged + lockout | Same |
| Composability with quiesce (Stage 0) | Works post-quiesce | Works post-quiesce |
| Code paths exercised | New (rollback-only) | Pre-existing (also tested via INSTALL/UNINSTALL) |

The trade is **mechanism (per-capability undo callbacks) vs cost
(redundant teardown+rebuild)**. For a capability portfolio of "sys
vars, query hooks, eventually thread workers and async writers" — all
of which already have to support clean install/uninstall — the
mechanism cost dominates. The alternative reduces it to zero by
declining to invent the mechanism at all.

### Open questions

- **`old_reg` lifetime.** The registry must hold a reference to the
  old `ExtensionRegistration` for the duration of the swap so that
  rollback can pass it to `on_install`. The catalog code already keeps
  the old version's descriptor in the victionary until commit (it's
  what `mark_extension_for_deletion` queues for deletion); we just
  need to make sure rollback runs before that pending-deletion is
  resolved.
- **Hook ordering.** `on_install(old_reg)` may have ordering
  requirements between capabilities (e.g. types before VDFs that
  reference them). The current install path already enforces that
  ordering; rollback walks capabilities in reverse-of-swap order,
  which is the same order INSTALL uses (each capability's
  `on_install` runs in registration order). This should compose
  cleanly, but worth verifying against real capability mixes when we
  build it.
- **What if `on_unload(new_reg)` fails partway?** Today
  `on_unload` is best-effort — UNINSTALL doesn't have a
  re-install-and-give-up escape hatch. We'd need the same: log,
  continue with `on_install(old_reg)` anyway, escalate to lockout if
  things look broken at the end.

### Scope

Roughly the same as the undo-registry version:

- Registry tracks the old `ExtensionRegistration` for the duration of
  swap.
- `execute_upgrade_swap` rollback path walks completed capabilities in
  reverse, calling `on_unload(new_reg)` then `on_install(old_reg)`.
- `execute_upgrade`'s post-swap failure paths trigger the same walk.
- Tests force failures at each rollback-relevant point and verify the
  capability state matches v1.

The actual code is *less* than the undo-registry version — no new
`add_undo` API, no per-capability callback writing, just a small
loop in the registry.

## Stage 1 alternative: snapshot + restore

A third shape: before any capability's `on_swap_update` runs, give
each capability a chance to **snapshot** whatever state it's about to
mutate. The framework holds the snapshot opaquely for the duration of
the swap. On any failure, the framework walks completed capabilities
in reverse and asks each one to **restore** from its snapshot.

This is the begin/commit/rollback shape, applied to non-catalog state.
The snapshot is the rollback log; restore is the rollback; free on
success is the commit.

### API

Three new fields on the capability registration struct:

```cpp
struct CapabilityRegistration {
  // ... existing fields ...

  // Capture whatever state on_swap_update is about to mutate.
  // Capability allocates an opaque blob and returns it via
  // out_snapshot; framework stores the pointer and threads it back
  // to restore_pre_upgrade on failure. Returns true on error
  // (snapshot infeasible) — ALTER refuses to proceed.
  bool (*snapshot_pre_upgrade)(const UpgradeContext &ctx,
                               void **out_snapshot,
                               std::string &error_message) = nullptr;

  // Restore from the snapshot. Called only on failure paths, in
  // reverse order across capabilities that successfully snapshotted.
  // Best-effort; failures here are logged and escalated to lockout.
  void (*restore_pre_upgrade)(const UpgradeContext &ctx,
                              void *snapshot) = nullptr;

  // Free a snapshot. Always called — on success after the swap
  // commits, on failure after restore_pre_upgrade runs.
  void (*free_snapshot)(void *snapshot) = nullptr;
};
```

### Driver shape

```cpp
// Phase A: snapshot all capabilities.
for (cap : caps) {
  if (snapshot_pre_upgrade(cap, &snapshots[cap])) {
    free_all(snapshots);  // capabilities that already snapshotted
    return abort_with_error;
  }
}

// Phase B: run the swaps.
for (cap : caps) {
  if (on_swap_update(cap)) {
    // Failure: restore in reverse, then free everything.
    for (prev : reverse(completed_caps))
      restore_pre_upgrade(prev, snapshots[prev]);
    free_all(snapshots);
    return abort_with_error;
  }
  completed_caps.push(cap);
}

// Phase C: success. Throw away snapshots.
free_all(snapshots);
```

### What it covers

- A later capability's `on_swap_update` fails. Earlier capabilities
  are restored from their snapshots in reverse order. The state on
  disk and in memory ends up identical to pre-ALTER.
- Downstream failures (`mark_extension_for_insertion`,
  `write_all_uncommitted_entries`, catalog commit) trigger the same
  restore walk.
- **Persistent side-effects** (`mysqld-auto.cnf` rewrite, log file
  rotation, anything the capability wrote during swap) — the
  snapshot can include the pre-swap content, and restore writes it
  back. This is the differentiating capability vs. the
  uninstall-new+install-old alternative, which cannot restore content
  that's already been overwritten.

### What it does NOT cover

- **Crash mid-update.** Snapshots live in process memory and die with
  the process. After a crash, the file has been rewritten by v2's
  swap and there's no snapshot to consult. Stage 2 (catalog source of
  truth) is still required for crash safety.
- **Snapshot itself fails.** A capability that can't snapshot (out of
  memory, can't read file, etc.) aborts ALTER before any mutation. By
  policy this is the *right* behavior — ALTER refuses rather than
  proceed with no rollback path — but it introduces a new failure
  mode that needs documenting.
- **Atomicity of restore.** If `restore_pre_upgrade` itself fails
  (disk full while writing back, permissions changed), state ends up
  partially restored. Same policy as the other alternatives: log,
  set the lockout flag, force a restart.
- **Cost on the happy path.** Every ALTER pays snapshot cost even on
  success. For small snapshots (sys-var: a few KB of
  `mysqld-auto.cnf`) this is negligible. For larger snapshots (a
  capability with megabytes of in-memory state) it's a real cost. We
  accept it as the price of having a rollback path that handles
  persistent state.

### How it compares to the other two alternatives

| Concern | Undo registry | Uninstall-new+install-old | Snapshot + restore |
|---|---|---|---|
| New API surface | `add_undo()` on UpdateSwapContext | None | 3 hooks per capability |
| Author burden per capability | Paired lambdas with each mutation | Zero (reuse existing lifecycle) | Two functions (snapshot, restore) |
| Cost on success | Zero | Zero | Snapshot cost (always paid) |
| Cost on failure | Selective undo of what ran | Full teardown + rebuild | Restore from snapshot |
| Persistent side-effects (e.g. `mysqld-auto.cnf`) | Capability does it via undo lambda | **NOT covered** — re-install reads current file | Capability captures pre-swap content |
| Crash mid-update | Not covered (memory only) | Not covered | Not covered |
| Failure of the failure-handling itself | Same — log + lockout | Same — log + lockout | Same — log + lockout |
| Composability with Stage 0 quiesce | Yes | Yes | Yes |
| Snapshot can fail | N/A | N/A | New failure mode (correct behavior, but new) |

### Where each alternative is best

The three alternatives are not all-or-nothing — they could even
coexist, with each capability picking the contract that fits its
state shape:

- **Undo registry** — best when mutations are many small steps and
  selective undo matters (e.g. a capability that does fine-grained
  N+1 updates where redoing them all from scratch is expensive).
- **Uninstall-new + install-old** — best when capability state is
  fully reconstructible from catalog state (no on-disk side effects
  beyond the catalog). After PR 2 of the index-capability-leak-cleanup
  lands, the index capability fits this — its state is entirely in
  the victionary, rebuildable from the install hooks.
- **Snapshot + restore** — best when capability state includes
  non-catalog persisted content the capability must restore byte-for-
  byte. Sys-var capability today (writes `mysqld-auto.cnf`) fits this
  more cleanly than the other two.

A natural reading of the trilemma:

- For capabilities that have only in-victionary state, prefer
  **uninstall-new + install-old**. Zero new code; reuses lifecycle
  hooks PR 2 of the cleanup will require anyway.
- For capabilities with non-catalog persisted state, prefer
  **snapshot + restore**. The persistent-side-effect coverage matters
  more than the snapshot-cost-on-success.
- **Undo registry** as a third option for capabilities whose
  mutations don't fit either shape cleanly (none today).

If we ship a single mechanism, **snapshot + restore is the most
defensive**: it handles every case the others handle plus persistent
state, at the cost of always-paid snapshot work. If we ship a
per-capability choice, sys vars pick snapshot+restore and index
picks uninstall+install — each gets the cheapest contract that works
for its state.

### Open questions

- **Snapshot ordering.** Snapshots must be taken before any
  capability's `on_swap_update` runs, otherwise an early capability's
  swap may have already mutated state a later capability would have
  snapshotted. Driver enforces "all snapshots, then all swaps."
- **Snapshot lifetime.** Snapshots are framework-owned for the
  duration of the swap. They must survive lock release / re-acquire
  / etc. — i.e. live in heap-allocated capability-owned blobs, not
  on the stack of the driver.
- **Co-existence with the other alternatives.** If we allow
  per-capability choice, the driver must handle a mix: some
  capabilities supply `snapshot_pre_upgrade` (and we restore), some
  don't (and we don't). Either alternative needs to be the *default*,
  not all-or-nothing.

### Scope

Comparable to the undo-registry version:

- Three hook fields on `CapabilityRegistration`.
- Snapshot map plumbing in `execute_upgrade_swap`.
- Sys-var capability implements `snapshot_pre_upgrade` (read
  `mysqld-auto.cnf` content, capture pending in-memory descriptors)
  and `restore_pre_upgrade` (write file back, restore descriptors).
- Tests force failure after the sys-var swap and verify file +
  in-memory state are byte-for-byte the pre-ALTER state.

Could be a single PR after the current ALTER stack lands. The hook
contract is independent of the undo-registry contract, so both
alternatives could be implemented and the choice deferred — but in
practice we'll pick one shape and stick with it (or allow per-
capability choice).

## Stage 1 alternative: capability-state WAL (recommended)

This shape combines Stage 1's in-process rollback with Stage 2's
crash detectability. The undo state lives in a new victionary
table that is written *independently* of the main ALTER
transaction. After the main ALTER commits, the WAL rows are
deleted. If the main ALTER fails (in-process or crash), the WAL
rows persist and the framework surfaces them to the operator with
a recovery hint that the capability supplied at prepare time.

The mental model is write-ahead log:

- Before mutating any non-catalog state, the capability writes an
  undo blob into the WAL table and commits it.
- The mutation runs.
- After the mutation succeeds, the capability marks the WAL row as
  committed.
- After the main ALTER transaction commits, the WAL rows are
  deleted (the upgrade succeeded, the undo log is no longer
  needed).
- On any failure path, the WAL rows linger and become the
  operator-facing record of an inconsistent state.

The WAL itself rides regular victionary transactional infrastructure
— its writes go through `MarkForInsertion`/`MarkForUpdate` and commit
via the same mechanism as any other victionary table. The
distinction from the main ALTER transaction is *only* that WAL
writes commit on their own (their own short-lived transactions),
not bundled with the catalog mutations.

### Preview-era framing

All capabilities today are preview features. GA features by
definition keep their state entirely under victionary
transactional control — they don't mutate anything outside the
catalog, so they don't write WAL rows.

This means the WAL exists specifically to make preview-feature
failures *visible* and *bounded*. Slice 1 of this work is
**detect-and-report**, not automated recovery. The promise to
operators is: "if a preview capability's mid-ALTER mutation fails,
we will tell you exactly what state is inconsistent; you are on
your own to restore it." This is an honest framing of what
preview means.

Slice 2 (later) adds *optional* automated recovery, capability by
capability, when each preview capability matures enough for its
author to claim "I can reliably restore from my undo blob."

### The WAL table

New framework-owned system table, opaque JSON blob body and a
capability-supplied recovery hint:

```sql
CREATE TABLE villagesql.capability_upgrade_state (
  extension_name VARCHAR(64) NOT NULL,
  capability_name VARCHAR(128) NOT NULL,
  target_version VARCHAR(64) NOT NULL,
  state ENUM('prepared', 'committed') NOT NULL,
  recovery_mode ENUM('restart_recovers',
                     'restore_from_blob',
                     'manual_only') NOT NULL,
  undo_blob JSON NOT NULL,
  created_at TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (extension_name, capability_name)
);
```

One row per capability per in-flight upgrade. The capability
serializes whatever it needs for rollback into `undo_blob` —
the framework doesn't interpret the body. `recovery_mode` is the
capability's classification of *what kind* of recovery this
particular mutation needs:

- `restart_recovers` — server restart re-establishes correct
  state. The capability's mutation was purely in-memory, and the
  server will reload it from the catalog on the next startup.
  Sys-var capability uses this when no `mysqld-auto.cnf` rewrite
  was needed (no persisted values changed by this ALTER).
- `restore_from_blob` — the undo blob contains the information
  needed to reverse the mutation. Sys-var uses this when
  `mysqld-auto.cnf` was rewritten; the blob carries the
  pre-rewrite file content. Slice 2 calls the capability's
  `restore_from_wal` hook automatically; slice 1 surfaces the
  blob to the operator for manual action.
- `manual_only` — no programmatic recovery exists. The
  capability did something only a human can reverse. The undo
  blob is informational (description of what was done), not
  prescriptive.

`target_version` is recorded so recovery can disambiguate "main
transaction committed (catalog at target), WAL cleanup didn't
finish" from "main transaction did not commit (catalog at old
version), WAL needs to drive a rollback."

### API exposed to capabilities

```cpp
enum class RecoveryMode {
  kRestartRecovers,
  kRestoreFromBlob,
  kManualOnly,
};

// Record undo state before mutating non-catalog state.
// Commits its own short transaction (does NOT participate in the
// main ALTER's transaction). Returns true on error; capability
// must abort the mutation if this fails.
bool capability_state_prepare(const char *extension_name,
                              const char *capability_name,
                              const char *target_version,
                              RecoveryMode recovery_mode,
                              const char *undo_blob_json,
                              std::string &error_message);

// Mark as committed after the non-catalog mutation succeeded.
// Also commits its own short transaction.
bool capability_state_mark_committed(const char *extension_name,
                                     const char *capability_name,
                                     std::string &error_message);

// Read all WAL rows for an extension. Used by detection passes
// and (slice 2) the automated recovery path.
struct CapabilityStateRow {
  std::string capability_name;
  std::string target_version;
  RecoveryMode recovery_mode;
  std::string undo_blob_json;
  enum State { kPrepared, kCommitted } state;
};
bool capability_state_get_for_extension(
    const char *extension_name,
    std::vector<CapabilityStateRow> &out_rows,
    std::string &error_message);

// Delete all WAL rows for an extension. Called after the main
// ALTER commits (success path cleanup), and after recovery is
// done.
bool capability_state_delete_for_extension(
    const char *extension_name,
    std::string &error_message);
```

The capability decides the `RecoveryMode` per WAL row at prepare
time. If a capability's mutation has heterogeneous parts (some
restart-recoverable, some not), it bundles them into one row and
uses the strictest mode that covers everything. Conservative
labeling is safe; over-promising is not.

### Hook on the capability registration (slice 2 only)

```cpp
struct CapabilityRegistration {
  // ... existing fields ...

  // Restore non-catalog state from a WAL undo blob. Only called
  // for WAL rows with recovery_mode == kRestoreFromBlob.
  //
  // Slice 2 only. In slice 1, this hook does not exist; all
  // recovery_mode rows surface to the operator.
  //
  // Returns true on failure (restore couldn't complete); the
  // framework treats this as if recovery_mode were
  // kManualOnly — surface to the operator, leave the row in
  // place, lockout the extension.
  bool (*restore_from_wal)(const RestoreContext &ctx,
                           const char *target_version,
                           const char *undo_blob_json,
                           std::string &error_message) = nullptr;
};
```

### Operational sequence (success path)

```
ALTER EXTENSION foo UPDATE TO '2.0.0':

  Start main ALTER transaction.
  (catalog mutations: types, VDFs, columns, extension entry)

  For each required capability:
    Capability captures undo state (e.g. read mysqld-auto.cnf
        into a string).
    capability_state_prepare(foo, sys_var, 2.0.0,
                             kRestoreFromBlob,
                             {file: "..."})
        — commits the WAL row immediately.
    Capability runs its mutation (rewrite mysqld-auto.cnf).
    capability_state_mark_committed(foo, sys_var) — commits the
        WAL row update immediately.

  Main ALTER transaction commits.
        ↑ both catalog and the persisted WAL rows are now durable.

  capability_state_delete_for_extension(foo)
        — commits the deletion of the WAL rows. The upgrade is
          complete, the undo log is no longer needed.
```

### Failure paths

#### Slice 1: detect-and-report

The WAL rows are not consulted programmatically beyond detection.
Stale rows are surfaced to the operator with the recovery_mode
shaping the message.

**In-process failure** before main commit:

```
Rollback the main ALTER transaction.
  ↑ catalog rolls back to v1. WAL rows remain (they were
    committed independently).

Return error from ALTER. The detection pass on the NEXT ALTER
attempt (or server restart) will see the stale WAL rows.
```

**Detection pass**, invoked at server startup AND at the start
of every `ALTER EXTENSION` for the named extension:

```
rows = capability_state_get_for_extension(target_extension)
if rows is empty: proceed normally.
else:
  installed_version = victionary.extensions.lookup(target).version

  if rows[0].state == 'committed' and installed_version ==
      rows[0].target_version:
    # The main ALTER committed; cleanup of the WAL just didn't
    # finish. Garbage-collect.
    capability_state_delete_for_extension(target_extension)
    proceed.

  else:
    # Inconsistent state.
    Surface error with row contents:
      For each row, report:
        capability_name
        recovery_mode
        target_version
        (undo_blob is referenced, not dumped)
      Per-row guidance:
        kRestartRecovers   → "restart the server to recover"
        kRestoreFromBlob   → "automated recovery not yet
                              implemented for this capability;
                              inspect the row and restore
                              manually"
        kManualOnly        → "manual intervention required;
                              inspect the row"
    Lockout: refuse further ALTER on this extension until the
             operator deletes the row(s).
    ALTER on OTHER extensions is unaffected.
```

The operator's manual recovery procedure for slice 1:

1. Read the row(s):
   `SELECT * FROM villagesql.capability_upgrade_state WHERE
        extension_name = 'foo';`
2. For each row, apply the appropriate recovery action:
   - `restart_recovers`: restart the server. The startup
     detection pass will see installed_version != target_version
     (or the row will be deleted by the detection pass after
     restart confirms the in-memory state matches what catalog
     says).
   - `restore_from_blob`: parse `undo_blob`, restore the state by
     hand (e.g. write `mysqld-auto.cnf` content back from the
     blob).
   - `manual_only`: follow the capability's documented recovery
     procedure.
3. Delete the row:
   `DELETE FROM villagesql.capability_upgrade_state WHERE
        extension_name = 'foo';`
4. ALTER on the extension is now unblocked.

#### Slice 2: automated recovery for `restore_from_blob`

When the `restore_from_wal` hook lands, the detection pass gains
an automated-recovery step for `kRestoreFromBlob` rows:

```
For each row in reverse order:
  if row.recovery_mode == kRestoreFromBlob and the capability
       registered restore_from_wal:
    Call restore_from_wal(ctx, row.target_version, row.undo_blob_json)
    if success: continue
    if failure: surface to operator, lockout, stop the walk.
  else:
    surface to operator, lockout, stop the walk.

Once the walk completes successfully:
  capability_state_delete_for_extension(target_extension)
  ALTER on this extension is unblocked.
```

The slice-2 work is *additive*: WAL writers don't change, the
recovery_mode labels don't change. Only the detection pass gains
the auto-recovery branch.

### What slice 1 covers

- **Detection of inconsistent state** from any in-process failure
  or crash during an ALTER on a preview capability that uses the
  WAL.
- **Surgical lockout** of the specific extension whose state is
  inconsistent, with a clear, actionable error referencing the
  WAL row.
- **Honest framing**: "you used a preview capability, an upgrade
  failed mid-flight, we noticed, we're telling you, you fix it."
- **GC of stale-clean rows** (where cleanup didn't finish but
  the main ALTER committed) so that successful upgrades don't
  leave debris that looks like an inconsistency.

### What slice 1 does NOT cover

- **Automated recovery.** Operators must follow the recovery hint
  manually. Slice 2 adds automation for `restore_from_blob` rows
  whose capability registered `restore_from_wal`.
- **WAL bloat from never-running detection.** If recovery is never
  attempted (no further ALTER and no restart), stale rows
  accumulate. In practice the next admin action against the
  extension triggers detection; the bloat is bounded.
- **WAL write failure during prepare.** If
  `capability_state_prepare` can't commit (catalog table full,
  etc.), the capability aborts and ALTER fails. Correct
  behavior — without the WAL we can't safely mutate.

### How it compares to the other Stage 1 alternatives

| Concern | Undo registry | Uninstall+install | Snapshot+restore | **Capability-state WAL** |
|---|---|---|---|---|
| New API surface | `add_undo()` | None | 3 hooks | 4 framework calls + new system table (slice 1); +1 hook (slice 2) |
| Author burden per capability | Paired lambdas | Zero | Two functions | Pick prepare/commit call sites + classify recovery_mode |
| Cost on success | Zero | Zero | Snapshot cost | Two extra commits per capability |
| Cost on failure | Selective undo | Full teardown+rebuild | Restore from snapshot | Slice 1: operator manual; slice 2: read WAL + restore from blob |
| Persistent side-effects | Cap does via lambda | NOT covered | Cap in snapshot | Cap in WAL blob |
| **Crash mid-update** | **NOT detected** | **NOT detected** | **NOT detected** | **Detected; report (slice 1) or recover (slice 2)** |
| Restore failure | Same — log + lockout | Same | Same | Per-row hint determines whether automated recovery was even attempted |
| Composability with Stage 0 quiesce | Yes | Yes | Yes | Yes |
| Subsumes Stage 2? | No | No | No | **Mostly** — see below |

### Why this is the recommended alternative

It's the only alternative that is **honest about the preview era**.
The other three all assume "we should automatically recover from
failure," but for preview capabilities that's a promise we can't
universally keep (some mutations aren't programmatically
reversible). The WAL framing flips the contract: every capability
*declares* what kind of recovery is possible for each mutation,
the framework promises *detection* universally, and *automation*
follows per-capability as capabilities mature.

The cost is two extra commits per capability per upgrade and the
new system table. Both are bounded, and both align with how the
catalog work already runs.

### Relationship to Stage 2

Stage 2 (sys-var-specific catalog-state-of-truth) was designed to
solve the crash window for sys vars by moving the persisted store
into the catalog. The WAL approach addresses the same crash
window *generically*: any capability with persistent state gets
crash detectability (slice 1) and a documented recovery path
(slice 2), not just sys vars.

After the WAL lands, Stage 2's *crash-safety* motivation is gone.
Stage 2's *conceptual cleanliness* motivation (catalog as single
source of truth) remains, but it's not load-bearing.

### Open questions

- **Transactional isolation of the WAL.** `capability_state_prepare`
  commits its own transaction. Standard READ COMMITTED visibility
  should suffice; the row is essentially admin-debug information.
- **WAL rows for INSTALL / UNINSTALL.** The same shape may be
  useful for INSTALL EXTENSION and UNINSTALL EXTENSION when
  preview capabilities have non-catalog side-effects there too.
  Out of scope for the initial design; the WAL is not
  UPDATE-specific.
- **Multiple capabilities, same step.** The PK is
  `(extension_name, capability_name)` — one row per capability.
  Capabilities with multiple internal mutations bundle the undo
  for all of them into the one JSON blob and use the strictest
  recovery_mode that covers them all.
- **`SET GLOBAL offline_mode = OFF` while WAL rows exist.**
  Operator's call; the framework warns but doesn't refuse.

### Scope of slice 1

- New system table `villagesql.capability_upgrade_state` and its
  victionary entity (entry struct, traits, container, schema
  manager registration).
- Three framework API functions exposed to capabilities
  (`prepare`, `mark_committed`, `delete_for_extension`) plus the
  `get_for_extension` reader used by the detection pass.
- `RecoveryMode` enum exposed to capabilities.
- Detection pass invoked at server startup AND at the start of
  every `ALTER EXTENSION` on the named extension.
- Stale-clean GC (`installed_version == target_version` rows
  cleaned automatically; only inconsistent rows surface).
- WAL cleanup invocation in `execute_upgrade`'s commit path.
- Sys-var capability migrated to use the WAL: classifies its
  mutation as `kRestartRecovers` when no file rewrite is needed,
  `kRestoreFromBlob` when it rewrites `mysqld-auto.cnf`.
- Operator-facing error messages with per-row guidance based on
  `recovery_mode`.
- Tests: force in-process failure after sys-var prepare, observe
  detection at next ALTER. Force crash (kill) between prepare and
  mark_committed and again between mark_committed and main commit,
  observe detection at startup. Verify lockout is per-extension.
  Verify operator-side DELETE clears the lockout.

### Scope of slice 2 (later)

- Add `restore_from_wal` hook to `CapabilityRegistration`.
- Extend the detection pass: for `kRestoreFromBlob` rows whose
  capability registered the hook, call it; on success, delete
  the row.
- Per-capability migration: each preview capability that wants
  automated recovery implements `restore_from_wal` and is tested
  with forced-failure scenarios.
- Tests verifying that auto-recovery is invisible to the operator
  in the success case, and that the failure mode (auto-recovery
  itself errored) falls through to the slice-1 manual path.

Slice 2 is strictly additive — slice 1's contract doesn't change,
existing WAL writers don't need updating.

## Stage 2: capability state in the victionary

Stage 1 closes the in-process gap but leaves crash recovery unsolved:
`mysqld-auto.cnf` is on disk, the catalog is in InnoDB, and a crash
between the two writes leaves them out of sync. Stage 2 removes the
problem entirely by moving extension sys-var persistence into a
victionary table — the catalog becomes the single source of truth, and
the on-disk file no longer holds extension state.

A new system table that holds persisted sys-var values, owned by the
sys-var capability:

```
villagesql.extension_sys_vars
  extension_name     VARCHAR
  extension_version  VARCHAR
  var_name           VARCHAR
  value              TEXT
  PRIMARY KEY (extension_name, var_name)
```

`extension_version` is stored alongside the value but the key is
`(extension_name, var_name)` so a persisted setting survives across
UNINSTALL+INSTALL of the same extension. (Open question — see "decisions"
below.)

Per the existing victionary pattern, this gets:

- A typed entity class (`ExtensionSysVarEntry` or similar) under
  `villagesql/schema/systable/`.
- A victionary container with `MarkForInsertion` / `MarkForUpdate` /
  `MarkForDeletion` semantics.
- A `villagesql.extension_sys_vars` row in the schema manager.
- An accessor on `VictionaryClient` (`extension_sys_vars()`), matching
  `columns()` / `sp_params()` / `funcs()`.

Once it's a victionary entity, it participates in the same transaction
as the rest of ALTER's catalog work. The mysqld-auto.cnf path is gone
entirely for extension vars.

### What changes in `execute_upgrade`

The DDL prepare phase already opens `custom_columns`, `custom_sp_params`,
and `extensions` as a chained `Table_ref`. It gains the new table:

```cpp
Table_ref cols_table(...);
Table_ref sp_params_table(...);
Table_ref sys_vars_table(...);  // NEW
Table_ref ext_table(...);
// chained in order
```

If a future capability needs its own table, it gets appended here. The
schema manager could expose `open_all_victionary_tables(thd)` to keep this
list in one place, but that's a small generalization to do when the second
capability table arrives — not now.

### What changes in the sys-var capability

The `update_sys_vars_for_extension` body
(`villagesql/services/preview/sys_var.cc`) gets restructured:

1. **Read** current persisted rows for the extension from
   `villagesql.extension_sys_vars` (via the victionary).
2. For each retained var (present in both old and new registrations):
   `MarkForUpdate` on its row to bump `extension_version`. If the type
   changed and the value is incompatible, `MarkForDeletion` with a
   warning.
3. For each dropped var (in old, not in new): `MarkForDeletion`.
4. For each new var (in new, not in old): nothing — no persisted value
   exists yet.
5. In-memory MySQL sys-var unregistration / registration still happens
   as today.

Steps 1-4 are pure victionary operations. They roll back atomically with
the rest of the transaction on failure. No file I/O during ALTER.

Step 5 is still in-memory only; a failure after step 5 leaves the
in-memory state inconsistent. But the in-memory state is fully
reconstructable from the table on next startup, so a restart recovers
cleanly even without the "lockout offline_mode" mechanism. (We may keep
the lockout anyway as a defense in depth, but it stops being load-bearing.)

### What changes in `SET PERSIST`

`SET PERSIST foo.x = 5` for an extension variable should write a row to
`villagesql.extension_sys_vars` instead of writing `mysqld-auto.cnf`. Two
plausible implementations:

- **Detect at the SQL layer**: hook into MySQL's `SET PERSIST` handler;
  if the variable is owned by an extension, divert to the capability's
  write path.
- **Per-variable persist callback**: when the sys-var capability registers
  a variable with MySQL, supply a custom persist callback that writes the
  victionary table instead of `mysqld-auto.cnf`. Cleaner if the
  registration API supports it; needs verification.

Either works; the second is preferable if available.

### Startup recovery

At startup, after the victionary has loaded its tables but before
extensions are loaded:

1. For each extension that has rows in `villagesql.extension_sys_vars`,
   the values are queued for application.
2. The extension's `.so` loads; the sys-var capability's `on_install`
   (or equivalent) reads its registration descriptors and registers the
   variables with MySQL.
3. The queued persisted values are applied to the newly-registered
   variables.

Ordering matters: vars must exist (step 2) before values are applied
(step 3). The capability owns this sequencing.

### Migration of existing `mysqld-auto.cnf` entries

Servers upgrading to this version may already have extension vars
persisted in `mysqld-auto.cnf`. One-time migration on first startup of
the new code:

1. Scan `mysqld-auto.cnf` for entries matching `<extension>.<var>`
   (extension-owned).
2. For each one, write a row to `villagesql.extension_sys_vars`.
3. Remove the entry from `mysqld-auto.cnf`.

Eager migration on startup is simplest. Lazy (migrate on first access)
would mean both stores are consulted indefinitely — more code paths to
get wrong.

### Open decisions

- **Key shape**: `(extension_name, var_name)` keeps persisted values
  across UNINSTALL+INSTALL of the same extension. `(extension_name,
  extension_version, var_name)` doesn't. The first matches the spirit of
  "persisted = remembers across reinstalls"; the second is simpler but
  has the surprising loss-on-uninstall behavior. Default: first.
- **Should the lockout-offline_mode mitigation still be implemented**
  after this fix lands? Probably yes, as defense in depth — but it
  becomes optional rather than required.
- **What about non-PERSIST `SET GLOBAL`?** Runtime-only writes don't need
  the table; they're already lost on restart by design.
- **Visibility**: should `mysqld-auto.cnf` continue to show extension vars
  (read-only, maybe)? Admins are used to looking there. Probably yes, but
  the file would be informational only — the victionary table is
  authoritative.

### Scope

This is a multi-week project on its own:

- New victionary entity type, key/value classes, container.
- Schema-manager registration of the new table.
- Sys-var capability rewrite to use the table.
- `SET PERSIST` integration.
- Startup recovery sequencing.
- Migration code for existing `mysqld-auto.cnf` entries.
- Tests: persisted value survives ALTER, survives mid-ALTER failure,
  survives crash, migrates correctly from `mysqld-auto.cnf` on upgrade.

Out of scope for the current ALTER EXTENSION stack. Tracked here so the
plan is in writing when we come back to it.
