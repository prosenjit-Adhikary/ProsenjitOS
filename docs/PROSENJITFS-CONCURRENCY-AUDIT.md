# ProsenjitFS Concurrency and Lifecycle Audit

**Status date:** 2026-10-25  
**Scope:** Source-level audit of the native ProsenjitFS I/O lock, VFS file callbacks, namespace callbacks and unmount lifecycle.  
**Status:** Partial audit; general concurrent VFS safety is **not verified**.

## 1. Source state and review boundary

- Repository: `prosenjit-Adhikary/ProsenjitOS`
- Working branch: `fix/prosenjitfs-serialize-file-read-truncate-20261025`
- Audit source baseline: `a9f8c553b3615b2e226cb5fcfb7652bd79fcadd4`
- Related implementation PR: [#265](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/265)
- Related journal validation PR: [#264](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/264)

This document records source observations and follow-up requirements. It does not claim that concurrency races have been reproduced, that the current PR has passed CI, or that the filesystem is ready for production concurrent execution.

## 2. Current file-I/O lock coverage

The source has a per-filesystem spinlock implemented by `pfs_io_lock()` and `pfs_io_unlock()` using `__sync_lock_test_and_set`, memory barriers and `__sync_lock_release`.

On the PR #265 branch, public file callbacks use it as follows:

| Entry point | Lock behavior | Current boundary |
|---|---|---|
| `pfs_file_read()` | Locks and calls `pfs_file_read_unserialized()` | Serializes the read callback with other callbacks using the same lock |
| `pfs_file_write()` | Locks and calls `pfs_file_write_unserialized()` for every write, not just append | Serializes file-write callback body with read/truncate callback bodies |
| `pfs_file_truncate()` | Locks and calls `pfs_file_truncate_unserialized()` | Serializes truncate callback body with read/write callback bodies |

The all-write correction was made in source commit `828def8a5cc8e53f9c03156973c1b60e96876572`; subsequent commits added audit documentation. Re-query the live PR head before relying on these SHA snapshots.

### Important limits

- The lock is a spinlock, not a sleepable mutex. Before using it as a general filesystem lock, confirm the block-device and allocation paths are safe while holding it, including interrupt/preemption rules.
- The wrappers do not protect every VFS namespace callback.
- VFS updates to per-open file position, vnode cached size and health counters occur outside the filesystem callback lock. Concurrent use of the **same** `pros_vfs_file` object is not established as safe.
- A callback lock does not automatically protect a caller that passed a mount-active check before unmount began and then waits to enter a callback.
- No deterministic concurrent interleaving regression test has been added in this change set. The current source change is a serialization measure, not a concurrency PASS.

## 3. Namespace callback audit

Source inspection found no `pfs_io_lock()` acquisition at the public entry point of these operations:

- `pfs_vnode_create()`
- `pfs_vnode_unlink()`
- `pfs_vnode_rmdir()`
- `pfs_vnode_rename()`
- `pfs_vnode_readdir()`

These callbacks can touch shared inode, directory index, extent/bitmap allocation or transaction state. This means the current lock does not establish mutual exclusion between file-data callbacks and namespace operations, or between two namespace mutations.

**Do not mechanically wrap these callbacks yet.** First inspect every call path and decide whether callbacks can be nested. Because `pfs_io_lock()` is non-recursive, nested acquisition would spin forever.

## 4. Unmount and object lifetime

`prosenjitfs_unmount()` checks the root vnode reference count, flushes the device, deactivates the mount and releases transaction/scratch buffers. The VFS has a documented parent-retention model: descendant vnodes retain parents and open handles retain their vnode, so the root-reference check is intended to reject unmount while externally visible vnode state remains.

That reference model is useful protection, but the inspected code does not establish an explicit filesystem active-operation counter/gate shared by all callbacks. Race-free behavior between path lookup, callback entry, deactivation and cleanup must be verified against the actual VFS reference acquisition/release paths. Do not claim a use-after-free has been demonstrated; record this as an open lifecycle proof/test requirement.

## 5. Integrity scans

The integrity API documents that callers must serialize scans against filesystem mutations. Repository search found in-source calls to `prosenjitfs_check_integrity_report()` and `prosenjitfs_check_integrity()` inside `prosenjitfs_selftest()`, not a separate production callsite in the inspected baseline.

The integrity checker is not currently covered by the file callback lock. If it becomes callable concurrently in production, add an explicit safe serialization boundary rather than relying on a comment or on the file-data lock.

## 6. Required next steps

1. Re-query PR #264/#265 current heads, mergeability and exact-head CI. Do not count runs for superseded SHAs.
2. Inspect all call paths for lock recursion, including VFS dispatch, helpers, allocator, transaction and device I/O calls.
3. Decide whether the kernel's synchronization model permits holding a spinlock across all block-device I/O and allocation operations. If not, introduce/use a sleepable lock only after the kernel primitive and context contract are verified.
4. Define a lifecycle protocol for mount/unmount versus in-flight callbacks. Document lock order and ownership.
5. Add deterministic tests or a testable synchronization abstraction for:
   - concurrent append operations on separate file handles;
   - ordinary write versus read and truncate on the same inode;
   - rename/unlink/create versus lookup/readdir;
   - integrity scan versus mutation;
   - unmount versus an active operation and an open descendant handle.
6. Run exact-head Build/boot-test and Security architecture CI after code changes. Add the exact commit SHA and run URLs to the completion matrix only after successful completion.
7. Keep mock-device/QEMU evidence separate from physical-device power-loss durability claims.

## 7. Current CI snapshot

At the last check for source/documentation head `a9f8c553b3615b2e226cb5fcfb7652bd79fcadd4`, Build/boot-test [#40209078741](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40209078741) and Security architecture [#40209078742](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40209078742) were queued. These are historical snapshots; check current workflow state and exact SHA before reporting results.

## 8. Handoff rule

The next AI should start by reading this document, then inspect the live source and PR head. Preserve the distinction between **implemented**, **tested**, **CI-passed**, and **hardware-verified**. Overall ProsenjitFS completion remains open.
