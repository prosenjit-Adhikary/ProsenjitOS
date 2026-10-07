# ProsenjitOS Debug Log

## 2026-10-04 — CPL3 syscall/IPC test fix

- Diagnosed the CPL3 user-mode syscall test stopping after the expected DUP-denial checks.
- Root cause: `kernel/kernel.c` created `user_test_handle` from a `PROS_OBJECT_FILE` object with READ/DUPLICATE/TRANSFER rights, while the CPL3 test subsequently used that handle for IPC SEND/RECEIVE.
- IPC SEND requires an IPC object and WRITE capability; IPC RECEIVE requires an IPC object and READ capability.
- Fixed the CPL3 test setup to initialize a `struct pros_ipc_channel` and grant its embedded object with READ, WRITE, DUPLICATE, and TRANSFER rights.
- Local QEMU verification passed through the complete CPL3 test:
  - CPL3 entry
  - syscall ABI/version checks
  - capability DUP negative/positive checks
  - IPC SEND
  - IPC RECEIVE
  - duplicated/original handle close
  - stale-handle rejection
  - final CPL3 completion
- Verified boot output ended with: `ProsenjitOS: CPL3 user-mode and IPC syscall test passed`.

## 2026-10-07 — Build blocker: network self-test receive buffer

### Diagnosis

After removing the committed duplicate implementation in `kernel/thread.c`, the clean build reached `kernel/network.c` and failed under `-Werror=array-bounds`.

The network self-test declared `payload[1]` but later intentionally received a two-byte segment and checked `payload[1]`. The receive call correctly used `sizeof(rxdata)` for the two-byte case, so the test buffer itself was undersized.

### Fix

Changed the self-test buffer from one byte to two bytes while preserving the existing one-byte send test and the two-byte receive regression.

Synced in commit `71dcd08` (`fix: size network selftest receive buffer`).

### Status

The source-level build blocker is fixed. Clean build and QEMU verification are still required; ProsenjitFS remains the next runtime target.

## 2026-10-07 — Link-stage regressions after thread duplicate cleanup

### Diagnosis

The clean build passed compilation after the network self-test fix but failed at link time with missing scheduler symbols: `thread_has_runnable`, `thread_wake`, `thread_block`, `thread_runnable_count`, `thread_blocked_count`, and `thread_dead_count`. GitHub history confirmed these APIs were part of the thread subsystem before the malformed duplicate cleanup; the cleanup removed the duplicate block that was accidentally carrying these implementations.

A separate link failure showed unresolved `strcmp` references from `kernel/service_manager.c` and `kernel/vfs_provider.c`. The repository's kernel code uses compiler builtins for string comparison because no libc `strcmp` implementation is linked into the kernel.

### Fix

Restored the missing thread block/wake/scheduler-observability APIs into the authoritative `kernel/thread.c`, preserving the newer scheduler and process-termination implementation. Replaced kernel `strcmp` calls with `__builtin_strcmp` in service manager and VFS provider.

Synced commits:
- `30662b2` — restore thread scheduler APIs
- `78c042a` — use builtin string comparison in service manager
- `4347b6e` — use builtin string comparison in VFS provider

### Status

These are build/link blockers only; clean build and QEMU runtime verification remain required. No ProsenjitFS source was changed in this step.
