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
