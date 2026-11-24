## 2026-11-14 — Unified OS architecture guide added to documentation handoff

- Added `docs/UNIFIED-OS-ARCHITECTURE.md` as a visual guide to the approved target architecture, with a Mermaid diagram and separate Native, Linux/POSIX, Windows, native-service, microkernel, driver/device, platform, and hardware layers.
- Documented the three distinct application execution flows and clarified that foreign-driver compatibility is a device-only branch, not a mandatory path for ordinary application or service requests.
- Clarified the distinction between user-space process/thread service adapters and kernel process/thread primitives; documented the kernel's authority over address spaces, capabilities, IPC, interrupts, DMA, and device access.
- Added links from `README.md`, `docs/ARCHITECTURE.md`, and this master specification. Added the guide to the required contributor reading order.
- The guide is a target-architecture explanation only. It does not assert that all depicted services, runtime layers, or driver frameworks are implemented, change the Phase 0–10 order, or authorize implementation ahead of dependency gates.
- Documentation-only change. No build, QEMU, hardware, or runtime test is claimed from the documentation edit. Exact-head required CI must run on the final PR head.

## 2026-10-31 — FAT32/exFAT provider PR #356 merged and exact-head verified

- Merged PR [#356](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/356) into `main`.
- Candidate head verified: `097086eb234b73ca71c281cbc63550c5f6a19e3e`; merge commit: `1d90a02391718bddc0e21bea478b8a334de62dc2`.
- Build/boot-test run [#9331](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41224348895) passed. QEMU serial output includes successful FatFs block adapter, FAT32/exFAT provider mount/I/O/remount, and VFS 64-bit file I/O self-test markers, along with the required native BIOS milestones.
- Security architecture run [#8826](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41224348832) passed.
- The root-volume metadata handling fix is included in the verified head. The prior FAT32/exFAT provider self-test failure no longer reproduces in this exact-head run.
- Scope limitations remain: USB Mass Storage/controller integration and physical removable-media/safe-removal testing are not included in this PR; exFAT distribution licensing review remains required before shipping. Formatting remains disabled and FAT12/FAT16 remain rejected.
- Verification is CI/QEMU and synthetic fixture evidence; it is not a claim of physical USB hardware validation or release readiness.

## 2026-10-31 — FAT provider flush-failure/unmount retry regression

- Added a deterministic mock-media flush-failure switch to the existing FAT32 provider integration fixture.
- The fixture now forces the device flush callback to fail during provider unmount and requires `PROS_VFS_EIO` while confirming the mount remains active and retains its provider association. It then clears the injected fault and requires a successful unmount retry.
- The failure flag is reset before fixture cleanup, so a failed assertion does not leave cleanup permanently blocked by the injected fault.
- This tests the provider bridge's safety contract: failed sync must abort before VFS teardown/provider release. The new check is source-level only until the exact-head build/boot workflow executes.

## 2026-10-31 — Cross-verification of current FAT provider head

- Reviewed the current PR #356 head `250e0cab69afad189c119f119b99f515ae81feac`, not an older local checkpoint.
- Cross-checked every FatFs API call in `kernel/fatfs_provider.c`: mount/probe, path lookup, directory iteration, create/remove/rename, open/read/write/truncate/close, sync, and unmount are all executed while holding the provider-wide `fatfs_enter()` gate. This is the serialization invariant required by `FF_USE_LFN=1` static LFN/ exFAT directory workspaces.
- Cross-checked VFS close failure semantics: a failed provider close leaves the VFS handle open and retryable; the FatFs provider likewise retains the live FIL in its volume registry until `f_close()` succeeds. This is consistent with the create-close orphan recovery path.
- Revalidated the FAT32 fixture's geometry and FAT16 negative fixture against the upstream FatFs classification path; the test exercises FatFs classification rather than a local handwritten BPB parser.
- Rechecked partition-local disk bounds, in-flight unbind protection, read-only disk admission, file-size ceiling checks, append-at-EOF, file rename, directory-rename rejection, open-FIL sync, and unmount-busy behavior at source level.
- Exact-head workflow state: Build/boot-test run #9307 (`41208400250`) is pending with its build job queued and no steps/logs; Security architecture run #8802 (`41208400262`) is queued with no steps/logs. No compiler, QEMU, self-test, or security-check pass is claimed.
- Remaining: dedicated exFAT synthetic fixture/runtime coverage, real USB Mass Storage transport/controller integration, physical-media and safe-removal/reinsert tests, exFAT redistribution licensing review, then exact-head CI/QEMU/security verification. PR remains Draft and unmerged.

## 2026-10-31 — Provider volume-count configuration consistency

- Removed the duplicated hard-coded provider slot count: `FATFS_VOLUME_MAX` now derives from upstream `FF_VOLUMES`.
- Added a compile-time guard for the numeric FatFs drive-string representation, which supports volume IDs 0–9 only. This prevents a future config change from silently generating invalid drive prefixes or an array/drive-count mismatch.

## 2026-10-31 — Create-close failure recovery and rollback reporting

- Cross-verification of upstream `f_close()` showed that when its internal `f_sync()` fails, FatFs leaves the FIL valid and open. The create callback previously used a stack-local FIL and could lose that live object on close failure.
- Reworked create-time FIL handling to use the provider's heap-backed open-file registry. A failed create-close is retained as an orphaned FIL; a later provider sync retries `f_sync()` and `f_close()`, then removes/frees it only after success. The orphan retains a direct volume pointer and does not depend on a parent vnode that may be released after the failed create callback.
- File and directory vnode-allocation rollback now checks `f_unlink()` results rather than silently discarding cleanup failures.
- This is source-level hardening only; CI/QEMU execution remains blocked by the offline self-hosted runner.

## 2026-10-31 — Exact-head workflow state and final source audit checkpoint

- Latest source-code commit in this checkpoint: `373b0b55253377a9c59216d9d36fb0684ff4d89b`; subsequent commits only updated `docs/DEBUG_LOG.md` and `docs/FILESYSTEM.md`.
- Build/boot-test run #9299 for the source commit is **cancelled**; its job has no steps or downloadable logs, so it provides no build result. Security architecture run #8794 remains **queued** with no executed steps/logs.
- Re-fetched the pinned upstream provenance and PR changed-file list. The upstream FatFs source remains unmodified locally; exFAT patent/distribution review is explicitly pending.
- Source-level cross-check confirms FAT12/FAT16 rejection, FAT32/EXFAT dispatch, 64-bit `FSIZE_t`/`LBA_t` configuration, read-only write admission, tracked open-FIL sync, safe unmount gating, FAT32 size-ceiling checks, file rename, and an explicit unsupported result for directory rename.
- No compile, QEMU, self-test, or physical-media result is claimed. The exact current PR head still needs a successful Build/boot-test and Security architecture run before merge consideration.

## 2026-10-31 — FAT rename and read-only open safety

- Source cross-check of upstream `f_rename()` found it does not enforce the FAT read-only attribute itself. The provider now stats the source before rename and rejects read-only objects.
- Directory rename is explicitly rejected with `PROS_VFS_ENOTSUP` for now: cached VFS directory nodes retain absolute paths, and renaming a directory without updating all descendant node paths would leave stale path references. File rename remains supported and is covered by the synthetic fixture.
- Writable file-open now checks the FAT read-only attribute before calling `f_open()`, returning `PROS_VFS_EPERM` rather than misreporting a read-only denial as no-space.
- These changes are source-level safeguards; no compiler/runtime result is claimed while the self-hosted runner is unavailable.

## 2026-10-31 — Disk-slot rollback and in-flight unbind hardening

- Cross-check found a mount rollback path that ignored a failed `pros_fatfs_disk_unbind()` when the global FatFs API gate could not be acquired. The provider now retains the allocated volume slot if unbind fails instead of recycling a potentially live physical-drive number.
- Extended the disk-adapter self-test to reject a duplicate bind and to simulate an in-flight operation, proving that unbind returns `PROS_FATFS_DISK_EBUSY` until the operation count drains.
- No runner execution is available for these changes; the new checks remain unexecuted source-level tests.

## 2026-10-31 — FAT32 maximum-file-size write boundary

- Cross-check of FatFs FAT32 write semantics found that upstream `f_write()` clips writes at the 32-bit FAT file-size ceiling. Returning a successful short/zero write at the ceiling would hide an invalid request from the VFS caller.
- Added an explicit FAT32 write-range guard for ordinary writes and append writes: reject a request if its resulting file size would exceed `UINT32_MAX`, before calling upstream FatFs. exFAT continues to use 64-bit file sizes.
- Extended the FAT32 synthetic fixture to assert that a one-byte write at offset `0xFFFFFFFF` fails with `PROS_VFS_EOVERFLOW`; existing tests also cover >4-GiB read and truncate rejection.
- Source revision: `79269482e2ba1321c89c7ca84ff31376884aaef9`. The workflow query did not yet return runs for this exact commit; no build/test result is claimed.
- Remaining implementation/verification gates are unchanged: exFAT fixture, USB MSC/controller integration, real-media and safe-removal tests, licensing review, exact-head CI/QEMU/security.

## 2026-10-31 — FAT32 append, rename, and lifecycle fixture cross-check

- Re-fetched the complete synthetic FAT32 mount fixture after the latest source updates and verified that it now covers writable-open EOF reads, FAT32 4-GiB read/truncate rejection, append-at-current-EOF behavior, file rename, directory create/remove/non-empty rejection, read-only remount, and persisted data readback.
- The open-handle sync regression now checks both sides of the contract: `f_sync()`/device flush happens while a FIL is open, and VFS refuses unmount until the handle is closed.
- Exact source revision reviewed: `7ce99660c251ed76e8a3ff56d7df096c7ab86b31`. Build/boot-test run #9290 and Security architecture run #8785 are queued; their jobs have not started and no test logs are available.
- This remains source-level cross-verification only. The synthetic fixture is not a test pass until the exact-head runner executes it. A dedicated exFAT fixture and physical USB/removal support remain incomplete.

## 2026-10-31 — Open-FIL sync and unmount lifetime cross-check
- CI precision: the latest code-bearing provider commit is `6875205364d25e398c1b3cb39be758437c2b6e83`; its Build/QEMU run `41207169570` was cancelled and Security run `41207169556` remained queued. The current PR head `d0789d4ca9341fe793c193bbddefbc12bbc2a536` is documentation-only and did not create a new build run.


- Rechecked the provider's open-file registry: successful `f_open()` publishes each live FIL under the global FatFs gate; successful `f_close()` removes it before freeing the wrapper; volume allocation/release resets the list.
- The provider sync callback now flushes every open FIL with `f_sync()` before issuing the block-device `CTRL_SYNC` barrier. It fails closed on the first file-sync error.
- The provider unmount callback refuses teardown while the live-FIL list is non-empty. The synthetic fixture explicitly requests unmount with a writable handle open, expects `PROS_VFS_EBUSY`, verifies sync caused a device flush, then closes the handle and continues.
- VFS mount teardown source inspection confirms active lookup pins and open vnode references prevent root destruction; the provider bridge preserves the mount on that busy result.
- Exact-head build/boot workflow was observed as cancelled while the self-hosted runner remained unavailable; security workflow remained queued. No CI or runtime success is claimed.

## 2026-10-31 — FAT32 boundary and sync regression coverage

- Extended the FAT32 synthetic VFS fixture to verify that reading beyond EOF returns zero bytes without extending the file, and that offsets above the FAT32 4-GiB limit are rejected with `PROS_VFS_EOVERFLOW`.
- Rechecked the VFS file-read contract: zero-length reads are handled by the VFS wrapper as a successful no-op; the provider does not call FatFs with a null buffer for a zero-length read.
- Rechecked upstream `FSIZE_t` and `LBA_t` declarations: with the selected `FF_FS_EXFAT=1` and `FF_LBA64=1` configuration both are 64-bit. Provider callbacks separately enforce FAT32's 32-bit file-size limit.
- Rechecked FatFs `f_sync()` and `f_close()`: a close that fails during sync leaves the FIL valid for retry; successful close invalidates the FIL and removes its tracked handle.
- Latest code revision reviewed: `57060385861aa33538bac3b12b9f6076573cd650`. Exact-head Build/boot-test run #9287 and Security architecture run #8782 are queued with their jobs still queued; no build or test steps/logs exist yet.
- No CI result is claimed. The PR remains Draft. Outstanding: dedicated exFAT fixture, USB Mass Storage/controller integration, physical-media and safe-removal tests, licensing review, and exact-head CI/QEMU/security completion.

## 2026-10-31 — FatFs open-handle sync and lifecycle cross-check

- Cross-checked the provider sync contract against `pros_vfs_provider_unmount_device()`: sync runs before VFS draining, and unmount must remain busy while an open handle retains the root vnode.
- Found that a block-device flush alone did not explicitly synchronize modified FatFs `FIL` metadata. Added a per-volume open-`FIL` list; successful opens publish it under the FatFs API gate, closes remove it under the same gate, and provider sync calls upstream `f_sync()` for every live handle before the final `CTRL_SYNC` barrier.
- Added a fixture assertion that provider unmount returns `PROS_VFS_EBUSY` while a file handle is open and that the sync path increments the mock media flush count before returning busy.
- Added a defensive unmount check that rejects teardown if the volume still has open handles or its mount/slot association is stale.
- Re-fetched the changed provider source and checked the upstream FatFs `f_sync()`/`f_close()` behavior. Source inspection only; no compiler or runtime test was executed.
- Current branch head: `e43e484b52b92f0c830f9834f274a4d0084babbd`. No workflow run is currently associated with this exact head in the workflow-run query; earlier queued runs belong to older SHAs and are not evidence for this head.
- Remaining major gaps: dedicated synthetic exFAT image/operations fixture, physical USB Mass Storage transport/controller and device registration, real pendrive/external-disk tests, safe media-removal/reinsert handling, exFAT redistribution licensing review, and exact-head CI/QEMU/security verification.

## 2026-10-31 — EOF size refresh and append regression coverage

- On an EOF read, the provider now refreshes the vnode's cached size from the live FatFs file object before returning zero bytes; this prevents stale vnode size after another handle changes the file.
- Extended the synthetic FAT32 integration fixture to reopen a truncated file with `O_APPEND`, deliberately seek the handle to offset zero, append the remaining payload, and verify the authoritative file size and persisted full contents after read-only remount.
- These are source-level changes and fixture additions only. Build/boot/security workflows have not yet executed because the self-hosted runner is offline.

## 2026-10-31 — FAT32 4 GiB and read-past-EOF cross-verification

- Cross-checked the provider's 64-bit callbacks against the exact pinned FatFs R0.16 source, including `f_lseek()`, `f_read()`, `f_write()`, and the `FSIZE_t` configuration.
- Found that FatFs silently clips non-exFAT seeks at the 4 GiB boundary. Added provider-side `PROS_VFS_EOVERFLOW` guards for FAT32 read/write offsets and truncate sizes rather than allowing a redirected operation.
- Added synthetic FAT32 regression coverage for >4 GiB read offsets and truncate requests.
- Found that upstream `f_lseek()` can allocate/extend a writable file when seeking beyond EOF. The provider now returns EOF before calling `f_lseek()` when a read offset is at or beyond the open file's current size. Added a regression check that a read beyond EOF on a writable empty file leaves its vnode size unchanged.
- Rechecked the provider's read-only remount, FAT16 classification fixture, directory creation/non-empty rmdir fixture, disk adapter bounds and cleanup, and VFS mount drain behavior by source inspection.
- These changes are committed to the feature branch, but the self-hosted runner is offline; no compiler, QEMU, or runtime pass is claimed. Exact-head build/boot and security workflows remain queued.
- Remaining engineering gaps include a dedicated exFAT integration fixture, USB Mass Storage host/controller integration, physical media/removal tests, and exFAT redistribution licensing review.

## 2026-10-31 — FAT directory semantics and kernel-stack footprint

- Added synthetic FAT32 coverage for directory creation, child-file creation, rejection of non-empty directory removal as `PROS_VFS_ENOTEMPTY`, and successful child removal followed by directory removal.
- FAT read-only attributes are now explicitly enforced for unlink/rmdir; non-empty-directory and read-only-attribute failures map to distinct VFS errors.
- Switched FatFs long-filename workspaces from per-call stack allocation (`FF_USE_LFN=2`) to static workspaces (`FF_USE_LFN=1`). This is safe only because every provider FatFs API transaction is serialized by the global `fatfs_enter()` gate; the disk self-test does not call FatFs path APIs.
- Hardened the disk-adapter self-test cleanup so failure paths attempt to release its temporary physical-drive slot instead of contaminating later tests.
- Exact-head CI is still queued on the self-hosted runner. None of the new source/test changes has been compiled or executed in this offline window.

## 2026-10-31 — FAT16 rejection and append-boundary regression coverage

- Added a synthetic, structurally plausible FAT16 geometry to the provider probe self-test. It must be rejected by the provider after upstream FatFs classification; no local FAT BPB parser was introduced.
- Extended the mock media read map for the FAT16 fixture's FAT copy.
- Hardened append writes against EOF-plus-length overflow while holding the FatFs API gate; zero-length reads now follow the VFS zero-length buffer contract.
- Runtime/build status remains pending because the local runner is offline. These are source changes and test additions, not verified test results.

## 2026-10-31 — FAT32/exFAT provider source hardening (runner offline)

- Continued source-level implementation on PR #356 while the self-hosted runner was offline; no compiler, QEMU, or CI pass is claimed.
- Hardened the FatFs VFS file callbacks: read/write/truncate now independently enforce the open-handle access mode, read/write offsets reject 64-bit addition overflow, and open rejects invalid access/flag combinations.
- Extended the synthetic FAT32 provider fixture to assert that a read-only remount rejects a write-open with `PROS_VFS_EPERM` before allowing persisted data to be read.
- Reconciled the filesystem status document: the FAT32 provider and synthetic fixture exist, while exFAT runtime fixture/verification and physical USB Mass Storage/media-removal integration remain outstanding.
- The implementation continues to use pinned upstream ChaN FatFs R0.16; no handwritten FAT32/exFAT parser or formatter was added.
- **Pending:** exact-head build/QEMU/security workflows, a dedicated synthetic exFAT fixture, USB Mass Storage/controller integration, physical media tests, safe-removal/reinsert lifecycle, and exFAT redistribution licensing review.

## 2026-10-25 — Owner confirmed filesystem architecture

- Owner confirmed that native ProsenjitFS is the only filesystem implementation intended to live in the kernel core.
- FAT32, exFAT, NTFS and future formats will be separate filesystem drivers/providers exposed through the common VFS interface, following the general multi-filesystem model used by mainstream operating systems.
- The earlier EXT4 boot + read/write proposal is not the selected first-release baseline and is not a kernel-core requirement. EXT4 may be considered later as an optional compatibility provider if explicitly prioritized.
- Provider execution model (loadable kernel module versus isolated userspace service) remains a separate technical decision; do not claim userspace isolation unless implemented and tested.
- Canonical updated plan: [docs/FILESYSTEM.md](FILESYSTEM.md). Roadmap updated to match.

## 2026-10-25 — Filesystem plan reconciliation audit

- Consolidated the former filesystem integration, NTFS status, encryption design, and permissions/encryption documents into `docs/FILESYSTEM.md`; their old paths are now navigation stubs to prevent parallel status sources.
- Audited the current source/build tree against those documents. `kernel/vfs_provider.c` and `include/vfs_provider.h` are only early registry scaffolding (probe/mount/unmount); `kernel/vfs.c` currently recognizes only native ProsenjitFS. A complete userspace provider process/IPC and compatibility provider operation path are not evidenced.
- The former NTFS write-status claims do not match the current branch source/build tree: no NTFS implementation source, filesystem enum, or NTFS object is present. NTFS, FAT32 and exFAT must remain planned, not advertised as supported.
- ATA PIO + MBR/partition + ProsenjitFS QEMU persistence does not prove USB mass-storage discovery or physical removable-media support.
- Permission enforcement and denial-without-mutation tests remain explicit release gates. Encryption remains deferred until its full reviewed design, key management, authenticated I/O, journal/recovery integration, and exact-head tests are complete.
- Initially identified a conflict between earlier EXT4 boot + read/write planning and the current ProsenjitFS implementation; the owner subsequently resolved it. EXT4 is not the first-release baseline and is not to be added to the native kernel core.
- This was a documentation/source-tree audit only. No kernel code changed and no filesystem runtime tests were run. An empty commit-status response is not a CI pass.

Authoritative plan/status and preserved historical notes: [docs/FILESYSTEM.md](FILESYSTEM.md).

## 2026-10-25 — ProsenjitFS Part 2 verification summary

- Verified source head: `70ed44cd45987682ac50f9fbfd1b82a510e25033`; PR #249 remains open and unmerged.
- **PASS:** [Build/boot-test #8548](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283756) and [Security architecture #8084](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283791) on that exact source head.
- The build log contains ProsenjitFS format/mount/allocation/journal/recovery success markers in both ISO and native BIOS runs, plus LBA-boundary and ATA PIO → MBR → ProsenjitFS persistent-mount success markers.
- Source changes: checksum-valid inode admission, directory-index owner identity, and child-reference validation in path lookup and readdir. Corruption fixtures restore original metadata and finish with integrity checks.
- The v1 disk format is unchanged. Broader P2/P3/P4 fault-injection and corruption families remain incomplete; no physical-device durability or general concurrency claim.
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
- Inspected `kernel/prosenjitfs.c`, `kernel/include/prosenjitfs.h`, `docs/FILESYSTEM.md`, and the CPL3 entry path in `boot/entry.asm`.
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


## 2026-10-23 — Namespace integrity and rename read-failure tests under review

- PR #211 adds self-tests for duplicate directory inode references and duplicate valid directory names. The corruption is crafted with valid checksums/hash placement so the integrity scan must detect the namespace violation itself; the tests check report-only behavior leaves durable, volatile, and dirty mock media unchanged and that restoring the entry yields a clean scan.
- PR #212 adds a bounded `fail_read_countdown` sweep for `vfs_rename`. Each injected read failure must not publish the destination or mutate durable/volatile/dirty mock-media views; the original payload must remain readable, the mount writable, and integrity clean. The first countdown not reached terminates the sweep.
- Exact PR #211 head `28446ed40aeb748e8e0a5b3172a3e86f799f4567` passed Build/boot-test [#39980888372](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39980888372) and Security architecture [#39980888146](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39980888146); PR #211 merged as `e8a27899d4fe6de72a6e0959fa9d04659ca34eec`. PR #212 fixed head `1204deed4e8899448422e5cc7702eca776de3c6f` passed Build/boot-test [#40072397646](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397646) and Security architecture [#40072397702](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397702); PR #212 merged as `b6b6eaf409c9407a822b0a6016147087edb6c365`.
- Mock-device regression coverage only; physical hardware durability and general concurrent VFS safety remain separate acceptance gates.


## 2026-10-24 — Rename read-failure sweep leaked its final fault injection

### Diagnosis
- Build/boot run [#39981319506](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39981319506) built the ISO but failed its ISO QEMU gate because the ProsenjitFS boot self-test stopped at source line 10605 (selftest_stage=0x296D, failure code -21).
- Source inspection found that when the selected read position was beyond the reads performed by vfs_rename, the test immediately ran the integrity scan and unmount while disk.fail_read_countdown was still armed. The unrelated follow-up integrity/unmount reads could consume that fault and falsely fail the sweep.
- This is a test fault-injection lifetime bug, not a missing-runner failure. The Security architecture run [#39981319411](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39981319411) passed.

### Fix
- Disarm disk.fail_read_countdown once the sweep proves the requested position was not reached by vfs_rename, before running integrity scan and unmount.
- Preserve all injected-failure assertions; the fault remains active during the rename operation itself.
- Source fix commit: a7a7a5b7eaca35dfdf91788d44e37daef433f2a0.

### Verification status
- **Verified:** fixed exact PR head `1204deed4e8899448422e5cc7702eca776de3c6f` passed Build/boot-test [#40072397646](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397646), including ISO QEMU and native BIOS QEMU, and Security architecture [#40072397702](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397702). PR #212 merged as `b6b6eaf409c9407a822b0a6016147087edb6c365`.


## 2026-10-24 — Rename write-failure sweep (verified)

### Change
- Added a generic one-shot block-write countdown to the ProsenjitFS mock device. It fails the selected future block write before accepting bytes, without changing the existing targeted journal-commit failure hook.
- Added a bounded write-position sweep around `vfs_rename`. For every reached write failure, the test simulates power loss and remounts, then requires exactly one of the old/new paths to exist, the original payload to remain intact, and the report-only integrity scan to pass.
- The first countdown not reached proves the tested rename write sequence ended; the countdown is disarmed before follow-up integrity/unmount operations.
- No assertions are weakened and no runtime success is claimed before CI.

### Verification status
- Source commit: `68b04cad273d9521c271942ae828926b0bcb3ab6`.
- Exact PR head `c897fa782f805778b0de051ca2b338a39be7d7ce` passed Build/boot-test [#40072964418](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072964418), including ISO QEMU and native BIOS QEMU, and Security architecture [#40072964417](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072964417). PR #222 merged as `2b14a2b67e7c3543e95140af033f603410c66bc5`.
- Mock-device tests do not establish physical-device power-loss durability or general concurrent VFS safety.


## 2026-10-24 — Unlink read-failure sweep (pending CI)

- Added a bounded mock-device read-failure countdown sweep around `vfs_unlink`.
- Each reached read failure must propagate as `PROS_VFS_EIO`, leave durable/volatile/dirty media unchanged, keep the mount writable, preserve the original payload, and leave the integrity scan clean.
- The first countdown not reached confirms the read sequence ended; the injection is disarmed before the follow-up lookup, integrity scan, and unmount.
- Source commit: `126a4786dbd9b0427625149cee3999efd4c6dbba`.
- Initial CI exposed an error-classification bug: `pfs_vnode_unlink()` converted any `pfs_read_extent()` failure to `PROSENJITFS_ECORRUPT`, hiding device I/O errors during extent reclamation. The fix now propagates the exact extent-read error through transaction cleanup. Source fix commit: `ab3c659d4784f82fa83b87f54e7e1d84122d8dd6`.
- **Verified:** fixed exact PR head `895ee35f7a2413e569b2d42cbc52ca0efc580928` passed Build/boot-test [#40073371338](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073371338), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073371343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073371343). PR #223 merged as `30acc248df1258fa6ac65a6b5eb8e8ef47e2ceb5`.


## 2026-10-24 — Unlink write-failure sweep (pending CI)

- Added a bounded mock-device block-write countdown sweep around `vfs_unlink`.
- For every reached write failure, the test simulates power loss and remount. Either the old file remains with byte-identical payload, or the committed unlink is visible; in both cases the report-only integrity scan must pass.
- The first countdown not reached proves the unlink write sequence ended; the fault is disarmed before lookup/integrity/unmount.
- Source commit: `a18e2a55b36c6a445657bb9786dd172fb0eaf95c`.
- **Verified:** exact PR head `c536d9e03f2e21ee559d73ecb1d1a4d92e30a74b` passed Build/boot-test [#40073592946](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073592946), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073592829](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073592829). PR #224 merged as `714fa8e66d3a46991cb80446d769ab4538a63120`.


## 2026-10-24 — rmdir cycle error classification (pending CI)

- Source audit found the parent-directory bucket traversal in `pfs_vnode_rmdir()` returned `PROS_VFS_EIO` when its bounded step count detected a cyclic directory-entry chain. This is malformed on-disk metadata, not a device I/O failure.
- Changed the internal result to `PROSENJITFS_ECORRUPT` and added a checksum-valid self-cycle regression. The test requires `PROS_VFS_ECORRUPT`, unchanged durable/volatile/dirty media during the operation, then restores the entry and requires a clean integrity scan and successful normal rmdir.
- **Verified:** fixed exact PR head `5a2dd5d63813498b0bd83f2aa4b6fed1f69e8e2a` passed Build/boot-test [#40073950609](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073950609), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073950625](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073950625). PR #225 merged as `0cdaab0ca75c286e839494d37c24420f74a5c039`.


## 2026-10-24 — rmdir read-failure sweep (pending CI)

- Added a bounded read-failure sweep around `vfs_rmdir`.
- Every reached read fault must return `PROS_VFS_EIO`, preserve durable/volatile/dirty media CRCs, keep the mount writable, preserve the directory, and pass the report-only integrity scan.
- The first not-reached countdown is disarmed before follow-up lookup/integrity/unmount.
- Source commit: `aefcac64b5ee80cf58c62c04e0a312d8878ab8dd`.
- **Verified:** exact PR head `5522c822d9bce6d01ca6c0c06b572c8032e63c5e` passed Build/boot-test [#40074188166](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40074188166), including ISO QEMU and native BIOS QEMU, and Security architecture [#40074188142](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40074188142). PR #226 merged as `84a707c354065d6f1c9aa0f418cf043f6d55bdd3`.


## 2026-10-24 — truncate write-failure sweep verified

- Added a bounded sweep of block-write failures through `vfs_file_truncate`; every reached failure triggers simulated power-loss reset and remount.
- The regression rejects intermediate EOF, altered original payload, or integrity-scan failures. It accepts only unchanged original content or fully truncated content.
- Exact tested PR head `2586aa0403d5c984cb64b962c5bfdd1bf360cfe4` passed Build/boot-test [#40077700893](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40077700893) and Security architecture [#40077700948](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40077700948); both ISO QEMU and native BIOS QEMU steps passed. PR #228 merged as `e6c391ae009b94dd8645140cfe86a571167ff54f`.
- This verifies the mock-device fault model only, not physical-device power-loss durability.


## 2026-10-24 — Part 1 repository audit and PR #233 failure mapping

- Audited `main` at `cfe04a8520ec917a90b284bbc726a8bd0cc83a2d`, the block/VFS/partition/ATA storage interfaces, version-1 format definitions, ProsenjitFS implementation, current design notes, build script and workflow.
- Main Build/boot-test [#8426](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40083299324) and Security architecture [#7962](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40083299325) both passed at that baseline.
- Inspected PR #233 head `74b8cedf5b96055823b9a9a5d4a963b8d2f1a3b0` and its failed Build/boot-test [#40112968528](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40112968528). QEMU reported `selftest_stage=0x28E1` and `selftest_rc=-27`. Stage `0x28E1` maps to `kernel/prosenjitfs.c:10465`, where one compound condition checks the expected `PROS_VFS_EIO`, durable-media CRC, volatile-media CRC, dirty-map CRC, and filesystem/mount writable state after an injected read error.
- The log did not identify which predicate failed. Source inspection found a concrete error-contract defect consistent with the operation-status predicate: `pfs_make_vnode()` returned `PROSENJITFS_EIO` (`-2`) directly into VFS, where `-2` means `PROS_VFS_ENOSYS`. PR #233 now keeps helper errors in the filesystem namespace and translates them at VFS callback boundaries. It also splits the compound invariant check into predicate-specific diagnostic codes. Current PR #233 head is `84d2b84948065011cf0c94291e8bcaf5d9f7079c`; exact-head Build/boot-test and Security architecture are queued, so this diagnosis is source-backed but not yet runtime-verified.
- Part 1 documentation lives in the existing flat `docs/` convention rather than a parallel `docs/prosenjitfs/` tree: the completion matrix is the current status report, this design document carries the verified architecture baseline, and separate implementation-plan/test-matrix documents track remaining acceptance work.
- No local compiler or emulator was run from this session and no real disk was formatted. GitHub Actions did run the failed PR on self-hosted runner `home`; the observed issue is a runtime self-test failure, not a proven runner outage.


## 2026-10-24 — Append/truncate read-failure sweep: error mapping fix verified on PR head

- The original PR #233 head `74b8cedf5b96055823b9a9a5d4a963b8d2f1a3b0` failed Build/boot-test #40112968528 at stage `0x28E1` / line 10465. The assertion checked the operation result, durable/volatile/dirty mock-media CRCs, and writable mount state.
- Source inspection found that `pfs_make_vnode()` returned ProsenjitFS error values directly. Both `PROSENJITFS_EIO` and `PROS_VFS_ENOSYS` are numerically `-2`, so a read failure could be misclassified at the VFS boundary.
- Fix: `pfs_make_vnode()` keeps filesystem-native error codes; filesystem callbacks translate through `pfs_vfs_error()`. The append-sweep invariant was split into individual diagnostic codes for operation status, each mock-media CRC, and filesystem/mount read-only state.
- Verified exact PR head `84d2b84948065011cf0c94291e8bcaf5d9f7079c`: [Build/boot-test #40116501666](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40116501666) passed and [Security architecture #40116501659](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40116501659) passed. Logs contain the full ProsenjitFS format/mount/allocation/journal/recovery marker plus the native ATA PIO -> MBR -> ProsenjitFS persistent-mount marker. Both ISO QEMU and native BIOS QEMU completed the workflow's required markers before expected timeout termination.
- PR #233 merged into `main` as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c` after both exact-head checks passed. Post-merge main Build/boot-test #40116980733 and Security #40116980685 are pending. This is deterministic mock-device/QEMU evidence; it does not establish physical-device power-loss durability or general concurrent VFS safety.


## 2026-10-25 — committed home-block flush failure recovery verified

- PR #239 merged as `1b87449daf9555d15723dea284d3725b87e354d0`.
- Exact PR head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf` passed Build/boot-test [#40156447788](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447788) and Security architecture [#40156447759](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447759). Build steps passed ISO build, ProsenjitFS block-init diagnostic, ISO QEMU and native BIOS QEMU.
- FS-18 injects a flush failure after the commit record is durable while home blocks are being persisted. It requires zero bytes reported, the current mount to freeze read-only, and simulated power-loss remount to replay the complete new payload with a clean integrity scan.
- Evidence is deterministic mock-device/QEMU coverage only; physical-device durability and general concurrent VFS safety remain unverified.

## 2026-10-25 — allocator geometry hardening ported to the latest main source

- Re-read the ProsenjitFS implementation plan, completion matrix, test matrix, architecture contract, version-1 format header, the real provider (`kernel/prosenjitfs.c`), block-device interface and VFS contract before editing.
- Confirmed the v1 filesystem core already includes superblock/version validation, persistent bitmaps, inode/extent metadata, hash-indexed directories, VFS file/namespace callbacks, redo journal/recovery, and report-only integrity validation. No replacement filesystem or disk-format migration was warranted.
- Ported the block-geometry fix from PR #237 onto the newer source containing merged FS-18. A shared `pfs_block_size_valid()` now guards filesystem block geometry before bitmap/index divisions, inode-location/read/write offsets, preallocation, integrity scanning, and vnode read/write/truncate arithmetic.
- Retained and ported the regressions for invalid sizes `0, 511, 513, 4097, 8192`, result-sentinel and mock-media invariance, bounded inode/data bitmap exhaustion, and final valid inode/data-block bits.
- Reconciled the overlapping rmdir sweep against the already-merged home-flush test: setup failure now resets simulated filesystem state, recovered child directory parent must be root, and a still-present recovered directory is removed only after a successful integrity check; integrity is checked again before unmount.
- Original PR #237 exact head `3d76a1fdbc2a58518bc914392caf5c846be04ce2` passed Build/boot-test [#40119726528](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119726528) and Security [#40119726499](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119726499). That verifies the original source branch, **not** the integration commit.
- Integration source commit: `f533f1a3f8c2a6dd2c0f6891c8323c68562b4fd1`. Exact-head Build/boot-test and Security workflows must run on this integration branch before its changes are called verified. No local compiler/QEMU was available in this tool session; no compiler output or runtime result is fabricated.
- Existing FS-18 home-block flush coverage passed on exact PR #239 head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf`: Build #40156447788, Security #40156447759; merged as `1b87449daf9555d15723dea284d3725b87e354d0`. Mock-device/QEMU evidence only, not physical power-loss certification.

## 2026-10-25 — allocator bitmap read-failure coverage added

- Added deterministic mock-device tests for `pfs_find_free_inode`, `prosenjitfs_alloc_inode`, `pfs_find_free_data_block`, `pfs_find_free_data_block_except`, `pfs_find_free_data_block_excluding`, and `pfs_data_block_is_free`.
- Every call gets an independent one-shot read fault. The test requires `PROSENJITFS_EIO`, untouched output sentinels for output-producing finders, fault consumption, writable filesystem/mount, and unchanged durable/volatile/dirty media CRCs. Integrity is rechecked after all injected read faults.
- Source commit: `c7218d60a8b5b499f2baeec4170c8642c6c11f55`. The exact integration PR head has not yet passed Build/boot-test and Security architecture; this regression remains pending until those exact-head runs complete.
- No disk format or runtime code behavior was changed by these test-only additions. No local compiler/QEMU run is claimed.

## 2026-10-25 — VFS total path-length guard and namespace edge tests

- Inspected `vfs_path_lookup()`, `vfs_next_component()`, the VFS name/path bounds, fake provider lookup callbacks, and the existing `vfs_vnode_api_selftest()` before editing.
- `vfs_path_lookup()` now checks the full NUL-terminated path against `PROS_VFS_PATH_MAX` before taking a vnode reference or walking components. This bounds scans made entirely of repeated separators; strings whose length reaches the maximum return `PROS_VFS_EOVERFLOW`.
- Added self-tests for repeated separators plus `.`, root-clamped `..`, empty input, exactly-over-limit path made from slashes, missing intermediate components, traversal through a file, and result pointer preservation on errors.
- Code was source-edited and mechanically checked for all intended guard/test anchors, but no compiler or runtime execution has been run locally. These additions require exact final-head Build/boot-test and Security CI; no pass is claimed.

## 2026-10-25 — build found rmdir test hunk in wrong scope; corrected

- Queried exact PR #242 workflow logs rather than relying only on the status badge. Build/boot-test #40159887034 completed desktop/DML unit tests successfully, then failed at ISO compilation: `kernel/prosenjitfs.c:10624:18: error: 'found_lba' undeclared`; ISO/native QEMU steps were skipped.
- Root cause: while porting PR #237, a cleanup hunk sharing a generic integrity/unmount anchor was applied to the earlier unlink read-failure sweep instead of the intended rmdir write-failure sweep.
- Removed the misplaced rmdir cleanup from the unlink block; added it only to the rmdir write-sweep block, where `found_lba` belongs and the condition means remove a child directory that survived recovery. Kept the recovered parent-inode/root invariant and test-fixture crash reset.
- Source-scope guards passed by inspection of the resulting GitHub file content; this is not compiler verification. Fresh exact-head Build/boot-test and Security runs are required.


## 2026-10-25 — removed temporary block-init tracing

- Removed the one-off `block_device_init()` entry/exit stack/register trace, its cross-layer dependency on `pfs_selftest_diag_stage`, and the forced `O0`/`noinline` attributes from `kernel/block.c`.
- Kept the generic ProsenjitFS self-test checkpoints immediately before and after block-device initialization (`0xF004` and `0xF005`) so high-level failure localization remains available.
- This is a cleanup of diagnostic instrumentation only; the exact-head Build/boot-test and Security architecture workflows must pass before treating it as verified.


## 2026-10-25 — FS-28 truncate-shrink tail isolation

- Assigned FS-28 to this truncate-shrink bug; FS-27 is already used for duplicate-target journal recovery.

- Audited `kernel/prosenjitfs.c` with the implementation plan, completion matrix, test matrix, format header and block-device contract at main head `d6e4f0998cbaa57d7234803011e3bbe21680b241`.
- Found that shrink truncation preserved bytes after a partial-block EOF. Since the write path overlays writes into an existing mapped block, a later sparse-offset write could extend the file and expose those stale bytes in the gap.
- Changed shrink truncation to stage the zeroed final-block suffix, extent/bitmap changes and new inode size through the existing redo journal transaction; the v1 layout is unchanged.
- Added a deterministic self-test for truncate-to-100, write-at-200, and zero-gap verification through VFS.
- Verification pending: exact-head Build/boot-test (ISO + native BIOS QEMU) and Security architecture workflows must run on the branch. No compiler, runtime or CI success is claimed here.


## 2026-10-25 — FS-29 inode allocation output contract

- Audit found `prosenjitfs_alloc_inode()` wrote its candidate inode number before the inode-bitmap redo transaction succeeded, so an I/O failure could return an error while modifying the caller's output.
- Moved output publication after successful bitmap transaction completion.
- Added a pre-commit journal-write fault regression requiring EIO, sentinel preservation, unchanged durable/volatile/dirty mock-media checksums, a writable mount and a passing integrity scan.
- Exact-head Build/boot-test and Security architecture CI remain pending; no runtime success is claimed.


## 2026-10-25 — verification result for FS-28 / FS-29

- Code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f` passed [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332).
- Build job completed toolchain verification, DML unit tests, ISO build, ISO QEMU boot, and native BIOS QEMU boot.
- Captured logs include the ProsenjitFS format/mount/allocation/journal/recovery self-test marker, native filesystem LBA-boundary diagnostic, and ATA PIO -> MBR -> ProsenjitFS persistent-mount marker.
- These results verify the code/test head; this follow-up only updates documentation. Physical-device power-loss certification and general concurrent mutation remain outside the evidence.


## 2026-10-25 — FS-30 journal-retirement error propagation

- Found `pfs_journal_clear()` returned undocumented values `-101..-106`, including for failed journal-block writes and flushes. Recovery/mount could therefore lose the block layer's intended `PROSENJITFS_EIO` classification.
- Replaced magic returns with named filesystem validation/corruption statuses and propagated the original LBA/write/flush result.
- Added a mount-level regression for the journal-clear write and flush failure after a durable stage-4 uncommitted transaction. Both failures must return EIO; retries must discard the uncommitted journal and retain the old file payload.
- Exact-head Build/boot-test and Security architecture CI are pending; no runtime result is claimed yet.


## 2026-10-25 — FS-30 verification and FS-31 replay-write regression

- FS-30 code/test head `5aa0569a24093b11aee7685c7f3f136f02aaaded` passed [Build/boot-test #8590](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503632) and [Security architecture #8126](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503601). The log contains four ProsenjitFS self-test markers and two each of the LBA-boundary and ATA/MBR persistent-mount milestones, covering ISO and native BIOS runs.
- Added FS-31: committed unlink interrupted after commit flush, second replay home-write fails, power loss discards the partial volatile replay, next mount must replay the entire transaction and pass integrity.
- FS-31 exact-head Build/boot-test and Security verification are pending; prior FS-30 results are not presented as verification of the new source/test head.


## 2026-10-25 — FS-31 exact-head verification

- Code/test head `73bff7fe8aad278e04f08ee7594b901bb11da6f9` passed [Build/boot-test #8593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866752) and [Security architecture #8129](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866715).
- The Build job completed toolchain verification, DML tests, ISO compilation, block-init diagnostic, ISO QEMU boot and native BIOS QEMU boot.
- Captured logs contain four ProsenjitFS format/mount/allocation/journal/recovery self-test markers, two native LBA-boundary diagnostic markers and two ATA PIO -> MBR -> ProsenjitFS persistent-mount markers; no compiler `error:` entries.
- FS-31's injected second home-block write failure during committed journal replay returned EIO; after simulated power loss, retry mount completed the committed unlink and integrity checks passed in the self-test.
- This is deterministic mock-device/QEMU evidence only; physical-device power-loss certification and general concurrent-mutation safety remain unverified.

## 2026-10-27 — Workflow backlog audit (evening runs)

- Audited the available exact-commit workflow records for the recent Security Control Center work. Current known PR #269 head from the prior handoff is `a380f268d691d04bbf54cc5c4c0db9d84ae79f98`.
- **PASS (limited scope):** Security architecture run [#8298 / run 40484817442](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40484817442) completed successfully for the queried commit. Its steps were checkout and the security architecture contract validation; this is not a kernel build or runtime test.
- **CANCELLED / not verified:** Build and boot-test run [#8762 / run 40484817506](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40484817506) was cancelled. The available job summary contains no executed steps, so it does not establish a compile, ISO boot, native BIOS boot, or filesystem test result. Do not classify this as a source-code failure without a failure log.
- Historical comparison only: commit `e2102a018b166d4bfd3c9819de6190effe5c652b` has successful Build/boot-test run [#8687 / 40338325240](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40338325240) and Security architecture run [#8223 / 40338325191](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40338325191). These results apply only to that older commit and must not be carried forward as proof for PR #269 or newer heads.
- **Action:** the cancelled build needs a fresh run on the exact current PR head once a runner is available. No automatic rerun was triggered because the run was cancelled rather than failed, and the reported local self-hosted runner is offline. Recheck queued/pending runs and runner availability before treating any current head as verified.
- **Native ProsenjitFS status remains gated:** no new evidence from this audit establishes scheduler-aware mutex/block-wakeup correctness, general VFS namespace concurrency, physical-device power-loss durability, or full persistence/crash-recovery certification. Keep those items open/NOT TESTED until exact-head build/QEMU and relevant regression evidence exists.


## 2026-10-27 — rerun exposed SCC protocol-test fixture failure

- Reran the previously cancelled Build/boot-test job [#8762 / run 40484817506](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40484817506). The rerun executed on self-hosted runner `home`; toolchain check and desktop manager tests passed, but `make security-protocol-test` failed at `tests/security_control_center_protocol_test.c:164`. ISO build and both QEMU boot tests were skipped because the test command exited first.
- Failure cause from source/log review: the test deliberately set the response header's flags field to 1 to verify `SCC_PARSE_BAD_FLAGS`, then reused the same buffer for a positive GET_STATUS payload assertion without resetting the response header. The positive assertion therefore still carried invalid flags; this is a test-fixture setup defect, not evidence that the parser's valid-frame path is broken.
- Fix committed to PR #269 branch `security/scc-u64-timestamp-get-event-guard` as `8ab6d01727e13e0dca11e3468f13c3a5ee59e47f`: reinitialize a valid GET_STATUS response header before writing the payload fields. The updated commit has not yet produced a confirmed workflow result; do not mark the fix passed until exact-head tests run.
- Next verification gate: rerun desktop/DML and SCC protocol tests on the new head, then require ISO build, ProsenjitFS block-init diagnostic, ISO QEMU boot, and native BIOS QEMU boot before claiming build/boot success. Filesystem concurrency and physical-device durability remain separate unverified gates.


## 2026-10-31 — FAT32/exFAT removable-media workstream opened

- Owner clarified that the required compatibility formats are **FAT32 and exFAT only**; FAT16 is not a target. Scope is USB pendrives and external HDD/SSD, not the native/root filesystem.
- Located the existing canonical specification: docs/FILESYSTEM.md. It already defines FAT32 + exFAT as ProsenjitOS 1.0 external providers and requires an end-to-end USB removable-media path. Updated that canonical document with REM-01 rather than creating a duplicate plan/status document.
- Audited include/vfs_provider.h and kernel/vfs_provider.c: current registry exposes only probe/mount/unmount and is not yet a complete VFS filesystem-provider runtime.
- Audited kernel/include/block.h: existing block API has sector size/count and read/write/flush callbacks suitable for a disk-I/O adapter. Partition-scoped integration and USB transport still require verification/implementation.
- Identified ChaN FatFs as the leading candidate for evaluation because upstream documents FAT/exFAT support and a platform-independent disk I/O interface. Linux VFAT/exFAT implementations will be used as behavioral references, not copied blindly.
- No third-party code was imported in this checkpoint. Before vendoring, pin the release, review license/notices and exFAT patent/distribution implications, configure exFAT + LFN/Unicode + required large-LBA support, and ensure FAT16 is rejected by our provider.
- Created branch feature/fat32-exfat-removable-provider from main.
- **Checkpoint status:** audit and implementation sequencing recorded; no runtime FAT32/exFAT support, USB mass-storage support, or physical-drive test is claimed yet.
- **Next:** design the VFS operation bridge and provider mount lifetime before integrating the external library.


## 2026-10-31 — repository-wide implementation guidelines added

- Audited the repository root and documentation index; no existing root `AGENTS.md`, `CONTRIBUTING.md`, or equivalent single contributor/agent rulebook was found.
- Reviewed the project README, architecture, roadmap, security model, filesystem specification, and existing debug-log conventions before drafting the rules.
- Added root `AGENTS.md` covering read-before-change workflow, architecture boundaries, upstream research and licensing, security/correctness, implementation discipline, exact-head testing, documentation/logging, PR/merge gates, and evidence-based status reporting.
- Added a README link so human contributors and coding agents can discover the guidelines.
- Changes are documentation-only and committed on `feature/fat32-exfat-removable-provider`; they do not implement FAT32/exFAT runtime support and do not alter kernel code.
- Verification: fetched the newly created file and README update through GitHub after commit. No build or test was run because no source code changed.
- Commits: `e83b4ff4be70fc2727b454c3c50a7584b7a17695` (AGENTS.md), `4566f42d374ae561aaea67719d23eaf6e71dda7d` (README link).
- Next: resume FAT32/exFAT work by inspecting the provider/VFS operation and mount-lifetime contracts under these guidelines.


## 2026-10-31 — FAT provider mount-lifetime foundation implemented

- Started source implementation on `feature/fat32-exfat-removable-provider` after auditing the provider registry and VFS mount lifecycle.
- Extended `include/vfs_provider.h` with an atomic active-mount count and explicit acquire/release API. Documented that provider registry membership changes remain externally serialized until a registry-wide synchronization primitive is implemented.
- Updated `kernel/vfs_provider.c`: unregister now refuses providers with live mount pins; a high-bit state closes new pin admission during unregister; acquire/release use atomic compare-exchange and reject unregistered providers, overflow, underflow, and unregistering state.
- Extended the registry self-test to exercise pin acquisition, busy unregister, balanced release, underflow rejection, successful unregister, and stale acquire rejection.
- Exact source commit: `924051a8b15853823a0a642d8584a55540dd96c4`. Header declaration/contract clarification and canonical filesystem milestone documentation were committed separately; see the branch history.
- Verification: fetched the edited source and header from GitHub and inspected the changed functions and self-test. No workflow run was associated with the source commit at the time of check. No compiler, unit test, QEMU, or hardware test has yet been run; the self-test is added but not yet executed.
- **Important limitation:** this API is not yet connected to the VFS mount/unmount path, so it does not yet establish live provider pinning for actual mounted filesystems. Registry-wide concurrency is also not solved. Do not claim FAT32/exFAT support.
- Next: design and implement the provider-to-VFS operations bridge and mount lifecycle integration; only then add the selected FAT32/exFAT library and its disk adapter.


- Follow-up self-test hardening: adjusted the pin test to work both when the registry has spare capacity and when it is already full. With an existing provider, the test pins it and confirms unregister returns busy without removing it; with an empty registry, it uses a temporary provider and verifies the complete unregister lifecycle. Commit: `ba8d2437d8a509974df5bfb2d9444c0f0413b031`.
- Re-fetched the final provider source, header, filesystem specification, and debug log from the feature branch. The pin API and full-registry-safe self-test are present. No GitHub Actions run was associated with the latest source commit at verification time; compilation and execution remain unverified.


## 2026-10-31 — provider-to-VFS mount lifecycle bridge

- Inspected `vfs_mount_init`, `vfs_mount_partition`, `vfs_mount_activate`, `vfs_mount_deactivate`, provider registry callbacks, block/partition interfaces, and build source inclusion.
- Found that the previous provider ABI had no separate durable sync phase and did not associate a live mount with its provider. Directly tearing down a provider-owned mount could therefore bypass flush and provider pin release.
- Bumped provider ABI to `0x00020000`; added required `sync` callback; added `pros_vfs_provider_mount_device()` and `pros_vfs_provider_unmount_device()`.
- Added `mount->provider` ownership, guarded provider-specific deactivate/abort paths, and made ordinary `vfs_mount_deactivate()` reject provider-owned mounts. The bridge flushes before teardown, preserves provider-private state until post-root cleanup, and retains the provider pin if cleanup must be retried.
- Updated provider registry self-test callback table for ABI v2. No concrete FAT provider or storage probe selection is wired yet.
- Latest implementation commits in this milestone: `8ff0159c37d39eb90859b6534e8e945031ce2336` (provider ABI v2), `4e10e12c18dc02ea31ae38bd446b9ffacce1fb06` (mount ownership field), `081084283bd9c8bd98e43dcdb09804814344b6ab` (provider-aware VFS teardown), `c86dc866afc8388a9d168812f0b0cda98842cbf5` (rollback declaration), `90e5bc7f52e46779794219658ecadf978c235ab2` (guarded rollback), `4a30bf70f5b2e3832b0555170c29c50bf215a1f6` (provider bridge APIs), `cab2993555c4e29e680d6379d32dfb2703d02e79` and `cf8704a509ae624da703a9b9b42ae44993ae58ce` (provider state preservation across cleanup).
- Verification remains source-level only. The GitHub Actions workflow has not been run for this work; no compiler/test/QEMU/hardware pass is claimed. Next, review and harden the bridge contract, then add focused lifecycle self-tests before connecting storage discovery and a FAT provider.


## 2026-10-31 — provider probing connected to partition discovery

- Added `pros_vfs_provider_probe_device()` with a required-capability mask and explicit `NO_MATCH` result so ordinary format mismatch is distinct from I/O/validation errors.
- Extended filesystem type and storage volume metadata to represent a provider-backed volume and retain its parent block device, partition view, and selected provider.
- Updated `pros_storage_discover()`: after native ProsenjitFS returns unsupported, it probes registered providers but accepts only providers declaring `PROS_VFS_PROVIDER_PARTITION_AWARE`. No registered match preserves the existing no-volume behavior.
- Added `pros_storage_mount_volume()` and `pros_storage_unmount_volume()` wrappers around the provider mount lifecycle bridge.
- Source commits: `429a05d4020259ca87b486e74056563834e64e09` (probe API), `04c84832143321495eb4f504651516c168d5f408` (storage volume metadata), `6ebaf8ef0157aab027e9df881d68baa1e1659bbc` (provider filesystem type), `e6d45d1feaf44d94836d2e7d20b6425d3ccfcca3` (probe dispatch), `7bdc0b1b83a2951ac3cca9c36a4e7af18e24107f` (storage discovery and mount), `13a57020193ee51abcf4d009275adc6d0fe4a2ef` (required capability filtering), `bc4897ace7ee4fecf288aa1cde6a90c4cbb5a7f6` (filter implementation), `4692d0041b04a0a857bb5ab2f15dbe23f6433789` (unmount API), and `7f45544e3439919b104aeab63edc942050d16c19` (unmount implementation).
- Verification: files re-fetched from the feature branch after edits. No build, self-test execution, QEMU boot, or hardware test has been run. No FAT provider is registered, so FAT32/exFAT volumes remain unsupported.


- Storage lifetime review found that `partition_device_init()` stores the view address in the block-device private-data pointer. Copying the stack-local partition view into `pros_storage_volume` would leave that pointer dangling. Updated discovery to initialize the view directly in its final volume-array slot after copying partition metadata; documented stable-address ownership in `kernel/include/storage.h`. Fix commit: `fa639c33b945eca2876208adeb9205a53ed39cf3`.

- Added an explicit provider-object lifetime constraint: provider objects and ops tables must have boot/static lifetime because the current registry has no discovery-reference or dynamic module-unload protocol. Unregister only removes registry visibility and must not be treated as safe memory reclamation while volume records may retain provider pointers.


- Lifecycle review found that the legacy `vfs_mount_init()` and `vfs_mount_partition()` preconditions did not reject an inactive mount whose provider cleanup callback was still pending. Reinitializing it could discard the provider association and leak its live-mount pin. Both entry points now reject any non-NULL `mount->provider`; commit `78b88c464a646e28dcc9a2864d59957ce95ac452`.


- Enforced the provider `PROS_VFS_PROVIDER_READ_ONLY` capability in the mount bridge: the mount callback receives read-only mode and the VFS mount is forced read-only even if the caller requested write access. Commit `09b8a7149e4fd850262bba40c6ba953ded73b7eb`.

- Follow-up: the bridge also forcibly preserves a caller-requested read-only mount mode even if a provider callback accidentally clears the mount flag. Commit `f61e0824ca79e89034fa3e9ff6e51f5ae6bff152`.

- Added synthetic provider bridge lifecycle self-test covering mount activation, sync failure preserving the live mount, direct deactivate rejection, cleanup failure/retry, and balanced pin release. Provider ABI validation now independently rejects a missing sync callback. Source commits: `9eaeb91c2480b57693645830e85396c33425d95b` and `cc3638ca6b4ef883734c12c400d3ae31f277d4d6`. Tests are added but not yet run.


## 2026-10-31 — FatFs R0.16 import, disk adapter, and FAT32/exFAT provider

- Researched the official ChaN FatFs docs and upstream patch notes. Selected the Zephyr FatFs mirror at pinned commit `4f60a12c58c3de4246857ad87c40239ecd93349b` because it carries R0.16 patch-2 and documents the 2026 security fixes/local hardening. Original R0.16 is not imported unpatched.
- Vendored `third_party/fatfs/ff.c`, `include/ff.h`, `include/diskio.h`, `include/ffconf.h`, `option/ffunicode.c`, `UPSTREAM.md`, and `UPSTREAM_README.md`. The original source notices remain in upstream source files. Upstream blob SHAs are recorded in `UPSTREAM.md`.
- Added `kernel/fatfs_disk.c` and its public header: partition-local block binding, read-only enforcement, range checks, sector count/size, block-size and CTRL_SYNC. Reused the in-flight-safe `kernel/fatfs_disk.c` adapter and its self-test; the temporary duplicate adapter was removed. The build also includes `kernel/fatfs_libc.c` for the freestanding string primitives required by FatFs.
- Added a concrete `fat32-exfat` provider with FAT32 geometry probing, exFAT signature probing, final FatFs format classification, VFS mount lifecycle, directory/file operations, per-open FIL state, and read/write/truncate paths. FAT12/16 are rejected; no format API is enabled.
- Added optional per-open VFS callback and 64-bit read/write operations; removed the global 4-GiB seek/truncate ceiling only for VFS file-op tables that provide the 64-bit callbacks. Added a boot self-test for wide offsets while retaining the legacy callback overflow guard.
- Added boot self-test calls and CI serial-log markers for the disk adapter, provider registration, and 64-bit VFS tests.
- Notable commits: FatFs core `baac6827e7ed81d2bae1b51bbe02809bbe8dff6b`, Unicode tables `3fd186ccfc46fab41db4ce4e7fc2f385ca9afe1f`, config/provenance `251adc6e3a0fd3281fab32244890ee0a9b6c286d` / `66f382fa78a5e5f507194d6a25ce2e5e9e8af6b0`, disk adapter source `e838c8f0ef266989608ca7548ce01df9725fedc0` (in-flight I/O pinning and unbind safety), VFS wide callbacks `39b85793d2bbbc73112c3c03fe6ba3934ba5dfab`, VFS wide-offset test `312040d9aeb388607ec1e2abebd7582337b478ee`, provider source `8c5622fbf82bba2a6bf2f24b3982e3d4f6ea15fd`, provider hardening `24566934784f1c5c2544cc848a0c6f1a582463a2`, build integration `258c48a728f88efe5bc551ed6ade59ed3fc261c0`.
- Source-level re-fetch verification confirms the vendored source/config, provider code, disk adapter, VFS 64-bit callbacks, build object list, and CI markers are present. No compiler, QEMU, fixture, or hardware pass is claimed. A draft PR is needed to trigger the repository's self-hosted pull-request build workflow.


## 2026-10-31 — FatFs duplicate-object and disk-adapter correction

- Build audit found duplicate compile/link entries: `ff.c` and `ffunicode.c` were each compiled twice, and both `fatfs_disk.c` and `fatfs_diskio.c` exported the same FatFs disk-I/O and bind/unbind symbols.
- Removed duplicate FatFs compilation and switched the build, provider, and boot self-test to the single in-flight-guarded `fatfs_disk.c` adapter. This adapter prevents unbind while disk I/O is in flight; the competing unguarded adapter source/header were removed.
- Commits: build de-duplication `a53b816c5ed9efb66607110345cef1a000771aa1`; provider adapter selection `a89625452bf80a257339b701876c18fdd88ea159`. The build/kernel adapter switch was also committed on the same feature branch.
- Re-fetched build script, kernel call site, and provider source to verify there is one disk adapter, one FatFs core object, one Unicode object, and matching API names. This is source-level verification only; no compiler or QEMU result is claimed.


- Extended `pros_fatfs_provider_selftest()` beyond probe-only coverage with a synthetic FAT32 BPB and sparse in-memory block device. The fixture exercises the real provider/VFS bridge: mount, create, write, read, truncate, directory enumeration, sync/unmount, read-only remount, and persisted readback. Existing probe tests separately cover FAT32 geometry, FAT16 rejection, exFAT geometry/bounds, and read-error propagation. This fixture is wired into the boot self-test but has not yet been compiled or executed.
- Updated the boot serial marker and both BIOS/GRUB QEMU workflow assertions to require the provider mount/I/O/remount self-test marker.


- Configuration audit found a redundant root-level `third_party/fatfs/ffconf.h` alongside the authoritative `third_party/fatfs/include/ffconf.h`. FatFs includes `ffconf.h` relative to its own include directory, so the root copy was not selected by the current compiler include order and could mislead future maintenance. Removed the stale duplicate; `third_party/fatfs/include/ffconf.h` remains the sole active configuration. Commit: df67b43540f5de6d5289001354b4096f8055fca0. Source-level path audit only; build/boot validation is still pending.


## 2026-10-31 — REM-09: Use upstream FatFs for filesystem identification

- User reiterated that FAT/exFAT filesystem implementation must not be handwritten; reuse and assemble mature upstream code to reduce development time and parser bugs.
- Replaced custom BPB/VBR arithmetic and exFAT boot-sector parsing in `kernel/fatfs_provider.c` with a read-only temporary `f_mount()` call through the pinned FatFs disk adapter. The provider accepts only `FS_FAT32` and `FS_EXFAT`, rejecting FAT12/FAT16 after upstream classification.
- Removed the custom parser's synthetic probe tests. The existing integration fixture still verifies the actual provider probe and FatFs VFS read/write path; it is not yet executed.
- Added the vendor root to `scripts/build.sh` include paths. The authoritative config remains `third_party/fatfs/include/ffconf.h`.
- Commits: probe delegation `8f2cfa2d19ee641627078da23a29e7e17c2cbeb3`; forward declaration fix `d98aa6ce3212c91017ad5371a4e0c8779f7251e9`; build include path `8fe7bf924c613c1692d5a972fcace4181650bac4`.
- Verification: re-fetched edited source and confirmed parser removal, upstream `f_mount()` probe, FAT32/exFAT type allow-list, and include path. GitHub Actions run for the latest head was queued at the time of this log update; no compile/test/boot success is claimed.
- Safety note: upstream documentation identifies security fixes and exFAT licensing considerations. Continue pinning source and review patch provenance; do not advertise the feature as bug-free.


- Stack-safety review found the kernel's static thread stacks are 4 KiB. Moved the 4 KiB probe sector and 4 KiB VFS path buffers out of provider callback stack frames into provider-owned static scratch, protected by the same fail-fast global FatFs gate. Reworked lookup/create/mkdir/remove/rename/readdir/metadata/open/probe paths to use those buffers only while holding the gate. Also restored probe self-tests for FAT16 rejection, valid exFAT geometry, out-of-range exFAT volume length, and disk-read failure. Source commits: `cc633deed488eca2a5420696168d2f5e1c39f4be`, `154ebf081b2852c8c686fc2e7620bb51845d5f6f`, and `e86ab14cb54d4345f15823d0650765bed02549e7`.


## 2026-10-31 — REM-10: Remove handwritten FAT/exFAT probe parser

- Review of the actual branch source found that `kernel/fatfs_provider.c` still contained a local FAT32 BPB and exFAT VBR parser, despite the REM-09 note claiming probe delegation had already landed. The earlier status note was therefore not accurate for the branch head.
- Replaced the hand-written format parser with a read-only temporary `f_mount()` through the pinned, patched ChaN FatFs implementation. The provider now accepts only FatFs classifications `FS_FAT32` and `FS_EXFAT`; FAT12/FAT16 and other formats are rejected by the provider boundary.
- The probe binds the candidate block device read-only, serializes FatFs access, always detaches the temporary FatFs work area before releasing it, and preserves the slot/binding if detach or unbind fails rather than risking reuse of live state.
- Removed the obsolete custom endian/geometry probe path and its synthetic parser-only FAT16/exFAT geometry tests. Probe self-tests now cover upstream FatFs recognition of the FAT32 fixture, rejection of invalid media, and distinction of block I/O failure. The end-to-end FAT32 VFS mount/read/write/remount fixture remains.
- Commit: `4b26184496baf187bd81986ada4ec6765e6a8c48`.
- Verification at log time: source edit committed to the feature branch. Compiler, QEMU, CI and physical-media tests have not yet been confirmed on this commit. No FAT32/exFAT runtime or USB hardware success is claimed until exact-head workflows and device tests pass.


## 2026-10-31 — REM-11: Exact-head CI and PR review recheck

- Re-fetched the FAT32/exFAT provider source from `feature/fat32-exfat-removable-provider`; current probe source is still delegated to the pinned patched FatFs `f_mount()` path and no handwritten BPB/VBR parser symbols remain in that probe.
- Rechecked build integration: `kernel/fatfs_disk.c`, `kernel/fatfs_provider.c`, `third_party/fatfs/ff.c`, `third_party/fatfs/option/ffunicode.c`, and `kernel/fatfs_libc.c` are each compiled and linked by `scripts/build.sh`.
- Rechecked boot self-test call sites and required serial-log assertions for the block adapter, VFS 64-bit file I/O, and FAT32/exFAT provider mount/I/O/remount tests.
- Rechecked PR #356: no review submissions and no inline review threads are currently present.
- Exact implementation commit `4b26184496baf187bd81986ada4ec6765e6a8c48` still has Build workflow run `41204948111` pending/queued and Security workflow run `41204948117` queued. Both jobs currently return empty step lists; no compiler output, boot log, or passing conclusion is available. No CI success is claimed and the draft PR is not merged.
- Remaining verification gates: obtain runner execution and inspect exact-head build/boot/security results; validate a valid exFAT image separately if retaining exFAT scope; exercise physical USB mass-storage media only after a USB MSC host driver exists. The existing synthetic fixture is not evidence of physical-device support.


## 2026-11-01 — FAT32/exFAT regression verification and documentation reconciliation

- Reconciled the canonical filesystem status with the merged FAT32/exFAT work. Updated the compatibility-provider status to distinguish deterministic fixture coverage from physical removable-media support.
- PR #362 merged as `19a431f79f3881a350714db98c98ef89228e5cac`; exact head `9a51453c63144bf9595c9862a01a2b2a2825c150`. Build/QEMU run #41232057012 and Security run #41232057015 passed. Added FAT32 block-read failure propagation/recovery coverage.
- PR #363 merged as `1c34a8a01b5d041062616614e9ab94b652749331`; exact head `2cc284b94809648b5fd3a7eaa21d30811e54cd23`. Build/QEMU run #41230977675 and Security run #41230977700 passed. Added fragmented exFAT allocation-bitmap rejection coverage.
- PR #364 merged as `da434cb818461fe6df983f530152fd749d49755d`; exact head `ca704dbe9ebd7273ef9cb06f34ca56b8c1f58c45`. Build/QEMU run #41232663531 and Security run #41232663522 passed. Added valid exFAT file create/write/seek/readback/close/reopen/readback/unlink coverage.
- Verified PR #364 is closed/merged and that `main` points to merge commit `da434cb818461fe6df983f530152fd749d49755d`.
- **Not tested / not claimed:** USB Mass Storage BOT/SCSI, physical USB device discovery, physical pendrive/HDD/SSD I/O, hotplug/surprise removal, and hardware-safe unmount. Dedicated exFAT Unicode/directory/large-file coverage and a wider corruption/fault-injection matrix remain outstanding.
- Next action: add at least two focused regression PRs for exFAT directory/Unicode/large-file behavior and FAT32/exFAT malformed-chain/fault-injection behavior; continue to merge only after exact-head Build/QEMU and Security checks pass.


## 2026-11-01 — PR #378 merge and FAT32/exFAT status reconciliation

- PR #378 (FAT32 corrupt directory-entry start-cluster regression) merged to `main` as `353d147392e98fe5dc92bc4080447148fe07d4a7`; exact PR head was `39065cf6b6a27186c47203f8199e4ac04cd01b66`.
- Exact-head Build/QEMU run [#41280875046](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41280875046) passed all steps: toolchain/desktop validation, ISO build, ProsenjitFS diagnostic, ISO QEMU boot, and native BIOS QEMU boot. Exact-head Security run [#41280875033](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41280875033) passed.
- The regression locates the actual `DIRENT  BIN` 8.3 short entry in the FAT32 root directory, replaces its start-cluster high/low words with reserved cluster `0x0FFFFFF0`, requires the first data read to return `PROS_VFS_ECORRUPT`, restores both on-disk and active FatFs sector-window bytes, closes/unmounts, then remounts and verifies the original payload.
- Rechecked earlier FAT32/exFAT PR states and exact-head CI: PR #366 merged (`63fe1d7a63e33347567715a8c3f17e5507802a72`), Build #41238120705 and Security #41238120765 passed; PR #369 merged (`f20924ab9d580ee367480abb24de60d227855478`), Build #41274413136 and Security #41274413065 passed; PR #372 merged (`19b113df35045eb2f2598c849d11cd5754653e7b`), Build #41278349083 and Security #41278349015 passed.
- Post-merge main workflows for #378 were triggered on commit `353d147392e98fe5dc92bc4080447148fe07d4a7`: Build/QEMU run #41283460414 and Security run #41283460556. At the time of this log update they were still in progress/queued; no post-merge main pass is claimed yet.
- Canonical `docs/FILESYSTEM.md` REM-12 was reconciled: #366/#369/#372 are merged and verified, #378 is merged and verified on its exact PR head, and the remaining limitations are explicitly separated from synthetic-media evidence.
- Remaining work: exFAT file-cluster allocation-bitmap/FAT-chain inconsistency coverage; further FAT32 directory metadata corruption cases; torn-metadata/power-loss recovery; active-I/O removal; and real USB host-controller, enumeration, DMA, BOT/SCSI integration, hotplug, and physical-device validation. Open PR #377 is only the BOT/SCSI class transport layer, not a USB host driver.


## 2026-11-02 — Bounded read-only xHCI runtime/interrupter accessors

- PR #411 was verified on exact head `fdc9b77f0c3054df81b73c95275d84049b1d10c6`: Build/ISO QEMU/Native BIOS QEMU run #41400534898 and Security run #41400534853 passed. It was squash-merged as `8de68c4ea8f850eb5da9a45081032c1ab59f7108`.
- Started the next isolated xHCI step on branch `feature/xhci-bounded-runtime-accessors`, based on that merge commit.
- Added read-only MFINDEX, interrupter 32-bit (IMAN/IMOD/ERSTSZ), and interrupter 64-bit (ERSTBA/ERDP) accessors. Access is gated on initialized + reset-complete controller state, validates the complete advertised runtime window, checks interrupter indices and register-offset allowlists, and uses 64-bit offset arithmetic before MMIO access.
- Added synthetic self-tests for valid reads, both interrupter indices, 32/64-bit register offsets, invalid index/offset/null output, reset gate, and runtime BAR bounds.
- Changed files: `kernel/usb_xhci.c`, `kernel/include/usb_xhci.h`. Source update commit: `2f5dec38b62b00cdc4b320d30512f33518c3931b`; header update commit: `9589ae8d93e2bf6ec720b55be30bf4331efea3dc`.
- **Verification state:** implementation committed to the feature branch; no local build or hardware test has been run. Exact-head CI and logs are still required. This is read-only register access only: no ERST programming, event-ring DMA programming, interrupt enabling, doorbell writes, PCI bus mastering, or device enumeration.
- Next action: inspect the full diff, open a draft PR, then require exact-head Build/ISO QEMU/Native BIOS QEMU and Security checks before considering it ready.


## 2026-11-02 — PR #413 implementation: bounded single-segment xHCI ERST builder

- Added a one-entry Event Ring Segment Table descriptor and initializer using caller-owned DMA descriptors.
- Validates allocation tokens, virtual/physical alignment, allocation and requested sizes, 16-bit segment TRB count (4..65535), and physical-address overflow before writing the table.
- Writes the segment base, TRB count, and zero reserved field, then publishes with a release fence.
- Synthetic tests cover a valid segment, too-small/too-large counts, undersized event DMA, misaligned event DMA, and invalid table allocation metadata.
- No xHCI MMIO register programming, PCI bus mastering, interrupt enable, or device enumeration is performed. Exact-head CI is required before merge.


- First Build attempt failed at compile time under `-Werror=type-limits`: the `uint32_t event_trb_count > UINT64_MAX / 16` comparison is impossible. Removed the redundant check; the 16-bit ERST field limit (4..65535) already bounds the multiplication safely. Awaiting exact-new-head CI rerun.


## 2026-11-02 — xHCI event ring dequeue helpers (rebased after ERST merge)

- Rebased this isolated implementation on main after PR #413 merged as `76f8151612cda5817c6005321d8455d8d2844ead`; this avoids validating an old-base branch.
- Added software-only ERDP value calculation for the current dequeue index, including EHB bit, with initialized-state, ring pointer, entry-count, index, alignment, and overflow checks.
- Added TRB event-type extraction and completion-code decoding for Transfer Event and Command Completion Event only.
- Added synthetic tests for initial/advanced/wrapped ERDP, EHB bit, event type and completion code, unsupported event type, and null arguments.
- No ERDP MMIO write, interrupt enabling, DMA programming, PCI bus mastering, or device enumeration. Exact-head Build/ISO QEMU/Native BIOS QEMU and Security checks are required.


## 2026-11-02 — USB MSC bulk endpoint packet-size validation (rebased)

- Recreated this change on current main after PR #415 merged as `e03356fd9c0bac390b11b5c39bf1a5501369d075`, so the new CI run covers the latest integrated xHCI event-ring code.
- Selected MSC bulk endpoint wMaxPacketSize is now limited to defined values 8, 16, 32, 64, 512, or 1024 bytes; reserved high bits and invalid endpoint attributes remain rejected.
- Added negative tests for 65-byte and zero-byte values and positive tests for 512-byte and 1024-byte values.
- Descriptor validation only; no USB transfers or hardware enumeration. Exact-head Build/ISO QEMU/Native BIOS QEMU and Security checks are required.


## 2026-11-02 — Bounded multi-segment xHCI ERST construction (rebased)

- Recreated the multi-segment ERST builder on current main after PR #417 merged as `ac8e1629b4ff1249a40a989bf2ae0fdf456242bd`.
- Supports 1..8 caller-provided event-ring DMA segments; validates table capacity/alignment, segment allocation metadata, 64-byte physical/virtual alignment, 16-byte TRB sizing, minimum 4 and maximum 65535 TRBs, and allocation bounds before writing.
- Populates segment base/count/reserved fields and publishes with a release fence.
- Tests cover two valid segments, zero/excessive segment count, malformed and over-limit segment size, and undersized table. The first QEMU run exposed an incorrect undersized-table fixture (64 bytes was enough for two entries); corrected it to one entry of capacity.
- Software table construction only; no ERST register programming, interrupts, PCI bus mastering, or device enumeration. Exact-head CI required.


## 2026-11-02 — USB standard descriptor length and configuration attribute validation (rebased)

- Recreated this descriptor parser hardening on current main after PR #419 merged.
- Require exact standard configuration/interface/endpoint descriptor lengths (9/9/7 bytes) and enforce mandatory configuration bmAttributes bit 7 with reserved low bits clear.
- Added malformed configuration attributes and interface/endpoint length tests. Descriptor validation only; no USB transfers or hardware enumeration.


## 2026-11-02 — xHCI ERST DMA range overlap hardening (rebased after PR #421)

- Reapplied overlap checks on current main after PR #421 merged, avoiding the stale-base conflict in the earlier draft.
- Rejects overlap between the ERST table and every event segment, and pairwise overlap among event segments, before writing any ERST entries.
- Uses a subtraction-based overlap predicate to avoid end-address addition overflow; each range is separately validated for size, alignment, and physical-address overflow before comparison.
- Synthetic tests cover segment-to-segment overlap, table-to-segment overlap, and a physical range that would overflow the address space.
- No controller register writes, interrupts, PCI bus mastering, or device enumeration. Exact-head Build/ISO QEMU/Native BIOS QEMU and Security checks required.


## 2026-11-02 — xHCI Command Completion Event reclaim helper

- Added software-side handling for Command Completion Event TRBs.
- Requires event type 33, a 16-byte-aligned command TRB pointer, a nonzero completion code, and the exact next outstanding command-ring dequeue address.
- Reclaims one command-ring entry only after all event checks pass; completion code is returned only on success.
- Synthetic tests cover success, empty ring, wrong event type, misaligned/wrong command pointer, and invalid zero completion code.
- No interrupt dispatch, controller MMIO, doorbell writes, PCI bus mastering, or device enumeration.


- First QEMU attempt failed in the combined xHCI ring self-test: the new command-completion test reused `command_dma` after an earlier negative test intentionally misaligned its physical address. Isolated the completion test with its own aligned DMA fixture; rerun exact-head CI before considering merge.


## 2026-11-02 — PCI bus-master permission control

- Added separate `pci_enable_bus_master` and `pci_disable_bus_master` operations.
- Both update only the PCI Command MASTER bit and preserve I/O and memory decoding bits; the existing combined enable helper now uses the shared update path.
- Kept PCI Status's upper write-one-to-clear halfword zero on command writes.
- Added pure command-bit transformation tests for enabling/disabling MASTER while preserving unrelated command bits.
- This adds the recovery/teardown primitive needed before later xHCI start/stop work; no device is enabled by this change itself.


## 2026-11-02 — xHCI multi-TRB TD packet-count correction

- Rebased the multi-TRB transfer TD work onto current main after PCI bus-master permission controls landed.
- Corrected Normal TRB TD Size to represent packets remaining after the current TRB, calculated from the endpoint maximum packet size and saturated at the 5-bit field maximum of 31.
- Require a valid endpoint maximum packet size (1–1024 bytes), validate transfer-ring indices/cycle state, and retain bounded TD/ring capacity checks.
- Added a negative test for missing packet size and a multi-TRB test that verifies an eight-packet remainder rather than a TRB count.


## 2026-11-07 — Cross-platform compatibility architecture and implementation guide

- Added docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md as the master entry point for Linux/POSIX, Windows application, shared compatibility-service, and later driver-compatibility work.
- Defined the native-kernel/security boundary and six-layer architecture; clarified that Linux .ko and Windows .sys are not directly loadable as native ProsenjitOS kernel drivers.
- Specified separate per-process Windows HANDLE and Linux FD tables backed by native typed objects/capabilities; defined distinct Windows/Linux VFS namespaces; documented capability-controlled shared-memory IPC and safe buffer lifetime requirements.
- Added process/thread and separate SEH/signal semantic requirements, plus a warning that the illustrative shortened IRP structure and raw physical-address UserBuffer mapping are not a valid WDM implementation.
- Added dependency-ordered phases, source-audit checklist, exact-head acceptance gates, compatibility matrix requirements, and a reusable coding-agent task prompt.
- Linked the master guide from README, AGENTS.md, ROADMAP.md, PROJECT-STRUCTURE.md, V2-UNIFIED-APPLICATION-RUNTIME.md, and DRIVER-MODEL.md.
- Documentation-only change. No source code, build, QEMU boot, hardware, or runtime compatibility tests were changed or executed by this documentation update. Exact-head CI verification remains required before merge.


## 2026-11-08 — Compatibility architecture expansion

- Extended the compatibility master guide with a microkernel-oriented migration boundary, PMM/VMM invariants, measured fast-IPC acceptance, separate Linux ELF/syscall and Windows PE/DLL/API paths, per-process DLL redirection policy, unified internal fault events with separate SEH and signal adapters, isolated driver hosts, watchdog/recovery, and class-specific fallback behavior.
- Expanded DRIVER-MODEL.md with per-driver address-space isolation, capability/DMA/interrupt constraints, supervisor/watchdog recovery, and explicit degraded-mode/fallback policy.
- Expanded the Version 2 application runtime with separate ELF/Linux and PE/Windows execution contracts, DLL resolution/redirection, guest-specific exception semantics, and runtime acceptance gates.
- Expanded ROADMAP.md with dependency-ordered, unchecked implementation tasks for kernel/user boundary verification, IPC, compatibility runtimes, exception handling, driver isolation and fallback testing.
- Documentation-only change. No kernel, driver, loader, syscall, memory manager, IPC or device implementation was changed. No build, QEMU, hardware or runtime test is claimed by this entry. All new requirements remain planned until source changes and exact-head tests demonstrate them.


## 2026-11-08 — Per-thread TSS RSP0 ownership foundation

- Added TSS RSP0 setter/getter assembly accessors and scheduler integration for the selected thread's owned high-half kernel stack.
- Added rollback restoration after failed address-space switches, bootstrap-stack restoration when the current thread is detached/terminated, and a reap guard for stacks still published as TSS RSP0.
- Added required ISO/native BIOS boot markers for setter validation, scheduler stack switching, CR3 rollback, and active-stack reap refusal.
- The source head `c6561bcb709f58463fce4c05c4c89f89f0e949e5` passed Build/boot run #9933 and Security architecture run #9430. The subsequent documentation-only updates on this branch require exact-head CI refresh before merge.
- Limitation: `INT 0x80` remains on shared IST1; CPL3 RSP/SS frame handling, scheduler-created user threads, and CPL3 timer preemption remain unimplemented.


## 2026-11-08 — Per-thread CPL3 syscall entry (PR in progress)

- Changed IDT vector 128 from shared IST1 to IST=0 so CPL3 syscall transitions use the scheduler-published TSS RSP0 stack.
- Adjusted the ISR stack layout and SysV call alignment for the five-word CPL3 hardware IRET frame plus 15 saved GPRs.
- Added a runtime guard in `syscall_dispatch()` that checks the live C stack pointer is inside the current thread's owned kernel stack, verifies the TSS RSP0 matches that stack's top, and validates user privilege bits in the saved CS/SS plus a non-zero lower-half user RSP.
- Added an exact serial milestone required by both ISO and native BIOS QEMU workflows.
- This is a narrow entry-stack conversion. It does not add a general CPL3 saved-frame type, a scheduler-created user thread, a spawn/exec syscall, or timer preemption. The saved CPL3 frame is still retained by pointer; its RSP/SS fields are not yet represented in the common scheduler frame contract.
- Exact-head verification: Build and boot-test run #9941 and Security architecture run #9438 both passed on `0733dbf87f5527b45e96bd4778434ec529daa59a`. ISO and native BIOS QEMU serial logs both contain the CPL3 per-thread RSP0/five-word-frame milestone and the syscall dispatcher ABI/policy self-test milestone. The QEMU sessions ended at their configured timeout after reaching the required boot milestones (exit status 124), as expected by the workflow.


### CI regression correction — in-kernel synthetic dispatcher frames

- First QEMU run exposed that the new entry validator was also applied to `syscall_selftest()`, which intentionally calls the dispatcher directly with a synthetic all-zero frame.
- Updated the guard to apply RSP0 and five-word IRET-tail checks only when the supplied saved CS has CPL3. Kernel-origin/synthetic dispatcher calls continue through the existing ABI/policy tests.
- Exact-head QEMU verification confirms both the kernel synthetic-frame self-test and the real CPL3 syscall-entry validation milestone in ISO and native BIOS boots.


## 2026-11-08 — Explicit scheduler interrupt-frame kinds (implementation in review)

- Added an explicit frame-kind tag for kernel-originated timer frames and user-originated syscall frames. The assembly entry stubs derive the tag from the saved CS privilege level and pass it to the scheduler.
- Extended the shared GPR-prefix structure with the CPL3-only user RSP/SS tail and compile-time offset/size assertions. The timer's synthetic vector/error words remain outside the scheduler frame pointer.
- The scheduler records the frame kind with the saved per-thread frame and validates CS privilege plus the CPL3 SS/RSP tail before accepting a handoff. A malformed frame cannot make a blocked caller fall through to IRET; the scheduler parks it.
- IPC blocked-syscall wakeup now requires a saved USER frame before rewinding RIP and restoring the syscall number.
- Both timer and syscall restore paths continue to use IRETQ; the CPU consumes either the three-word CPL0 or five-word CPL3 tail based on the saved CS privilege level. CPL3 timer preemption is explicitly gated to the no-switch return path and remains disabled until user-thread launch and full user-frame scheduling tests exist.
- Added source-level assembly checks for CS-derived frame-kind propagation in timer and syscall entry. Exact-head warnings-as-errors build, ISO QEMU, native BIOS QEMU, IPC wakeup and security workflows are pending; no runtime pass is claimed yet.
- Follow-up source audit tightened the scheduler contract: saved frames are validated both when captured and immediately before restore, including exact kernel/user selectors, canonical lower-half user RSP, and safe CPL3 RFLAGS (IOPL/NT/VM rejected).
- Invalid or stale frames are failed closed: the affected thread is detached, any IPC waiter is cancelled, and the CPU parks on the bootstrap stack instead of attempting an unsafe IRET.
- Corrected the synthetic first-run kernel frame allocation to reserve only the 18-qword CPL0 layout (144 bytes); the CPL3-only user RSP/SS tail is not allocated for a ring-0 first run.
- Closed a no-successor scheduler edge case: when the interrupted thread has already entered BLOCKED state, an empty/fully-filtered run queue now parks on the bootstrap stack even if its owning process remains runnable; it cannot fall through to IRET and resume the blocked syscall.
- Extended the IPC wakeup regression to reject a kernel-tagged frame, a malformed user SS, and privileged CPL3 RFLAGS before confirming the valid USER frame is rewound and resumed.
- Latest source head `6d436567ddcd645e1bc7f456ddafd8bf269c3cb8`. Exact-head Build and Security workflows have been queued but have not started/completed yet; no pass is claimed. CPL3 timer preemption remains disabled.

## 2026-11-10 — USB MSC registration status reconciled against merged PR #457

- Rechecked merged main commit `9da428d3b1135483e88c74d1cb0c30e421333e3a` and the source paths `kernel/usb_msc.c`, `kernel/kernel.c`, `kernel/block.c`, `kernel/include/usb_msc.h`, and `kernel/include/usb_xhci.h`.
- Confirmed the xHCI bootstrap initializes MSC, obtains the embedded block-device pointer, calls `block_device_register()`, then enumerates the registry and verifies that the same pointer is present before emitting the success marker.
- Confirmed registry self-tests cover duplicate rejection, ordering/lookup, unregister/ENOENT, capacity exhaustion, and freed-slot reuse. MSC self-tests cover busy detach refusal and successful unregister/cleanup.
- Exact post-merge Build/boot-test PASS: [run #9860](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42080509094), ID `42080509094`, job `127431994644`, on exact merge SHA `9da428d3b1135483e88c74d1cb0c30e421333e3a`, self-hosted runner `home`. BIOS and Native BIOS QEMU logs show USB MSC registry publication (count 1) and BOT/SCSI INQUIRY + READ CAPACITY (524288 sectors, 512-byte sectors).
- Exact post-merge Security architecture PASS: [run #9861](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42080509726) on the same merge SHA.
- **Status:** USB MSC block-device registration is IMPLEMENTED and QEMU/CI-VERIFIED on merged main. This is not proof of complete physical USB removable storage.
- **Still pending:** xHCI disconnect/Port Status Change integration, DMA quiescence, surprise removal during idle/active I/O, reconnect/re-enumeration, safe physical eject, and physical FAT32/exFAT pendrive tests. No physical hardware validation is claimed.
- The canonical current status is recorded at the top of `docs/FILESYSTEM.md`; historical entries below remain historical and must not override that summary.



## 2026-11-14 — Master Architecture Completion source revalidation

- Audited current `main` at `98ed2f76c5a03fecba38b742423619bfe8238d97`. The source audit was performed at `19a86627a57b75b15f1611650e6da897132b6e42`; comparison confirmed the only subsequent main commit changed `README.md` and `docs/FILESYSTEM.md`, not kernel source. Re-fetched the native process, thread, scheduler, interrupt, PMM, paging/address-space, ELF loader, syscall/usercopy, object/capability, IPC, VFS, driver registry/device broker, PE inspector and NT compatibility sources.
- Confirmed PR #472 is already represented on main by merge commit `05fdcd8561327ca1122626378297edae4be56565`; it is the architecture-documentation baseline only.
- Reconciled the current CPL3 frame implementation: the 20-qword frame carries user RSP/SS, user-originated frames are copied to per-thread storage and validated, and syscall entry uses the per-thread RSP0 stack. CPL3 timer preemption remains disabled and no scheduler-created initial CPL3 frame/general spawn transaction exists.
- Confirmed the native ELF loader supports a bounded static ELF64 ET_EXEC/PT_LOAD profile and rejects PT_DYNAMIC, PT_INTERP and PT_TLS. The native syscall ABI still exposes IDs 1–7 and has no spawn/exec syscall.
- Confirmed Linux runtime, Windows PE execution/Win32 runtime, guest HANDLE/FD tables, guest path namespaces, shared-memory objects, Linux signals, Windows SEH and isolated user-space driver hosts are not implemented on main. Existing PE inspector and NT driver policy code are not executable compatibility runtimes.
- Rechecked open architecture-related PRs. Historical branches #152/#157/#125/#123/#124 are substantially behind current main; do not merge or cherry-pick them without fresh comparison and exact-head tests. PR #137 and #164 remain orchestration/registry scaffolding, not production process launch or installation.
- Current main exact-head Actions status was not available from the PR-triggered workflow lookup in this session; no build, QEMU, host test or hardware test was run here. No pass is claimed.
- Next implementation slice: a single native spawn transaction PR, reusing current process, ELF, address-space, kernel-stack, usercopy, capability and scheduler code. Preserve existing boot-time CPL3 tests and keep CPL3 timer preemption disabled until the new path is proven.


## 2026-11-14 — Authoritative unified OS architecture and dependency sequence consolidated

- Updated `docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md` as the single authoritative cross-platform target specification. It now defines the one-OS-authority architecture, separate Native/Linux/Windows ABI paths, guest-specific semantic adapters, native user-space services, microkernel authority boundary, native foundation/HAL and hardware layers.
- Consolidated all competing implementation lists into one Phase 0–10 dependency sequence: (0) specification/source baseline; (1) native microkernel and transactional process execution; (2) native object lifecycle plus separate Windows HANDLE/Linux FD adapters; (3) VFS namespaces, IPC and shared memory; (4) Linux/POSIX runtime; (5) Windows PE loader/DLL resolver; (6) Win32/NT runtime, Registry Engine, security, async I/O and SEH; (7) graphics/audio/network/desktop; (8) installer/package management; (9) isolated user-mode driver framework; (10) restricted WDM/KMDF research.
- Added an explicit Windows API translation classification contract (direct native mapping only when semantics match; native user-space service; composite compatibility implementation; deterministic unsupported result) and a distinct Registry Engine contract.
- Reconciled `docs/ARCHITECTURE.md` with the no-Wine-runtime policy and the single native OS authority. Removed the obsolete suggestion to integrate an external Wine-like runtime. The architecture document now points to the source-backed foundation audit for implementation status.
- Reconciled `docs/V2-UNIFIED-APPLICATION-RUNTIME.md` and `docs/ROADMAP.md` so they map to, rather than compete with, the master Phase 0–10 order. Existing subsystem-specific detail remains in its designated canonical document.
- Updated documentation only. No kernel, loader, filesystem, driver, syscall, test or workflow source was changed. This documentation update does not claim native spawn, Linux runtime, Windows PE execution, Win32/NT runtime, or driver compatibility is implemented.
- PR #641 baseline commit before this consolidation was `2f7c33291ffa3d77baf940a376729948acc43a0c`; its Build/boot-test and Security architecture workflows had completed successfully. The new documentation commits require fresh exact-head CI; prior green runs are not evidence for the updated head.
- Next dependent implementation milestone after documentation review and exact-head required checks: **Phase 1 — Native microkernel and process-execution foundation**, beginning with a fresh audit of process/thread creation, ELF loading, initial CPL3 frame construction, TSS RSP0 ownership, scheduler handoff, and transactional wait/reap.

## 2026-11-15 — Transactional native process-spawn preparation (PR #642)

- Added a kernel-facing static ELF64 spawn transaction and first-user-thread frame builder.
- Spawn keeps the child unpublished until image load, argv/envp construction, user entry/stack validation, and thread-stack allocation succeed. PID registration precedes run-queue publication; failure before registration rolls back the thread, handles, address space and process object.
- Limited parents cannot request a child with a higher security level.
- Added a malformed-ELF spawn rollback regression. Exact-head build/QEMU verification is pending for this source update.
- The scheduler-created first CPL3 handoff and end-to-end exit/wait/reap test are still outstanding; Phase 1 remains incomplete.


## 2026-11-15 — Scheduler-created CPL3 native process launch

- Reworked the boot CPL3 regression to use `process_spawn()` rather than directly entering a loaded address space with `enter_user_mode()`.
- The boot ELF has separate RX code and RW/NX data PT_LOAD segments plus a loader-built stack. The spawned thread carries a validated first-run CPL3 IRET frame.
- The scheduler switches to the child process and its owned TSS RSP0 stack; an assembly helper restores the user frame and IRETQs to CPL3.
- The user test completes IPC/syscall checks, exits, and is reaped by the parent. Required serial markers verify first launch and exit/wait/reap.
- CPL3 timer preemption and SMP remain disabled/unimplemented. Exact-head Build/ISO QEMU/Native BIOS QEMU and Security checks are pending.


- Added required ISO and Native BIOS QEMU log assertions for invalid-image rollback, loaded ELF mapping/argv/envp, scheduler-created CPL3 first entry, and parent exit/wait/reap. The boot handoff now disables interrupts without retaining an unused flags variable; IRETQ establishes the initial user RFLAGS.


- First CPL3 IRET now explicitly loads user DS/ES selectors and leaves IF clear, preserving the current non-preemptive CPL3 contract. The boot regression checks CS, SS and RFLAGS in the initial user frame before handoff.


- Added public ABI syscall 8 (`PROSENJIT_SYS_EXIT`) with CPL3/current-thread ownership validation and exit-status forwarding. The CPL3 regression now performs a distinct user-test mapping check followed by SYS_EXIT(42); the parent-side trampoline verifies ZOMBIE state and wait/reap cleanup rather than calling process_exit on behalf of the child.


- Added five deterministic spawn transaction failpoints covering address-space initialization, completed ELF mapping/stack construction, initial user-thread allocation, process registration, and run-queue publication. Each QEMU case verifies complete rollback of PID/object/address-space/thread-stack/registry/queue ownership.


- Added a scheduler handoff failure-injection test: temporarily invalidate the child CR3 root, require the failed address-space switch to restore bootstrap RSP0 and requeue the child once, then restore the root and perform the successful CPL3 handoff.


- Fixed the scheduler-test cleanup failure: process thread termination now restores bootstrap TSS RSP0 whenever the current-thread pointer is NULL, including the detached-current edge case. Added serial diagnostics for terminate/reap status, reapability, current thread, run-queue count and TSS RSP0.


## 2026-11-16 — Phase 1 thread-stack lifecycle audit (PR #642)

- Re-fetched and inspected the PR branch's thread-stack allocator/release, page-table translation/unmap, physical-page free, process spawn rollback, and thread-registry paths.
- Confirmed that stack allocation rollback and release use void unmap/free helpers that do not prove expected-PA unmapping or all-or-nothing non-contiguous page release. Confirmed that current page-table walkers dereference intermediate table addresses without allocator-ownership authentication.
- Confirmed that several thread-registry scans are unbounded/cycle-unchecked, thread_manager_init() clears ownership state without a non-empty-state guard, and thread_init_user() publishes its registry node before completing the CPL3 frame unless the caller supplies the critical section.
- Recorded the implementation contract and regression matrix in docs/NATIVE-SPAWN-CONTRACT.md and docs/NATIVE-SPAWN-IMPLEMENTATION-LOG.md. No kernel/test/workflow source was changed.
- Exact existing PR head reviewed: 1fa1687ad870a406faa0c3de208699d9c5d9e41d. Its Build/boot run 43742860484 and Security run 43742860533 report success; no new tests were run for this documentation-only audit.
- PR #642 remains open and has no submitted reviews or inline review threads. Next: implement checked page-table unmap and non-contiguous physical-page batch release, then a bounded global registry snapshot shared by allocation rollback and all reap/abort paths. Do not merge or advance to Phase 2 until the new regression matrix and exact-head checks pass.


## 2026-11-16 — Checked stack release implementation

- Added bounded all-or-nothing physical page batch free and expected-PA page-table unmap primitives, with allocator/paging self-test cases.
- The checked page-table walk authenticates allocator ownership of the root and intermediate table pages before dereference. Stack validation now uses checked translations for guard and data pages.
- Thread-stack rollback/release uses the new batch APIs; uncertain rollback keeps the slot reserved.
- No local compiler/QEMU was run in this session. Exact-head CI evidence is pending; no runtime PASS is claimed.
- Continue with bounded registry snapshot/cycle/alias checks and fail-closed manager initialization, then verify argv/envp contents and the full spawn/wait/reap matrix.


## 2026-11-16 — Checked stack release and registry hardening verification

- Implemented all-or-nothing bounded physical-page batch free and expected-PA page unmap; added rejection/success self-tests.
- Added checked kernel page translation and switched thread-stack guard/data validation away from unchecked translation.
- Integrated checked batch release into stack rollback and normal stack release. Uncertain slots are quarantined.
- Added bounded global registry validation for cycles, repeated nodes, duplicate slots, cross-thread physical-page aliases, and bitmap disagreement; lifecycle traversals validate before iteration.
- Changed thread manager initialization to fail closed instead of clearing live ownership. Added regression checks for cycle, page alias, and reinitialization preservation.
- Updated the stack guard source-invariant script to match the new checked contract.
- Exact tested head: 1f4d2c9ed249855878234a436741dfee926af171. Build/boot run 43865301602 **PASS**; Security architecture run 43865301636 **PASS**. Build included warnings-as-errors compilation, ISO QEMU, and native BIOS QEMU. GitHub-hosted fallback was selected.
- Remaining: enforce atomic CPL3 frame construction before registry publication; complete argv/envp content assertions and wait/reap fault-injection coverage. SMP remains unsupported. Phase 1 remains incomplete.

## 2026-11-17 — Crash-record persistence integration audit (PR #642)

- Re-inspected `kernel/crash_record.c`, its public contract, block-device error paths, health event handling, and the native BIOS storage initialization path.
- Confirmed two separate production-integration gaps in the reviewed PR branch:
  1. `pros_crash_record_attach()` has no production call-site. The native BIOS path probes `native_ata.block`, discovers the ProsenjitFS partition beginning at LBA 2048, and mounts ProsenjitFS, but it never attaches the crash-record store. The default crash-record LBA is 2046 (two sectors); its intended reservation/layout and non-overlap must be explicitly established before using it.
  2. `pros_crash_record_flush_pending()` has no production caller. The only observed flush call is in the crash-record self-test.
- The exception path deliberately captures only to volatile pending memory and then enters a fatal halt for ordinary unhandled exceptions. Therefore, merely adding a later safe-context flush call is not sufficient to persist fatal crashes: the halted CPU never reaches that later context, and pending RAM state is lost on reset. Do not add block I/O to `exception_dispatch()`; it can recurse through storage health reporting and is not a safe device-I/O context.
- A correct design decision is still required for fatal persistence: either a separately proven panic-safe storage path with strict non-recursive/bounded hardware semantics, or a reset/recovery protocol that preserves a crash payload across the transition. The current capture-only implementation must be described as in-memory staging, not durable crash logging.
- Existing retry semantics are partly sound: a failed `pros_crash_record_flush_pending()` leaves `pending_valid` set, so a later retry can attempt the write again. However, the self-test does not inject write or flush failures and does not prove pending-record retention/retry. Add deterministic write-failure and flush-failure fixtures before claiming retry coverage.
- The native BIOS disk layout currently locates the ProsenjitFS partition at LBA 2048. LBA 2046–2047 appears immediately before that partition in this boot fixture, but no production reservation/format contract was found in the reviewed path; do not silently treat this gap as a guaranteed crash-record region.
- No source code was changed, no production storage attachment was added, and no compiler/QEMU/CI test was run for this documentation-only audit. PR #642 remains open/unmerged; Phase 1 remains incomplete.
- Next action: define and document the crash-persistence mechanism and disk reservation contract first; then implement attach/flush integration plus write/flush failure-and-retry regressions as a separately reviewable slice. Keep all block I/O out of the fatal exception handler unless a dedicated panic-safe path is independently validated.

## 2026-11-17 — Crash-record disk-layout contract and persistence decision

- Inspected the native BIOS image builder (`scripts/build.sh`) rather than inferring disk geometry from the kernel's partition probe alone.
- The image builder places stage 1 at LBA 0, stage 2 at LBAs 1–24, the kernel from LBA 25, and the ProsenjitFS MBR partition at LBA 2048 for 512 sectors. It checks only that the kernel ends no later than LBA 2048. The two-sector default crash-record range 2046–2047 is therefore a **candidate gap**, not yet a guaranteed reservation: kernel growth is currently allowed to consume it, and the image-layout verifier does not assert that those sectors are excluded/zero.
- ProsenjitFS is mounted through a partition-relative block view beginning at absolute LBA 2048. Its v1 format reserves filesystem-relative blocks 0 and 1; those filesystem reservations do not reserve absolute disk LBAs 2046–2047. The host formatter operates on the ProsenjitFS partition view, so it does not establish ownership of the preceding disk gap.
- Required layout hardening before any production attachment: define the crash-record extent as exactly [2046, 2048); require the native kernel payload end to be <= 2046; require the ProsenjitFS partition start to be >= 2048; verify the image geometry and that no boot/kernel/partition extent overlaps the crash-record sectors; and document the reservation as part of the native disk-image ABI. Do not assume the default LBA is safe merely because the current partition starts at 2048.
- **Persistence design decision for the current Phase 1 slice:** keep exception dispatch capture-only and do not add ATA/block I/O to the fatal exception handler. All ordinary unhandled exceptions currently halt in that same dispatch path, so a later safe-context flush is unreachable for those fatal crashes. A safe-context flush is useful only for a future recoverable/panic-recovery lifecycle that actually resumes normal kernel execution; it cannot by itself make a fatal halt durable across reset.
- Before claiming durable fatal-crash persistence, the architecture must separately specify a panic-safe write mechanism and its lock/timeout/re-entry behavior, or a platform-supported nonvolatile handoff. The present ATA PIO path uses a fail-fast per-device I/O lock and bounded polling; a crash during an ATA operation may find the device lock already held. A naive call to the normal block API from exception context is therefore rejected.
- The existing “persistent crash record self-test” boot marker is a mock in-memory block-device test. It proves record encoding/slot selection on a test device, not production disk attachment, fatal exception persistence, or physical power-loss durability. Future reporting must state this distinction explicitly; consider renaming the marker to “crash-record mock block-device self-test passed”.
- No source code, disk image builder, test, or workflow was changed in this design pass. No compiler/QEMU/CI result is claimed. Next source slice should first add the explicit two-sector disk-layout reservation and image-verifier assertions; production attach/flush remains blocked on the panic-safe persistence design and deterministic write/flush failure-retry tests.



## Crash-record retry cleanup verification — 2026-11-17

- Source head: `5c0cc17bf06c497932e67b44fa03fb26e2400599` (`test: centralize crash-record selftest cleanup`).
- Centralized crash-record self-test failure cleanup so all assertion failures reset write/flush injection flags and detach the test device.
- Exact-head Build and boot-test workflow: [44087100741](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44087100741) — success.
- Exact-head Security architecture workflow: [44087100999](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44087100999) — success.
- Build job selected the active local runner. Kernel build, ISO QEMU, and native BIOS QEMU completed successfully. Both QEMU paths emitted the required `persistent crash record self-test passed` marker.
- This verifies the in-memory mock-device write/flush retry and cleanup self-test, not physical-media durability or panic-safe exception-time persistence. The native crash-record sectors remain reserved but not attached to production storage.
- PR #642 remains open and unmerged; Phase 1 remains incomplete.

## 2026-11-17 — Wait/reap cross-thread stack-alias regression and disk-layout correction

- Added a second independently allocated process-owned thread to the wait/reap corruption fixture. After both threads enter DEAD state, the test aliases one physical stack page in the peer thread record to a page owned by the first thread.
- `process_wait()` must reject the cross-record alias during the bounded global thread-registry preflight while preserving the child's exit-code output, ZOMBIE state, address-space root, object reference count, and PID registry membership. After restoring the peer page record, the same child is successfully reaped and both thread-stack records are checked for release.
- This exercises the existing global validator's cross-thread physical-page uniqueness invariant; it does not claim SMP safety or arbitrary memory-corruption recovery.
- Exact source head: `b016ae7c299a2d66e9ef6b33ef100926b99ed937`. Build/boot workflow run [44132992103](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44132992103) **PASS**; Security architecture run [44132991876](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44132991876) **PASS**. Build logs include six successful process-lifecycle self-test invocations and the native BIOS disk-image layout/readback verification. No cross-thread-specific serial marker is emitted; the regression is covered by the aggregate process self-test result.
- **Disk-layout audit correction:** the current `scripts/build.sh` now explicitly reserves absolute LBAs 2046–2047, rejects a kernel extent entering that range, verifies non-overlap among stage-1, stage-2, kernel, crash-record reservation and ProsenjitFS extents, checks the reservation is within image geometry and initially zero, and verifies ProsenjitFS formatting leaves those sectors unchanged. The prior audit note describing this as an unprotected candidate gap is superseded for the current source head.
- **Production persistence remains incomplete:** no production `pros_crash_record_attach()` or `pros_crash_record_flush_pending()` call-site exists. The reserved sectors are not yet attached to the ATA block device, and fatal exception handling remains capture-only before halt. Do not describe crash records as durably persisted. A panic-safe write or platform nonvolatile handoff design remains required before production attachment.
- Phase 1 remains incomplete; PR #642 remains open and unmerged.


## 2026-11-18 — Crash-record production binding audit correction

- Rechecked the current PR branch after the disk-layout reservation was added. The image builder now formally reserves LBAs 2046–2047, rejects kernel overlap, validates image bounds/non-overlap, checks the reservation starts zeroed, and verifies the ProsenjitFS formatter preserves it. The earlier 2026-11-17 candidate-gap statements above are superseded by this verified build-script contract.
- Corrected the earlier retry-test note: `kernel/crash_record.c` does inject both write failure and flush failure. Each failed flush must return EIO while preserving the pending record; clearing the injected fault and retrying must succeed. These are mock block-device tests, not physical-disk durability tests.
- The native BIOS runtime path probes the primary ATA master (`drive = 0`) and then locates the fixed ProsenjitFS MBR partition at LBA 2048. The bootloader retains BIOS drive number DL internally for INT 13h reads, but the reviewed handoff does not expose that identity to the kernel or prove that ATA primary-master device is the same physical disk from which firmware booted.
- The current MBR partition parser validates partition geometry and overlap among declared partitions; it cannot prove that unpartitioned LBAs 2046–2047 are unused by vendor metadata, another boot environment, or a different disk layout. The builder's reservation contract is authoritative for images it creates, not arbitrary physical disks.
- Therefore **do not attach crash-record storage to the probed ATA device yet**. A safe production design must bind the kernel to the actual boot device (or use an explicit, validated device identity), establish that the device carries the ProsenjitOS native layout, and verify a versioned reservation descriptor/partition ownership before any write. Unknown or ambiguous media must remain read-only for crash-record purposes.
- Fatal exception dispatch remains capture-only and halts; a normal safe-context flush cannot run after that fatal halt. Do not call the ordinary block API from exception context. Durable fatal-crash persistence needs a separately validated panic-safe bounded I/O path or a platform-supported nonvolatile handoff; this is distinct from the existing mock-device retry test.
- No production attachment or source change was made in this audit. No new Build/QEMU/Security run is claimed. PR #642 remains open/unmerged; Phase 1 remains incomplete.


## 2026-11-18 — BIOS boot-device identity handoff audit

- Inspected the actual BIOS path from `boot/native_stage1.asm` through `boot/native_stage2.asm` to `kernel/kernel.c`. Stage 1 and stage 2 preserve BIOS drive number `DL` in their private `boot_drive` byte and use it for INT 13h reads. At `protected_entry`, stage 2 passes only `EAX = NATIVE_MAGIC` and `EBX = MB2_BUFFER`; the boot-drive byte is not included in the Multiboot2-compatible information block or another documented handoff field.
- Kernel `kernel_main(boot_magic, boot_info)` accepts the native magic and memory-map handoff, but the native storage path independently calls `pros_ata_pio_probe()`, which hard-codes primary ATA master (`drive = 0`). There is no verified mapping from BIOS `DL` to this ATA device.
- Consequence: the current BIOS image builder's LBA 2046–2047 reservation proves ownership only for images produced and laid out by that builder. It does not authorize writing those sectors on an arbitrary physical ATA device discovered independently by the kernel.
- Do not infer equivalence from QEMU's common BIOS/IDE mapping. Real firmware may boot from a different disk/controller or expose devices through a different path.
- Safe next design: version the native boot-info extension (or define a new boot-protocol tag) carrying firmware boot-device identity with explicit source/type and validity flags; validate the complete handoff before use; add an explicit platform mapping layer that can prove which kernel block device corresponds to that identity. If identity cannot be resolved or reservation metadata is absent/invalid, crash-record storage must remain detached/read-only.
- This identity work is a boot-protocol/platform prerequisite, not permission to add ATA writes to the fatal exception path. The current exception path halts after volatile capture; a separate panic-safe persistence design remains necessary.
- No source code was changed and no build/QEMU/CI run is claimed for this audit. PR #642 remains open/unmerged; Master Phase 1 remains incomplete.


## 2026-11-18 — Phase 1 spawn rollback and argv/envp gate closed

- Re-audited `kernel/process.c`, `kernel/elf_loader.c`, `kernel/kernel.c`, the native spawn contract, and the exact PR #642 source head `646388874996d60dc6bfc88457d5d5f26354273a`.
- Confirmed spawn failpoints 1–5 cover the construction/publication stages and failpoints 6–7 prove aggregate rollback-preflight rejection preserves unpublished and registered/queued children until the injected invariant is restored and cleanup retried.
- Confirmed ELF-loader and scheduler-created-child tests inspect the actual mapped initial stack: argc, argv/envp NULL terminators, pointer bounds, 16-byte alignment, and exact NUL-terminated argument/environment bytes. The boot fixture checks `/init`, `elf-runtime`, and `PROS_ELF_RUNTIME=1`.
- Exact source-head Build/boot run [44292223796](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44292223796) and Security run [44292223802](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44292223802) both succeeded. Build merge-checkout verification matched PR head `6463888`; ISO and Native BIOS QEMU emitted all required spawn/argv-envp/CPL3/SYS_EXIT/wait-reap markers. Native BIOS QEMU ended at its bounded timeout (124) after those markers. GitHub-hosted fallback was used; no local-runner pass is claimed.
- Updated `docs/NATIVE-SPAWN-CONTRACT.md` and `docs/NATIVE-SPAWN-IMPLEMENTATION-LOG.md` to remove superseded claims that argv/envp assertions and initial CPL3 construction remain unimplemented.
- This closes the **spawn rollback + initial-stack verification work package**, not Phase 1 as a whole. Remaining gates include the full wait/reap zero-mutation/failure-injection matrix and panic/fault handoff. PR #642 remains open/unmerged; no Phase 2 work or merge was started.
- Source-head evidence is for `646388874996d60dc6bfc88457d5d5f26354273a`. This documentation-only reconciliation creates a newer head and must receive its own exact-head required workflow checks before subsequent implementation work begins.


## 2026-11-18 — End-to-end stack-alias wait/reap zero-mutation snapshot

- Added a read-only `thread_stack_allocator_snapshot()` for the 64-slot allocation/quarantine bitmaps. The helper preserves IF state and intentionally does not validate the registry, allowing the corruption fixture to inspect allocator ownership while a deliberate alias is active.
- Extended the process-owned two-thread physical-stack-page alias regression to snapshot slot allocation/quarantine bitmaps, physical free-page count, all eight thread stack-page records and allocation states, and each corresponding virtual mapping before invoking `process_wait()` and `thread_reap_process()`.
- Both paths must reject with no mutation to those snapshots; the displaced page remains free. The fixture restores a unique allocated page and verifies the process is reapable afterward.
- Exact source head `0168b8bed009efdb824762f5b8b21b8ea97a8397`: Build/boot [44303434840](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44303434840) **PASS**; Security architecture [44303434802](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44303434802) **PASS**. Build merge checkout `11fe330d` matched the head; warnings-as-errors build, ISO QEMU, Native BIOS QEMU, and existing wait/reap marker all passed. Native BIOS QEMU returned bounded timeout 124 after required markers. GitHub-hosted fallback used; local runner not verified.
- This closes the targeted cross-thread alias zero-mutation snapshot gap only. The full wait/reap failure-injection matrix and panic/fault handoff remain open. PR #642 remains open/unmerged.
- Documentation-only reconciliation now creates a new PR head and therefore needs its own exact-head required CI before the next implementation slice.


## 2026-11-18 — Late-target process-thread reap failure atomicity snapshot

- Extended the malformed-second-thread-stack regression to snapshot the stack allocation/quarantine bitmaps, physical allocator free-page count, all eight physical stack-page records and allocated states, and every corresponding stack mapping before injecting the invalid second-thread page record.
- The real `process_wait()` and `thread_reap_process()` calls must both reject without changing those snapshots or the earlier valid thread's stack, TSS RSP0, process ZOMBIE/PID/address-space/handle state, or exit-code sentinel. The fixture restores the corrupted field before later retry/success tests.
- This closes the targeted late-target zero-mutation snapshot gap; it does not close every wait/reap failure boundary or prove arbitrary memory corruption/SMP safety.
- Exact source head `cf7a567fe0eb37ea9fee524e82236e9b6a9a58c3`: Build/boot [44305026983](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44305026983) **PASS**; Security [44305026995](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44305026995) **PASS**. Build merge checkout `7488b105` matched the head; warnings-as-errors build, ISO QEMU and Native BIOS QEMU passed. Native BIOS QEMU ended at bounded timeout 124 after required markers. GitHub-hosted fallback used; no local-runner pass claimed.
- PR #642 remains open/unmerged; Phase 1 remains incomplete. The documentation-only update creates a new head requiring its own exact-head required CI before the next implementation slice.


## 2026-11-18 — Wait/reap live-stack context fault injection

- Refactored the read-only process-reap eligibility check into `thread_process_reapable_with_context(process, rsp, active_tss_stack)`; the production wrapper supplies the actual current RSP and TSS.RSP0, so production semantics remain the same.
- Added a deterministic boot self-test that injects a synthetic current RSP inside each target thread's kernel stack, then injects TSS.RSP0 inside that stack (including the stack-top boundary). Both must be rejected; the same zombie process must remain reapable with both pointers outside its stacks. No real stack switch or ownership mutation is performed by the fixture.
- Hooked this self-test into `process_selftest()` before the existing destructive wait/reap corruption matrix.
- Exact PR head: `131eecf015f877e9ed22a1627bd025464f2e4d3d`. Build and boot-test [run 44311155682](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44311155682) — **success**; Security architecture [run 44311155718](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44311155718) — **success**. Both run records confirm this exact head SHA. Build included warnings-as-errors kernel/artifact build, ISO QEMU boot and Native BIOS QEMU boot; all required steps succeeded.
- Runner selector used GitHub-hosted fallback; no local-runner execution is claimed. PR #642 remains open and unmerged.
- This closes the synthetic live-RSP/RSP0 preflight test gap only. The broader Phase 1 wait/reap work remains open for additional release-plan/allocator/rollback failure coverage; panic/fault handoff and final Phase 1 acceptance remain outstanding.

## 2026-11-18 — IPC waiter-count and address-space physical-alias hardening

- `kernel/ipc.c`: `ipc_channel_valid()` now rejects `waiter_count` values above `PROSENJIT_IPC_CAPACITY` before send, receive, wait, cancel, wake or count operations can index the fixed-size waiter array. Added `ipc_channel_integrity_selftest()` with an over-capacity corruption fixture that exercises all public operations and is invoked during boot.
- `kernel/address_space.c`: generic `address_space_map_user_page()` now rejects a physical user page already mapped at another virtual address before creating any page-table nodes. This aligns map-time admission with `destroy_table_preflight()`'s existing one-physical-page/one-leaf invariant; the self-test now exercises alias rejection through the generic mapper, not only the owned-page wrapper.
- Latest exact source head `b51c436bec7991a42e7b2b5bfc9c60e23f05a003`: Build/boot [run 44319913608](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44319913608) **PASS**; Security [run 44319914111](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44319914111) **PASS**. Warnings-as-errors build, ISO QEMU and Native BIOS QEMU completed successfully. Runner selection used GitHub-hosted fallback; no local-runner execution is claimed.
- These close the specific IPC waiter-count validation gap and address-space duplicate-leaf admission gap. They do not establish complete IPC lifecycle verification, all PMM/VMM failure-injection coverage, panic/fault handoff, or Phase 1 exit-gate completion. PR #642 remains open/unmerged; Phase 2 remains blocked.

## 2026-11-18 — Exception entry preserves interrupted state and C ABI

- `kernel/idt.asm`: `isr_common` now saves/restores all 15 general-purpose registers around `exception_dispatch()`. It dynamically aligns the temporary C stack, passes the original raw exception-frame pointer at the saved-prefix offset, restores the original stack, discards only vector/error words, and returns through the untouched hardware IRET frame. This preserves the recoverable user-copy #PF RIP fixup while preventing C caller-saved register clobber from leaking into interrupted code.
- `isr128`, `isr32`, and `isr_common` now execute `cld` before any C handler call. The original DF state remains in the hardware-saved RFLAGS and is restored by IRET; C handlers therefore receive the SysV-required clear direction flag.
- `scripts/test_timer_irq_assembly.sh` checks the exception GPR save/restore set, raw-frame offset, dynamic stack alignment, return cleanup, and DF clearing before all three assembly-to-C entry paths.
- Exact source head `15a1db46c21daacd3f34c73540b1748f23533c92`: Build/boot [run 44321691845](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44321691845) **PASS**; Security [run 44321691841](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44321691841) **PASS**. The warnings-as-errors build, assembly regression, ISO QEMU and Native BIOS QEMU completed successfully. Native BIOS QEMU reached the scheduler-created CPL3 launch, SYS_EXIT and wait/reap pass markers before its bounded observation timeout. GitHub-hosted fallback was selected; no local-runner pass is claimed.
- This closes the identified exception register-preservation, dynamic C-stack-alignment and DF-entry ABI gaps. It does not implement the normalized panic-frame producer/consumer, dedicated panic-stack switch, first-owner/nested-fault fail-stop, or recoverable-usercopy panic-handoff regression. Phase 1 remains open; PR #642 remains unmerged.
## 2026-11-20 — Process PID reservation through final registry unlink

- Source audit found that `process_lookup()` intentionally hides `PROS_PROCESS_DEAD` records, while `process_register()` and `process_create()` used lookup visibility as their PID-uniqueness check. A DEAD process that remained registered after a failed final teardown could therefore stop being discoverable but still occupy the registry, allowing a duplicate PID lifetime to be published.
- `kernel/process.c` now uses `process_pid_registered()` for registration and creation admission. It validates the registry and checks every registered entry regardless of lifecycle visibility; a PID remains reserved until `process_unregister()` removes the record.
- Added a boot self-test that marks an existing registered child DEAD without unlinking it, attempts to create a second process with the same PID, and verifies rejection plus preservation of the original registry record. The fixture restores the child's original READY state immediately.
- Commit: `d8968660bee8c1e98d66b099ac832976ec38232e`. Build/Security verification is pending for this exact head. PR #642 remains open and unmerged; this finding does not close the wider Phase 1 audit or acceptance gates.


## 2026-11-20 — PMM mutation serialization against local preemption

- A source audit found that physical-page allocation/free operations changed shared allocator bitmaps, page-type metadata, and the global free-page counter without owning interrupt exclusion. On the supported single-CPU scheduler, a timer-driven context switch could interleave two allocator callers between bitmap observation and mutation, permitting duplicate page allocation or inconsistent accounting. This was a source-level concurrency finding; no runtime corruption was claimed or reproduced.
- `kernel/memory.c` now wraps page allocation, page-table allocation/free, contiguous allocation/free, user-page allocation, single-page free, and batch-free/preflight entry points in save/disable/restore interrupt-state sections. Compound helpers preserve the outer disabled state across nested public calls. The allocator self-test now verifies that the IF bit observed at entry is unchanged at successful exit.
- The exclusion is explicitly limited to the existing single-CPU model. It is not an SMP lock and does not claim cross-CPU safety. `physical_memory_init()` remains an early-boot-only initializer and must run before concurrent allocator use.
- New source commit: `dc7c563fe8a9cf9948e995ffa42579053d69562e`; follow-up assembly correction: `3b2d4108a775a41e1a3f989e506d8435e79371a6`. Exact-head Build/boot and Security workflows have not yet been run for the corrected head; no verification pass is claimed.
- PR #642 remains open/unmerged. Continue auditing PMM/VMM call-site assumptions and run the required exact-head CI before treating this slice as closed.


## 2026-11-20 — PMM query-side consistency

- Follow-up caller audit found that allocator mutations were now protected, but public state-query helpers still read multiple ownership bitmaps and counters without the same local exclusion. A preemption between `managed_bitmap`, allocation/type bitmap, and counter reads could expose a mixed allocator snapshot to stack ownership checks, user-page admission, or health reporting.
- Serialized `physical_page_is_page_table()`, `physical_page_is_allocated()`, `physical_page_is_user_mappable()`, `physical_memory_free_pages()`, and `physical_memory_failed_allocations()` with the same save/disable/restore mechanism. These wrappers are nest-safe for the current single-CPU design because they restore the caller's prior IF state. Immutable `total_pages` remains a direct read after early-boot initialization.
- Cross-checked callers: thread stack ownership validation, address-space user-page admission, USB DMA accounting tests, and health snapshots. No caller requires SMP semantics; this remains a single-CPU exclusion mechanism, not a multiprocessor lock.
- Source commit: `24b31f3c2c9ef2913920b77fe0f33ab8474fa44d`. Exact-head Build/QEMU and Security workflows are required after this follow-up; no CI result is claimed here.


## 2026-11-20 — Address-space teardown enforces global thread-stack ownership

- Follow-up API-boundary audit found that `process_wait()` and `process_release_resources()` explicitly called `thread_address_space_disjoint_preflight()`, but direct callers of `address_space_destroy_preflight()` / `address_space_destroy()` could bypass that cross-subsystem ownership check.
- A corrupted or aliased user-owned-page record could therefore pass the address-space-local page-table/allocator checks while naming a physical page still owned by a registered kernel thread stack. A direct destroy caller could then free a page still required by the stack owner. This is a source-level API-contract gap; no runtime corruption was claimed.
- `kernel/address_space.c` now invokes `thread_address_space_disjoint_preflight()` inside the locked address-space destruction preflight, after validating the address-space's own owned-page records. This makes the cross-subsystem invariant mandatory for every destroy caller, not only process wait/release callers.
- The check runs under the same local interrupt exclusion as the address-space preflight and relies on the existing bounded/identity-authenticated thread registry validator. This remains single-CPU protection, not SMP synchronization.
- Source commit: `7a24e6ab937d4e5b13c34bdc3390f35745cafbb2`. Exact-head Build/QEMU and Security workflows are pending; no CI pass is claimed yet.
- PR #642 remains open and unmerged pending exact-head CI and independent source review. This closes only the API-boundary bypass; it does not complete Phase 1.


## 2026-11-20 — Reject process creation into already-registered storage

- Source audit found that `process_create()` checked PID uniqueness but did not prove that its destination `struct pros_process` was unpublished before calling `process_init()`. Passing an already-registered process object (including `child == parent`) could overwrite its object/refcount, handle table, address-space fields and parent metadata before the later `process_register()` membership check rejected publication. Its error cleanup could then destroy the newly initialized address space while the old registry node still pointed at that storage.
- `kernel/process.c` now rejects `child == parent` and requires `process_registry_validate(child, 0)` to prove the destination object is absent from the process registry before any initialization or resource allocation. The same pre-init membership guard is applied to `process_spawn_internal()`, which has a separate direct `process_init()` path.
- Added boot regressions for both self-aliasing the registered parent and reusing a different registered child object with a fresh PID through `process_create()`. Each case must reject and preserve the original PID, lifecycle state, address-space root, object reference count, parent relationship and registry lookup. A separate spawn-path regression is being added to exercise the same pre-init membership gate through `process_spawn_internal()`.
- Source commit: `1c4bf2501467b51965ed7eda14f35cda5c04f268`. Exact-head Build/QEMU and Security checks are required; no pass is claimed until both workflows complete on the resulting head.
- This is a source-confirmed API admission bug, not a reported ordinary-path runtime failure. PR #642 remains open/unmerged; Phase 1 is not complete.

## 2026-11-20 — checked heap-growth cleanup when page mapping fails

A focused source audit found one remaining cleanup path in `heap_grow()`: after allocating a heap data page, a failed `paging_map_page()` called the legacy void `physical_page_free()`. That API cannot report whether the allocated page was actually returned to the PMM, so the heap could continue after an unobservable cleanup failure.

The failure path now uses `paging_unmap_pages_expected_and_free()` with zero mapped leaves and the one allocated-but-unmapped page. This runs the checked page-table hierarchy and PMM batch-free preflights before returning the page. If cleanup rejects, the heap is marked poisoned and growth returns a distinct cleanup error instead of allowing future allocations to proceed.

Added a boot regression that injects page-table allocation failure at the next 2 MiB heap page-table boundary after earlier data pages have been mapped. It requires the allocation to fail, the committed heap frontier and usage to remain unchanged, the PMM free-page count to return to its snapshot, and the first heap leaf to remain unmapped.

This is a source/test hardening change; no runtime failure was previously claimed. Exact-head Build/Boot and Security CI are pending for commit `af79023a271dac60ae45412223ff2f21d83e091a`.

## 2026-11-20 — include thread-stack PMM/page-table release in wait preflight

A follow-up transaction audit found that `thread_process_reapable()` previously checked registry integrity, dead/unqueued state, stack mappings, and RSP/TSS exclusion, while the later `thread_reap_process()` still performed a separate fallible page-table-hierarchy and PMM batch preflight after `process_wait()` had released handles and the address space. That left a late rejection path which could restore the process to ZOMBIE after its other resources were already gone.

Added `paging_unmap_pages_expected_and_free_preflight()`, a read-only half of the checked paging/PMM release transaction. It validates kernel page-table hierarchy uniqueness, every expected VA-to-PA leaf, complete allocation membership, duplicate mappings, and the full PMM free batch without changing PTEs or allocator state.

The process-thread reapability gate now builds the complete bounded stack-page release plan and invokes that read-only preflight while holding the single-CPU interrupt exclusion. The same stack-page preflight is included in spawn rollback validation before an unpublished/registered child is mutated. The eventual release path still reruns the checked commit helper.

Added a corruption regression which temporarily aliases a present kernel PML4 subtree into an unused high-half slot. Each target stack mapping remains valid, but the global hierarchy is no longer unique; the aggregate reapability gate must reject, preserve the PMM free-page count, and pass again after the root entry is restored.

This closes the source-identified late page-table/PMM rejection window for process wait and spawn rollback under the existing single-CPU exclusion model. It does not claim SMP safety. Exact-head CI is pending for the current source/doc commits.

## 2026-11-20 — preflight IPC waiter unlink before process termination

A process/thread lifecycle audit found that `thread_terminate_process()` ignored the return value from `ipc_channel_cancel_waiter()` and cleared `thread->wait_channel` regardless. If a channel's waiter array was malformed, had a duplicate entry, or no longer contained the blocked thread, termination could proceed without proving the external waiter reference had been removed.

Added `ipc_channel_cancel_waiter_preflight()` to validate the bounded waiter array, require non-null unique active entries, require a zeroed inactive tail, and require exactly one occurrence of the target thread. The IPC integrity self-test covers valid membership, missing membership, duplicate membership, stale tail entries, and an invalid waiter count.

`thread_terminate_process()` now owns save/restore interrupt exclusion, validates all process-owned wait-channel links before its first run-queue mutation, and only then performs cancellation. Dead threads carrying a stale wait-channel pointer or stale run-queue links, and non-blocked threads carrying a wait-channel pointer, are rejected before mutation. The commit path checks cancellation success rather than discarding the result.

A follow-up process-exit audit also found that the old ordering switched CR3 and cleared `current_process` before `thread_terminate_process()` could reject a bad waiter link. Added a separate read-only `thread_terminate_process_preflight()` and call it before the CR3 transition, so a failed IPC/thread preflight leaves the current process and address space unchanged. The commit path repeats the same gate under interrupt exclusion. Added a process self-test with a deliberately missing IPC waiter entry while the child is temporarily designated as `current_process`; rejection must preserve the child state, blocked thread/wait-channel metadata, current-process pointer, and CR3 value.

A follow-up audit tightened the waiter-array contract further: a non-null, unique pointer is not necessarily a live registered thread. Added `thread_ipc_waiter_validate()` to authenticate every waiter against the bounded thread registry and require `BLOCKED` state plus an exact channel back-pointer before process termination can cancel any entry. The process regression now places an unregistered synthetic pointer beside the valid target and proves preflight rejects it without dereferencing it; it separately proves a valid registered waiter passes, then verifies missing target membership blocks process exit without changing CR3/current-process state.

This is source-level fail-closed hardening; no runtime dangling waiter or failed CR3 rollback was claimed or reproduced. Exact-head Build/Boot and Security CI are pending for the current source/doc commits.

## 2026-11-20 — authenticate current-thread and thread-owner registry identities

A bounded thread-registry audit found that the validator authenticated registered node addresses and stack ownership but did not prove that the scheduler's `current_thread` pointer was one of those registered nodes. Lifecycle helpers subsequently dereference `current_thread` when checking whether a process is current, so a corrupted pointer could bypass the registry gate and fault during process unregister/reap.

The validator now compares `current_thread` against the already authenticated bounded node snapshot before returning success. It also validates each node's owner kind and the owner/process nullability contract: process-owned threads require a non-null process pointer, while kernel-owned threads require a null process pointer.

The registry integrity self-test now injects a forged current-thread pointer, an invalid owner kind, and an owner-kind/process-pointer mismatch. Each corruption must be rejected without dereferencing the forged pointer, and the original registry must validate after restoration.

This is a source-level identity-contract hardening; no runtime invalid-current-thread fault was claimed or reproduced. Exact-head Build/Boot and Security CI are pending for the current source/doc commits.

## 2026-11-20 — direct process unregister preflights thread-stack reclamation

A direct-entry audit of `process_unregister()` found that it checked for live/current threads but did not run the aggregate dead-thread stack-reap preflight before switching away from `current_process`. If a dead thread stack failed the later RSP/TSS or mapping ownership checks, the unregister call could reject only after the CR3 transition.

`process_unregister()` now requires `thread_process_reapable()` to pass as part of its read-only admission gate, before `paging_switch_kernel()`. `thread_reap_process()` repeats the check during commit.

Added a direct API regression: a child with a dead registered thread has its address space released, then a synthetic TSS.RSP0 alias is pointed into that thread's stack. `process_unregister()` must reject while preserving current_process, CR3, registry membership, object reference, and stack ownership. After restoring TSS.RSP0, a retry must successfully reap and unregister the child.

The first regression assertion incorrectly used `process_lookup()` to prove that a `DEAD` process remained registered. Since lookup intentionally hides `DEAD` entries, the test was corrected to use `process_registry_validate(&child, 1)` for exact registry membership. This changes the assertion, not production lookup semantics. The regression now also preserves distinct failure-stage diagnostics so future boot failures identify the failing fixture step.

This is source-level transactional hardening with a deterministic boot regression; no naturally occurring TSS alias was claimed or reproduced. Build/Boot and Security both passed on exact source/test head `5a97f4e499d2a7ac6ea012a75bc1901d5038034a` (ISO QEMU and Native BIOS QEMU included).


## 2026-11-20 — bind published process objects to private registry identities

- The process registry previously validated mapped supervisor storage and object fields but had no private record binding a published node to its exact caller-owned storage address. A copied `struct pros_process` could therefore imitate the fields of a registered object if substituted into the registry links.
- Added a private kernel-heap sidecar record per successful `process_register()`. Process-list and sidecar-list publication/removal occur under the same local interrupt exclusion. The validator checks both chains against the trusted cardinality and walks them in lockstep, requiring each sidecar's owner pointer to equal the exact process node before reading lifecycle fields. This introduces no fixed process-count cap.
- `process_unregister()` removes and frees the matching sidecar only after thread/resource teardown and final object-reference release. If sidecar allocation fails during `process_create()`, registration remains unpublished and the existing creation rollback releases the address space and initial object reference.
- Added QEMU regressions for copied-object substitution at the same list position, a cyclic identity-sidecar chain, and injected sidecar allocation failure with no PID publication or retained address-space root. The allocation-failure fixture was moved to static storage after the first QEMU attempt exposed test-stack exhaustion from an additional large process object; the corrected fixture passes ISO and Native BIOS QEMU.
- Source commits: `e8d9e866222373690daa2d03a18c5dd89fe57dfe`, `2b66838047aa3ce922082ef9ce1ac586aadf5585`, `982b79ea25851cdd49972526ac2c492f72520322`, and `491bd960cccb9b470f4a9030e89dc8766516add5`. Exact-head Build/boot-test and Security architecture workflows both passed on `491bd960cccb9b470f4a9030e89dc8766516`; see the checks attached to PR #642.
- This is exact-address registry membership evidence, not cryptographic authentication against arbitrary corruption of privileged kernel memory. Caller-owned storage must remain stable until unregister. No SMP safety is claimed. Phase 1 remains open for the other documented exit gates.


## 2026-11-20 — authenticate thread registry descriptors before dereference

- A follow-up thread-registry audit found that registry links were authenticated by pointer equality against `thread_registry_slots[]`, but the validator did not independently prove that the complete descriptor storage was mapped supervisor memory before reading its fields. If both the list head and its matching slot entry were corrupted to the same non-canonical address, the identity comparison alone would not prevent a fault.
- Added `thread_registry_node_readable()`, which validates overflow-safe coverage of every page occupied by the descriptor through `paging_query_page()` and rejects absent or user-mapped pages before any descriptor-field access.
- Hardened `thread_reap_kernel()` and `thread_set_state()` so the candidate pointer must be a member of the already validated bounded registry before it is dereferenced. `thread_set_state()` now holds the existing nest-safe local interrupt exclusion across validation and state mutation.
- Extended `thread_registry_integrity_selftest()` to corrupt both the registry head and its corresponding trusted slot to the same non-canonical pointer, require bounded rejection, restore the original registry, and verify that the public state/reap APIs reject an unregistered invalid pointer.
- Source/test commit: `52a330ca72e7fc4ccc3a8a176d6b49427b1a5855`. Exact-head Build/boot-test [run 44709994378](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44709994378) and Security architecture [run 44709994421](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44709994421) both completed successfully. Build/boot included warnings-as-errors compilation, ProsenjitFS/exFAT checks, ISO QEMU, and Native BIOS QEMU. Both boot paths reached the existing process wait/reap, scheduler-created CPL3 launch, and isolated panic fail-stop assertions.
- GitHub-hosted fallback was selected for this run; no local-runner pass is claimed. This remains a single-CPU/interrupt-exclusion design, not SMP safety. PR #642 remains open/unmerged; reviewer approval and the broader production panic-routing, boot-device identity, and Phase 1 architecture exit gates remain outstanding.


## 2026-11-24 — Phase 1 CPL3 exception-policy dispatch correction

- Cross-checked the production exception dispatcher against the shared exhaustive vector classifier and the canonical panic/fault disposition contract.
- Found that a broad CPL3 vector-range condition could route reserved/unsupported vectors 9, 15, 22–27 and 31 into process termination even though the classifier marks them fail-stop.
- Changed production CPL3 routing to enter process-exit/scheduler handoff only for `PROS_EXCEPTION_USER_FAULT_PENDING`; all other dispositions continue to the fatal path.
- Strengthened the source regression script to require the classifier call and explicit disposition gate. Existing classifier self-tests cover reserved-vector classification at both CPL0 and CPL3.
- Parent source head: `228113c28715e35f818108b87a92496fdcbb4392`. This is source-level work only. Exact-new-head Build/QEMU and Security checks, compiler/runtime execution, and independent review remain pending. No merge or runtime PASS is claimed.
