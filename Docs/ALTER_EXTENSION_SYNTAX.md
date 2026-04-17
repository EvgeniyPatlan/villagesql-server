# ALTER EXTENSION syntax — design notes

Captures the question of how to phrase the version-swap action in SQL, what
else `ALTER EXTENSION` might do later, and the alternatives we considered.

## Current state of the patch

The version-swap action is parsed as:

```sql
ALTER EXTENSION name UPDATE TO 'x.y.z'
```

This is the only `ALTER EXTENSION` form today. Internally it constructs
`Sql_cmd_install_extension(name, version, update=true)` and is dispatched via
`Sql_cmd_install_extension::execute_update`.

## PostgreSQL precedent

Postgres ([docs](https://www.postgresql.org/docs/current/sql-alterextension.html))
defines four forms:

```sql
ALTER EXTENSION name UPDATE [ TO 'new_version' ]
ALTER EXTENSION name SET SCHEMA new_schema
ALTER EXTENSION name ADD member_object
ALTER EXTENSION name DROP member_object
```

- **UPDATE** — bumps the installed version. `TO 'x'` is optional; without it
  Postgres picks the default version from the control file.
- **SET SCHEMA** — relocates the extension's objects into another schema.
  Requires the extension to be marked relocatable.
- **ADD** / **DROP** — adds/removes membership of an existing object to/from
  the extension. Mostly used inside extension upgrade scripts; does not drop
  the object itself.

The relevant lesson: Postgres reserved `ALTER EXTENSION` as the *umbrella*
verb and tagged each action with its own sub-keyword (`UPDATE`, `SET SCHEMA`,
`ADD`, `DROP`). The version-swap form is not the bare default — it is
explicitly `UPDATE`.

## Why `ALTER` as the top-level verb (and not `UPDATE`)?

Postgres also picked `ALTER` as the top-level verb, then disambiguated with
sub-keywords (`UPDATE`, `SET SCHEMA`, `ADD`, `DROP`). It did *not* introduce
a top-level `UPDATE EXTENSION` statement. We are following the same shape.
The reasoning, made explicit:

- **`ALTER` is the SQL convention for "modify an existing schema object."**
  `ALTER TABLE`, `ALTER DATABASE`, `ALTER FUNCTION`, `ALTER VIEW`, `ALTER
  USER`, `ALTER SERVER`, `ALTER TABLESPACE` — every other mutation of an
  existing catalog object lives under `ALTER`. Putting extension mutations
  in a different top-level verb would be a one-off exception.
- **`UPDATE` is taken.** `UPDATE` is the DML row-mutation verb. Introducing
  a separate `UPDATE EXTENSION` statement makes humans (and tooling) do a
  double-take: same verb, completely different semantics. The grammar would
  parse, but the cognitive collision is real.
- **`ALTER` scales.** The version-swap is one of several plausible future
  mutations (see next section). A top-level `UPDATE EXTENSION` reads
  naturally only for the version-bump case. `ALTER EXTENSION ... <action>`
  naturally accommodates any future action.
- **`UPDATE` is the right *sub-verb*, not the right top-level verb.**
  Within the `ALTER EXTENSION` umbrella, the version-swap action is best
  described as `UPDATE` — that is exactly what Postgres does. The sub-verb
  position avoids the DML collision because the parser already knows we
  are mid-`ALTER`.

So: top-level `ALTER` matches the rest of SQL DDL and leaves room for other
mutations; sub-verb `UPDATE` describes the specific action.

## Install vs. alter — why not extend `INSTALL EXTENSION`?

An earlier shape of this patch used `INSTALL EXTENSION name VERSION 'x.y.z'
UPDATE` (a trailing modifier on the install statement). We moved off it
because:

- **Install and alter are different operations.** `INSTALL` puts a new
  extension into the catalog. The version-swap leaves the extension
  installed and changes the running version. Same catalog row, different
  semantics. Conflating them under `INSTALL` makes both forms harder to
  describe.
- **Reads backwards.** The action verb comes at the end (`... UPDATE`)
  instead of at the start. SQL statements normally lead with the action,
  not modify it after the object.
- **Reserves the wrong syntactic space.** Future install-only options
  (e.g. `INSTALL EXTENSION foo VERSION 'x.y.z' [WITH ...]`) would have to
  contend with the trailing-modifier slot.

## What else might `ALTER EXTENSION` do later in VillageSQL?

Speculative — none of these are committed or even designed:

- **Configure / tune** — e.g. `ALTER EXTENSION foo SET parameter = value`
  for runtime config on the extension instance (sys-var bindings, capability
  parameters).
- **Rename** — `ALTER EXTENSION foo RENAME TO bar` without
  uninstall/reinstall. Probably rare in practice.
- **Schema relocation** — `ALTER EXTENSION foo SET SCHEMA new_schema`, if we
  ever expose extension objects under a schema namespace (we currently
  don't).
- **Ownership** — `ALTER EXTENSION foo OWNER TO user`, if extensions ever
  acquire owners.
- **Membership** — Postgres-style `ADD` / `DROP MEMBER` for stitching
  existing DB objects into an extension. Likely not applicable to VEF, but
  not ruled out.
- **Replication / capability toggles** — enable or disable specific
  capabilities on a loaded extension without reinstall.

The point is not that we will build any of these — it is that we should not
back ourselves into a corner where the only verb `ALTER EXTENSION foo X`
can do is version-swap.

## Alternatives for the version-swap syntax

### 1. `ALTER EXTENSION name VERSION 'x.y.z'` — earlier form (rejected)

**Pros**
- Shortest form to type.
- No new keyword.

**Cons**
- The action is implicit: "alter the version" is the only thing the bare
  form can mean. Adding `ALTER EXTENSION name SET SCHEMA ...` later would
  feel inconsistent because the existing form has no sub-keyword.
- The version-swap is a heavyweight, offline-only operation. Hiding it
  behind a bare `ALTER` makes it look as cheap as the (future) `SET
  parameter = ...` form.

### 2. `ALTER EXTENSION name UPDATE TO 'x.y.z'` — Postgres-style (Adopted)

**Pros**
- Matches Postgres exactly — anyone coming from PG knows what this does.
- Leaves the bare `ALTER EXTENSION name ...` namespace free for future
  actions (`SET SCHEMA`, `RENAME`, parameter tweaks, etc.) without breaking
  the version-swap syntax.
- `UPDATE` correctly conveys "bump to a newer version of the same
  extension," distinct from `INSTALL` (first install) and `UNINSTALL`.
- The `TO` reads naturally.

**Cons**
- One extra keyword to type vs. option 1.
- `UPDATE` is a heavily overloaded SQL keyword. The grammar parses
  unambiguously, but humans reading the manual page might briefly wonder if
  it relates to DML.
- Postgres makes `TO 'version'` optional (falls back to the default
  version). We do not have a "default version" concept yet — we would
  either require `TO`, or compute a default from the filesystem the same
  way bare `INSTALL EXTENSION` does today.

### 3. `ALTER EXTENSION name UPGRADE TO 'x.y.z'`

**Pros**
- `UPGRADE` is unambiguous about the intent (bumping versions); it does not
  collide with DML's `UPDATE`.
- Reads slightly more naturally than `UPDATE TO`.

**Cons**
- Diverges from Postgres precedent for no real gain.
- `UPGRADE` suggests forward-only motion; downgrading to an older version
  would feel awkwardly named. (We do allow downgrades.)
- `UPGRADE` is not currently a reserved word — adding it costs a keyword.

### 4. `ALTER EXTENSION name SET VERSION = 'x.y.z'` or `... SET VERSION 'x.y.z'`

**Pros**
- Keeps a `SET <property>` framing that scales to other settable properties
  (`SET SCHEMA`, future `SET parameter`).
- Internally consistent with `SET GLOBAL ...` style.

**Cons**
- Misleading. `SET VERSION` makes it sound like a metadata flip — the
  reality is a full unload+reload that requires `offline_mode = ON`. A
  `SET`-style verb undersells the cost.
- Postgres uses `SET SCHEMA` for relocation but does *not* use `SET
  VERSION` for the version-swap; deviating from that precedent loses
  discoverability.

### 5. Keep `INSTALL EXTENSION name VERSION 'x.y.z' UPDATE`

**Pros**
- Already implemented (the grammar still has this rule alongside the
  `ALTER` rule).
- The trailing `UPDATE` modifier is unambiguous in scope.

**Cons**
- Wrong shape: install-vs-alter is a meaningful distinction. Tagging an
  alter as a flavor of install conflates the two.
- The action is described by a trailing modifier rather than a leading
  verb, which is unusual in SQL.
- Tests and docs already moved to `ALTER EXTENSION` — keeping the install
  form means two surface syntaxes for the same operation.

## Recommendation

Adopt **option 2**: `ALTER EXTENSION name UPDATE TO 'x.y.z'`.

- Matches Postgres.
- Reserves the `ALTER EXTENSION name <verb> ...` shape for future actions
  (`SET SCHEMA`, `RENAME`, parameter changes).
- Internal identifiers stay as `m_update` / `execute_update` — `UPDATE` is
  the action sub-verb that the class actually performs. The top-level
  `ALTER` keyword is a parser-level concern; it doesn't propagate into the
  class because `Sql_cmd_install_extension` already handles multiple
  top-level keywords (`INSTALL` and `ALTER`).

Future actions would slot in alongside it as separate grammar rules:

```sql
ALTER EXTENSION name UPDATE TO 'x.y.z'      -- this patch
ALTER EXTENSION name SET SCHEMA s           -- hypothetical
ALTER EXTENSION name RENAME TO new_name     -- hypothetical
ALTER EXTENSION name SET p = v              -- hypothetical
```

## Decisions

- **Require `TO`**: yes. The grammar is `ALTER EXTENSION name UPDATE TO
  'x.y.z'`. Matches Postgres; `TO` reads naturally.
- **`ALTER EXTENSION name UPDATE` with no version**: not allowed. The
  current `execute_update` requires an explicit version
  (`require_explicit=true` in `resolve_veb_version`). We have no
  "default version" concept and adding one is out of scope.
- **Keep the `INSTALL EXTENSION ... UPDATE` grammar rule?**: removed. The
  rule had no users — tests and docs already moved to `ALTER EXTENSION`.
