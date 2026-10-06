# ProsenjitFS Copy-on-Write and Snapshot Design

## Goal

Add persistent filesystem snapshots using copy-on-write (COW) without weakening the existing redo-journal, checksum, LBA-boundary, sparse-file, or extent guarantees.

The design uses the proven filesystem pattern of persistent reference counts for shared blocks and shadowing a block before modification.

## Safety rule

Snapshot support is **not enabled in the existing v1 format** until the complete on-disk format and write-path changes are implemented. A v1 volume must never interpret unused fields as snapshot metadata.

## Planned format extension

A version-2 superblock will add:

- snapshot metadata region
- block-reference-count region
- snapshot feature bit
- snapshot generation/sequence

The reference-count map is persistent and checksum protected. A count of one means the block is owned by one live filesystem view; a count greater than one means it is shared by snapshots and/or the live view.

Snapshot records contain snapshot id, generation, creation time, root inode, snapshot state, and checksum.

## COW rules

Before modifying an existing physical block:

1. Determine its persistent reference count.
2. If the count is one, the live view may modify it in place.
3. If the count is greater than one, allocate a fresh block.
4. Copy the old block into the new block.
5. Apply the modification to the new block.
6. Update the owning extent/metadata pointer to the new block.
7. Decrement the old block reference count.
8. Keep the old block available while any snapshot still references it.
9. Commit mapping, reference-count, allocation, and data changes in one redo transaction.

The same rule applies to file data and mutable metadata reachable from a snapshot: inode blocks, extent blocks, directory-index blocks, and directory-entry blocks.

## Snapshot creation

Creation is atomic: validate health, freeze the logical root generation, register the snapshot, increment references for every reachable block, journal the complete metadata update, flush/commit/home-flush, then publish the snapshot. A partially-created snapshot must never be mount-visible.

## Snapshot deletion

Deletion is transactional. Mark the snapshot deleting, walk its reachable block graph, decrement references, free blocks only when the count reaches zero, remove the record, and commit. Interrupted deletion must resume from journaled state rather than guessing ownership.

## Multiple snapshots

Reference counts are required instead of a single snapshot-active flag. This permits multiple snapshots, live writes while snapshots remain stable, deletion in any order, and blocks shared by multiple snapshots.

## Crash recovery

Snapshot creation, COW writes, and deletion must obey the existing redo-journal ordering: journal images -> flush -> commit -> flush -> home blocks -> flush -> retire.

Recovery must reject invalid snapshot checksums, out-of-range snapshot roots, invalid reference counts, references to metadata regions, extents outside the data region, and impossible ownership. Writable mount must never expose an unvalidated snapshot.

## Regression matrix

The self-test must cover create file -> create snapshot -> modify live file -> verify snapshot retains original bytes -> verify live file has new bytes -> remount -> repeat; multiple snapshots; repeated modification of the same logical block; snapshot deletion and block reclamation; interrupted COW/snapshot create/delete; corrupted reference-count metadata; corrupted snapshot record; and invalid snapshot root.

## Completion rule

This document is **not** an implementation claim. COW/snapshot support becomes complete only after the v2 on-disk structures, reference-count updates, COW write path, snapshot lifecycle, recovery handling, and deterministic plus QEMU regression tests are implemented and verified.
