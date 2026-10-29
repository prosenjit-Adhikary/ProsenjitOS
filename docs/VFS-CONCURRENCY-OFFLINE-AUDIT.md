# Native VFS Concurrency Audit — Offline Runner Checkpoint

**Status:** Source-level audit only. No runtime tests, build, or QEMU boot are claimed.
**Audited main commit:** `fc8604aca130561182e0e1b50077654569ba5e80` (2026-10-29).
**Scope:** `kernel/vfs.c`, `kernel/include/vfs.h`, and `docs/SCHEDULER-AWARE-MUTEX-PREREQUISITES.md`.
**Purpose:** Preserve actionable findings while the self-hosted local runner is offline.

## Findings confirmed from source

### VFS file position is shared mutable state without a per-handle synchronization contract

- `struct pros_vfs_file` contains a mutable `uint64_t position` and no lock/operation-reference field.
- `vfs_file_seek()` validates the handle and then assigns `file->position = position` directly.
- `vfs_file_read()` and `vfs_file_write()` dispatch through provider callbacks using the handle position and advance it after successful I/O. The ProsenjitFS provider I/O lock serializes provider-side data operations, but that does not by itself serialize the VFS-level position read/update with seek or with another operation on the same handle.
- Therefore concurrent read/write/seek on the same handle is **not proven safe**. Treat this as a race-risk finding, not a reproduced failure.

### Mount lifecycle requires an operation-entry/drain protocol before general concurrency can be claimed

- File and directory entry points check mount/handle state, while mount deactivation has busy checks for existing references.
- A state check alone is not an atomic lifetime guarantee if operation entry and deactivation can race. Before adding a per-handle or sleeping lock, define how an operation pins the mount/vnode, how unmount blocks new operations, and how active operations drain before resources are released.
- Do not add a naive blocking mutex yet: `docs/SCHEDULER-AWARE-MUTEX-PREREQUISITES.md` records that scheduler block/wakeup does not currently provide the proven atomic waiter-registration and handoff contract required to prevent lost wakeups.

### Namespace operations are a separate concurrency domain

- Rename/unlink/mkdir/rmdir require review for lookup-to-mutation races, vnode lifetime, parent-directory serialization, and lock ordering.
- The filesystem I/O lock does not automatically make a multi-step VFS namespace transaction atomic.
- Do not claim this is fixed until the scheduler/locking design and deterministic tests cover competing namespace operations.

## Work to implement when safe synchronization primitives are available

1. Introduce an explicit VFS operation lifetime protocol: reject new operations after unmount begins, pin the mount/vnode while an operation is active, drain active operations, then flush/deactivate. Define rollback on flush failure.
2. Add a per-open-handle operation serialization contract for read/write/seek/truncate/close. Ensure the position and handle lifetime are protected as one coherent operation. Do not hold a spinlock across potentially blocking storage I/O.
3. Define and document lock order across scheduler guard, handle state, namespace/vnode state, ProsenjitFS I/O lock, block device, and allocator.
4. Audit namespace mutation paths and make lookup/validation/mutation/lifetime rules explicit; add concurrent rename-vs-unlink and cross-directory rename tests.
5. Add deterministic scheduler/interleaving tests for: seek racing with read/write; close racing with I/O; unmount racing with open/read/write; rename racing with lookup/unlink; waiter registration racing with unlock; and cancellation/teardown of blocked waiters.
6. Run exact-head Build/boot-test and Security architecture workflows, then inspect ISO QEMU, native BIOS QEMU, filesystem integrity, and persistent ATA→MBR→ProsenjitFS markers. Record the exact SHA and results.

## Verification boundary

- Source review only; this document does not change runtime behavior.
- No build, local runner, QEMU, stress test, or physical power-loss test was executed for this checkpoint.
- Existing passing CI on earlier commits does not verify this audit commit.
- The project must not claim general concurrent VFS safety or physical-drive power-loss durability until those acceptance gates pass.


## Additional concrete findings from the follow-up audit

### Close-vs-operation is a handle-lifetime race, not only a position race

- `vfs_file_close()` calls the provider's `close` callback before marking `file->open = 0` and clearing the handle fields.
- Read/write/truncate validate `open` and then use `ops`, `vnode`, `mount`, `private_data`, and `position` across a provider callback. There is no shared operation reference or per-handle gate in `struct pros_vfs_file`.
- If the same handle is shared between concurrent callers, close can race with an operation after its initial validation. The source does not prove that provider state remains alive until that operation finishes. This is not a reproduced runtime failure; it is an unclosed lifetime guarantee.

### Vnode reference counts are not concurrency primitives

- `vfs_vnode_get()` and `vfs_vnode_put()` perform ordinary increment/decrement operations on `refcount`; no atomic operation or lock is used in these functions.
- This is safe only if all reference ownership transitions are externally serialized. Current interfaces do not express that requirement in the type/API contract. Before claiming concurrent lookup/open/unlink safety, audit every call site and establish the lock that protects refcount, parent retention, release callbacks, and initialized-state transitions.
- The root-refcount check in `vfs_mount_deactivate()` is also a check-then-act sequence. It prevents unmount when references are already visible, but does not alone serialize a concurrent operation that is acquiring a new reference.

### Directory handles have the same close/lifetime concern

- `vfs_dir_read()` checks `dir->open` and uses the vnode and mutable `cookie`; `vfs_dir_close()` drops the vnode reference and clears those fields without a per-directory operation gate.
- Concurrent readdir/close on the same directory handle is therefore not established as safe. Include it in the same handle-lifetime and deterministic interleaving test family.

## Updated severity and scope

These findings do **not** prove a specific exploitable race on the current single-CPU execution model. They establish that the API does not provide a general concurrency guarantee: correctness depends on scheduling, caller ownership, and serialization assumptions that are not enforced by the file/directory handle structures themselves. Do not patch this by adding a spinlock around provider I/O; synchronous storage I/O may block, and the scheduler-aware mutex prerequisites must be satisfied first.


## Scheduler/IPC cross-check (follow-up)

The current source review found an important qualification that must be preserved:

- `kernel/thread.c`: `thread_block()` sets `wait_channel` and marks the current thread BLOCKED; `thread_wake()` changes BLOCKED to READY, enqueues the thread, and clears `wait_channel`. These operations do not themselves acquire a shared scheduler guard.
- `kernel/ipc.c`: unlike a plain spinlock, `ipc_lock()` saves RFLAGS and executes `cli` before taking the atomic channel lock; `ipc_unlock()` clears the lock and restores interrupts only if IF was previously set. `ipc_channel_wait()` registers the waiter while that lock/interrupt exclusion is held.
- Therefore the specific scenario “local timer preempts the waiter while it owns the IPC channel lock” is **not supported by the inspected current code path**, because local interrupts are disabled during that critical section. The broader claim in `docs/SCHEDULER-AWARE-MUTEX-PREREQUISITES.md` should be re-reviewed/worded more precisely rather than repeated as a confirmed defect.
- The inspected protocol is still not an SMP guarantee: disabling interrupts affects only the local CPU, while the atomic channel lock alone does not make scheduler queue/state changes globally atomic against another CPU. Also, the generic `thread_block/thread_wake` API does not encode the caller's exclusion/guard contract, so a new sleeping mutex must define and enforce that contract explicitly.
- No runtime interleaving test was run. This is a source-level correction to the audit narrative, not a claim that IPC or scheduler concurrency is fully proven.

### Safe next implementation gate

Before implementing a sleeping mutex, map all callers of `thread_block()`, `thread_wake()`, and scheduler queue/state mutators; state whether the supported target is strictly uniprocessor or SMP; then add a testable scheduler guard/atomic wait protocol consistent with that target. Do not replace the filesystem spinlock until that prerequisite and the “never sleep while holding a spinlock” rule are proven.
