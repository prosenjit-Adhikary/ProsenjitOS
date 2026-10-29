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
