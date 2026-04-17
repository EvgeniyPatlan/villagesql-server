# Extension Update in a Replicated Setup

When updating an extension across a replication topology, the procedure depends on
whether the new version adds capabilities, removes them, or both. The key concern is
that binlog events referencing extension-defined features (types, VDFs) must be
replayable on every node.

## Scenario 1: Additive Update (new capabilities only)

The new version adds types or VDFs but does not remove any. Existing binlog events
remain replayable on both old and new versions of the extension.

This is analogous to a normal database upgrade and follows the standard rolling
upgrade procedure:

1. Update replicas one at a time while the primary remains online and
   read/write.
2. Update the primary last.
3. The cluster remains fully available throughout.

## Scenario 2: Removal (capabilities dropped)

The new version removes one or more types or VDFs. After the primary is updated,
binlog events that reference the removed features cannot be replayed on replicas
still running the old version.

Procedure:

1. Put the cluster into read-only mode so no new binlog events referencing the
   old capabilities are generated.
2. Allow replicas to catch up and drain in-flight replication.
3. Update all nodes. One or more nodes may remain online as read-only replicas
   throughout.
4. Re-enable writes once all nodes are updated.

## Scenario 3: Mixed Update (some removed, some added)

The new version both removes existing capabilities and adds new ones. The removal
aspect dominates: events referencing removed features cannot be replayed on nodes
still running the old version, so the same procedure as scenario 2 applies.

Procedure: same as Scenario 2.

## Summary

| Scenario          | Writes during update | Rolling update |
|-------------------|----------------------|----------------|
| Additive only     | Yes                  | Yes            |
| Removal (or mixed)| No (read-only)       | No             |
