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

## 2026-10-15 — Self-hosted CI recovered; build and ELF regression tests advanced

### Runner and CI evidence
- GitHub Actions build jobs ran on self-hosted runner `office` (machine `debian`). Toolchain verification passed. This confirms the runner is executing jobs; recent failures are source/test failures rather than runner-offline failures.
- Run #6061 (commit `09100e4ab0debb8a4a93eed78abedb25e6852598`) passed security architecture checks but failed compilation because `kernel/prosenjitfs.c:pfs_data_start()` was defined but unused under `-Werror=unused-function`.
- Removed the unused helper in commit `e9648a902f0e4faa687f547a0e1ca42b8167c537`. On the subsequent run, ISO build passed and QEMU boot reached kernel initialization.

### Runtime failure and source fixes
- QEMU log for run #6062 (commit `e9648a902f0e4faa687f547a0e1ca42b8167c537`) reported `ProsenjitOS: ERROR ELF validation negative self-test failed` after paging, heap, address-space isolation, and persistent crash-record self-tests passed.
- In `kernel/elf_loader.c`, the overlapping-segment regression gave the second PT_LOAD a virtual address offset of `0x800` while retaining a page-aligned file offset. The validator correctly rejects the page-incongruent segment as `PROS_ELF_EFORMAT` before it can reach the intended overlap check. Updated the fixture to use the same page-congruent virtual address as the first segment, so the test specifically expects `PROS_ELF_ECONFLICT`. Commit: `44128ef27138fc22f87a38230071f8a4ee870924`.
- Also aligned `kernel/security_exec.c` self-test fixtures with the current security contract: TRUSTED/ALLOW records now include both `PROS_FILE_EVIDENCE_FORMAT_VALID` and `PROS_FILE_EVIDENCE_LOCAL_TRUST`; the terminal-process test models an already-zombie process directly because `process_set_state()` intentionally rejects terminal transitions, and it now uses a valid trusted record to test the terminal-state boundary. Commit: `5de14837b20dab928c767efe5da0ec33b4a8222e`.
- Earlier in the same sequence, fixed the NASM `security_halt` label-scope issue in `boot/entry.asm` (the later main-branch source uses a non-local label) and removed the unused ProsenjitFS helper as described above.

### Verification status / next steps
- The run after `e9648a9` built the ISO successfully but failed during QEMU boot at the ELF validation negative self-test; it did not reach CPL3/IPC milestones.
- Commits `44128ef` and `5de1483` are source/test fixes and require a fresh build/QEMU run. Do not mark boot or CPL3/IPC as passed until the new run's logs confirm the corresponding milestones.
- GitHub Actions artifact upload has separately reported storage quota exhaustion on previous runs; this does not explain the ELF self-test failure.
- Remaining kernel risks—shared IST1 syscall-frame lifetime, blocked/quarantined scheduler eligibility, no-successor handling, and IPC wait/wakeup synchronization—remain open.

## 2026-10-15 — ProsenjitFS preallocation extent-chain fix

### Diagnosis
- The latest clean CI build succeeded and QEMU passed the ELF validation, address-space, process/thread, IPC wait-queue, security runtime, partition discovery, and VFS provider checks before stopping in the ProsenjitFS keep-size preallocation regression.
- The self-test showed the inode's extent chain still ended after the original one-block extent even though preallocation reported success and incremented `allocated_bytes`.
- Root cause in `prosenjitfs_preallocate_blocks()`: the append path loaded the tail extent block into `tail_image`, changed only the local `tail.next` field, then recomputed the checksum over the unchanged `tail_image`. The transaction therefore committed a valid checksum for an unchanged zero `next` pointer, leaving the new extent disconnected.

### Fix
- Copy the updated tail extent structure back into `tail_image` before recalculating its checksum and staging the transaction. This makes the tail link and checksum consistent in the persisted block.
- Commit: `9beef04` (`fix: persist preallocation extent tail link`).

### Verification status
- Source fix is committed to `main`; the new GitHub Actions/QEMU run must verify it.
- Do not mark the OS complete until the QEMU ISO test and native BIOS boot test pass their required milestones. GitHub Actions artifact upload may still be independently blocked by the repository storage quota.

