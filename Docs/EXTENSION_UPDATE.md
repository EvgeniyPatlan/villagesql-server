# Extension Update

## SQL Syntax

```sql
ALTER EXTENSION name UPDATE TO 'x.y.z'
```

## VEB File Naming

Two naming conventions are supported:

| File name              | Description |
|------------------------|-------------|
| `{name}.veb`           | Unversioned. Version is read from `manifest.json` inside the archive. |
| `{name}-{version}.veb` | Versioned. Version in the filename must match `manifest.json`. |

`ALTER EXTENSION ... UPDATE TO` always requires an explicit version and loads
`{name}-{version}.veb`.

`INSTALL EXTENSION` (without `VERSION`) auto-discovers the version: if exactly one
`{name}-*.veb` exists in `veb_dir` it is used; if multiple exist the user must
specify `VERSION`; if none exist, `{name}.veb` is the fallback.

## Prerequisites

The goal is to ensure no session is currently using the extension and no new session
can start using it while the update is in progress.

**1. `offline_mode = ON`** — prevents new non-admin connections from being
established, ensuring no new queries can start using the extension.

**2. No active non-admin connections** — connections open before `offline_mode` was
set may still be running queries that use the extension. `ALTER EXTENSION` checks
and refuses to proceed if any are found. Admin connections (`CONNECTION_ADMIN`
privilege) follow the same boundary as `offline_mode` itself and are trusted.

To find and kill remaining connections:

```sql
-- Show active non-admin connections
SELECT id, user, host, db, command, time, state
FROM information_schema.processlist
WHERE command != 'Daemon';

-- Kill a specific connection
KILL CONNECTION <id>;
```

Full procedure:

```sql
SET GLOBAL offline_mode = ON;
-- wait for / kill remaining connections
ALTER EXTENSION my_ext UPDATE TO '2.0.0';
SET GLOBAL offline_mode = OFF;
```

`INSTALL EXTENSION` and `UNINSTALL EXTENSION` do not require offline mode.

## Compatibility Rules

- **Retained types** (in both old and new): `persisted_length` must not change —
  existing binary data on disk would be misinterpreted.
- **Dropped types** (in old but not new): allowed only if no columns or stored
  procedure parameters use that type.
- **New types** (in new but not old): always allowed.

If any check fails the command aborts and the old extension remains loaded.

## Failures During Update

Failures detected before the new extension's capabilities start swapping
(name validation, type compatibility, capability `on_check_update` hooks)
are fully reversible — the server is left exactly as it was.

Failures *during or after* the capability swap (a sys-var registration
failing partway through, the catalog write failing after the swap, etc.)
leave in-memory capability state inconsistent with the catalog. There is
no automatic rollback for this case. **Restart the server before exiting
offline mode** if `ALTER EXTENSION` reports a failure that mentions a
capability. The restart re-initializes capabilities from the catalog and
restores a consistent state. See `EXTENSION_UPDATE_IMPL.md` for the
implementation-level details.

**Persisted system variables**: if your extension uses sys vars set via
`SET PERSIST` (stored in `mysqld-auto.cnf`), be aware that a failed
`ALTER EXTENSION` can lose those persisted values even after a restart.
The sys-var capability rewrites `mysqld-auto.cnf` part-way through the
swap, and the rewrite cannot be undone. Snapshot `mysqld-auto.cnf` before
running `ALTER EXTENSION` if your extension has persisted vars that
matter.
