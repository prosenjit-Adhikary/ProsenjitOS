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


## Follow-up: VFS file-handle state is outside callback lock

Review of `kernel/vfs.c` confirms `vfs_file_read()` advances `file->position` after the filesystem read callback returns, and `vfs_file_write()` advances position (or sets append position from `file->vnode->size`) after the filesystem write callback returns. The ProsenjitFS callback lock is released before those VFS updates. Therefore the lock does **not** serialize two simultaneous calls using the same `pros_vfs_file` object, and it does not make shared file position updates race-free. Concurrent operations on separate file handles may still require a carefully defined position/append contract; same-handle concurrent use must remain unsupported until the VFS itself adds synchronization and tests.

Required test/decision: add a test or explicit API contract for concurrent same-handle read/write/append; decide whether VFS should own a per-open-file lock or whether callers must externally serialize each handle. Do not fix this by adding the filesystem lock around VFS dispatch without checking recursive lock acquisition.


## FS-34 implementation note — per-open VFS file lock

The VFS now carries a per-open-file spinlock initialized by `vfs_file_init()`. Read/write/truncate/seek/close operations acquire it before validating mutable handle state and hold it across callback dispatch and position/lifetime updates. This addresses same-handle position interleaving at source level. It does not serialize different file objects; provider-level filesystem locking remains necessary. **Important unresolved kernel contract:** this is a spinlock held across synchronous filesystem callbacks and potentially block-device I/O. Verify the kernel execution model forbids sleeping/preemption in this path or replace it with the supported sleepable mutex before claiming production-safe concurrency. Deterministic concurrent tests and exact-head CI remain required.


## Follow-up source review — VFS handle lock and kernel synchronization (2026-10-25)

Reviewed the PR #265 head `7b41a5128be74462db884ef169a1e8f125fb7d26` and its VFS implementation.

- `vfs_file_lock()` is an atomic spin loop around `__sync_lock_test_and_set`; it does not disable interrupts, yield, or provide a sleep/wait path. The lock is held across provider callbacks for read/write/truncate and across the provider close callback.
- `vfs_file_init()` initializes `io_lock` to zero. All direct `struct pros_vfs_file` objects found in the VFS self-test are zero-initialized or passed through the normal open/init path in the inspected source; nevertheless, every future construction path must preserve that invariant.
- `vfs_file_close()` releases the lock after clearing handle fields. This serializes an operation already contending on the same live object, but does not by itself establish safe object destruction/reuse if a caller retains and concurrently reuses the same pointer after close. The handle's storage lifetime must remain externally stable until all users have stopped.
- The kernel has `thread_schedule_preempt()` and `thread_schedule_interrupt()` declarations/definitions, but this review did not establish the active interrupt call path or prove whether a callback can sleep/yield while the lock is held. Do not infer either “preemption is impossible” or “a deadlock was reproduced” from this inspection alone.
- Repository inspection did not find an established sleepable mutex primitive in the kernel synchronization headers/files reviewed. Do not invent one or silently substitute a different primitive. First trace the actual interrupt/scheduler call path and block-device callbacks; then define the lock/context contract. If callback paths may block or yield, this spinlock design is not acceptable as-is.
- No deterministic concurrent test, local build, or exact-head CI pass is claimed by this audit update. PR #265 remains validation-open; do not merge until lock/context/lifetime assumptions are proved and the exact current head passes required CI.


## Confirmed timer-preemption hazard — follow-up (2026-10-25)

A deeper trace of `kernel/idt.asm`, `kernel/timer.c`, and `kernel/thread.c` confirms that the prior note's uncertainty about the active preemption path is resolved:

- The timer ISR calls `timer_dispatch`, then `timer_should_preempt`, and on a selected tick calls `thread_schedule_interrupt` (see `kernel/idt.asm`, timer path around lines 120–138).
- `timer_should_preempt()` requests preemption on every tenth tick (see `kernel/timer.c`). The scheduler can therefore switch away from a thread while that thread is in an ordinary kernel path, unless that path explicitly protects its critical section.
- `vfs_file_lock()` and `pfs_io_lock()` use busy-wait atomic spin loops and neither helper disables preemption/interrupts. Both locks are held across filesystem work; ProsenjitFS callbacks can issue block-device read/write/flush operations.
- **High-priority correctness risk:** if the timer preempts a lock owner and schedules another runnable thread that tries to acquire that same lock, the new thread can spin while the owner remains suspended, preventing the owner from releasing the lock. This is a source-level deadlock scenario implied by the current implementation; it has not been reproduced in a runtime test.
- The same concern applies to the filesystem-wide `pfs_io_lock`, not only the VFS per-handle `io_lock`. Merely changing one lock is insufficient. Do not add more spinlock coverage until the kernel defines a consistent preemption-safe lock protocol.
- Required remediation decision: add a kernel-supported IRQ/preemption-safe critical-section primitive (with correct nested save/restore semantics), or introduce a scheduler-aware sleepable mutex after verifying it can safely be used in these call paths. Then apply it consistently to VFS handle state and ProsenjitFS shared state, audit lock ordering and interrupt-context restrictions, and add deterministic multi-thread regression tests. Do not hand-roll a mutex in filesystem code.
- This finding supersedes the earlier sentence saying the active interrupt call path had not been established. It raises the VFS/filesystem locking issue from “contract to verify” to a confirmed source-level risk requiring remediation before merge.

No runtime deadlock reproduction, local build, or passing CI is claimed. PR #265 must remain unmerged until the lock protocol is corrected and validated on its exact head.


## 8. IRQ-save spinlock correction added on 2026-10-25

Source commit [3d73412](https://github.com/prosenjit-Adhikary/ProsenjitOS/commit/3d73412f8e9c7145dfaa96b1740145980c5f4d48) changes both the VFS per-open-file lock and ProsenjitFS I/O lock to save RFLAGS, execute `cli` **before** attempting the atomic spin, and restore IF only when the caller's saved flags had IF set. The lock token is returned to and held by the caller, rather than stored in the shared lock object. This is intended to address the confirmed same-CPU timer-preemption deadlock and preserve nested VFS-lock → filesystem-lock interrupt state.

This is a source-level correction, **not yet a verified fix**. At this snapshot, exact-head Build/boot-test and Security architecture workflow results have not been confirmed, and no local build or QEMU runtime test is claimed.

### Trade-offs and remaining gates

- Local interrupts remain disabled for the full critical section, including filesystem callbacks and synchronous block-device I/O. This prevents timer-driven preemption deadlock on the lock-owning CPU but can increase interrupt latency; a future scheduler-aware mutex or carefully specified kernel synchronization primitive may be preferable for long I/O paths.
- IRQ-save locking alone is not a complete SMP proof. Before enabling concurrent execution on multiple CPUs, verify the scheduler/lock-owner progress assumptions and use a per-CPU interrupt-save protocol with a reviewed SMP locking contract.
- The nested-token rule must remain intact: an inner lock acquired while IF is already clear must not execute `sti` on release; the outermost caller restores the original IF state.
- Add/execute deterministic tests for nested lock restoration, timer-preemption stress, concurrent same-handle operations, separate-handle append/write, namespace mutation races, and unmount versus in-flight operations.
- Keep PR #265 open until exact-head CI passes and the runtime/latency implications are reviewed.


## IRQ-save self-test coverage refinement — 2026-10-25

The nested IRQ-save regression added to `vfs_vnode_api_selftest()` checks that IF stays clear while both locks are held, remains clear after the inner unlock, and matches the initial IF state after the outer unlock. However, `kernel_main()` executes `cli` during early initialization and the inspected boot path does not execute `sti` before the VFS self-test. Therefore this boot-time test exercises the initially-disabled IF case; it does **not** prove the initially-enabled outer-unlock path.

This is a test-coverage limitation, not evidence that the restoration implementation is wrong. Do not enable interrupts ad hoc during early self-tests: that could admit timer scheduling while kernel initialization/test fixtures are in progress. Add an isolated test of the enabled-IF restoration path only when there is a safe test harness or a reviewed point in boot where IRQs and scheduler state are ready.

Current source state:
- Both `vfs_file_lock()` and `pfs_io_lock()` now save RFLAGS and execute `cli` before the atomic spin; both restore IF only if it was set on entry.
- The lock token is caller-local, preserving nested VFS → filesystem locking semantics on one CPU.
- This is still not an SMP synchronization contract and leaves interrupts disabled across the critical section, including synchronous filesystem/block I/O.
- The latest PR head is `04fafab7ad78d5b60ba39aa94d3dd0299e4d8df0`. Workflow runs associated with this PR were queued/cancelled for older commit SHAs; no passing exact-head Build/boot-test or Security architecture result was found during this review. No local build or QEMU runtime result is claimed.
- Remaining merge gates: exact-head CI, an isolated enabled-IF restoration test, concurrency/lifetime tests, and explicit review of interrupt latency and the single-CPU assumption.
