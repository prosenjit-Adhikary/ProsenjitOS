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


## IPC blocked-thread teardown regression checkpoint

The kernel startup regression now creates two dedicated child process/thread fixtures and seeds one blocked-thread entry in each of two IPC channels. Exiting the first child must make only its thread DEAD and detach only its own waiter; the test checks that the unrelated thread remains BLOCKED on its original channel, its queue entry is preserved, the runnable population is unchanged, and the first child is reaped with the expected exit code. It then exits/reaps the second child and verifies that its waiter is independently detached.

This is a **source-level regression addition**, not a runtime PASS claim. The test seeds the wait queue directly because `ipc_channel_wait()` intentionally requires the target to be `thread_current()`; it does not yet exercise the full context-switch-to-blocked-child sequence. Scheduler-wide synchronization and channel lifetime during concurrent teardown remain unproven. No scheduler lock was added because a safe lock order between run-queue mutation and channel-lock acquisition has not been established.

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
- Atomic test-and-set/release serializes contenders on the same lock word across CPUs; cli masks only local interrupts. However, system-wide SMP correctness remains unverified because not all namespace/lifetime paths take these locks and no SMP stress test or full lock-order proof exists. Interrupts remain disabled across the critical section, including synchronous filesystem/block I/O.
- The latest PR head is `04fafab7ad78d5b60ba39aa94d3dd0299e4d8df0`. Workflow runs associated with this PR were queued/cancelled for older commit SHAs; no passing exact-head Build/boot-test or Security architecture result was found during this review. No local build or QEMU runtime result is claimed.
- Remaining merge gates: exact-head CI, an isolated enabled-IF restoration test, concurrency/lifetime tests, and explicit review of interrupt latency and the single-CPU assumption.

## Locking follow-up — IRQ-off duration and namespace boundary (2026-10-25)

A fresh source review of PR #265 confirms that the file-handle lock and filesystem I/O lock now save RFLAGS and execute `cli` **before** spinning, then restore IF only when it was enabled on acquisition. This addresses the identified same-CPU timer-preemption spin deadlock at those lock sites, assuming the current single-CPU execution model.

It does not yet establish a complete kernel locking contract:

- Both locks can remain held while a filesystem callback performs synchronous block-device I/O. Interrupts therefore remain disabled for the full callback on the normal enabled-IF path; this can delay the PIT heartbeat and any interrupt-driven work. Measure worst-case hold time before treating this as an acceptable long-term design.
- The atomic lock word does provide cross-CPU exclusion for these specific lock instances; local cli alone does not provide system-wide synchronization. Before claiming SMP safety, audit all shared-state paths and lock ordering and run multi-CPU stress tests.
- Source call-site search finds the filesystem lock only in the read, write, and truncate wrappers. Namespace operations (`create`, `mkdir`, `unlink`, `rmdir`, `rename`, `lookup`, `readdir`) are not all serialized by that wrapper protocol. Do not mechanically wrap them until the call graph is checked for nested/re-entrant filesystem helpers and a consistent lock order is defined.
- The VFS lock self-test runs during early initialization with IF already clear, so it checks nested disabled-IF restoration but does not exercise the enabled-IF restore path. Do not turn interrupts on during early self-tests just to cover that branch.

Verification snapshot:
- PR #265 head at this review: `dbc8baa85cd8bb82723a9d13db46fd202c79d695`.
- No GitHub Actions workflow run was associated with this exact head at query time; older queued/cancelled runs are not valid evidence for this commit.
- No local build or QEMU run is claimed.
- Keep the PR unmerged until exact-head Build/boot-test and Security architecture checks pass, the enabled-IF path has safe isolated coverage, and same-handle/lifetime/namespace concurrency plus interrupt-latency risks have been reviewed.


## Fresh source and CI recheck — 2026-10-25 23:46 IST

- Exact PR #265 source head checked: `34b508bbf7a77ea01e56401dc3869fef77cb6ea3`.
- The source still contains IRQ-save locking in both `vfs_file_lock()` and `pfs_io_lock()`: save RFLAGS and execute `cli` before the atomic spin; restore IF only when it was set on entry. The VFS self-test checks nested restoration while the boot-time IF state is disabled; it does not safely cover an initially enabled IF state.
- File callbacks read/write/truncate/close/seek use the per-handle VFS lock. ProsenjitFS read/write/truncate use the filesystem I/O lock. The locks are nested VFS-handle → filesystem-I/O on the regular file callback path.
- The critical section still includes synchronous filesystem/block I/O with local interrupts disabled. This remains a latency concern; the present source-level mitigation is not a substitute for a scheduler-aware sleepable mutex if callbacks may block or become long-running.
- Atomic lock-word exclusion is present, but the complete filesystem/VFS design has no verified SMP contract yet. SMP enablement still requires full shared-state and lock-order review plus stress testing.
- Exact-head workflow query: no run was found for `34b508bbf7a77ea01e56401dc3869fef77cb6ea3`. The branch's latest visible Build run was queued for older source commit `3a759c2498bbcdda5fd15ebbfc762fe996bf0409`; it is not exact-head verification. No local build or QEMU run is claimed.
- PR #265 remains open and unmerged. Do not merge until Build/boot-test and Security architecture checks complete for the exact final code head and the interrupt-off latency plus missing concurrent lifetime/namespace tests are reviewed.
- PR #264 also remains open/unmerged; its Build and Security runs for `266ee0c48a5f0b13fb0023539ffd22100c7d8d64` were still queued at this recheck.

This section is a verification snapshot, not a claim that the filesystem is complete or release-ready.


## Exact-head correction — 2026-10-25 23:51 IST

- The current PR #265 head at this recheck is `7ce8f6f6bddd3cea59fa891781cbd44a0ba2815a`. The prior section's `34b508bbf7a77ea01e56401dc3869fef77cb6ea3` label is not the current PR head and must not be used as the verification target.
- A query filtered to workflow runs whose `head_sha` exactly equals `7ce8f6f6bddd3cea59fa891781cbd44a0ba2815a` returned no runs. The visible Build run for PR #265 references older commit `3a759c2498bbcdda5fd15ebbfc762fe996bf0409`; it is not evidence that the current head built or booted.
- PR #264's exact-head Security and Build runs remain queued/pending for `266ee0c48a5f0b13fb0023539ffd22100c7d8d64`; neither result is a pass yet.
- Current lock review remains source-level only: IRQ-save atomic spinlocks mitigate the same-CPU timer-preemption deadlock and serialize contenders on each lock word across CPUs, but hold local interrupts off across callbacks. Keep both PRs unmerged until exact-head CI completes and interrupt latency, namespace/lifetime races, full SMP/lock-order safety, and deterministic concurrent tests are addressed.


## Exact-head recheck — 2026-10-25 23:56 IST

- Live PR #265 head at this recheck: `e48f27b8401502dd2161726e0fe59e6c584fa8d3`; PR remains open and GitHub currently reports it as not mergeable.
- The branch's visible Build/boot-test run `40209491598` and Security architecture run `40209491593` both reference older head `3a759c2498bbcdda5fd15ebbfc762fe996bf0409` and remain queued. They are not exact-head verification for `e48f27b8401502dd2161726e0fe59e6c584fa8d3`; the exact-head workflow query returned no matching run.
- Source inspection confirms the IRQ-save spinlock correction and nested IF-restoration regression are present. The test executes during early initialization with IF disabled, so the initially-enabled IF restoration path is still not directly exercised.
- No local build or QEMU result is claimed. Do not merge based on the stale queued runs. The next verification gate is Build/boot-test and Security architecture on the exact functional source head, followed by controlled same-handle and mount/namespace lifecycle tests and review of IRQ-off latency.
- This is a status correction only; it does not claim the concurrency gaps are fixed.


## Shared IRQ-save primitive refactor — 2026-10-26

- The duplicate x86-64 save-RFLAGS/cli/atomic-spin/restore-IF implementations have been centralized in `kernel/include/irq_spinlock.h`. The VFS handle lock and ProsenjitFS I/O lock delegate to the same inline helper while keeping each acquisition's saved flags in a caller-local token.
- A standalone freestanding x86-64 GCC compile smoke test of the helper's inline-assembly pattern succeeded. This is **not** a full repository build, boot test, or runtime deadlock test.
- Exact functional head for this refactor: `2ea1a1067ce13676f0453d388065944e2ddab48d`. At the time of this update, no PR-triggered workflow run or commit status was available for that exact SHA.
- The long interrupt-disabled duration across synchronous filesystem callbacks remains unresolved. This refactor centralizes the primitive; it does not convert the callbacks to a sleepable-mutex design or close namespace/mount-lifetime races.


## Continued verification handoff — 2026-10-26 07:35 IST

- Live PR #265 head rechecked: `90cbf9d0328436d000c3e14737fbbd4ce5e1e39d`; PR remains open, unmerged, and GitHub reports `mergeable=false`.
- The IRQ-save helper is now centralized at `kernel/include/irq_spinlock.h`. It captures caller RFLAGS, executes `cli` before contending, atomically acquires the lock, releases it before conditionally restoring IF, and keeps saved flags in a caller-local token. VFS-handle and ProsenjitFS locks both use this helper.
- VFS file initialization clears `io_lock`; read/write/truncate/seek/close release the lock on the inspected success and validation/error paths. The boot-time nested-lock self-test verifies that IF remains clear during nested acquisition and is restored to its incoming state. Because early initialization enters with IF clear, this test does **not** exercise the incoming-IF-set branch.
- The helper's source comment explicitly limits it to short, non-sleeping critical sections. Current filesystem callbacks can perform synchronous block-device I/O while interrupts are disabled, so interrupt latency remains an unresolved design limitation. Do not treat this as a final scheduler-safe mutex.
- Exact-head query for `90cbf9d0328436d000c3e14737fbbd4ce5e1e39d` returned no PR-triggered workflow runs. Visible Build and Security runs are queued for older SHA `3a759c2498bbcdda5fd15ebbfc762fe996bf0409`, so neither is valid verification for the current head. No local full build or QEMU boot test is claimed.
- Next gates: obtain exact-head Build/boot-test and Security results; add controlled multi-thread same-handle and distinct-handle read/write/truncate/append interleaving tests; test close/unmount races; audit namespace operations and lock ordering; and decide whether long-running filesystem callbacks require a scheduler-aware mutex before marking concurrency complete.
- PR #264 remains open and unmerged as well; its journal-target regression also requires exact-head CI before merge.

This is a source/CI handoff update only. It does not mark filesystem concurrency or release readiness as PASS.


## Follow-up source review — IRQ-save lock contract (2026-10-26 08:20 IST)

- Source head inspected before this documentation-only update: `5d9e04df0ac83b03f4329f0a7a1f89cb5999dae6`; the audit note itself was committed afterward.
- `kernel/include/irq_spinlock.h` is now the shared inline primitive used by both `kernel/vfs.c` and `kernel/prosenjitfs.c`. It captures caller RFLAGS, executes `cli` before attempting the atomic lock, uses `pause` while contending, releases the lock before restoring IF, and keeps the saved flags in a caller-local token. This addresses the identified same-CPU timer-preemption deadlock pattern for callers that obey the lock protocol.
- Nested VFS-handle -> ProsenjitFS locking restores IF in layers: the inner acquisition observes IF clear and does not re-enable it; the outer unlock restores the original IF state. The shared helper comment correctly limits this to short, non-sleeping critical sections.
- **Newly re-confirmed design limit:** filesystem callbacks may synchronously perform block-device I/O while these locks are held. Therefore local interrupts may remain disabled for the duration of I/O. This can delay timer ticks, scheduler preemption and device interrupt service; it is a correctness mitigation, not a final latency-safe mutex architecture. Do not claim general concurrency is complete.
- The kernel is x86-64 and the lock word uses atomic builtins, but no repository-wide SMP enablement/contract was established by this review. IRQ masking is local-CPU behavior; it is not, by itself, an SMP lock.
- Exact-head PR workflow lookup for the inspected source head `5d9e04df0ac83b03f4329f0a7a1f89cb5999dae6` returned no PR-triggered workflow runs; combined commit status also reported no status entries. The subsequent documentation-only head was rechecked separately and also has no workflow runs or status entries. No local full build or QEMU boot test is claimed.
- PR #265 remains open/unmerged and GitHub reports `mergeable=false`. PR #264 also remains open/unmerged and reports `mergeable=false`; its journal-target tests still need exact-head Build/boot-test and Security architecture verification.
- Next safe work: add controlled concurrency tests and audit namespace/mount lifecycle paths; then decide whether the kernel needs a scheduler-aware sleepable mutex before widening lock scope. Do not mechanically wrap create/unlink/rename/readdir with this non-recursive lock until call-graph recursion and lock order are proven.


## Live PR recheck — 2026-10-26 08:22 IST

- Live PR #265 source head rechecked: `4787959042ee751524d8c1fa8fe6596f3625cf8c`; PR remains open, unmerged, and GitHub reports `mergeable=false`.
- The source uses the shared `kernel/include/irq_spinlock.h` helper from both `kernel/vfs.c` and `kernel/prosenjitfs.c`. It saves RFLAGS and executes `cli` before contending; caller-local saved flags preserve nested VFS → filesystem lock restoration.
- The visible Build/boot-test run [#40209491598](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40209491598) and Security architecture run [#40209491593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40209491593) are queued against older commit `3a759c2498bbcdda5fd15ebbfc762fe996bf0409`, not the live head. The branch workflow query found no exact-head run; these queued runs do not verify current source.
- Remaining gaps: local interrupts stay disabled across synchronous filesystem/device I/O; namespace and mount-lifecycle operations are not comprehensively serialized; deterministic concurrent tests are missing; and SMP safety is not established. No local full build or QEMU result is claimed.
- PR #264 remains open/unmerged at `266ee0c48a5f0b13fb0023539ffd22100c7d8d64`; its exact-head CI also requires rechecking.
- Keep both PRs unmerged until fresh exact-head CI passes and the remaining lock-latency, namespace/lifetime, and concurrency-test gates are reviewed.


## Post-refresh verification — 2026-10-26 08:25 IST

- Rechecked PR metadata after the prior audit refresh: current PR #265 head is `7efcf9b14fc60fa798c36721a22a38ac629e16d7`; it remains open and unmerged, with GitHub reporting `mergeable=false`.
- The current branch's visible Build/boot-test and Security workflow runs remain attached to older source commits, including `3a759c2498bbcdda5fd15ebbfc762fe996bf0409`. The commit-specific PR workflow lookup for `7efcf9b14fc60fa798c36721a22a38ac629e16d7` returned no runs. Therefore there is still no passing exact-head CI evidence for the current branch.
- Confirmed implementation detail: `kernel/include/irq_spinlock.h` saves caller RFLAGS, executes `cli` before attempting the atomic lock, and restores IF only when it was enabled at entry. VFS and ProsenjitFS use caller-local saved-flags tokens, preserving nested VFS-handle → filesystem lock restoration on the current CPU.
- The mitigation does not make long critical sections latency-safe. Synchronous filesystem callbacks can hold local interrupts disabled during block-device I/O. A scheduler-aware sleepable mutex remains a separate design task; IRQ masking alone does not establish SMP safety.
- No local build, QEMU boot, runtime deadlock reproduction, or concurrency stress-test pass is claimed. Keep PR #265 unmerged pending exact-head CI and deterministic concurrency/lifecycle tests.



## Scheduler-aware mutex feasibility review — 2026-10-26 08:27 IST

- Reviewed the current scheduler/wait primitives in `kernel/thread.c`, `kernel/include/thread.h`, and `kernel/ipc.c` against the filesystem lock's long interrupt-off window.
- `thread_block(wait_channel)` only marks the current thread BLOCKED and stores a wait-channel pointer; `thread_wake(thread)` marks a blocked thread READY and enqueues it. The IPC layer maintains its own waiter array under a separate channel spinlock. These primitives do not, by themselves, provide a general mutex with atomic lock-release / waiter-enqueue / block handoff semantics.
- Do **not** replace `pros_irq_spin_lock()` with a naive sleepable lock built only from `thread_block/thread_wake`: a lost-wakeup race could leave a waiter blocked indefinitely, and blocking while holding the VFS or filesystem lock would deadlock other I/O. A proper mutex needs a reviewed wait queue and an atomic scheduler handoff, with a documented lock-order protocol.
- The existing VFS self-test contains a focused nested-lock IF-state regression: it checks IF is clear while both locks are held, remains clear after inner unlock, and matches the original IF state after outer unlock. This is source-level test coverage only; no current exact-head build/boot result was found, so the test is not marked passed.
- The IRQ-save spinlock remains the immediate same-CPU preemption mitigation. It does not solve the interrupt-latency cost of synchronous block-device callbacks, nor establish SMP safety. Keep namespace mutation/readdir and unmount-vs-I/O lifetime gates open.
- Required next implementation step: design a kernel-wide sleepable mutex/wait-queue primitive with atomic block/wake handoff and tests before using it for filesystem I/O; until then, keep the current lock narrow and do not add lock wrappers mechanically to namespace callbacks.



## Fresh source review — 2026-10-26 08:40 IST

- Re-read the current PR branch source rather than relying on prior audit summaries. The filesystem-level IRQ-save lock is acquired by the ProsenjitFS VFS provider at the read, write, and truncate wrappers; the VFS handle lock is acquired by read, write, truncate, close, and seek.
- The VFS lock is outermost for normal file I/O: VFS read/write/truncate holds the handle lock while invoking the provider, and the provider then takes the filesystem lock. Preserve this VFS-handle → filesystem lock order. No general namespace-operation lock has been introduced by this change.
- `pros_irq_spin_lock()` disables local maskable interrupts before its atomic test-and-set loop. This addresses the identified same-CPU timer-preemption deadlock, but it means potentially lengthy block-device callbacks execute with local interrupts disabled. This is a temporary correctness mitigation, not a production-quality latency strategy; do not extend it mechanically to directory walks, namespace mutations, or mount/unmount.
- The existing nested-lock self-test checks IF state restoration, but it does not prove two scheduled threads serialize correctly, that close cannot race another thread using the same handle, or that separate handles safely race append/truncate. Those require deterministic scheduler-aware tests and should be added alongside a real mutex/wait-queue design.
- Freshness check: PR #265 head at review time was `3fccf3d1fc11333845f9a4b433f8406a78b600d5`; the branch workflow listing showed Build/boot-test and Security architecture runs queued for older SHA `3a759c2498bbcdda5fd15ebbfc762fe996bf0409`, not this head. A commit-specific workflow query for the current head returned no runs. This documentation commit should trigger new PR workflows, but results must be checked after GitHub schedules them.
- No local compile, QEMU boot, runtime deadlock reproduction, or concurrency stress-test pass is claimed. Keep PR #265 unmerged until exact-head CI passes and the lock-latency / lifecycle / concurrency gates are resolved.


## Fresh head and lock-primitive review — 2026-10-26 08:50 IST

- Current PR #265 head rechecked as `25e44619e9a62f344104b1829b526b3fc2ed7b9e`; PR is open, unmerged, and GitHub reports `mergeable=false`.
- The branch now contains `kernel/include/irq_spinlock.h`, included by both `kernel/vfs.c` and `kernel/prosenjitfs.c`. The helper saves RFLAGS and disables local maskable interrupts before contending, then uses an atomic test-and-set and restores IF only if it was set on entry. This is safer than the prior plain spinlock against same-CPU timer preemption and preserves nested VFS-handle → filesystem lock state using caller-local flags tokens.
- The helper is inline, so no separate build-object entry is needed. This is a source inspection only; it does not substitute for compiling the exact branch.
- The helper's documented limitation remains material: interrupts are disabled across synchronous provider callbacks and block-device I/O. Long interrupt-off windows can delay timer ticks and other maskable IRQ handling. This is a temporary serialization mitigation, not the desired final design.
- The branch-level workflow listing includes Build/boot-test run [#40209491598](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40209491598) and Security architecture run [#40209491593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40209491593), but their recorded `head_sha` is older (`3a759c2498bbcdda5fd15ebbfc762fe996bf0409`). The commit-specific workflow lookup for `25e44619e9a62f344104b1829b526b3fc2ed7b9e` returned no runs. Do not count the older queued runs as exact-head validation.
- Remaining blockers: no confirmed exact-head CI; no local build or QEMU boot result; no deterministic two-thread same-handle/close race or separate-handle append/truncate tests; namespace and mount/unmount lifetime synchronization remain incomplete; SMP safety is not established.
- Next implementation priority is not to broaden the IRQ-save critical section. Design a scheduler-aware mutex/wait queue with atomic waiter handoff and lost-wakeup tests, then migrate long filesystem callbacks under an explicit lock-order contract. Until that exists, keep the IRQ-save lock narrow and do not wrap namespace callbacks mechanically.
- Keep PR #265 unmerged until exact-head Build/boot-test and Security architecture checks pass and the concurrency/lifecycle gaps are reviewed.

## Fresh status checkpoint — 2026-10-26 08:53 IST

This checkpoint supersedes older SHA/status snapshots above; retain older entries as historical evidence.

- PR #265 currently points to `5d38bef352cdc6fa846e436a763643938c93fc60`, remains open and unmerged, and GitHub reports `mergeable=false`.
- PR #264 currently remains open and unmerged at `266ee0c48a5f0b13fb0023539ffd22100c7d8d64`; GitHub also reports `mergeable=false`. Recheck base-branch conflicts before attempting to merge either PR.
- The latest observed Build/boot-test run [#40209491598](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40209491598) and Security architecture run [#40209491593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40209491593) are still queued and both identify older workflow `head_sha` `3a759c2498bbcdda5fd15ebbfc762fe996bf0409`. Their PR metadata points at the current PR branch, but that does not make them exact-head validation.
- A commit-specific workflow query for `5d38bef352cdc6fa846e436a763643938c93fc60` returned no runs, and its commit status has zero reported statuses. No current build/boot/security pass is established.
- The IRQ-save helper is now present in source and addresses the identified same-CPU timer-preemption spin loop at the locking primitive level. It still holds local maskable interrupts off through synchronous filesystem callbacks, so interrupt-latency concerns remain. No claim is made that this branch has compiled or booted.
- No deterministic scheduler-controlled tests currently prove same-handle operation-vs-close serialization or separate-handle append/truncate serialization. Namespace mutation, directory enumeration, unmount-vs-I/O lifetime and SMP behavior remain open audit items.
- **Merge gate:** do not merge PR #265 or #264 until conflicts are resolved, exact-head Build/boot-test and Security checks pass, and the filesystem concurrency/lifecycle risks have been explicitly reviewed.


## Scheduler-aware mutex prerequisite audit — 2026-10-26

A source review of `kernel/thread.c` and `kernel/ipc.c` found that the existing blocking/wait path cannot safely be reused as a filesystem mutex without additional synchronization:

- `thread_block(wait_channel)` sets the current thread's wait channel and BLOCKED state, but it does not itself save/disable interrupts or atomically coordinate the scheduler state transition with publication in the wait queue.
- `ipc_channel_wait()` holds the IPC channel's ordinary test-and-set spinlock, calls `thread_block()`, and only then appends the thread to `channel->waiters`. Since the timer ISR can preempt kernel threads, the thread can be preempted while holding that ordinary spinlock; a scheduled contender can spin on the lock while its owner is suspended. This is a source-level deadlock risk, not a runtime-reproduced failure.
- `ipc_lock()` is also a plain busy-spin lock and does not mask local interrupts before contention. It must not be treated as a ready-made synchronization primitive for a new sleepable mutex.
- The filesystem IRQ-save lock addresses the same-CPU timer-preemption window for its own critical sections, but it can keep interrupts disabled across synchronous provider and block-device callbacks. That is a latency trade-off, not a final mutex design.

### Required mutex design gates

1. Establish one kernel-wide lock/wait protocol that makes waiter registration and transition to BLOCKED atomic with respect to wakeup and timer preemption; define the exact IRQ-state save/restore contract.
2. Use a scheduler wait queue with a no-lost-wakeup handoff. Do not block or yield while holding an ordinary spinlock; define and test lock ordering.
3. Specify whether the kernel is single-core only or supports SMP. IRQ masking prevents local timer preemption but does not replace atomic mutual exclusion between CPUs.
4. Add deterministic tests for contended acquisition, wakeup-before-block, block-before-wakeup, nested lock state restoration, owner exit/termination, and lock handoff under timer preemption.
5. Only after that primitive and tests exist should long filesystem callbacks migrate away from IRQ-off critical sections. Keep namespace operations and unmount lifecycle out of a mechanical blanket-locking pass until their call graph and lock order are audited.

**Status:** design prerequisite recorded; no scheduler-aware mutex implementation or runtime proof is claimed by this documentation update.



## Scheduler/mutex implementation contract — next coding phase (2026-10-26)

This section is a design gate, not an implementation claim. It is intended to prevent an unsafe mutex from being built on top of the current non-atomic `thread_block()` / wait-queue sequence.

### A. Required kernel primitive boundaries

Keep three concepts separate:

1. **IRQ-save spinlock:** protects short scheduler/wait-queue state transitions. It must be non-sleeping and restore the caller's original interrupt state. It is not suitable for a filesystem critical section that may perform block I/O.
2. **Scheduler-aware mutex:** protects sleepable process-context work. Contention must enqueue the current thread and atomically release the internal wait-queue guard while changing it to BLOCKED, so a wakeup cannot be lost between checking the mutex and sleeping.
3. **Filesystem operation/lifecycle gate:** prevents new operations from entering while unmount drains in-flight operations. It is a separate protocol from the mutex protecting file data and namespace metadata.

Do not implement the mutex by calling the current `thread_block()` and then separately appending to a wait queue. That ordering has a lost-wakeup window. Do not call `thread_block()` while holding a plain spinlock that a preempted owner might need to release.

### B. Mutex state machine and ownership

The eventual mutex API should have a small, auditable contract, for example:

- `pros_mutex_init(mutex)`: initialize unlocked state and an empty FIFO waiter queue.
- `pros_mutex_lock(mutex)`: valid only in a schedulable thread context; acquire immediately if free, otherwise register the current thread exactly once and block through the scheduler's atomic wait protocol.
- `pros_mutex_unlock(mutex)`: only the owner may unlock. Transfer ownership to a selected waiter before making that waiter READY, or use an equivalently proven handoff protocol; do not expose an unlocked interval that lets a newcomer bypass a queued waiter unless barging is explicitly permitted and tested.
- `pros_mutex_trylock(mutex)`: optional non-blocking operation; it must use the same internal guard and ownership invariants.
- Interrupt/exception context must not call a blocking mutex operation. Such callers need a separate short IRQ-safe primitive or must defer work to thread context.

These names are proposed design names, not symbols confirmed to exist in the repository.

### C. Lost-wakeup-safe block/wakeup transaction

The implementation must define one indivisible protocol across the mutex waiter queue, thread state, run queue, and wakeup path. On a uniprocessor, a reviewed IRQ-save scheduler guard may make the transition atomic against the timer ISR; on SMP, an inter-CPU atomic lock is also required. The exact mechanism must be selected after auditing every scheduler/run-queue mutation site.

Required invariants:

- A thread is either runnable/on the run queue, running, or blocked on exactly one wait channel; it cannot be both queued and blocked.
- Publishing a waiter and setting BLOCKED happen under the same guard that the unlock/wakeup path uses.
- Unlock cannot miss a waiter that has committed to sleeping.
- Wakeup cannot enqueue a thread twice, wake a DEAD thread, or clear a wait channel before the scheduler state transition is committed.
- A thread must never sleep while holding a guard that its waker or scheduler needs.
- Cancellation/process termination must remove or invalidate blocked waiters without leaving dangling queue nodes.
- Every exit/error path restores the original interrupt state and leaves queue membership and thread state consistent.

Do not implement this as a condition-variable loop until the underlying wait primitive satisfies these invariants.

### D. Filesystem lock migration order

After the mutex and its scheduler tests are accepted:

1. Add a filesystem operation/lifecycle gate and define unmount admission/drain semantics.
2. Replace the VFS per-open-file IRQ-off lock with a sleepable per-handle mutex only if all callers are guaranteed to be thread context and the callback contract permits sleeping.
3. Replace the ProsenjitFS filesystem-wide IRQ-off lock with a sleepable mutex for long synchronous I/O paths.
4. Audit nested order explicitly (proposed direction: per-handle state before filesystem state, only if no reverse acquisition exists); inspect close, vnode release, namespace callbacks, transactions, allocator and block-device callbacks before finalizing.
5. Add namespace locking only after nested callback paths are classified; never mechanically add a non-recursive mutex at both public and helper layers.
6. Keep interrupt handlers and any code that may run before scheduler initialization on IRQ-safe, non-blocking paths.

The proposed lock order is provisional until a complete call-graph audit confirms it.

### E. Deterministic test plan

Add test hooks or a host-testable synchronization model where possible; a self-test that only calls lock/unlock sequentially is not a concurrency test.

- **Lost wakeup:** force unlock between waiter intent and block; prove the waiter either acquires immediately or is queued and subsequently awakened.
- **Contention:** two or more threads contend; each critical-section counter must show no overlap and all waiters must eventually progress.
- **FIFO/handoff:** verify the documented fairness policy and that a newcomer cannot corrupt or duplicate queue membership.
- **Interrupt state:** test initially IF=0 and IF=1 in a safe harness; nested guard release must preserve the outer state.
- **Preemption:** force timer ticks at each lock/wait transition and prove no owner is suspended while a contender spins on a lock needed by that owner.
- **Cancellation:** terminate a blocked owner/waiter and verify no stale waiter or locked mutex remains.
- **Filesystem integration:** separate-handle append, ordinary write/read, truncate, create/unlink/rename/readdir, integrity scan versus mutation, and unmount versus active I/O/open descendants.
- **Failure paths:** invalid context, queue insertion failure, scheduler switch failure and device I/O error must leave locks and wait queues recoverable.

### F. Acceptance evidence

For every implementation commit, record the exact source SHA and exact-head Build/boot-test + Security architecture results. A queued, cancelled, stale-head or absent workflow is **not PASS**. Run QEMU/runtime concurrency tests after the build succeeds. Keep physical-device power-loss durability as a separate, unclaimed property unless it is tested on appropriate hardware.

**Current phase:** prerequisite/design specification only. Scheduler-aware mutex code, deterministic concurrency tests, exact-head CI PASS and production-readiness are still OPEN.


## Follow-up source review — block/wakeup implementation gate (2026-10-26)

After the mutex contract was recorded, the current `kernel/thread.c` and `kernel/ipc.c` implementation was re-read before making scheduler changes. This is a source-level review only; no runtime race has been reproduced.

### Findings that block a safe mutex patch

- `thread_block(wait_channel)` only writes `current_thread->wait_channel` and changes the state to `PROS_THREAD_BLOCKED`. It does not take a scheduler guard, atomically publish a waiter, or trigger a context switch itself.
- `ipc_channel_wait()` currently takes the channel's test-and-set lock, calls `thread_block()`, then appends the thread to `channel->waiters`. A timer preemption can occur between these operations. A sender/waker can observe an incomplete waiter publication, and the channel lock itself can be held by a preempted thread.
- `ipc_lock()` uses an ordinary busy-spin lock without local IRQ masking. If its owner is timer-preempted and a scheduled thread attempts the same channel lock, the contender may spin while the owner cannot run to release it.
- `thread_wake()` mutates the thread state and run queue but there is no single shared scheduler/wait-queue guard that makes waiter removal, BLOCKED-to-READY transition and run-queue insertion one atomic operation.
- `thread_schedule_interrupt()` can save an interrupt frame and change current/run-queue state. Any scheduler guard must be designed around this interrupt entry/exit path; adding a lock only to `thread_block()` and `thread_wake()` would leave scheduler mutations outside the protocol.
- The current IPC channel stores waiters in a fixed-capacity array. A mutex needs a separate queue/lifetime design or a carefully generalized wait queue; reusing the IPC array would couple mutex capacity to message-channel capacity and would make cancellation/ownership semantics unclear.

### Decision

Do **not** add a superficial mutex implementation on top of these functions. The safe next code change must first introduce a scheduler-owned wait protocol with explicit IRQ-state preservation and one atomic commit point for waiter registration plus BLOCKED state. Audit all run-queue mutation sites and process termination before exposing that protocol to mutex callers. Then add deterministic tests for both orderings (wake-before-block and block-before-wake), and timer preemption at each transition.

The source review did not modify `kernel/thread.c` or `kernel/ipc.c`; no scheduler fix or mutex implementation is claimed. This deliberate stop avoids presenting an uncompiled, race-prone synchronization primitive as a kernel fix.

### Verification checkpoint after documentation commit

- Documentation commit: to be filled with the resulting commit SHA after this section is committed.
- Build/boot-test and Security architecture: require runs attached to the exact resulting source SHA; absent/queued/stale-head runs are not PASS.
- PR #265 remains open/unmerged and its mergeability/conflict state must be rechecked after any source change.


## Implementation decision after IRQ-path audit (2026-10-26)

The follow-up review also inspected `kernel/idt.asm`, `kernel/kernel.c`, `kernel/include/irq_spinlock.h`, `kernel/ipc.c`, and `scripts/build.sh`.

### Why the next patch is not a one-line IRQ change

- The timer ISR executes with interrupts disabled and can call `thread_schedule_interrupt()`; scheduler state is modified from both ordinary thread paths and interrupt paths.
- `pros_irq_spin_lock()` masks local interrupts and preserves the prior IF bit, but its current documented contract is only a short non-sleeping critical section. It cannot be used as the mutex itself.
- The current build script compiles the kernel from an explicit source list and creates ISO/native BIOS images. New synchronization source files would have to be added to that list, and both boot artifacts need validation.
- Changing IPC locking alone would not establish a scheduler-wide invariant for `thread_enqueue`, `thread_dequeue`, `thread_schedule_interrupt`, process termination, and waiter cancellation. A local fix could mask one race while leaving the global run-queue protocol unproven.

### Safe next implementation boundary

Implement and review a scheduler-owned IRQ-save guard first, then refactor all scheduler/run-queue state transitions to use it consistently. The guard must not be acquired recursively or held over a context switch, blocking operation, or storage callback. For the current single-CPU target, local IRQ masking can serialize timer-interrupt access; this is not sufficient for SMP, which will require inter-CPU exclusion as well.

After that refactor, implement a wait-queue operation that publishes a waiter and commits the thread to BLOCKED under the same scheduler guard. The wake path must remove that waiter and transition/enqueue it under the same guard. Audit lock ordering carefully: do not call a scheduler function that takes the scheduler guard while already holding it unless a specifically designed internal “guard already held” API exists.

### Verification status for this checkpoint

- Source files reviewed; no kernel synchronization code changed in this checkpoint.
- Documentation commit: the commit containing this review must be verified by exact SHA below.
- CI and runtime behavior remain unverified until successful exact-head build, ISO/native boot, and deterministic scheduler tests are attached to the same commit.

## IPC lock representation and interrupt-context finding (2026-10-26)

The exact IPC header confirms that `struct pros_ipc_channel::lock` is a `volatile uint8_t`, while `pros_irq_spin_lock()` accepts a `volatile uint32_t *`. The existing IRQ-save helper therefore cannot be applied to the IPC lock by a cast: that would access a wider word than the field and could overwrite adjacent channel state.

A future IPC lock conversion must deliberately choose one of these approaches:

- Change the lock field and initialization to a correctly aligned `volatile uint32_t`, then use an IRQ-save token local to each acquisition/unlock pair; or
- Keep a byte lock and implement an equally reviewed byte-specific IRQ-save primitive, preserving the caller's prior IF state.

Either option only prevents same-CPU interrupt re-entry from spinning on a lock held by the interrupted owner. It does **not** by itself serialize all scheduler state transitions or make waiter publication atomic. The lock must not be held across a blocking/context-switch operation, and the scheduler/IPC lock order must be documented and enforced.

### Additional acceptance checks

- Verify the IPC lock's width/alignment and all initialization sites match its primitive.
- Exercise IPC send/wait/wake while timer interrupts are enabled; include the lock-owner-preempted scenario where the test harness can deterministically trigger it.
- Assert no waiter is simultaneously linked in the channel waiter collection and runnable queue in an inconsistent state.
- Verify failed wakeups preserve waiter membership and that process termination cancels waiters without corrupting the queue.
- Build both ISO and native BIOS artifacts with the repository's `-Wall -Wextra -Werror` flags, then run the existing QEMU boot/regression workflow on the exact PR head.

This is an audit clarification only. It does not claim that the IPC lock, scheduler, or mutex has been fixed.

## IPC lock IRQ-safety patch (2026-10-26)

### Source change

- `kernel/include/ipc.h`: changed the channel lock storage from `volatile uint8_t` to `volatile uint32_t`, matching the existing `pros_irq_spin_lock(volatile uint32_t *)` API and avoiding an unsafe pointer cast or wider-than-field access.
- `kernel/ipc.c`: replaced the ordinary test-and-set lock with `pros_irq_spin_lock()` / `pros_irq_spin_unlock()`. Each acquisition keeps its own saved RFLAGS token and restores the caller's prior IF state on every normal/error return path.
- The IRQ-save lock covers the existing short channel critical sections, including the waiter publication sequence (`thread_block(channel)` followed by appending the waiter), so the local timer IRQ cannot preempt that sequence on the current single-CPU target. It also prevents a timer interrupt on the same CPU from spinning on a channel lock held by the interrupted code.

### Scope and limits

This is a **narrow IPC lock fix**, not the scheduler-aware mutex implementation and not a claim that all scheduler/run-queue operations are globally synchronized. The helper masks local interrupts; SMP scheduler correctness still needs inter-CPU synchronization and a reviewed scheduler guard. The lock must remain non-sleeping and must not be held over a real context switch or storage callback.

### Verification state

- Source/header changes committed on the active PR branch; exact source SHA: `58af7b5c7a8701054ca101fd93fa0e806ba941fa`.
- Static source check: every IPC lock acquisition now has a saved IRQ token, and no legacy one-argument `ipc_unlock(channel)` calls remain.
- A local build could not be run in this environment because the GitHub host could not be resolved from the build container.
- At the time checked, the GitHub connector returned no workflow runs and no commit status for this exact SHA. This is **unverified**, not PASS.
- Next required gate: run the repository's `-Wall -Wextra -Werror` build, then both ISO and native BIOS QEMU boot/regression workflows on this exact source head. Add deterministic tests for prior-IF preservation and waiter publication/wakeup ordering before treating the race as closed.

### IPC interrupt-state regression added

- `kernel/kernel.c` now snapshots RFLAGS immediately before the existing IPC send/wakeup regression and checks that the IF bit is unchanged immediately after `ipc_channel_send()`.
- This is a narrow regression against accidentally enabling interrupts when the caller entered with them disabled. It does not simulate an IF-enabled caller or deterministic timer preemption; those remain required follow-up tests.
- Test source commit: `7cbf5768d3129fd4fcdb87c605cdc5d7cdff97a4`.

### IPC wait API ownership guard

- `ipc_channel_wait()` now rejects a waiter pointer that is not `thread_current()`. This is required because `thread_block()` blocks the current thread, not an arbitrary supplied thread; accepting a mismatch could previously publish a waiter that had not actually been blocked.
- The kernel IPC regression now passes the READY worker thread as a negative case and asserts the call returns `PROSENJIT_IPC_EINVAL`, the running init thread remains RUNNING, and no waiter is published before the valid current-thread wait is exercised.
- Implementation commit: `6ec54b4f52ce2a890dc4c5dbb0e961df1697a002`.
- Regression commit: `5f5169ffd4cbb722ab2dd90a680220f118f00fbe`.
- These tests are source changes only until the exact PR head builds and the QEMU boot markers pass.

### IPC message-count read synchronization

- `ipc_channel_count()` previously read the mutable `count` field without acquiring the channel lock. It now takes the IRQ-save channel lock while copying the count to a local value, then releases the lock before returning.
- The kernel IPC regression checks the count is zero immediately after initialization and one after a successful send/wakeup.
- Implementation commit: `225b1e3376d8390e5ca9a52eead1eca16b74b373`.
- Test commit: `44ca1ce7e82418bc9659eeb6f4271d300f8cad31`.
- The channel object lifetime must still be stabilized by its owner while APIs execute. This change protects the count field; it does not yet make all pre-lock validation reads or object lifetime transitions race-free.

### IPC mutable-state validation moved under lock

- Split channel validation into stable object/lifetime preconditions and mutable queue-state invariants.
- `head`, `tail`, `count`, and `waiter_count` are now checked only after acquiring the IRQ-save channel lock in send, receive, wait, cancel-waiter, and wake-one paths. This closes the validation-to-lock window where a same-CPU interrupt could change queue state after validation but before acquisition.
- `ipc_channel_count()` checks the same mutable invariants while holding the lock and returns zero if the queue state is invalid.
- Implementation commit: `c47759a3fa0206929b447447e8c5e8f798d02e59`.
- Object lifetime still requires external ownership/refcount discipline; this patch does not add reference acquisition or an SMP scheduler lock.

### IPC receive/count end-to-end regression

- Extended the existing IPC block/wakeup boot self-test to receive the message after the blocked receiver is scheduled again.
- The regression checks successful receive, exact payload length, every payload byte, and that the synchronized queue count returns to zero.
- Test commit: `75d18b06acaabe0c9858bfef8a81c585f3b9469d`.
- This adds a source-level regression gate; the marker must still be observed in an actual successful ISO/QEMU boot before this is considered verified.


## Scheduler source audit checkpoint — exact mutation inventory (2026-10-26)

This checkpoint re-read the active branch's `kernel/thread.c` and `kernel/include/thread.h` after the IPC receive/count regression was added. It is source inspection only; no scheduler behavior or race has been runtime-verified.

### Current mutation surface

The following functions mutate shared scheduler state and must be covered by one consistent synchronization protocol before a sleepable mutex can safely be introduced:

- `thread_manager_init()`: resets run-queue head/tail, current thread, and registry. This is initialization-only and must not race with scheduler entry.
- `thread_init()`: traverses and prepends to `thread_registry`, initializes state and queue links. Concurrent initialization can race with registry readers and duplicate-TID checks.
- `thread_terminate_process()`: traverses/unlinks the run queue, walks the registry, cancels IPC waiters, clears saved frames and wait channels, marks threads DEAD, and may clear `current_thread`. This is a multi-object transaction, not a single-thread state assignment.
- `thread_enqueue()` / `thread_dequeue()` / `thread_remove_queued()`: mutate shared queue links and head/tail. The enqueue helper also changes the thread state.
- `thread_schedule()`: dequeues candidates, may requeue the outgoing thread, switches process address space, and updates `current_thread` and RUNNING state.
- `thread_schedule_interrupt()`: performs the corresponding queue/current-thread transitions from the timer interrupt path and saves/restores interrupt frames.
- `thread_block()` / `thread_wake()`: change state and wait-channel ownership. `thread_wake()` also enqueues the thread and adjusts blocked-syscall restart state.
- `thread_runnable_count()`, `thread_blocked_count()`, `thread_dead_count()`, `thread_has_runnable()`, `thread_current()`, and `thread_state()`: read shared mutable state without a snapshot guard. They are observations, not synchronization barriers.

### Concrete hazards visible in the current source

1. **No scheduler-wide guard:** queue links, thread state, registry membership, current-thread pointer, and wait-channel state are modified without a common guard. The timer ISR calls `thread_schedule_interrupt()`, so ordinary thread code can be interrupted while these structures are mid-update.
2. **Block and queue publication are separate operations:** `thread_block()` sets `wait_channel` and BLOCKED, while IPC publishes the waiter in its channel queue in a separate step. The channel IRQ-save lock currently masks local timer IRQs while IPC performs this sequence on the single-CPU target, but this does not make general scheduler APIs atomic for other callers or establish SMP safety.
3. **Wake accepts no expected channel:** `thread_wake(thread)` verifies BLOCKED state but does not verify that the thread is blocked on the queue/channel performing the wake. A stale or mismatched caller can attempt to wake a thread owned by another wait source. The future wait-queue API should pair wake with an expected wait object and verify ownership under the scheduler guard.
4. **Termination spans scheduler and IPC state:** `thread_terminate_process()` unlinks threads from the run queue and invokes `ipc_channel_cancel_waiter()` while iterating the registry, then clears wait metadata and marks threads DEAD. A future guard cannot simply be held across the IPC call unless lock order and re-entry are proven; cancellation needs a defined detach/mark-dead protocol.
5. **Context-switch boundary matters:** both scheduling paths update run-queue/current-thread state around `process_set_current()`. A guard must not be released too early such that another scheduler entry observes half-committed state, nor be held across a context switch or blocking operation. This needs a narrowly designed internal transition API, not mechanical lock calls at each public function.
6. **Current helper is single-CPU only:** `pros_irq_spin_lock()` disables interrupts on the current CPU. It cannot prevent a second CPU from concurrently mutating scheduler state. SMP remains out of scope until an inter-CPU lock and CPU-local interrupt semantics are designed.

### Safe implementation order

1. Define a scheduler-owned guard contract and enumerate which public functions acquire it versus internal `*_locked` helpers that require it already held. Avoid recursive acquisition.
2. Refactor queue and state transitions first, including both cooperative and interrupt-driven scheduler paths. Keep process address-space switching and context-switch operations outside any spinlock unless a formally justified transition protocol says otherwise.
3. Introduce a scheduler-owned wait-queue abstraction whose atomic operation registers the current thread and commits BLOCKED under the same guard. Wake must remove the matching waiter, verify the expected wait object, commit READY/run-queue membership, and clear wait metadata in one transaction.
4. Refactor process termination to detach blocked waiters without holding the scheduler guard across IPC callbacks; document and test the exact lock order.
5. Add deterministic tests for enqueue/dequeue consistency, wake-on-wrong-channel rejection, block/wake orderings, timer preemption at transition boundaries, termination during wait, and prior-IF preservation.
6. Only after those tests pass, build a scheduler-aware mutex on the wait queue, then migrate filesystem critical sections from IRQ-off spinlocks to sleepable locks.

### Verification gate

- Branch source reviewed: `kernel/thread.c` blob SHA `f67c245ede7319c49d37fea0eff148758e652d7a`; `kernel/include/thread.h` blob SHA `a614eda8f3eff8bb0186b4d1753e4e9e61d793bf`.
- This checkpoint changes documentation only. It does not claim a scheduler fix, mutex implementation, or race closure.
- Build/boot verification must be attached to the exact resulting commit. At the preceding checkpoint (`7297a187c755bbe1d20a1d930227fa1007b6e11d`), the GitHub connector reported no commit statuses and no PR workflow runs; that state is unverified, not PASS.


## Channel-owned wake validation implementation (2026-10-26)

The scheduler now exposes `thread_wake_on_channel(thread, wait_channel)`. It rejects a wake when the thread is no longer BLOCKED on the expected channel. The existing `thread_wake(thread)` API remains for callers that intentionally perform a generic wake. IPC send and explicit wake now use the channel-specific API, preventing a stale/mismatched IPC waiter entry from waking a thread blocked on a different wait object.

This is a narrow ownership check, not a scheduler-wide synchronization fix. The run queue and thread state still lack a common scheduler guard; SMP safety, full termination ordering, and atomic publication across arbitrary wait sources remain unresolved. No build, boot, or runtime test is claimed until CI verifies the exact commit.


### Wrong-channel IPC wake regression

The kernel self-test now deliberately changes a registered receiver's `wait_channel` to a different channel and calls `ipc_channel_wake_one()` on the original channel. It requires the operation to return `PROSENJIT_IPC_EINVAL`, leave the thread BLOCKED, and retain the original channel's waiter entry. The test restores the expected channel and continues the existing normal send/wakeup/receive payload regression.

This is a deterministic source-level regression test added to the kernel startup self-test. It has not yet been executed in QEMU; passing behavior is not claimed until the exact-head build and boot workflow runs.


### Generic wake must not bypass wait-queue ownership (2026-10-26)

- `thread_wake()` now rejects a thread whose `wait_channel` is non-null. Such a thread is owned by a wait source; waking it generically would clear its wait metadata and enqueue it without removing the source's waiter entry.
- The caller must use the owning queue's channel-specific wake/cancel path, which can validate ownership and update the queue consistently.
- The kernel startup IPC regression now calls generic `thread_wake()` on a registered blocked receiver and requires rejection while preserving BLOCKED state, the channel pointer, and waiter count. The existing wrong-channel wake regression and normal send/wake/receive path then continue.
- This is a narrow API guard plus source-level regression. It does not add a scheduler-wide guard, prove SMP safety, or establish runtime behavior until the exact-head build and QEMU boot test pass.


## Process termination and IPC waiter cancellation review (2026-10-26)

### Current behavior and failure semantics

The current termination path first unlinks a process's threads from the runnable queue, then iterates the registry. For each live owned thread with a non-null `wait_channel`, it calls `ipc_channel_cancel_waiter(channel, thread)`, ignores the return code, clears `wait_channel`, clears blocked-syscall/frame state, and marks the thread DEAD.

The cancellation API has three relevant outcomes:

- `PROSENJIT_IPC_OK`: the matching waiter was found and removed under the channel IRQ-save lock.
- `PROSENJIT_IPC_EEMPTY`: the channel is valid but no matching waiter entry exists.
- `PROSENJIT_IPC_EINVAL`: the channel pointer fails the stable validity checks, or the channel's mutable queue invariants are invalid.

A missing waiter entry is not equivalent to a successful detach. However, clearing the dead thread's wait metadata is still necessary for teardown. The more serious unresolved case is an invalid/stale channel pointer: `thread_terminate_process()` currently assumes the wait-channel object remains alive and dereferenceable until cancellation returns. This is consistent with the existing IPC owner-lifetime contract, but it means channel lifetime must be enforced by the owner and cannot be inferred from `wait_channel` alone.

### Lock ordering and why not to patch by nesting locks

- Termination currently calls the IPC cancellation routine without a scheduler-wide guard held.
- Cancellation acquires the channel lock and only edits the channel waiter array; it does not call back into the scheduler.
- The IPC wake path takes the channel lock and then performs a thread wake/run-queue transition. A future scheduler guard therefore cannot be introduced mechanically around all of `thread_terminate_process()`: doing so would create scheduler-lock -> channel-lock ordering in termination while wake uses channel-lock -> scheduler-lock ordering, a potential inversion.
- The existing local IRQ-save channel lock does not protect against other CPUs. SMP correctness is not established.

### Required follow-up before scheduler synchronization is implemented

1. Define one lock-order rule and a scheduler-owned wait-queue protocol before adding a scheduler guard to the termination path.
2. Make waiter detachment and thread death a documented transaction: the channel owner must remain alive while cancellation runs, and a failed cancellation must be diagnosed rather than silently treated as successful removal.
3. Add deterministic tests for successful cancellation, cancellation when the waiter is absent, termination of a blocked process thread, and preservation of unrelated channel waiters.
4. Keep cancellation outside a scheduler guard unless a dedicated non-recursive internal API and an explicitly proven lock order are implemented.

This review is a source audit only. No termination code was changed, and no build, QEMU boot, or runtime pass is claimed. The safe implementation boundary is to establish the shared scheduler/wait-queue protocol first, then make process teardown use it; a local return-code check alone cannot prove that the scheduler queue, IPC waiter array, and thread registry remain consistent.


### Absent-waiter cancellation regression added (2026-10-26)

The kernel startup IPC regression now calls `ipc_channel_cancel_waiter()` on a valid, separate channel that has no entry for the blocked thread. It requires `PROSENJIT_IPC_EEMPTY`, zero waiter entries on that unrelated channel, and preservation of the original channel's waiter entry and the blocked thread's `wait_channel`. This exercises the non-mutating “waiter not found” path without dismantling the primary send/wakeup regression.

This is a source-level regression addition only. It has not yet been compiled or executed in QEMU. Successful cancellation during process termination and the lifetime/lock-order protocol remain open follow-ups.


### IPC cancellation API contract clarified (2026-10-26)

- `kernel/include/ipc.h` now documents that `ipc_channel_cancel_waiter()` detaches only the channel's waiter-array entry. It does not wake the thread and does not clear `thread->wait_channel`; callers must keep the channel object alive and coordinate teardown.
- The startup regression's failure diagnostic was restored to a meaningful cancellation-specific message after an earlier edit accidentally shortened the string. This is source review only; compile and boot remain unverified.
- This documentation does not solve the scheduler/IPC lock-order issue or prove that process termination and cancellation are atomic. The blocked-process termination regression remains an open item until a safe fixture and queue/lifetime protocol are established.


### Typed IPC wait-channel ownership (2026-10-26)

- `struct pros_thread.wait_channel` and `thread_block()` now use `struct pros_ipc_channel *` rather than an untyped `void *`. This matches the only current blocking implementation, `ipc_channel_wait()`, and makes accidental use of an unrelated object type diagnosable by the compiler.
- Process termination passes the typed pointer directly to `ipc_channel_cancel_waiter()`; the unchecked cast from a generic pointer is removed.
- This is an API/type-safety improvement, not a lifetime proof: the channel owner must still keep the channel alive while a thread points to it, and cancellation failure still needs a deliberate teardown policy. It does not add scheduler-wide locking or SMP safety.
- Verification is source-only until the exact PR head builds and the QEMU startup regressions execute.
