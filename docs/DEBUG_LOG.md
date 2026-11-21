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
## 2026-11-17 — Phase 1 boot protocol contract draft

- Inspected the current `main` boot path: `boot/native_stage1.asm`, `boot/native_stage2.asm`, `boot/entry.asm`, `boot/multiboot2_header.asm`, `kernel/kernel.c`, `kernel/init.c`, `scripts/build.sh`, `docs/BOOT.md`, `docs/BOOT-ARCHITECTURE.md`, and the Phase 1 checklist in `docs/ROADMAP.md`.
- Confirmed the native BIOS path currently hands off the native magic plus a Multiboot2-compatible memory-map block; the GRUB path uses the Multiboot2 magic/block. `kernel_main()` accepts both magics and passes the information pointer to the existing physical-memory initialization path.
- Confirmed the native BIOS image layout is generated from the build-derived values: stage-1 at LBA 0, 24 stage-2 sectors at LBA 1..24, kernel beginning at LBA 25, and the reserved ProsenjitFS test partition at LBA 2048. The build script contains overlap checks and image readback validation.
- Added a versioned ProsenjitOS Boot Protocol v1 contract draft to `docs/BOOT-ARCHITECTURE.md`, covering fixed-width ABI, memory/module descriptors, ownership, fail-closed validation, compatibility migration, UEFI ExitBootServices requirements, and acceptance tests.
- This is documentation/design work only. No kernel ABI header, parser, loader producer, UEFI loader, source code, or runtime behavior changed. No build, host test, QEMU test, or CI workflow was run in this checkpoint.
- Exact base main SHA inspected: `aad3c14c0814a542a27c2a86096c768e7b9bda99`. Documentation branch: `phase1/boot-protocol-contract-20261117`. Documentation commit: `bfa6af7963be0adde255158941bfe11d88cdfab6`.
- Next: review the v1 wire layout, implement the shared ABI header and host-side malformed-input parser tests as one isolated change, then add the GRUB adapter before changing either existing boot producer. Keep Phase 1 unchecked until native BIOS, GRUB, and eventually UEFI acceptance gates have exact-head evidence.

## 2026-11-17 — Boot Protocol v1 wire-layout review

- Re-reviewed the v1 contract against the existing 32-bit protected-mode entry and the current kernel/GRUB/native BIOS handoff split. The initial draft described the required fields but did not pin down byte offsets, array representation, record sizes, or optional-record constants sufficiently for an implementation that can be independently tested.
- Extended `docs/BOOT-ARCHITECTURE.md` with a canonical 128-byte v1 header layout, exact field offsets and widths, contiguous handoff-allocation model, 24-byte memory-map and module records, offset/count/entry-size rules, non-overlap and overflow requirements, memory ownership constraints, and defined optional framebuffer/ACPI/module/boot-device semantics.
- Clarified that all in-allocation arrays/payloads use offsets relative to the handoff block, while module/framebuffer/ACPI payloads are external physical ranges. The handoff block itself must be reserved in the memory map. The initial protected-mode register ABI requires the block below 4 GiB.
- This remains a specification review baseline, not implemented behavior. No C ABI header, parser, adapter, producer migration, build, host test, QEMU test, or CI run is claimed.
- Follow-up review is still required before coding: verify every chosen field and enum against the project’s intended kernel requirements, then implement compile-time size/offset assertions and pure host-side malformed-input tests. Do not migrate either existing boot producer until parser tests and the GRUB adapter are independently reviewed.
- Documentation commits on `phase1/boot-protocol-contract-20261117`: `40e2fad0223f9305dae2f9d44139da5626fa04c6` (wire layout) and `7e8c5ffcb61ffacde1af56ec612e15051f525eaf` (optional metadata constants). No CI was triggered because the local runner remains unavailable per project instruction.


## 2026-11-17 — Boot Protocol v1 consistency and producer audit

- Re-read the complete native stage-2 E820 collection and `build_mb2_info` conversion. The native producer currently retains 24-byte E820 records, converts disabled E820 entries to Multiboot2 reserved type 2, and emits the legacy Multiboot2 memory-map tag; it does not yet produce the proposed v1 ABI. The kernel currently enters `physical_memory_init()` with the legacy Multiboot2 information address for both the native and GRUB magic paths.
- Re-read `kernel/memory.c`: the existing allocator parser consumes Multiboot2 tags directly, initializes allocator state before completing all input validation, and has no ProsenjitOS v1 parser. This path must remain unchanged until the new parser and adapters are independently tested; do not route a v1 block into the legacy parser.
- Reconciled ambiguities in the v1 specification: optional framebuffer/ACPI absence is represented by all-zero sentinel fields; boot-device kind 0 requires no payload, kind 1 requires a non-empty payload, and unknown kinds reject; the memory map is non-empty, contains at least one usable record, is sorted and strictly non-overlapping; the complete handoff allocation is capped at 16 MiB and must fit below 4 GiB for the initial EAX/EBX entry ABI.
- Confirmed current native stage-2 uses fixed physical buffers (`E820_BUFFER=0x5000`, `MB2_BUFFER=0x6000`) and the existing native magic `0x534F5250`; the proposed v1 magic is distinct (`0x31534F50`). Migration therefore requires a deliberate producer/entry/kernel boundary change and cannot be accomplished by changing a constant alone.
- This checkpoint remains documentation-only. No kernel/assembly source, C ABI header, parser, test, build, QEMU run, or CI workflow was changed or executed. The local runner remains unavailable, so no CI was triggered.
- Documentation commits on `phase1/boot-protocol-contract-20261117`: `f2bd302c825991927251980418eb85f652f0a057` (contract consistency corrections). Next: finish protocol review against memory-manager requirements, then—only after this specification PR is reviewed—create a dependent parser/header/test PR. Keep the GRUB and native BIOS producers on the legacy path until the parser and adapter are proven.


## 2026-11-17 — Boot Protocol v1 bounds and optional-array audit

- Cross-checked the v1 wire table against the current kernel bootstrap mapping and PMM limit. `kernel/memory.h` sets `PROSENJIT_PHYS_LIMIT` to 1 GiB, and the memory-manager comments state that the bootstrap page tables identity-map only the first 1 GiB. Tightened the specification so the complete handoff allocation and every early-read external physical object must remain below 1 GiB until a later high-memory mapping milestone; the 4 GiB EBX limit alone is not sufficient for this kernel today.
- Resolved a contradictory optional-module-array rule: when `modules_count == 0`, both `modules_offset` and `module_entry_size` are zero; when nonzero, the offset is nonzero and entry size is exactly 24. The memory-map array remains mandatory/nonempty. Boot-device absence now consistently requires offset, size, and kind all zero.
- The contract's reserved-range ownership rule is intentionally strict: module payloads and the handoff allocation must be covered by non-usable memory-map entries. Producers must split/normalize firmware descriptors before emission rather than marking loader-owned bytes as available RAM. This needs dedicated parser fixtures for full containment, partial containment, gaps, overlap, and overflow.
- Review remains documentation-only; no source, tests, build, QEMU, or CI were run. PR #647 remains Draft and unmerged; local runner unavailable.


## 2026-11-17 — Boot Protocol v1 memory-ownership and MMIO audit

- Reviewed the canonical v1 wire table and optional metadata rules against the current kernel's 1 GiB identity-map/PMM boundary and the distinction between RAM descriptors and framebuffer MMIO.
- Corrected `mmap_offset` to be mandatory/nonzero, replaced the inaccurate “absence flags” test wording with all-zero sentinel semantics, and clarified that the handoff, modules, and ACPI RSDP must be fully covered by non-usable memory-map entries. Adjacent reserved descriptors may jointly cover a range; gaps or usable-memory overlap reject.
- Explicitly excluded framebuffer MMIO from RAM-map ownership coverage. The protocol parser validates framebuffer address arithmetic and dimensions but does not dereference pixel memory; the eventual display driver must map the range before use. Thus framebuffer MMIO may lie above the current 1 GiB identity-map limit, while the handoff and ACPI bytes that the parser reads must remain accessible below that limit.
- Tightened ACPI revision-2+ RSDP validation to bound its internal declared length by the supplied size and require reserved-range coverage.
- GitHub-hosted Actions is now confirmed available. The latest PR #647 exact-head runs were green, but path filtering skipped kernel build and QEMU because this PR changes documentation only. No runtime validation is claimed by those runs.


## 2026-11-21 — Boot Protocol v1 alignment and ownership cross-check

- Continued the PR #647 specification audit after reviewing the 128-byte header, array bounds, memory-map ownership, optional metadata, and the current 1 GiB bootstrap/PMM limit.
- Clarified that `self_phys` (and therefore EBX) is 8-byte aligned; removed the ambiguous reference to unknown mandatory memory-map records because v1 has no per-record mandatory flag and unknown memory types are reserved.
- Clarified the optional-record policy: all-zero absence sentinels are valid, but malformed/partially populated present framebuffer or ACPI metadata is fatal before PMM initialization.
- Added an explicit kernel-image/live-loader ownership rule. A legacy adapter/parser must carve any retained kernel-owned ranges out of usable descriptors before PMM initialization; reclaimable loader memory is released only after ownership transfer and no retained references.
- Defined initial PMM admission above the existing 1 GiB boundary: full descriptor intervals still receive overflow/physical-width validation, but only the intersection with `[0, 0x40000000)` may enter the current allocator.
- Documentation-only update. No ABI header, parser, boot producer, kernel source, build, host test, QEMU test, or runtime behavior changed. Exact source commit: `c9e5ec8e375aa74c54b8e7ad89086c42e38bf5ab`.
- Next audit: verify the exact E820/Multiboot2/GRUB memory-map semantics against these ownership rules, then re-check the complete spec for any remaining ambiguous wire-level cases. PR #648 remains blocked until PR #647 review is resolved.


## 2026-11-21 — Boot Protocol v1 E820/PMM producer cross-check

- Re-read `boot/native_stage2.asm`, `boot/entry.asm`, `kernel/memory.c`, and `kernel/include/memory.h` on `main` to compare the v1 contract with the actual legacy handoff and allocator behavior.
- Confirmed the native stage-2 E820 collector stores up to 32 firmware records, retains 24-byte entries, maps disabled E820 entries to Multiboot2 reserved type 2, and copies records into the legacy Multiboot2 memory-map tag. It does not sort or normalize overlapping E820 descriptors. The v1 native producer must therefore add explicit sorting/normalization and reject incomplete/contradictory maps rather than copying this legacy array unchanged.
- Confirmed the GRUB/native entry assembly currently preserves the legacy magic and Multiboot2 information pointer; the proposed POS1 register ABI is not implemented. No existing boot entry should be redirected to the v1 parser until an explicit adapter/dispatch boundary exists.
- Confirmed the current PMM clips available ranges to `PROSENJIT_PHYS_LIMIT` (1 GiB), then explicitly reserves low memory, the linked kernel image, and the Multiboot information block. It does not currently parse v1 module or ACPI descriptors. The future v1 path must preserve these exclusions and ensure every referenced module/ACPI range remains non-usable or explicitly reserved before PMM admission.
- The v1 contract's strict sorted/non-overlapping memory-map rule is necessary because the current legacy PMM marks type-1 ranges available and later reserves only selected ranges; it does not resolve conflicting overlapping firmware descriptors. The v1 parser must reject overlap before any allocator state is admitted.
- No source code changed; no build, host test, QEMU test, or CI run was performed. This was a source-level cross-check only.
- Remaining review focus: prove the v1 record/physical range rules cover the full kernel image and every retained loader allocation; verify the 1 GiB boundary and map normalization have unambiguous parser test cases; then perform a final complete wire-contract audit before un-drafting/reviewing PR #647.


## 2026-11-21 — Boot Protocol v1 exact record-size and E820 attribute clarification

- Closed a wire-level inconsistency: the general ABI paragraph previously rejected only undersized entry records while the canonical v1 table required exactly 24-byte records. v1 now explicitly rejects both undersized and oversized memory-map/module entry sizes.
- Earlier notes incorrectly described E820 extended-attribute bit 0 as an enabled flag. ACPI 6.6 defines E820 type 6 as AddressRangeDisabled; extended-attribute bit 0 is reserved and must be 1, while bits 2:1 must be 0. The v1 memory-map `attributes` field remains zero.
- Documentation-only correction. No loader, parser, PMM, or runtime code changed. The documentation commit is `7db4899f7fcbc6abe2e3dcf89528ec7321fd30d3`.


## 2026-11-21 — Boot Protocol v1 allocator viability boundary

- Found one final edge case while reconciling the 1 GiB PMM clipping rule: requiring merely one type-1 record would allow a syntactically valid map whose only usable RAM lies above the current PMM limit, or whose below-limit pages are all consumed by mandatory reservations.
- Tightened the v1 contract to require at least one complete allocatable page below `0x40000000` after low-memory, kernel-image, handoff, module, and retained-loader exclusions. If no page remains, validation/PMM initialization must fail before allocator use.
- Documentation-only correction; no source, tests, build, QEMU, or CI executed. The preceding specification commit is `00614976ca4e9ce619a9d7b56670429c48352375`.


## 2026-11-21 — Boot Protocol v1 final consistency review

- Re-audited PR #647's canonical v1 wire layout and found two specification ambiguities: the earlier generic validation wording said minimum header/record sizes while the canonical v1 table requires fixed sizes; and “8-byte aligned where applicable” did not explicitly name the handoff physical base or distinguish the opaque byte payload.
- Clarified that v1 requires exactly a 128-byte header, exactly 24-byte memory-map records, and exactly 24-byte module records when present. Larger structures/records are not accepted as v1 extensions.
- Explicitly required 8-byte alignment for `self_phys`, the memory-map array offset, and the module array offset. The opaque boot-device byte payload has no extra alignment requirement. Also stated that the 64-bit `self_phys` must equal zero-extended EBX and satisfy the current below-1-GiB early-access limit.
- Documentation-only change. No parser, ABI header, loader, kernel source, build, host test, QEMU run, or runtime behavior changed or was verified by this commit.

## 2026-11-21 — Boot Protocol v1 firmware-source and adapter cross-check

- Cross-checked the v1 contract against the Multiboot2 v2.0 boot-information specification, ACPI 6.6 RSDP rules, and the UEFI memory-type mapping / ExitBootServices requirements.
- Found that normalize overlapping firmware descriptors needed a deterministic rule. Added a conservative interval-splitting policy: gaps remain unavailable; usable is emitted only when all covering descriptors are usable/enabled; any reserved/disabled/unknown conflict wins; final records are sorted and disjoint.
- Confirmed Multiboot2 memory-map entries are allowed to grow beyond 24 bytes, are 8-byte-multiple strides, and carry an entry version. The adapter must accept supported source strides >=24, require version 0, and emit exactly 24-byte POS1 records. Also documented that Multiboot2 type-1 ranges may include kernel, MBI, segments, and modules, so those allocations require carve-outs.
- Confirmed Multiboot2 ACPI old/new tags contain copies of the RSDP, not a physical RSDP pointer. The GRUB adapter must copy the selected validated RSDP into separately reserved early-accessible memory before publishing a POS1 physical address. ACPI BIOS scan alignment is not imposed as a universal v1 pointer-alignment rule.
- Tightened framebuffer translation: unsupported but well-formed source pixel formats become the v1 absent sentinel; malformed tag geometry is rejected; RGBX/BGRX is selected only from exact channel metadata; framebuffer ranges are excluded from PMM admission if a source map labels them usable.
- Added UEFI descriptor-stride/version checks, conservative memory-type admission, pre-ExitBootServices RSDP capture, and bounded GetMemoryMap/ExitBootServices retry requirements. Runtime-services, ACPI, MMIO, unusable, and unsupported persistent/unknown memory remain non-usable under the current v1 PMM policy.
- Re-read boot/native_stage2.asm: the current E820 collector has a 32-record bound and fails rather than silently truncating, but it does not validate the returned E820 record length or normalize overlapping descriptors. Its load-range check accepts one enabled type-1 descriptor covering the requested range and does not detect a conflicting overlapping reserved descriptor. These are explicit requirements for the v1 producer migration; no current BIOS producer behavior was changed.
- Documentation-only update on PR #647. No kernel, loader, parser, ABI header, or test source changed. No build, host test, QEMU run, or runtime validation is claimed. The PR remains Draft pending final specification review and exact-head required checks.


## 2026-11-21 — Boot Protocol v1 final ACPI byte-access audit

- Completed an adversarial review of the v1 ACPI RSDP physical-range contract against both source forms: Multiboot2 ACPI tags carry an in-block byte copy, while UEFI exposes a configuration-table pointer and the adapter creates a reserved copy.
- Clarified that the RSDP storage is read-only metadata and remains reserved; it is not allocator-usable merely because the kernel has read or copied it. Removed the inaccurate implication that every adapter-published copy remains firmware-owned.
- Added an explicit unaligned-source safety rule: v1 does not impose a universal physical-pointer alignment requirement on the RSDP. Parsers must use byte-safe access or copy into aligned local storage and must not dereference an unaligned physical address through a native/packed structure pointer. The published copy remains reserved while consumers may read it.
- Full PR #647 exact-head checks at `8c192dd9722aab4c80a5b7019819c3139c88e994` completed successfully: Security architecture contract passed; documentation contract passed. Kernel build and both QEMU boot jobs were intentionally skipped by changed-path policy because the PR is documentation-only. No runtime or boot-test pass is claimed.
- Documentation-only follow-up. No ABI header, parser, firmware adapter, loader, kernel source, build, host test, or QEMU test was changed or executed.

## 2026-11-21 — Boot Protocol v1 cross-object aliasing audit

- Re-reviewed physical ownership relationships across kernel image, handoff allocation, module payloads, ACPI RSDP storage, and framebuffer MMIO span.
- Found that the earlier module rule explicitly rejected overlap with the kernel image, other modules, and handoff block, but did not explicitly reject module aliases with the ACPI RSDP copy or framebuffer span. Added a mutual-disjointness rule for these physical objects and required producers to reject aliases before publishing v1.
- Tightened framebuffer arithmetic: define RGBX/BGRX as actual in-memory byte order; compute width×4 and pitch×height using at least 64-bit arithmetic; validate declared size and physical-end overflow/address-width; validate the full span against every other physical object.
- Documentation-only specification refinement. No ABI header, parser, loader, kernel source, build, host test, or QEMU test changed or executed.

## 2026-11-21 — Boot Protocol v1 canonical sentinel and negative-test audit

- Reconciled the generic parser-validation sequence and acceptance-test list with the fixed v1 wire contract: version must be exactly 1, header size exactly 128, and memory-map/module records exactly 24 bytes. Negative tests now explicitly include both undersized and oversized records, nonzero reserved bytes, and overlapping in-allocation regions.
- Added a canonical absence-sentinel matrix for framebuffer, ACPI RSDP, modules, and boot-device identity. Each optional record now has an explicit all-zero absence tuple; any partial population is treated as a present record and must pass full validation or reject.
- Documentation-only specification refinement. No ABI header, parser, loader, kernel source, build, host test, or QEMU test changed or executed. The exact documentation commit is recorded by the GitHub update result for this entry.


## 2026-11-21 — Boot Protocol v1 offset versus physical-range audit

- Found ambiguity in the introductory ABI paragraph: it described memory-map/module array locations as physical bases and could be read as bounding external module payload lengths by the handoff allocation's `total_size`.
- Clarified that array locations are byte offsets relative to `self_phys` and only those in-allocation regions are bounded by `total_size`. Module payloads, ACPI RSDP, and framebuffer are external physical ranges with independent overflow, address-width, accessibility, ownership, and alias checks.
- Documentation-only correction. No ABI header, parser, loader, kernel source, build, host test, or QEMU test changed or executed. Documentation commit: d7b3fcaa1c75d98c258b3e58bf41bfc8898b6aba.


## 2026-11-21 — Boot Protocol v1 firmware-adapter test matrix audit

- Reviewed the acceptance tests against the source-normalization requirements and found that the generic malformed-map tests did not enumerate key E820, Multiboot2, and UEFI adapter boundaries.
- Added explicit future test cases for physical-address width and the 1 GiB boundary; conflicting/overlapping/gapped firmware maps; E820 record lengths and bounded continuation; Multiboot2 tag alignment, duplicates, variable entry stride and framebuffer channel mapping; and UEFI descriptor stride, arithmetic, ACPI capture, ExitBootServices retry, and post-exit service prohibition.
- These are required test cases for the future adapter/producer implementation, not tests executed by this documentation PR. No source code, build, host test, or QEMU test changed or ran. Specification commit: 72596b7a2f398c33d2355985c6e1675dc88d3d91.


## 2026-11-21 — Boot Protocol v1 canonical header-table reconciliation

- Reconciled two remaining table-versus-constants inconsistencies in the canonical 128-byte wire layout: the header flags field now explicitly says all bits must be zero in v1, and the framebuffer format field now lists the already-defined v1 values (0 absent, 1 RGBX8888, 2 BGRX8888).
- This is a documentation-only consistency correction. No ABI header, parser, loader, kernel source, build, host test, QEMU test, or runtime behavior changed or was verified.


## 2026-11-21 — Boot Protocol v1 flag and offset-alignment audit

- Found stale wording in the canonical layout introduction that allowed the possibility of defined header flag bits, despite the v1 constants requiring the entire flags field to be zero. Replaced it with the exact all-zero rule.
- Replaced the ambiguous “8-byte aligned where applicable” region statement with explicit rules: header occupies bytes `[0,128)`, memory-map and present module-array offsets are 8-byte aligned, and the opaque boot-device byte payload has no extra alignment requirement. All present regions remain bounded and mutually non-overlapping.
- Documentation-only correction; no ABI, parser, loader, kernel source, build, host test, QEMU test, or runtime behavior changed or verified.


## 2026-11-21 — external physical range null/empty validation

- Clarified that a present framebuffer, module payload, and ACPI RSDP range must have a nonzero physical base and nonzero byte length before checked end calculation or physical access. This removes ambiguity where a partially populated framebuffer could otherwise use physical address zero as its base.
- Added negative acceptance cases for zero-based and zero-sized external ranges. Documentation-only; no parser, producer, kernel, or runtime tests were implemented.


## 2026-11-21 — Boot Protocol v1 physical-address-width contract

- Closed a specification gap: prior text required physical-address-width checks and tests but did not define how the width is discovered or the exact accepted half-open interval boundary.
- Defined effective MAXPHYADDR discovery from CPUID.80000008H:EAX[7:0], with the architectural legacy fallback based on CPUID.01H:EDX.PAE; documented 32..52-bit validity bounds and `base < 2^MAXPHYADDR`, checked `end <= 2^MAXPHYADDR` for all non-empty physical intervals.
- Applied the same rule to memory-map descriptors and all physical objects; retained the 1 GiB early-dereference constraint as an additional independent limit.
- Added boundary and CPUID fallback negative/positive test requirements. Intel SDM corroborates the CPUID field, fallback, and 52-bit maximum; see https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf.
- Documentation-only. No CPUID implementation, parser, producer, kernel, build, or runtime test was changed or executed.


## 2026-11-21 — MAXPHYADDR legacy fallback correction

- Tightened the legacy CPUID fallback for the x86-64 target: when CPUID.80000008H is unavailable, PAE must be present and the fallback width is 36 bits. A processor without PAE is rejected rather than accepting a 32-bit physical width as a valid long-mode environment.
- Documentation-only consistency refinement; no runtime CPUID or boot code was changed.


## 2026-11-21 — E820 partial extended-record audit

- Rechecked `boot/native_stage2.asm` E820 collection against the adapter contract. The current collector preinitializes the 24-byte extension to enabled, requests 24 bytes, but does not inspect the BIOS-returned ECX before consuming the record; it then reads the extension dword unconditionally when normalizing the map.
- Made the v1 producer rule precise for returned lengths 20–23: accept the complete legacy 20-byte base record but ignore incomplete trailing extension bytes; only inspect the extended-attribute enabled bit when at least 24 bytes were returned. Reject lengths below 20. Added explicit tests for 21–23-byte partial responses and 24-byte-or-larger enabled/disabled records.
- This exposes a legacy source-path correctness gap to fix during native BIOS producer migration; no current boot code was changed. No build, test, QEMU, or runtime result is claimed.


## 2026-11-21 — Multiboot2 tag, module, and framebuffer adapter audit

- Re-read the current legacy `kernel/memory.c` parser and the Boot Protocol v1 GRUB/Multiboot2 adapter contract. The legacy parser does not validate the Multiboot2 information base's 8-byte alignment, accepts a type-0 tag without first checking its size, does not prove the terminator is the final tag at the exact declared block end, and does not validate that the memory-map tag's entry version is zero or that the payload is an exact multiple of its declared stride. It also ignores module and framebuffer tags entirely. These are legacy-parser limitations; no current parser behavior was changed.
- Cross-checked the source-format rules against the GNU GRUB Multiboot2 Specification v2.0: the information structure begins 8-byte aligned; tag headers are 8 bytes; tag starts are 8-byte aligned; the end tag is type 0/size 8; module tags contain 32-bit start/end plus a bounded NUL-terminated UTF-8 string and may repeat; framebuffer information has a 32-byte fixed prefix and RGB channel metadata follows it. Source: https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html.
- Tightened the v1 adapter contract: require aligned information base, total_size >=16 and divisible by 8, bounded tag sizes and checked aligned advancement, and a single exact final end tag with no trailing bytes. The fixed reserved dword remains ignored as required by the specification.
- Added module source validation: each module tag must include the 16-byte fixed prefix plus a NUL byte; require start < end; validate the complete non-empty physical payload and ownership/alias rules. Since Multiboot2's module command string is arbitrary and does not define ProsenjitOS module type or optionality, require an explicit loader manifest/config mapping; do not infer type or optionality from path/name.
- Added framebuffer source validation: require the 32-byte fixed prefix; RGB tags must include channel metadata and only exact 32-bpp 8-bit RGB channel layouts can map to v1 RGBX/BGRX. Validate indexed palette count/size before mapping it to the v1 absent sentinel; unsupported but well-formed formats become absent, while truncated/malformed source tags reject.
- Documentation-only specification update. No parser, ABI header, GRUB adapter, kernel, loader, build, host test, QEMU boot, or runtime behavior changed or was executed. Current legacy source observations are not implementation claims.


- RGB byte-order cross-check: Multiboot2 channel positions are bit positions within the little-endian pixel value. Therefore red/green/blue positions 0/8/16 map to v1 RGBX byte order, while 16/8/0 map to v1 BGRX byte order. The boot architecture contract was corrected to state this explicitly before validation.


- Follow-up boundary review: the complete Multiboot2 source information block itself must be readable below the current 1 GiB early identity-map limit while the adapter parses it, and must remain reserved until all required source data has been copied/consumed. Also reject duplicate framebuffer singleton tags instead of silently selecting one. These are adapter input rules; no runtime code was changed.


- Completed the generic Multiboot2 tag policy: unknown information tags may be skipped only after their common header, minimum size, checked payload end, and aligned next-tag position are validated. The adapter must never inspect an unknown payload. This avoids rejecting forward-compatible tags while preserving strict bounds checking.


- Final Multiboot2 source-boundary refinement: cap the source information block at 16 MiB (matching the v1 handoff allocation cap), require at least one source memory-map entry, and validate module command strings as bounded UTF-8/NUL data. These prevent oversized source metadata, an empty mandatory memory map, and unbounded/invalid module labels. Documentation only; no source implementation or tests were changed/run.


- Multiboot2 mmap entry reserved-field handling was made explicit: the adapter ignores the source entry's reserved dword rather than assigning semantics or rejecting a nonzero value, while still translating only the defined base/length/type fields. Added a nonzero-reserved-field test case. Documentation-only.


## 2026-11-21 — Multiboot2 legacy parser arithmetic and termination audit

- Re-read the complete current `kernel/memory.c` implementation on the PR branch. Additional concrete legacy parser gaps:
  - It does not require the information-block base to be 8-byte aligned or prove the full source block lies below the current 1 GiB early identity-map limit before dereferencing it.
  - It checks `current + tag->size > end` using an addition that can wrap before comparison; the adapter contract now explicitly forbids unchecked tag-end arithmetic.
  - It breaks on tag type 0 without validating the terminator size is exactly 8 or that the terminator ends exactly at `total_size`; it can also leave the loop without seeing a terminator and continue to allocator setup.
  - Its mmap loop silently ignores a trailing partial entry because it only iterates while a full stride remains; it does not require exact payload divisibility, nonzero mmap entry count, or entry_version == 0.
  - It accepts duplicate mmap tags and has no explicit requirement that a valid mmap tag was seen.
  - It uses typed pointer casts for the MBI, tags, and entries; future adapter code must ensure alignment before typed access or use byte-safe loads.
- Tightened the v1 GRUB adapter rules and negative-test matrix: reject missing/duplicate mmap tags, reject missing or non-final end tags, reject tag-end arithmetic wrap, and reject partial mmap-entry tails rather than silently ignoring them.
- These findings describe current legacy source only. This commit changes documentation only; no parser/kernel source, build, host test, or QEMU runtime was changed or executed.


- Follow-up PMM source review: current `kernel/memory.c` admits every Multiboot2 type-1 entry by calling `mark_available()` and ignores non-type-1 entries rather than subtracting them from overlapping usable ranges. Thus a reserved descriptor overlapping a usable descriptor does not reliably win; admission can depend on firmware descriptor contents/order. The current PMM also consumes records directly without sorting, splitting, or merging. The v1 adapter must normalize the full source map before PMM exposure, with non-usable/reserved/disabled/unknown ranges winning overlaps and gaps remaining unavailable.
- Added explicit order-independence tests: feed identical usable/reserved overlaps in both input orders and require identical sorted, disjoint output; include nested, partial, adjacent, and exact-boundary overlaps, plus gaps. This remains a documented migration requirement, not a claim about current behavior.


## 2026-11-21 — UEFI implementation inventory checkpoint

- Inspected the current repository `boot/` directory and searched for UEFI entry points and Boot Services interfaces: `EFI_SYSTEM_TABLE`, `EFI_BOOT_SERVICES`, `EFI_MEMORY_DESCRIPTOR`, `GetMemoryMap`, and `ExitBootServices`.
- The current boot directory contains the legacy native BIOS assembly path and GRUB/Multiboot2 configuration/header/entry files. No UEFI application entry point, EFI loader source, or executable memory-map/ExitBootServices implementation was found.
- Confirmed the UEFI descriptor-stride/version, ACPI Configuration Table capture, map-key retry, and post-exit service prohibition rules are future producer requirements in `docs/BOOT-ARCHITECTURE.md`, not implemented behavior.
- No UEFI source was available for runtime/source-path auditing; no UEFI build, host test, firmware boot, or QEMU UEFI test was run or claimed.
- This checkpoint changes documentation only. Re-run the inventory and perform line-by-line source audit when a UEFI implementation is added.


## 2026-11-21 — Boot Protocol v1 entry-dispatch and sentinel consistency audit

- Cross-checked the contract's entry-register description against the actual `boot/entry.asm`, `boot/native_stage2.asm`, and `boot/grub.cfg` sources.
- Found an ambiguity: the contract's generic EAX/EBX wording could be read as allowing the current legacy GRUB Multiboot2 magic or native BIOS magic to enter the v1 parser directly. The sources show GRUB enters with Multiboot2 magic `0x36D76289`, while native stage-2 currently enters with legacy magic `0x534F5250`; both pass a Multiboot2-compatible information buffer. Clarified that only a v1 producer uses `0x31534F50`, and legacy magic dispatches to its adapter path before normalized v1 parsing.
- Corrected the boot-device table's zero-value description from ambiguous “unspecified” to the exact v1 absence meaning; kind 0 is absent, kind 1 is the supported opaque payload, and other kinds reject.
- Documentation-only specification correction. No boot entry, loader, kernel, ABI header, parser, build, host test, or QEMU runtime was changed or executed.


#### v1 final field-table reconciliation — 2026-11-21

- Reconciled the canonical header table and boot-device payload prose with the later sentinel matrix: `boot_device_kind = 0` means absent (and requires zero offset and size), `1` is the only supported v1 payload kind (and requires a non-empty payload), and all other values reject. The earlier wording “zero means unspecified” was ambiguous and is removed.
- Cross-checked the current contract's exact header offsets against the 128-byte header declaration; the fixed field widths and final 24-byte reserved tail account for the full header without implicit padding. This is a documentation-level arithmetic check only; compile-time `offsetof`/sizeof assertions remain required in the future ABI-header implementation.
- Reconfirmed that ACPI RSDP revision 0 uses the 20-byte legacy checksum, revision 1 is reserved/unsupported, and revision 2 or later requires the extended length and checksum checks. No parser or ACPI runtime test exists yet.
- No ABI, kernel, loader, or executable test code changed in this checkpoint.


#### v1 reserved-ownership and bounded-E820 test reconciliation — 2026-11-21

- Closed an ownership-description gap: the kernel image and every loader allocation retained after handoff now have the same explicit full non-usable memory-map coverage requirement already applied to the handoff allocation, modules, and ACPI RSDP. A producer/adapter must carve live objects out of firmware-usable intervals into explicit reserved coverage; simply excluding them internally from PMM while publishing them as usable is not a valid normalized v1 map.
- Made the bounded E820 overflow acceptance case unambiguous: if the continuation token says more descriptors remain after the 32-record buffer fills, the producer must fail the boot. It must not truncate the map or claim success with an incomplete view.
- These changes reconcile ownership semantics and test expectations only. No boot code, ABI header, parser, or runtime test was added or executed.


#### v1 acceptance-test completeness audit — 2026-11-21

- Compared the detailed wire rules against the top-level acceptance checklist. The contract specified alignment, non-overlap, reserved-map ownership, ACPI length/checksum, and framebuffer geometry, but the acceptance list did not enumerate enough of those exact boundary cases to make implementation review deterministic.
- Added explicit tests for v1 base/array alignment and the deliberately unaligned opaque byte payload; all in-allocation region pairings; full reserved coverage of handoff/kernel/modules/RSDP/retained-loader objects including adjacent-record success and gap/usable-overlap rejection; framebuffer MMIO non-coverage plus alias rejection; ACPI revision/declared-length/checksum boundaries; and framebuffer pitch/size/overflow/above-1-GiB behavior.
- Documentation-only test-plan refinement. No parser or producer tests have been implemented or executed.


## 2026-11-21 — Boot Protocol v1 E820, Multiboot2, and UEFI source-boundary closure

- Re-read the PR-head sources `boot/native_stage2.asm`, `boot/entry.asm`, `boot/grub.cfg`, and `kernel/memory.c`, and cross-checked the documented adapter contracts against the GNU GRUB Multiboot2 Specification 2.0 and UEFI Specification 2.10.
- Confirmed a concrete E820 legacy-source gap: `collect_e820` requests 24 bytes and preinitializes the extension dword to 1, but never checks the BIOS-returned ECX before the extension is consumed by `validate_usable_range` and `build_mb2_info`. Thus a 21–23-byte response can incorrectly inherit the preinitialized enabled bit. The future producer must capture/validate returned ECX, reject <20, ignore extension bytes for 20–23, and inspect the enabled bit only for >=24. No legacy source fix was made in this docs-only PR.
- Reconfirmed the current E820 bounded collector rejects a nonzero continuation token once its 32-record buffer is full. This is a source observation only; the exact final-slot and incomplete-response cases remain acceptance tests.
- Confirmed the current GRUB/Multiboot2 parser in `kernel/memory.c` still has the previously recorded alignment, unchecked tag-end arithmetic, terminator, partial mmap-tail, duplicate/missing mmap, and module/framebuffer handling gaps. No current parser code was changed.
- Added explicit Multiboot2 acceptance cases for mmap entry sizes below 24, exact 24, compatible larger strides, non-8-byte strides, nonzero entry versions, and partial payload tails; repeated module ordering/string bounds; ACPI type-15 preference/type-14 fallback and malformed higher-priority rejection; framebuffer reserved-byte ignore semantics; and source tag padding boundaries.
- Rechecked UEFI 2.10 GetMemoryMap and ExitBootServices requirements. Added tests for descriptor stride/version/truncation, zero page counts, page-byte and physical-end overflow, explicit memory-type classification, GetMemoryMap buffer slack and EFI_BUFFER_TOO_SMALL handling, stale MapKey and bounded retry exhaustion, and the restriction that after the first ExitBootServices attempt only Memory Allocation Services may be called for retry. After successful exit, Boot Services and device-handle protocols are unavailable.
- The repository still contains no UEFI application/loader or executable GetMemoryMap/ExitBootServices path; UEFI checks are future requirements, not implementation findings.
- Documentation-only change. No ABI header, parser, producer, loader, executable test, build, firmware test, or QEMU runtime was implemented or run. No PASS is claimed for runtime or producer behavior.
- References: UEFI Specification 2.10 https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html ; GNU GRUB Multiboot2 Specification 2.0 https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html .


## 2026-11-21 — Native BIOS E820 normalization and PMM ownership audit

- Re-read the complete current `boot/native_stage2.asm` E820 collector, load-range checker, E820-to-Multiboot2 serializer, and the `kernel/memory.c` physical allocator initialization path.
- Confirmed `validate_usable_range` accepts a load/bounce/kernel range when any single enabled type-1 E820 entry covers it. It does not account for overlapping reserved, disabled, or unknown descriptors. A conflicting reserved range can therefore be ignored before BIOS disk reads or kernel copies. This is a concrete safety gap in the legacy BIOS source path.
- Confirmed adjacent usable E820 records are not combined by the current single-record checker. This causes conservative false rejection for ranges covered by adjacent usable intervals; the v1 producer must normalize first and evaluate the complete usable union.
- Confirmed `build_mb2_info` serializes E820 records in source order and only maps disabled records to Multiboot2 type 2. It does not split overlaps, apply reserved-wins conflict resolution, sort, or merge adjacent equal-type intervals.
- Confirmed `physical_memory_init` marks every Multiboot2 type-1 record available and ignores reserved/unknown records. A reserved record overlapping a usable record therefore does not subtract pages from allocator admission; result can depend on firmware input contents/order. Fixed low-memory/kernel/MBI reservations do not cover arbitrary reserved overlaps.
- Reconfirmed the legacy parser does not require a final end tag, does not require exactly one mmap tag, can silently skip a partial mmap-entry tail, and does not validate mmap entry_version or normalized ordering. Do not treat this path as a v1 parser.
- Added targeted source findings and regression requirements to `docs/BOOT-ARCHITECTURE.md`: overlap shapes and input-order invariance, adjacent usable coverage versus gaps, BIOS write-target preflight, PMM reserved-page exclusion, complete low-memory/live-allocation reservations, and the 1 GiB clipping boundary.
- No BIOS loader, PMM, parser, or adapter source code changed. No compiler, host regression, CI, BIOS firmware, or QEMU runtime was run. This is documentation-only; no runtime PASS is claimed.


## 2026-11-21 — Multiboot2 legacy PMM parser boundary audit

- Inspected the full current `kernel/memory.c::physical_memory_init` parser and its caller in `kernel/kernel.c`.
- Confirmed the information-block pointer is not checked for 8-byte alignment before typed reads; the total-size field is dereferenced before alignment validation.
- Confirmed `current + tag->size` is used in a bounds comparison without checked addition. The aligned-next calculation checks addition of 7 but not the preceding tag-end calculation.
- Confirmed end-tag handling breaks immediately without validating size == 8 or exact placement at the end of the information block; missing end tag is not explicitly rejected.
- Confirmed exactly one mmap tag is not enforced. Duplicate mmap tags are processed; missing mmap is detected only indirectly if no free pages were admitted.
- Confirmed mmap entry_version is not checked, entry_size only has a >=24 lower bound, non-8-byte strides are accepted, and a trailing partial entry is silently ignored.
- Confirmed mmap entries mutate global allocator bitmaps as they are parsed. A later malformed tag returns an error after partial state mutation. The current kernel caller does halt on nonzero return before normal allocation, but this does not make the parser transactional or safe for future recoverable paths.
- Confirmed the legacy PMM parser ignores module/framebuffer/ACPI tags and does not normalize overlapping usable/reserved map entries before marking pages available. These findings reinforce that it is a narrow legacy consumer, not the v1 boot-protocol parser.
- Added detailed acceptance cases to `docs/BOOT-ARCHITECTURE.md` for alignment, total-size bounds, checked tag arithmetic, exact end-tag placement, exactly-one mmap tag, stride/version/remainder validation, transactional PMM admission, singleton tag validation, and fail-closed caller behavior.
- No source implementation changed. No host tests, compiler, CI, GRUB/QEMU runtime, or firmware test was run. Documentation-only; no PASS is claimed.


## 2026-11-21 — ACPI, framebuffer, and module ownership audit

- Inspected `boot/native_stage2.asm`, `kernel/kernel.c`, `kernel/memory.c`, `kernel/display.c`, and `kernel/include/display.h` on the PR branch.
- Native BIOS stage-2 emits only a Multiboot2-compatible mmap tag plus end tag; it currently emits no module, framebuffer, or ACPI tags.
- `kernel_main` distinguishes the legacy native and GRUB magic values for logging/acceptance, but both paths call the same `physical_memory_init` parser. There is no current v1 normalization adapter between entry and PMM.
- The PMM parser ignores module tags and does not reserve module payload ranges. A GRUB handoff with modules is therefore not proven safe: payload pages could be admitted if the firmware map labels them usable.
- No ACPI/RSDP parser or RSDP source reference was found in the inspected kernel source/search; signature, revision, checksum, length, physical-width, and ownership validation are not implemented in this path.
- No Multiboot2 framebuffer parser or boot-framebuffer consumer was found. The display layer is a generic backend API; its test backend uses a synthetic framebuffer address and is not boot framebuffer evidence.
- The generic display info validation checks dimensions, pitch >= width, and nonzero framebuffer/size, but does not derive and checked-multiply the required format-aware pitch-by-height byte span or ensure framebuffer_size covers the visible surface. The present path only bounds caller size against the advertised size.
- Added ACPI/RSDP validation and negative-test requirements (legacy/extended checksums, revision-specific lengths, truncation, overflow, address width, ownership), framebuffer requirements (format-aware checked span, MMIO mapping and alias exclusion), and module ownership/reservation requirements (bounds, pairwise overlap, reserved-map coverage, PMM exclusion, explicit reclamation) to `docs/BOOT-ARCHITECTURE.md`.
- No implementation changed. No compiler, CI, QEMU, firmware, ACPI, real framebuffer, or module runtime test was run. Documentation-only; no PASS is claimed.


## 2026-11-21 — Multiboot2 specification and PMM overflow addendum

- Cross-checked the legacy parser against the GNU GRUB Multiboot2 specification (information block/tag alignment, mmap entry layout, and loader-owned memory rules).
- Confirmed the MBI and tags require 8-byte alignment; current parser does not validate MBI alignment before dereferencing.
- Confirmed mmap `entry_size` is guaranteed to be a multiple of 8 and current `entry_version` is 0. Current parser accepts non-multiple-of-8 strides and nonzero versions.
- Confirmed the specification allows the MBI to be placed anywhere in memory, while the current bootstrap identity map covers only 0..1 GiB. Current parser reads `total_size` before checking whether the complete MBI is accessible under the active early mapping; an above-1-GiB MBI can fault rather than fail cleanly.
- Found an additional concrete PMM admission bug in `mark_available`: when `base + length` wraps, the helper substitutes the 1-GiB physical limit and marks the remainder of the low physical range available. A malformed overflowing type-1 descriptor can therefore expose pages to PMM. Overflow must be rejected before allocator state changes; only a valid non-wrapping interval may be clipped to the allocator's 1-GiB ceiling.
- Clarified that the legacy Multiboot2 information-block reserved dword is specified as ignored by the OS image; it must not be confused with reserved-byte validation in the new ProsenjitOS v1 ABI.
- Added source-backed acceptance tests and the specification reference to `docs/BOOT-ARCHITECTURE.md`.
- No implementation changed; no compiler, runtime, QEMU, or firmware test was run. Documentation-only.


## 2026-11-21 — Native BIOS/GRUB entry-state and BSS equivalence audit

- Re-read `boot/entry.asm`, `boot/native_stage2.asm`, `boot/grub.cfg`, `scripts/build.sh`, `kernel/linker.ld`, `kernel/kernel.c`, `kernel/memory.c`, and `kernel/paging.c` on the PR branch.
- Confirmed the native BIOS stage-2 explicitly sets the legacy native magic in EAX and the Multiboot2-compatible information-block address in EBX, then jumps to the linked `_start` offset. `_start` captures those two 32-bit values before replacing ESP and constructing the bootstrap page tables.
- Cross-checked the GRUB path against the GNU Multiboot2 i386 machine-state contract: EAX/EBX carry the magic and physical MBI address; CS and data segments are flat 32-bit segments; paging is disabled; interrupts are disabled; and GDTR may be invalid. The current entry code does not reload segment registers before loading its own GDT, and it disables interrupts immediately. The native stage-2 path installs its own flat GDT/data segments and enables A20 before jumping. These register/segment observations are source-level only.
- Found a concrete native-vs-GRUB initialization gap: `scripts/build.sh` converts the linked ELF to a raw image with `objcopy -O binary` for native BIOS, while `kernel/linker.ld` defines `.bss` as a `SHT_NOBITS` section bounded by `__kernel_bss_start`/`__kernel_bss_end`. Native stage-2 loads only the raw file's sectors and has no BSS-zeroing step before jumping to `_start`. The common entry only clears its page-table array; it does not clear the full linker-defined BSS interval. Thus zero-initialized static-storage objects and BSS-backed assembly storage are not guaranteed to start at zero on native BIOS, unlike the ELF/GRUB load path.
- Verified the kernel uses BSS-backed state beyond the entry page tables, including allocator bitmaps/counters in `kernel/memory.c` and `mmio_next_virtual` in `kernel/paging.c`; some components explicitly reset their state, but there is no global BSS clear. The linker also places bootstrap stacks, page tables, TSS, and IST stacks in BSS.
- Added a required native producer/entry contract and regression matrix to `docs/BOOT-ARCHITECTURE.md`: validate linker BSS bounds, zero the complete BSS interval before BSS-dependent execution, preserve handoff registers and low-memory stage-2 stack, test nonzero prefill/sentinels/bounds, and compare GRUB/native initial state.
- Documentation-only update. No loader, entry assembly, linker script, kernel source, ABI, or parser was changed. No build, host test, BIOS firmware, or QEMU runtime was run; no runtime PASS is claimed.
- References: GNU GRUB Multiboot2 Specification section 3.3/3.6.1 (https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html); GNU Binutils `objcopy` raw-binary behavior (https://sourceware.org/binutils/docs/binutils/objcopy.html).

## 2026-11-21 — Native kernel file extent versus complete memory extent

- Followed the native image-size calculation in `scripts/build.sh`: `objcopy -O binary` output length determines `KERNEL_BYTES` and `NATIVE_KERNEL_SECTORS`; the linked `_start` symbol is separately resolved relative to the 1-MiB load address.
- Followed `boot/native_stage2.asm`: `validate_load_ranges` validates the BIOS bounce buffer and `[KERNEL_LOAD, KERNEL_LOAD + KERNEL_SECTORS * 512)` only. It does not validate the complete linker-defined `[__kernel_start, __kernel_end)` memory extent or the BSS tail.
- Cross-checked `kernel/linker.ld`: `.bss` is after `.data`, bounded by `__kernel_bss_start`/`__kernel_bss_end`, and `__kernel_end` follows it. Because BSS is `SHT_NOBITS`, it is not equivalent to the raw flat file byte count. GNU ld documentation distinguishes NOBITS/NOLOAD memory allocation from ordinary file payload behavior (https://sourceware.org/binutils/docs/ld/Output-Section-Type.html).
- Confirmed `kernel/memory.c::physical_memory_init` later reserves the complete linker interval `[__kernel_start, __kernel_end)` from PMM. This protects the kernel/BSS from allocator reuse after PMM initialization, but does not prove that the native loader's BSS range is valid writable usable RAM or zeroed before `_start` executes.
- Added a required image-manifest distinction between flat file extent and complete memory extent, checked arithmetic, complete E820 coverage, reserved-wins overlap policy, loader/partition collision checks, pre-entry BSS clearing, PMM reservation assertions, and targeted regression cases to `docs/BOOT-ARCHITECTURE.md`.
- Important nuance: this is not a claim that PMM currently allocates kernel BSS after initialization; source explicitly reserves the linker range. The finding is the earlier native-loader validation/initialization gap before that later reservation can protect it.
- Documentation-only. No build script, loader, linker script, entry assembly, kernel source, or tests were changed or run. No runtime PASS is claimed.

## 2026-11-21 — E820 descriptor end-overflow audit

- Rechecked `boot/native_stage2.asm::validate_usable_range` after the full kernel memory-extent audit.
- Confirmed that when an enabled type-1 E820 entry has a low 32-bit base but a nonzero high dword in its 64-bit length, the current code immediately treats it as covering the requested low-memory range. It does not first validate the full-width exclusive end `base + length`.
- A malformed descriptor whose 64-bit end wraps can therefore be treated as a valid covering range during pre-write validation for the BIOS bounce buffer or kernel load. The high length dword alone is not proof of a valid interval.
- Added a fail-closed full-width checked-add requirement and regression cases (valid crossing 4 GiB, length/base overflow, zero length, exact end, one byte short) to `docs/BOOT-ARCHITECTURE.md`.
- Source/documentation audit only; no stage-2 implementation, test, build, firmware, or QEMU run was performed.

## 2026-11-21 — Native BIOS E820 carry-flag termination audit

- Cross-checked `boot/native_stage2.asm::collect_e820` against ACPI 6.6 section 15.1.
- Found that the collector treats every CF-set E820 response as fatal. The specification allows a BIOS to report that the final descriptor was already returned on the previous iteration by setting CF on the subsequent call; other returned register/buffer contents must then be ignored.
- A later carry after at least one valid descriptor can therefore be a valid end-of-map indication, rather than an incomplete-map error. A carry on the first call still cannot provide a usable map and must fail.
- Added the distinction and boundary tests to `docs/BOOT-ARCHITECTURE.md`, including first-call carry, later carry after a nonzero continuation token, ordinary EBX=0 termination, exact 32-record completion, and overflow past the 32-record capacity.
- No loader implementation or tests changed; no build, firmware, or QEMU runtime was run. Documentation/source audit only; no runtime PASS is claimed.
- Reference: https://uefi.org/specs/ACPI/6.6/15_System_Address_Map_Interfaces.html .


## 2026-11-21 — E820 returned descriptor length upper-bound audit

- The BIOS collector passes ECX=24 but currently does not inspect the returned ECX. The E820 interface defines returned length as bounded by the caller's supplied buffer size; ECX>24 is therefore a malformed response and must fail closed.
- Tightened the exact response-length matrix: reject below 20 and above 24; use only legacy fields for 20–23; inspect the extension only at exactly 24. Carry-set termination is separate and all outputs from a CF-set call are ignored.
- Added 19/20/21/23/24/25-byte boundary cases and an extension-prefill sentinel test to the canonical boot contract.
- Documentation-only; no loader or tests changed and no build/firmware/QEMU runtime was run.
- Reference: ACPI 6.6 section 15.1, https://uefi.org/specs/ACPI/6.6/15_System_Address_Map_Interfaces.html .


## 2026-11-21 — ACPI 6.6 E820 attribute semantics correction

- Cross-checked the E820 type and extended-attribute tables in ACPI 6.6 section 15. The current specification defines type 6 as AddressRangeDisabled. Extended-attribute bit 0 is reserved and must be set to 1; bits 2:1 are reserved and must be zero. Bit 0 is not an enabled/disabled flag in this contract.
- Found stale assumptions in the native stage-2 comments and checks: `validate_usable_range` and `build_mb2_info` interpret bit 0 as an enable flag. Type 6 itself is already non-usable under the type-1-only load-range check and PMM policy, so this does not mean the current allocator admits disabled memory. It is a specification mismatch and a conservative false-rejection/validation gap; malformed reserved bits are not explicitly rejected.
- Corrected the v1 producer requirements and previous architecture test wording: classify disabled ranges by type 6, validate complete 24-byte reserved-bit constraints, ignore incomplete extension bytes for 20–23-byte responses, and never promote non-type-1 ranges to usable RAM.
- Documentation/source-audit only. No loader implementation, tests, build, firmware, or QEMU runtime was changed or executed.
- Reference: ACPI 6.6 Tables 15.1, 15.4, and 15.5, https://uefi.org/specs/ACPI/6.6/15_System_Address_Map_Interfaces.html .
