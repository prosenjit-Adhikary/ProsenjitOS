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

## 2026-10-07 — Link-stage correction: freestanding kernel cannot rely on builtin strcmp lowering

### Diagnosis

The previous `__builtin_strcmp` replacement did not remove the linker dependency: with the kernel's freestanding `-fno-builtin` build configuration, GCC still emitted calls to the external `strcmp` symbol for these non-constant comparisons. The clean build therefore continued to fail in `service_manager.o` and `vfs_provider.o` with undefined `strcmp` references.

### Fix

Replaced the remaining `__builtin_strcmp` uses with small local byte-wise equality helpers in `kernel/service_manager.c` and `kernel/vfs_provider.c`. These helpers perform the required equality checks without introducing any libc dependency.

Synced commits:
- d8e5e6c — avoid libc strcmp in service manager
- 25889a8 — avoid libc strcmp in VFS provider

### Status

This correction specifically addresses the observed link failure. A clean build must be rerun before any runtime or ProsenjitFS diagnosis continues.


## 2026-10-07 — VFS provider registry audit completed

### Diagnosis

The current provider registry now uses an internal byte-wise name comparator, but its self-test still exercised only a narrow mutation path and could return success without testing the full registry. It did not prove malformed operation-table rejection, registry-full behavior, repeated registration/unregistration, or preservation of the exact live provider set.

### Fix

Expanded \`kernel/vfs_provider.c\` self-testing without changing the provider ABI or unregister semantics. The test now snapshots live providers, validates malformed ABI/name/flags/operation-table inputs, generates collision-safe test names, fills every available registry slot, verifies ENOSPC at capacity, checks duplicate and unknown unregister rejection, performs repeated register/unregister cycles, verifies lookup after add/remove, restores all test providers in reverse order, and confirms the exact pre-test provider pointers remain unchanged. Callback argument validation is also exercised directly.

The current provider ABI still has no active-mount reference counter, so active-mount lifetime/unregister exclusion remains explicitly unverified rather than being falsely claimed as complete.

### Status

Implementation synced in the current branch. Build and boot execution are still environment-blocked: the latest GitHub Actions build run for HEAD \`56f01f0\` failed before runner allocation (runner_id=0, no runner name, zero steps). The local Debian repository is not mounted in this execution environment, so no compile/QEMU result is being fabricated.


## 2026-10-07 — VFS registry full-state edge-case correction

### Diagnosis

The first expanded registry self-test draft still assumed at least one temporary test provider existed. That was unsafe when the live registry was already full with real providers: it could index an uninitialized test slot and it could not perform repeated temporary cycles without removing a real provider.

### Fix

Adjusted the self-test to use a copied live provider for duplicate-name rejection when the registry is full, use a valid-but-unregistered synthetic provider for unknown-unregister rejection, generate a non-colliding valid overflow name, and run repeated temporary registration/unregistration cycles only when the self-test actually owns a temporary slot. No real provider is removed or reordered.

Synced in commit \`7d7566a\` (\`test: cover full VFS registry state\`).

### Status

The source-level full/partial registry paths are now covered by the self-test design. Compilation and QEMU execution remain blocked by the current environment: GitHub-hosted Actions for the repository repeatedly fail before runner allocation with \`runner_id=0\`, empty runner name, and zero executed steps. The local Debian clone is not mounted in this execution environment.


## 2026-10-07 — VFS registry overflow-name collision correction

### Diagnosis

The full-registry test needed an overflow provider name that was valid and not already registered. The prior draft searched for an unused name after the registry had been filled, which is impossible when the registry is full and could turn a correct ENOSPC check into a false self-test failure.

### Fix

Reserve/select the overflow test name before the capacity check, using a separate generated name family that cannot collide with the normal self-test names. The full registry now tests a genuinely new valid provider and expects ENOSPC without modifying registry contents.

Synced in commit 33231f3 (test: preserve non-colliding VFS overflow case).

### Status

VFS provider registry source audit is complete at the current achievable level. The remaining provider-lifetime gap is architectural: the ABI has no mounted-provider reference ownership. Build/QEMU verification remains environment-blocked because GitHub-hosted jobs continue to fail before runner allocation (runner_id=0, empty runner name, zero steps), and the local Debian clone is unavailable in this execution environment.


## 2026-10-07 — VFS full-registry overflow self-test correction

### Root cause
The overflow-provider test name was being selected after the registry had been filled. At full capacity, every candidate name was necessarily already present, so the test could reject its own ENOSPC scenario before exercising ENOSPC.

### Fix
A separate overflow-name namespace is now selected and reserved before filling the registry. The selected valid, non-colliding name is then used for the final full-registry registration attempt, which must return ENOSPC without mutating the registry.

### Verification status
Source audit completed. Runtime build/QEMU verification remains blocked by the repository Actions runner: the latest jobs terminate with runner_id=0, empty runner_name, and zero steps. No runtime pass is claimed.

Commit: 9a50ea0 (fix: reserve VFS registry overflow name before fill).


## 2026-10-07 — VFS root ownership hardening

### Root cause
vfs_mount_set_root() could replace an already-installed mount root. The previous root reference was not released, violating the invariant that a mount owns exactly one root reference and potentially leaking a live root/vnode chain.

### Fix
vfs_mount_set_root() now rejects root replacement with PROS_VFS_EBUSY. A root can therefore be installed once and must be released by mount deactivation before another root can be attached.

### Verification status
Source-level ownership invariant verified. Runtime build/QEMU verification remains blocked by GitHub runner allocation: latest build job has runner_id=0, empty runner_name, and zero executed steps.

Commit: c89ab51 (fix: reject VFS root replacement on live mount).


## 2026-10-07 — VFS mount reinitialization hardening

### Root cause
vfs_mount_init() could overwrite an existing mount object's fields without requiring the caller to deactivate the previous mount first. If the mount was active or still held a root, this could discard ownership state and bypass the normal unmount lifetime checks.

### Fix
vfs_mount_init() now returns PROS_VFS_EBUSY when the mount is active or still owns a root vnode. Reinitialization therefore requires the existing mount lifetime to be closed cleanly first.

Commit: 7913dda (fix: reject live VFS mount reinitialization).


## 2026-10-07 — ProsenjitFS zero-length I/O contract enforcement

### Root cause
The VFS contract treats zero-length reads and writes as successful no-ops that do not require a data buffer. ProsenjitFS still rejected a null buffer before checking length == 0, so the backend violated the VFS contract despite the existing zero-length self-test only exercising a non-null argument.

### Fix
Adjusted `pfs_file_read()` and `pfs_file_write_unserialized()` so a null buffer is rejected only for non-zero I/O. Zero-length operations now reach the existing successful no-op path.

### Verification status
Source-level contract audit completed. Runtime build/QEMU verification remains environment-blocked until a GitHub Actions runner is allocated; no runtime pass is claimed.


## 2026-10-07 — VFS partition-mount failure atomicity

### Root cause
`vfs_mount_partition()` initialized and mutated the partition view before validating the mount type and existing mount lifetime. A rejected call (unknown filesystem type or a live mount) could therefore modify the supplied partition view even though the VFS mount operation failed.

### Fix
Moved the mount-state/type validation ahead of `partition_device_init()`. Rejected calls now fail without mutating the partition view; successful calls retain the existing partition initialization and mount setup path.

### Verification status
Source-level failure-atomicity audit completed for this path. Runtime build/QEMU verification remains environment-blocked until a GitHub Actions runner is allocated; no runtime pass is claimed.


## 2026-10-07 — ProsenjitFS stale directory mutation hardening

### Root cause
After inode-generation tracking was added for stale vnode protection, `pfs_vnode_create()` and `pfs_vnode_rmdir()` still read their parent inode directly by inode number. A retained directory vnode whose inode had been freed and reused could therefore pass into a mutation callback and operate on the replacement inode instead of being rejected as stale.

### Fix
Both directory mutation callbacks now validate the vnode's captured inode generation through `pfs_vnode_validate_inode()` before reading or modifying directory state. Stale/free/reused directory handles therefore fail closed with the established stale-handle mapping.

### Verification status
Source-level stale-vnode audit completed for create, mkdir, rmdir, unlink, rename, lookup, and readdir paths. Runtime build/QEMU verification remains environment-blocked until a GitHub Actions runner is allocated; no runtime pass is claimed.


## 2026-10-07 — ProsenjitFS readdir stale-vnode hardening

### Root cause
`pfs_vnode_readdir()` validated the directory vnode at entry, but later re-read the directory inode directly by inode number. That second read bypassed generation validation and could observe an inode replacement if the retained vnode became stale during the operation.

### Fix
The readdir directory-inode read now uses `pfs_vnode_validate_inode()`, keeping the operation on the vnode's captured generation rather than accepting a reused inode number.

### Verification status
Source-level stale-vnode audit completed for directory traversal and mutation callbacks. Runtime build/QEMU verification remains environment-blocked until a GitHub Actions runner is allocated; no runtime pass is claimed.


## 2026-10-15 — Cross-subsystem source review after reported old-bug fix

Review target: branch `kernel/syscall-context-switch-contract-20261015` at `527cdad65893ac74409c7571a500dff2cb515d39`, plus open PR heads #128, #131, #132, and #134 where their changes affect the same subsystems. This is a source review; it is not a clean-build or runtime verification result.

### Confirmed kernel correctness risks
- `kernel/idt.asm:isr128` enters through shared TSS IST1, while `kernel/thread.c:thread_schedule_interrupt()` retains the supplied frame pointer in `thread->interrupt_frame`. That pointer is not stable per-thread storage across subsequent syscall entries.
- `struct pros_interrupt_frame` models only the CPL0 IRET tail (RIP/CS/RFLAGS); a CPL3 return frame additionally contains user RSP/SS. Any user-mode context switch must preserve the full frame kind and layout.
- `thread_process_runnable()` excludes only ZOMBIE/DEAD processes; it does not exclude process BLOCKED or `quarantined`.
- `security_quarantine()` changes process state but does not detach READY threads from the run queue; `security_clear_quarantine()` restores process state but does not requeue threads that were runnable before quarantine.
- `thread_schedule_interrupt()` returns the incoming frame when there is no successor. That can resume a caller that has already blocked or been quarantined.
- The dequeue loop marks a candidate DEAD when the process-runnable predicate fails. Therefore, tightening the predicate alone would incorrectly kill temporarily blocked/quarantined threads.
- IPC receive publishes `blocked_syscall` after waiter registration unlocks the channel; the scheduler stores the saved frame later. The local interrupt-disabled path may serialize this on one CPU, but it is not an SMP-safe publication protocol.

### Cross-branch integration note
- PR #134 (`kernel/ipc-receive-wakeup-race-20261015`) adds an EALREADY/retry path for a message arriving between empty receive and waiter registration. It explicitly does not solve the shared-IST saved-frame lifetime or SMP publication problem.
- PR #132 contains timer IRQ register-preservation/alignment changes; these do not solve syscall-frame lifetime, TSS RSP0 per-thread updates, or quarantine handoff.
- PR #133 is documentation-only and must not be treated as an implementation fix.
- Mainline and these feature branches are separate until merged; verify the exact branch/commit before attributing a fix to the integrated kernel.

### CI and verification
- Build/boot run #38688576028 and security-design run #38688576057 for commit `527cdad` were queued with `runner_id=0`, no runner name, and no executed steps when inspected.
- No clean `-Wall -Wextra -Werror -O2` build, QEMU boot, native BIOS boot, or runtime context-switch/quarantine pass is claimed from this review.
- Keep the scheduler/syscall-context work open until stable per-thread frame storage, complete CPL3 frame handling, per-thread kernel-entry stack/TSS RSP0, quarantine-aware queue transitions, no-successor handoff, and IPC publication/wakeup regressions are implemented and tested together.
