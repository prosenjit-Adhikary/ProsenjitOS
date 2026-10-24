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

## 2026-10-15 — ProsenjitFS seek regression corrected

### Diagnosis
- After the extent-link fix, QEMU advanced beyond the preallocation-chain failure and reached the file I/O boundary regressions.
- The test expected `vfs_file_seek(file, 701)` to fail for a 700-byte file. That contradicts the filesystem's deliberate sparse-file behavior: seeking past EOF is allowed so a subsequent write can create a hole. The actual VFS contract rejects offsets above its 32-bit limit, not offsets merely beyond EOF.

### Fix
- Updated the regression to seek to `0x100000000ULL`, expect `PROS_VFS_EOVERFLOW`, and verify that the current position remains 700.
- Commit: `ef55d48` (`test: align seek regression with sparse-file semantics`).

### Verification status
- The prior QEMU run confirms the preallocation extent-chain failure is no longer the first failing test; it then failed at this seek regression.
- Awaiting the next clean ISO/QEMU run to identify any remaining failures and reach the CPL3 and native BIOS tests.

## 2026-10-15 — ProsenjitFS consistency checker and keep-size preallocation

### Diagnosis
- The clean QEMU boot passed the ELF validation, address-space, process/thread, IPC wait-queue, security runtime, partition, VFS provider, and storage-discovery milestones, then failed during the full persistence self-test.
- The failing assertion was `pfs_selftest_consistency()`. Its file-extent validation rejected any extent whose logical end exceeded the current EOF.
- That invariant is invalid for this filesystem: `prosenjitfs_preallocate_blocks()` intentionally reserves zeroed blocks beyond EOF without increasing `inode.size`. The extra allocated extent can remain beyond EOF until later writes consume it; `allocated_bytes` accounts for it.

### Fix
- Removed the EOF-bound check from extent ownership consistency validation while retaining overflow, logical-order, physical-range, ownership-bitmap, and `allocated_bytes` checks. File size remains a separate logical EOF boundary for reads.
- Commit: `43b52c2` (`fix: allow keep-size preallocation beyond EOF in consistency check`).

### Verification status
- The next clean CI/QEMU run must verify this change. The full persistence test still has further namespace, truncate, remount, and allocation assertions after this checker, so completion is not yet established.



## 2026-10-21 — Network receive-size regression exposed by native boot test

### Diagnosis

The latest Linux self-hosted QEMU run built the ISO successfully and printed `ProsenjitOS: ProsenjitFS format/mount/allocation/journal/recovery self-test passed`. Boot then stopped at the network self-test with return code `-10`, before the later CPL3 milestone. The failing `-10` block intentionally checks that receiving a two-byte queued segment into a one-byte destination returns `PROS_NET_EBOUNDS`, then checks successful two-byte receive.

The earlier safety fix correctly enlarged the test's `payload` array from one byte to two bytes because the successful path reads `payload[1]`; however, the undersized-buffer assertion still passed `sizeof(payload)`, now two bytes. The test was therefore no longer undersized and failed its own expected error check.

### Fix

Changed only the undersized-receive assertion to pass a capacity of `1u`; retained the two-byte array and the subsequent two-byte successful receive checks. No runtime assertion was removed or weakened.

Commit: `4fdc8603` (`test: preserve undersized network receive regression`).

### Verification status

The preceding run `7460` on Linux runner `Office1` built successfully but failed QEMU at the network self-test (`rc=-10`); ProsenjitFS's format/mount/allocation/journal/recovery self-test passed in that run. The new commit triggers another full build/QEMU run; ISO boot, native BIOS boot, and final all-green status remain pending until that run completes.


## 2026-10-21 — Native BIOS ProsenjitFS persistence path

### Confirmed progress

- The network receive-size regression was fixed by restoring its deliberately undersized one-byte receive capacity; the two-byte successful receive remains covered.
- The CPL3 process context was restored before user-mode entry, and the ISO/QEMU boot milestones now pass, including ProsenjitFS format/mount/allocation/journal/recovery and the CPL3 syscall test.
- Main run `7470` and PR run `7469` passed the current build workflow, including ISO and native BIOS boot checks.

### Native disk gap identified for deeper validation

The native BIOS image reserves an MBR partition (type `0xDA`, LBA 2048, 512 sectors), but the current build script does not format that partition and the kernel does not invoke the ATA PIO provider. The current green boot run therefore validates the native bootloader and the in-memory ProsenjitFS self-test, but does not yet prove that the real ATA disk partition can be mounted and written.

The completed change adds a host formatter that reuses the kernel's existing `prosenjitfs_format()` implementation on the freshly-created disposable image, plus a native-boot-only ATA/MBR/partition test that writes a file, unmounts, remounts read-only, and verifies persisted bytes. The kernel never auto-formats an unknown disk.

Verification: build run [`7471`](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39555440828) passed on self-hosted runner `Office1`. ISO build and QEMU boot passed; native BIOS QEMU boot passed; the log contained the ATA PIO, LBA boundary, and ATA→MBR→ProsenjitFS persistence success markers. The native test verified `/native-persist.bin` after unmount/remount. Security architecture run `7009` also passed. Final verification: build run [7472](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39555737435) passed on commit `de7e740` (ISO build, ISO QEMU, native BIOS QEMU, Windows runner smoke); security architecture run [7010](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39555737412) passed. The native ATA/MBR/ProsenjitFS persistence markers were required by the workflow and the native boot step succeeded.


## 2026-10-23 — Populated-namespace ProsenjitFS integrity I/O-failure sweep

### Investigation and result

- Commit `e14e0c1` first separated the populated-namespace integrity sweep's generic failure into distinct diagnostics. Its ISO QEMU boot failed at self-test stage `0x227F` (source line 8831), which identified the `scan_rc != PROSENJITFS_EIO` assertion as the failing check; the same run's build and DML checks passed, but the required filesystem success milestone was missing.
- Commit `4bdaf54` improved this diagnostic further: if the scan return value is unexpected, the self-test now encodes the actual `scan_rc` into its failure code (`-3000 + scan_rc`). It does not change the integrity scan or error propagation itself.
- Full Build run [39930150534](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39930150534) passed after the diagnostic commit: ISO QEMU boot and native BIOS QEMU boot both reached the required ProsenjitFS format/mount/allocation/journal/recovery milestone; both QEMU steps exited with the expected timeout status 124 after the required milestones. Security architecture run [39930150525](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39930150525) passed.
- The same Build run was re-run independently and passed again. Both ISO and native BIOS QEMU boot tests passed, and the ProsenjitFS milestone appeared in both serial logs.

### Status / next action

- Current verification is green on commit `4bdaf54fb1911a7c04f8bc914c7c0263444b65e9` for Build/QEMU and Security architecture checks.
- The earlier `scan_rc` mismatch was not reproduced on two runs after adding diagnostics. Because the diagnostic change does not alter filesystem runtime behavior, treat the original failure as an intermittent/unresolved test anomaly rather than claiming the underlying cause is fixed. Preserve the stronger return-code diagnostic and investigate further if it recurs.


## 2026-10-23 — ProsenjitFS rename commit-write failure across remount

### Regression added
- Added a populated-file namespace regression that injects a failure when the journal commit record is written during `vfs_rename`.
- The test requires the operation to fail and the active mount to become read-only, simulates power loss, remounts, verifies the original path and all payload bytes remain intact, verifies the destination returns `PROS_VFS_ENOENT`, and runs the report-only integrity check.
- Change: commit `13cf8cc0201ba642be8960475f866d3a732029cb`, PR [#197](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/197).

### Verification
- Build and boot-test run [#8188](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946623102): **PASS** on self-hosted runner `office`. ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Security architecture run [#7724](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946623071): **PASS**.
- This verifies the injected mock-device commit-write failure and namespace recovery path under QEMU. It does not prove physical-device power-loss durability or cover every namespace mutation/failure boundary.


## 2026-10-23 — ProsenjitFS unlink commit-write failure across remount

### Regression added
- Added a populated-file namespace regression that injects a failure when the journal commit record is written during `vfs_unlink`.
- The test requires the unlink to fail and the mount to become read-only, simulates power loss, remounts, verifies the original path and every payload byte remain intact, and runs the report-only integrity check.
- Change: commit `ed197bf77328ae419f8f45480640da0063641537`, PR [#198](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/198).

### Verification
- Build and boot-test run [#8194](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946938083): **PASS** on self-hosted runner `office`. ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Security architecture run [#7730](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946938090): **PASS**.
- This verifies one unlink commit-write failure and recovery path under the mock block device. It does not prove physical-device power-loss durability or cover every mutation/failure boundary.


## 2026-10-23 — ProsenjitFS unlink read-error classification

### Root cause and fix
- Source audit found `pfs_vnode_unlink()` combined an invalid directory-entry LBA and a failed `pfs_read_block()` into the same `PROSENJITFS_ECORRUPT` result.
- Split the checks: invalid bounds remain corruption; actual block-device errors now propagate through the existing VFS error mapping as I/O failures.
- Added a fault-injection regression targeting the root directory-entry read. It requires `PROS_VFS_EIO`, unchanged durable/volatile/dirty media, a writable mount, intact file bytes, and a successful integrity scan.
- Change: commit `1c051dc7f4d2561352e7728e1b0da86cadfa3c54`, PR [#199](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/199).

### Verification
- Build and boot-test run [#8199](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947309841): **PASS** on self-hosted runner `office`; ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Security architecture run [#7735](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947309844): **PASS**.
- This fixes and verifies the specific unlink directory-entry read-error classification path; it is not a claim that all concurrency or physical-device durability requirements are closed.


## 2026-10-23 — ProsenjitFS truncate commit-write failure across remount

### Regression added
- Added a populated-file regression that injects failure at the journal commit-record write while shrinking a file to zero bytes.
- The test requires the truncate to fail with `PROS_VFS_EIO`, the mount to become read-only, then simulates power loss and remounts to verify the original EOF and all payload bytes remain intact; the report-only integrity scan must pass.
- Change: commit `2c8409065afb624f6bf115fb0aece5095f777355`, PR [#200](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/200).

### Verification
- Build and boot-test run [#8205](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947772776): **PASS** on self-hosted runner `office`; ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Security architecture run [#7741](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947772767): **PASS**.
- This verifies one truncate rollback/remount case under the mock block device; it does not establish physical-device power-loss durability.


## 2026-10-23 — ProsenjitFS mkdir commit-write failure across remount

### Regression added
- Added a journal commit-record write-failure test for `vfs_mkdir`, which updates multiple metadata images.
- The test requires the operation to fail with `PROS_VFS_EIO` and the mount to become read-only, simulates power loss, remounts, verifies the new directory is absent, and runs the report-only integrity scan.
- Change: commit `af7cbf0187aa717d483d146878051bd5c2cf584a`, PR [#201](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/201).

### Verification
- Build and boot-test run [#8211](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948217344): **PASS** on self-hosted runner `office`; ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Security architecture run [#7747](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948217395): **PASS**.
- This verifies one mkdir commit-write rollback/remount case under mock-device fault injection, not physical-device power-loss durability.


## 2026-10-23 — ProsenjitFS rmdir commit-write failure across remount

### Regression added
- Added a journal commit-record write-failure regression for `vfs_rmdir` on an existing empty directory.
- The test requires the rmdir to fail with `PROS_VFS_EIO` and the mount to become read-only, simulates power loss, remounts, verifies the original directory is still present and openable, and runs the report-only integrity scan.
- Change: commit `f137d83af1df8fdfbff9f3c21672d65827d198a9`, PR [#202](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/202).

### Verification
- Build and boot-test run [#8216](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948709380): **PASS** on self-hosted runner `office`; ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Security architecture run [#7752](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948709352): **PASS**.
- This verifies one rmdir commit-write rollback/remount case under mock-device fault injection, not physical-device power-loss durability.


## 2026-10-23 — ProsenjitFS file-create commit-write failure across remount

### Regression added
- Added a journal commit-record write-failure regression during `vfs_file_open(..., PROS_VFS_O_CREATE)` for a previously missing path.
- The test requires creation to fail with `PROS_VFS_EIO` and the mount to become read-only, simulates power loss, remounts, verifies the new file is absent, and runs the report-only integrity scan.
- Change: commit `4f25d7d711d799bac1974049dc7972e57eb52378`, PR [#203](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/203).

### Verification
- Build and boot-test run [#8221](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39949042895): **PASS** on self-hosted runner `office`; ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Security architecture run [#7757](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39949043002): **PASS**.
- This verifies one file-create commit-write rollback/remount case under mock-device fault injection, not physical-device power-loss durability.


## 2026-10-23 — ProsenjitFS integrity-scan caller-serialization audit

### Checked
- Inspected `kernel/prosenjitfs.c`, `kernel/include/prosenjitfs.h`, `docs/PROSENJITFS.md`, and the CPL3 entry path in `boot/entry.asm`.
- The integrity API explicitly requires callers to serialize scans against mutations. All in-source calls to the report/status APIs are within `prosenjitfs_selftest()`; repository search found no separate production kernel callsite.
- `pfs_io_lock()` is currently used by the append-write wrapper only. It is not a filesystem-wide lock and must not be described as making all VFS operations thread-safe.
- `boot/entry.asm` masks IF in the CPL3 interrupt frame because user-mode timer preemption is not yet scheduler-safe.

### Result / boundary
- Current callsite review found no production integrity-scan caller that violates the documented serialization precondition.
- This does **not** establish general concurrent VFS safety. When timer preemption, SMP, or concurrent production scan callers are introduced, add a reviewed operation-locking boundary and tests for lock ordering/recursion and mutation-versus-scan exclusion before enabling them.


## 2026-10-23 — ProsenjitFS recovery-test failure cleanup

### Source audit and change
- Reviewed the current `kernel/prosenjitfs.c` recovery-restart regression and its `pfs_test_crash_reset()` helper.
- Found one unexpected-failure branch in the stage-10/11 recovery interruption test that cleared `fs` directly with `pfs_zero(&fs, sizeof(fs))`. That bypassed the helper's volatile-device reset, mount deactivation, and transaction-buffer cleanup.
- Changed only that branch to call `pfs_test_crash_reset(&fs, &disk)` before returning the same self-test failure. No expected assertion or runtime filesystem behavior was weakened.

### Verification
- Source change commit: `ce2d39f71aaf1f33545b67e4f3cb18c3fb3b611d`.
- Build/boot-test run [#8227](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39956134203): **PASS** on self-hosted runner `office`; ISO and native BIOS QEMU reached the required ProsenjitFS recovery and persistence markers. Security architecture run [#7763](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39956134201): **PASS**.
- Exact tested PR head: `f2df43395bbf2f07c56b6796df2818cc75aec220`. This is self-test cleanup verification, not physical-device power-loss durability.
- This is a self-test failure-path cleanup; it is not a claim of production filesystem completion or physical-device power-loss durability.


## 2026-10-24 — allocator invalid-block-size guard

- Source review found `pfs_find_free_data_block_except` and `pfs_inode_block_location` did not reject zero or sub-512 block sizes before geometry arithmetic; zero could cause division by zero. `pfs_data_block_is_free` also accepted nonzero sub-512 sizes.
- Added the supported 512–4096-byte power-of-two block-size bounds to all three helpers and self-tests for block sizes 0, 511, and 513, including unchanged inode-location output sentinels.
- Exact-head runtime CI is pending/unverified.


## 2026-10-24 — allocator geometry consistency

- Follow-up review found the primary `pfs_find_free_inode` and `pfs_find_free_data_block` helpers enforced the size range but did not reject non-power-of-two values. They now reject 513-byte geometry before allocating a bitmap buffer or computing bitmap indexes.
- Regression coverage checks both helpers return `PROSENJITFS_EINVAL` for 513 and preserve the caller's output sentinel. Exact-head CI is pending/unverified.


## 2026-10-24 — inode allocator exhaustion test

- Added a bounded test that temporarily fills all valid inode bitmap bits in the one-block test-fixture layout, verifies `pfs_find_free_inode` returns `PROSENJITFS_ENOSPC` and leaves the output sentinel plus durable/volatile/dirty mock-media CRCs unchanged, then restores the original bitmap.
- The fixture guard intentionally skips layouts with multiple inode-bitmap blocks; no broader exhaustion claim is made. Exact-head CI is pending/unverified.


## 2026-10-24 — data allocator exhaustion test

- Added `pfs_find_free_data_block` coverage for the one-block 512-byte test fixture: all valid data bits are temporarily set, the finder must return `PROSENJITFS_ENOSPC` with the result sentinel unchanged, and durable/volatile/dirty mock-media CRCs must remain stable across the search. The saved bitmap is restored afterward.
- Cross-review tightened both exhaustion tests to run only for the fixture's exact 512-byte sector/block geometry and one-block bitmap layout, avoiding accidental assumptions for larger filesystem blocks. Runner-based verification is pending.


- Cross-review caught the sibling `pfs_find_free_data_block_excluding` entry point still checking only the size range; it now rejects non-power-of-two sizes before bitmap-index arithmetic as well.


- Cross-verification hardening: inode/data exhaustion tests now preserve both durable and volatile mock-sector bytes and leave the dirty bitmap untouched by directly mutating/restoring only the isolated test fixture. The test setup no longer calls the block-write callback to restore state, which could otherwise leave a previously clean sector marked dirty.


- Added final-valid-bit boundary checks after each exhaustion assertion. The inode/data allocator must find the final valid index when that is the only free bit, while durable bytes, volatile bytes and the dirty map remain unchanged by the search. The test restores both original mock-sector views afterward.


- Expanded invalid-geometry regression coverage across all allocator entry points, including `pfs_find_free_data_block_excluding`, at sizes 0, 511, and 513. Each must return `PROSENJITFS_EINVAL` and preserve output sentinels; durable/volatile/dirty CRC snapshots verify the calls do not mutate mock media.


- Additional allocator cross-check: with only the final valid data block free, `pfs_find_free_data_block_except` and `pfs_find_free_data_block_excluding` must both return `PROSENJITFS_ENOSPC` when that block is excluded, preserve the sentinel, and keep durable/volatile media plus the dirty map unchanged. Exact-head runner results are still pending.


## 2026-10-24 — allocator cross-review checkpoint

- Re-read the complete malformed-geometry regression and verified it exercises all six relevant allocator/location helpers with block sizes 0, 511, and 513, checking error codes and unchanged output sentinels.
- Re-read the inode/data exhaustion fixtures and confirmed they are bounded to a 512-byte block and one bitmap block. They inject the synthetic bitmap directly into mock durable/volatile sectors, snapshot those views and the dirty map around read-only allocator calls, and restore both original sector views on success and handled failure paths.
- The final-valid-data-bit case also checks both exclusion-aware helpers return `PROSENJITFS_ENOSPC` when that sole free bit is excluded. Exact reviewed head: `941d079bedf686aa7a9a661f6323ce867a6a392e`.
- Verification status at this checkpoint: build/boot-test and security architecture workflows were queued, combined status had no reported checks. Runtime verification is pending; do not treat this source review as a passing run.