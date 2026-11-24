# FS-13 — VFS/provider lifetime contract audit

Date: 2026-11-14  
Audited baseline: `408c47d8bfcb9d527b8a947f8284c12d83a53067`

## Result

**PASS only for the existing single-CPU, fail-fast lifetime contract.** This is not a claim of general concurrent teardown, scheduler-aware blocking locks, or SMP safety.

## Source findings

### VFS mount lifecycle

- `kernel/vfs.c` encodes ACTIVE, DRAINING, and an active-operation count in an atomic lifecycle word.
- Path lookup pins the mount for the complete lookup operation.
- Namespace mutations pin the mount across lookup and the provider namespace callback, and share a fail-fast namespace gate.
- Deactivation uses an exact ACTIVE-to-DRAINING compare/exchange. If an operation is active, admission fails with EBUSY and the mount remains active.
- Before dropping the mount-owned root reference, deactivation requires the root vnode reference count to be exactly the mount-owned reference. Open file/directory handles and returned descendant vnode references therefore prevent root teardown.

### File and directory handles

- File and directory handles use byte-sized atomic operation gates.
- Re-entry/contention returns EBUSY; the code deliberately does not spin while a provider callback may be executing.
- The VFS self-test exercises re-entry into read/seek/write/truncate/close on the same file handle, mount teardown attempted from inside lookup, namespace contention/corrupt gate state, and stale directory-handle behavior.

### Provider registry and mount bridge

- `kernel/vfs_provider.c` uses an atomic active-mount counter with a reserved unregistering bit.
- Provider unregister fails while a mount pin exists.
- The bridge orders provider sync, VFS deactivation/root teardown, provider unmount, and provider-pin release.
- Sync failure leaves the mount active. Provider-unmount failure after VFS teardown retains the provider association and pin so cleanup can be retried.
- Provider self-tests cover mount-pin/unregister rejection, balanced pin release, sync failure, provider-unmount failure/retry, and registry preservation.

### FatFs disk/provider lifetime

- `kernel/fatfs_disk.c` counts in-flight disk operations and uses an unbinding gate. Bind, unbind, and read-only transitions reject while I/O remains in flight.
- The FatFs provider serializes API operations, tracks open `FIL` handles, syncs each live file before the device `CTRL_SYNC` barrier, and rejects unmount while open files remain.
- FatFs disk/provider self-tests cover in-flight unbind rejection and open-file/sync/unmount lifetime behavior.

## Runtime verification evidence

For exact PR-head source `fdc378528e374f2bfbccc89ff7f29e189d19e3a5`:

- Build/boot-test run [43596474406](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43596474406): PASS. The workflow checked out merge ref `31ef0443bdadf36a1815d9a2d08123235945f013` and verified its second parent was the exact PR head. ISO build, ProsenjitFS block-init diagnostic, exFAT formatter, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Security architecture run [43596474397](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43596474397): PASS.
- Build logs contain VFS provider registry boundary, FAT32/exFAT provider mount/I/O/remount, VFS permission/security, and VFS 64-bit file I/O pass markers in both ISO QEMU and native BIOS QEMU paths.

## Explicit limitations

- This is a fail-fast contract: contention returns EBUSY. It does not implement blocking wait-and-drain semantics.
- The kernel is a single-CPU foundation. Atomic gates do not prove SMP correctness or arbitrary concurrent teardown under a future multicore scheduler.
- `docs/SCHEDULER-AWARE-MUTEX-PREREQUISITES.md` remains binding. A safe scheduler-aware mutex requires an IRQ-safe guard that atomically coordinates waiter registration, BLOCKED transition, unlock ownership transfer, wakeup, cancellation, and teardown. A naive wrapper around current `thread_block()`/`thread_wake()` risks lost wakeups.
- The scheduler/IPC waiter-lock and preemption hazard must be solved as part of that work. Do not claim that the ProsenjitFS append serialization lock is a filesystem-wide lock for all reads, writes, truncate, namespace, integrity, and teardown paths.

## Acceptance boundary

FS-13 is accepted for the documented VFS/provider fail-fast lifetime guarantees on the current single-CPU execution model. Scheduler-aware blocking locks, general concurrent teardown, and SMP safety remain future work and are not ProsenjitFS 1.0 claims.
