# Per-Thread Reference Tracking Plan

## Current State (Completed)
- ✅ Added 3 performance schema tables:
  - `extension_references` - shows extension objects with reference counts and column dependencies
  - `victionary_statistics` - shows cache statistics per VictionaryClient map
  - `victionary_columns` - shows all custom column definitions
- ✅ Reference counts currently show total across all threads (from `shared_ptr::use_count()`)
- ✅ Column dependencies tracked via `villagesql.custom_columns` table

## Problem
Currently we cannot answer: **"Which thread/connection holds references to which extension objects?"**

The global `VictionaryClient` tracks total reference counts but doesn't know:
- Which thread holds user variable `@x` with a TypeContext reference
- Which connections have active queries using custom types
- Per-thread breakdown of references

## Proposed Solution: Per-Thread Reference Tracking

### Architecture

**Global VictionaryClient (unchanged role):**
- Stores the catalog (what extensions/types are installed)
- Requires write lock for INSTALL/UNINSTALL operations
- During UNINSTALL, blocks new references via flag
- Total reference count computed on-demand by iterating all THDs

**Per-Thread Tracker (NEW):**
```cpp
// Add to THD class in sql/sql_class.h
class THD {
  ...
  // VillageSQL: Per-thread reference tracking
  // Each thread has its own maps tracking only what THIS thread holds
  villagesql::ThreadReferenceTracker m_villagesql_references;
  ...
};
```

**ThreadReferenceTracker structure:**
```cpp
class ThreadReferenceTracker {
  // Same structure as VictionaryClient but for THIS thread only
  SystemTableMap<TypeContext> m_type_contexts;
  SystemTableMap<TypeDescriptor> m_type_descriptors;
  SystemTableMap<FuncDescriptor> m_funcs;
  SystemTableMap<ExtensionDescriptor> m_extension_descriptors;

  // No locking needed - single-threaded access
  // Automatically cleaned up when thread/connection ends
};
```

### What Gets Tracked Per-Thread

**User Variables:**
- `SET @var = bytearray_from_string('x')` → increment `thd->m_villagesql_references.type_contexts["bytearray.vsql_simple.0.0.1"]`
- `SET @var = NULL` → decrement reference

**Query-Scoped References:**
- Temporary TypeContext references during query execution
- Released when query completes

**Statement-Scoped References:**
- References acquired during statement execution
- Released when statement ends

### What Does NOT Get Tracked Per-Thread

**TABLE_SHARE References:**
- TABLE_SHARE objects are shared across threads
- Already protected by column dependency tracking in `villagesql.custom_columns`
- If a table has a custom type column, UNINSTALL will fail regardless of memory state

### Benefits

✅ **No global mutex contention** - each thread owns its data
✅ **Automatic cleanup** - when thread/connection ends
✅ **Lock-free reads** - thread reads its own data
✅ **Natural MySQL architecture** - fits per-thread state model
✅ **Easy queries** - "what does connection 123 hold?"

### UNINSTALL EXTENSION Flow

1. Acquire global VictionaryClient write lock
2. Set flag: "extension X is uninstalling" to block new references
3. Check column dependencies (from `villagesql.custom_columns`)
4. Iterate all active THDs, sum up their per-thread reference counts
5. If any references exist (or columns depend on it), fail with error
6. If no references, remove from global catalog
7. Clear flag and release lock

### New Performance Schema Table

**`performance_schema.villagesql_thread_references`**

Columns:
- `THREAD_ID` - internal thread ID
- `PROCESSLIST_ID` - connection ID visible in SHOW PROCESSLIST
- `OBJECT_TYPE` - TYPE_CONTEXT, FUNC_DESCRIPTOR, etc.
- `OBJECT_KEY` - clean object name (e.g., "bytearray")
- `EXTENSION_NAME` - extension name
- `EXTENSION_VERSION` - extension version
- `REFERENCE_COUNT` - count for THIS thread only

Implementation: Iterate through all active THDs and read their `m_villagesql_references` maps.

## Implementation Steps

### Phase 1: Add ThreadReferenceTracker Class
1. Create `villagesql/schema/thread_reference_tracker.h`
2. Define ThreadReferenceTracker with same map structure as VictionaryClient
3. No locking needed (single-threaded access)
4. Add helper methods: `acquire_reference()`, `release_reference()`, `get_references()`

### Phase 2: Add to THD
1. Modify `sql/sql_class.h` to add `m_villagesql_references` member
2. Initialize in THD constructor
3. Cleanup handled automatically by destructor

### Phase 3: Update Reference Acquisition Points
1. **User variables**: Modify `AcquireTypeContextClientManaged()` to track in both global and per-thread
2. **Item execution**: Track temporary references during query execution
3. **Prepared statements**: Track statement-scoped references

### Phase 4: Update Reference Release Points
1. **User variables**: Modify `ReleaseTypeContextClientManaged()` to release from per-thread tracker
2. **Query cleanup**: Release query-scoped references
3. **Statement cleanup**: Release statement-scoped references
4. **Connection cleanup**: Automatically handled by THD destructor

### Phase 5: Update UNINSTALL EXTENSION
1. Add blocking mechanism during uninstall
2. Iterate all THDs to compute total references
3. Check per-thread trackers for active references
4. Block new reference acquisitions during uninstall

### Phase 6: Add Performance Schema Table
1. Create `table_villagesql_thread_references.h/cc`
2. Implement iteration over all THDs
3. Read each THD's `m_villagesql_references` map
4. Register table in `pfs_engine_table.cc`
5. Add tests

## Open Questions

1. **How to iterate all active THDs?**
   - MySQL has `Global_THD_manager` for this
   - Need to acquire appropriate locks

2. **Blocking new references during UNINSTALL:**
   - Add atomic flag in global VictionaryClient?
   - Check flag in `AcquireTypeContextClientManaged()`?

3. **Prepared statements:**
   - Do prepared statements hold references across executions?
   - Need to investigate lifetime

4. **Edge cases:**
   - What happens if thread dies while holding references?
   - Already handled by THD destructor cleanup

## Testing Plan

1. Create user variables with custom types, verify per-thread tracking
2. Multiple connections with variables, verify isolation
3. Query execution with custom types, verify temporary references
4. UNINSTALL while references held, verify failure with correct count
5. Connection disconnect, verify automatic cleanup
6. Performance testing: overhead of per-thread tracking

## Files to Modify

**New Files:**
- `villagesql/schema/thread_reference_tracker.h`
- `villagesql/schema/thread_reference_tracker.cc`
- `villagesql/perfschema/table_villagesql_thread_references.h`
- `villagesql/perfschema/table_villagesql_thread_references.cc`
- Tests in `mysql-test/suite/villagesql/perfschema/`

**Modified Files:**
- `sql/sql_class.h` - add `m_villagesql_references` to THD
- `villagesql/types/util.h/cc` - update `AcquireTypeContextClientManaged()` / `ReleaseTypeContextClientManaged()`
- `villagesql/veb/sql_extension.cc` - update UNINSTALL EXTENSION logic
- `villagesql/schema/victionary_client.h/cc` - add blocking flag for uninstall
- `storage/perfschema/pfs_engine_table.cc` - register new table

## Current Branch

Work committed to: `tomas/extension-reference-perfschema`

The performance schema tables are complete and working. Next step is to implement per-thread reference tracking as described above.
