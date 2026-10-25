## FS-28 — truncate-shrink partial-block tail isolation (**PASS** on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- [x] Source audit found a privacy/correctness gap: shrink retained the final allocated data block without clearing bytes beyond a non-block-aligned new EOF; a later write past EOF could re-expose pre-truncate bytes.
- [x] Updated `pfs_file_truncate()` to locate the retained EOF block before changing the extent chain, zero the suffix, and add that image to the same redo transaction as the inode size and any extent/bitmap changes. Duplicate transaction targets fail closed; staging errors reset the volatile transaction.
- [x] Added a native self-test: truncate `/hello.bin` to 100 bytes, write one byte at offset 200, then require bytes 100–199 to read as zero and byte 200 to match the new payload.
- [x] Preserved the version-1 on-disk format; the change uses the existing redo journal.
- [x] Code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f` passed [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332). Logs include successful ProsenjitFS self-test, LBA-boundary diagnostic, and ATA/MBR persistent-mount markers in ISO and native BIOS QEMU runs.
- **Durability scope:** deterministic mock-device/QEMU evidence only; no physical-device power-loss certification or general concurrent-mutation guarantee.

## FS-29 — inode allocator output on transaction failure (**PASS** on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- [x] Source audit found `prosenjitfs_alloc_inode()` published the candidate inode number before the inode-bitmap redo transaction completed.
- [x] The output is now assigned only after `pfs_inode_bitmap_update()` succeeds; all error returns preserve the caller's value.
- [x] Added a mock-device regression that fails the first journal write during inode reservation and checks `EIO`, unchanged output sentinel, unchanged durable/volatile/dirty media, writable mount state, and a clean integrity scan.
- [x] The existing v1 metadata format and journal protocol are unchanged.
- [x] Code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f` passed [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332); the native self-test marker is present in both QEMU boot paths.

## FS-30 — journal-retirement I/O error propagation (**PASS** on code/test head `5aa0569a24093b11aee7685c7f3f136f02aaaded`)

- [x] Replaced undocumented `-101..-106` returns in `pfs_journal_clear()` with the filesystem's named validation/corruption errors and direct propagation of the original LBA, write and flush status.
- [x] Added a mount-recovery regression for both journal-retirement write failure and flush failure. Each starts from a stage-4 interrupted transaction with durable header/images and no matching commit, requires mount to return `PROSENJITFS_EIO`, then retries recovery and verifies the old payload and integrity.
- [x] No on-disk v1 format or journal protocol changes.
- [x] [Build/boot-test #8590](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503632) and [Security architecture #8126](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503601) passed; logs show the filesystem self-test marker in ISO and native BIOS QEMU runs.

## FS-31 — committed-journal replay write failure (**PASS** on code/test head `73bff7fe8aad278e04f08ee7594b901bb11da6f9`)

- [x] Added a regression that interrupts a file unlink at stage 5, confirms the durable journal header and matching commit, then fails the second home-block write during mount recovery.
- [x] The test requires recovery mount to return EIO, simulates power loss to discard the partial unflushed replay, retries mount, and verifies the committed unlink is fully applied and integrity is clean.
- [x] The v1 disk layout and redo-journal protocol are unchanged.
- [x] [Build/boot-test #8593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866752) and [Security architecture #8129](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866715) passed on this code/test head. Logs contain the filesystem self-test marker in ISO and native BIOS QEMU paths; replay-write fault, power-loss retry, unlink state and integrity assertions are inside that self-test.

## Part 2 verification checkpoint — 2026-10-25

- [x] Inode-record admission: `prosenjitfs_read_inode()` rejects checksum-valid records with unsupported type/flags, zero generation, invalid parent, oversized logical size, or invalid allocated-byte accounting. Seven malformed-record cases restore the root and end with a passing integrity scan.
- [x] Directory-index ownership: readers and callers supply the expected owner ID; a CRC-valid index naming another in-range inode is rejected, and the original index is restored through the journal path.
- [x] Namespace child references: path lookup and readdir reject dangling references, entry/inode type mismatch, wrong parent, and direct directory self-links. Corruption regressions restore original metadata and verify integrity.
- [x] On-disk v1 layout/version unchanged. No physical-device durability or general concurrency claim.
- [x] Exact source head `70ed44cd45987682ac50f9fbfd1b82a510e25033` passed [Build/boot-test #8548](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283756) and [Security architecture #8084](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283791). Build includes ISO and native BIOS QEMU boot, ProsenjitFS self-test, LBA-boundary diagnostic, and ATA-to-MBR persistent-mount checks.
- [ ] Broader P2/P3/P4 work remains open: systematic read/write/flush fault sweeps, additional journal recovery cut points, and remaining metadata/extent/namespace boundary cases.
## Part 2 continuation — 2026-10-25

## Integration CI failure and source correction — 2026-10-25

- [x] Build/boot-test run `#40159887034` exposed a compile failure at `kernel/prosenjitfs.c:10624`: an `rmdir-write-sweep` cleanup clause referenced `found_lba` from the wrong (unlink-read-sweep) scope after branch-conflict reconciliation.
- [x] Reverted that misplaced cleanup from the unlink read-failure sweep and anchored the optional post-recovery `vfs_rmdir()` cleanup to the actual rmdir write-failure sweep, where `found_lba` is declared and indicates whether the directory survives recovery.
- [x] Corrected integration branch was verified by PR #242 exact-head Build/boot-test #40160091435 and Security architecture #40160091518; ISO and native BIOS QEMU boot checks passed.


- Reconciled the existing native filesystem implementation rather than introducing a parallel design.
- Ported allocator block-geometry validation and boundary/exhaustion regressions onto the latest `main` source while preserving the already-merged FS-18 home-flush recovery test. Integrated source commit: `f533f1a3f8c2a6dd2c0f6891c8323c68562b4fd1`.
- Original allocator-hardening exact-head CI passed; this integration commit own Build/boot-test and Security checks remain required and are not pre-claimed.
# ProsenjitFS Implementation Plan — Audit-Driven

## Part 2 follow-up — VFS read output-count contract (2026-10-24)

- [x] Audited `vfs_file_read()`: unlike the write fix in this branch, it cleared `bytes_read` only after argument and permission checks. A rejected read could therefore leave a stale caller-provided count.
- [x] Changed `kernel/vfs.c` to clear a non-null output count before validation/dispatch; added regressions for permission-denied and filesystem-`EIO` reads using a nonzero sentinel.
- [x] Exact PR-head Build/boot-test #40119299299 and Security architecture #40119299311 passed on `89a35608bcd278b01977f5abded40f2208208d9e`; PR #235 merged as `d82d1010f3c58696c15dfb09c326ba2b6d612e43`.
- Scope: VFS output contract only; no on-disk format change, no crash-atomicity or hardware-durability claim.


## Part 2 execution update — 2026-10-24

**Execution branch:** `fix/prosenjitfs-vfs-write-error-count-20261024`  
**Starting main HEAD:** `e875810769cb022f1a450bce86027966a3982fcc`

- [x] Re-read the completion matrix, implementation plan, test matrix, architecture contract, `kernel/prosenjitfs.c`, `kernel/vfs.c`, and `kernel/include/vfs.h` before changing code.
- [x] Confirmed the existing v1 core already implements superblock/version validation, bitmap allocation, inode/extent metadata, hash-bucket directories, VFS namespace/file operations, redo journaling/recovery, and a read-only integrity scan. The immediate gap is not a missing replacement filesystem; continue hardening the existing provider.
- [x] Fixed a VFS output-contract defect in `vfs_file_write()`: the caller-provided `bytes_written` value was not cleared before permission checks or filesystem callback dispatch, so an error could leave a stale count that looked like successful partial I/O.
- [x] Added a focused VFS self-test callback that returns `PROS_VFS_EIO`; the regression seeds the output count with a nonzero sentinel and requires `EIO` plus a zero byte count.
- [ ] Verify the exact execution branch with Build/boot-test and Security architecture workflows. Source changes are not runtime-verified until those gates finish on this branch.
- [x] Reviewed PR #230 exact head `22da8502bce69a3c69c6ddf222eb643ffa89aab0`; Build #40088773369 and Security #40088773288 passed, and it merged as `3df77093edec9a6961a507fe05f4c32420d73391`.
- [x] Confirmed `main` already contains the equivalent rmdir write-failure sweep. PR #232 is duplicate coverage on an older/conflicting branch; do not merge the duplicate blindly.


**Audit date:** 2026-10-24  
**Baseline:** `main` at `cfe04a8520ec917a90b284bbc726a8bd0cc83a2d`  
**Status source:** [Completion matrix](PROSENJITFS-COMPLETION-MATRIX.md)  
**Architecture source:** [ProsenjitFS design and verified audit](PROSENJITFS.md)  
**Test gates:** [Test matrix](PROSENJITFS-TEST-MATRIX.md)

This is the working checklist for completing existing ProsenjitFS safely. The order is dependency-driven; do not mark a step complete from code inspection alone when its acceptance criteria require runtime evidence.

## P0 — Diagnose and repair the currently failing fault-injection test

- [x] Inspect PR #233 head `74b8cedf5b96055823b9a9a5d4a963b8d2f1a3b0` and map QEMU stage `0x28E1` to the invariant assertion at `kernel/prosenjitfs.c:10465`.
- [x] Split the compound assertion into distinct diagnostic returns for operation status, durable-media CRC, volatile-media CRC, dirty-map CRC, filesystem read-only state and mount read-only state.
- [x] Fix the error namespace boundary: `pfs_make_vnode()` returns filesystem-native errors and VFS callback callsites map through `pfs_vfs_error()`; the earlier direct return could confuse ProsenjitFS `EIO (-2)` with VFS `ENOSYS (-2)`.
- [x] Verify by source inspection that a not-reached countdown is disarmed before readback/integrity/unmount.
- [x] Confirm on exact PR #233 head `84d2b84948065011cf0c94291e8bcaf5d9f7079c`: Build/boot-test #40116501666 and Security architecture #40116501659 pass; workflow logs include the full ProsenjitFS format/mount/allocation/journal/recovery success marker and native ATA PIO -> MBR -> ProsenjitFS persistent mount marker. This is mock-device/QEMU evidence, not physical-device certification.
- **Acceptance:** met on PR #233 exact head `84d2b84948065011cf0c94291e8bcaf5d9f7079c` (Build/boot-test #40116501666 and Security #40116501659 passed). The fix passed exact-head CI and merged into `main` as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`; post-merge main CI is pending.

## P1 — Complete and reconcile per-operation fault-injection coverage

- [x] PR #233 append/truncate read-failure sweeps passed exact-head Build/boot-test #40116501666 and Security #40116501659 on `84d2b84948065011cf0c94291e8bcaf5d9f7079c`; merged into `main` as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`.
- [x] PR #230 create/overwrite/rmdir write-failure sweep reviewed and exact-head CI passed (Build #40088773369; Security #40088773288).
- [ ] For every accepted test PR, capture exact head SHA and both Build/boot-test and Security architecture run links in `docs/PROSENJITFS-COMPLETION-MATRIX.md`.
- [x] Deduplicated overlapping sweeps: closed PRs #232, #240 and #241 as redundant/superseded after their unique coverage was merged or ported through PRs #239 and #242.
- **Acceptance:** no open test branch is called verified until its exact head passes the required CI workflows; failure injection never leaks into post-operation verification.

## P2 — Make I/O-failure classification and cleanup systematic

### Allocator bitmap read-failure regressions — current integration

- [x] Added mock-device read-failure checks for `pfs_find_free_inode` and `prosenjitfs_alloc_inode`.
- [x] Added independent data-bitmap read-failure checks for `pfs_find_free_data_block`, `pfs_find_free_data_block_except`, `pfs_find_free_data_block_excluding`, and `pfs_data_block_is_free`.
- [x] Each reached fault must propagate `PROSENJITFS_EIO`, preserve candidate sentinels, consume the injected fault, leave durable/volatile/dirty media CRCs unchanged, keep the mount writable, and pass a subsequent integrity scan.
- [x] Exact PR #242 head `96418d7bf512ee49e42eb325d0864738781891ab` passed [Build/boot-test #40160091435](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091435) and [Security architecture #40160091518](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091518). Build includes ISO and native BIOS QEMU boot checks; no physical-device durability or general concurrency claim.

- [x] Targeted journal-image flush failure/retry (FS-17) and post-commit home-block flush failure/recovery (FS-18) now have passing exact-head Build/boot-test and Security evidence. Broader stage-by-stage fault injection remains open.

- [ ] Audit `kernel/prosenjitfs.c` read/write/flush propagation paths for superblocks, journal, bitmaps, inode table, extents, directory indexes and directory entries.
- [ ] Extend mock-device tests for read, write and flush errors at each reachable operation point, including short/partial transfers if the interface supports them.
- [ ] Ensure device `EIO` stays an I/O error and is not converted into metadata corruption unless bytes were successfully read and shown malformed.
- [ ] Check all failure exits for transaction-buffer reset, temporary heap/scratch cleanup, vnode/file closure, allocation rollback, mount state, and preservation of prior file data.
- **Acceptance:** every injected fault has a bounded, deterministic expected result; clean integrity checks pass after recovery; no in-memory staged transaction contaminates the next operation.

## P3 — Re-validate transactional durability and recovery invariants

- [x] Added a targeted home-block flush failure test at transaction flush #4: the commit record is already durable, the current mount freezes read-only, and remount recovery replays the complete new payload. Exact PR #239 head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf` passed Build/boot-test #40156447788 and Security #40156447759; merged as `1b87449daf9555d15723dea284d3725b87e354d0`.
- [ ] Sweep remaining failure points around journal-slot invalidation, journal header/images, flush barriers, commit record, home-block application, and journal retirement.
- [ ] For pre-commit interruption, recovery must expose the old consistent state; for a valid committed transaction, recovery may expose the complete new state. Reject mixed/partial logical results.
- [ ] Re-run recovery interruption tests both before and after flush, including repeat recovery after simulated power loss.
- [ ] Test missing/stale commit markers, matching transaction IDs with contradictory record counts, invalid checksums, duplicate/out-of-range transaction targets and overflowed region calculations.
- **Acceptance:** all committed/uncommitted outcomes are deterministic and idempotent; ambiguous commit state fails closed or leaves the mount read-only until recovery; no repair writes occur during a read-only scan.

## Allocator geometry hardening — ported onto current main (2026-10-25)

- [x] Centralized accepted filesystem block geometry in `pfs_block_size_valid()`: only 512, 1024, 2048 or 4096-byte power-of-two blocks pass. Applied before allocator bitmap/index arithmetic, inode location and public inode I/O, preallocation, integrity scanning, and vnode read/write/truncate arithmetic.
- [x] Added malformed-geometry coverage for 0, 511, 513, 4097 and 8192; output-sentinel/media-invariance assertions; bounded inode/data bitmap exhaustion checks; final-valid-bit checks; and strengthened the rmdir crash-recovery assertions/cleanup.
- [x] The original geometry PR exact head `3d76a1fdbc2a58518bc914392caf5c846be04ce2` passed Build/boot-test #40119726528 and Security architecture #40119726499. This current-main port is source-committed as `f533f1a3f8c2a6dd2c0f6891c8323c68562b4fd1`; the integrated head still requires its own CI before it is marked verified.
## P4 — Validate metadata, allocation and namespace invariants adversarially

- [ ] Expand remaining boundary tests for primary/backup superblocks, feature/version/block-size validation, region overlap/order, bitmap capacity/tail bits, inode-table bounds, further inode semantic combinations, extent loops/overlaps, extent ownership, and block-to-sector arithmetic.
- [x] Source/test coverage for checksum-valid inode type/flag/generation/parent/size/allocated-byte admission passed on exact source head `70ed44cd45987682ac50f9fbfd1b82a510e25033` (Build #8548 and Security #8084). P4 remains open for remaining boundary families.
- [ ] Expand directory tests for malformed UTF-8, zero/overlong names, checksum-valid bad hashes, wrong buckets, duplicate names/references, self-cycles, chain cycles, bad child types/parents and parent chains that do not reach root.
- [x] Added a bounded total-path-length guard in `vfs_path_lookup()` before vnode reference acquisition; an unterminated path reaching `PROS_VFS_PATH_MAX` is rejected with `PROS_VFS_EOVERFLOW` instead of scanning arbitrarily many separators.
- [x] Added VFS self-tests for empty and overlong paths, repeated separators, `.`, `..` and root escape clamping, missing intermediate directory, non-directory components, and preservation of the caller's result pointer on error.
- [ ] Exact PR #242 Build/boot-test and Security architecture CI must pass before these regressions are considered runtime-verified.
- [ ] Keep orphan detection report-only until a separate recovery design proves how to distinguish an orphan from valid but temporarily unreachable metadata.
- **Acceptance:** malformed media is rejected without out-of-range access or media mutation; valid media passes the read-only scan; all error values are stable and documented.

## P5 — Specify operation serialization before expanding concurrency

- [ ] Audit uses of `pfs_io_lock`, all VFS mutation entry points and integrity-scan callers in `kernel/prosenjitfs.c` and `kernel/vfs.c`.
- [ ] Define lock ownership, lock ordering, recursion rules and whether VFS calls can be re-entered. Do not infer filesystem-wide exclusion from the narrow append lock.
- [ ] Add controlled interleaving tests for append/write/truncate/rename/unlink versus scans and for vnode/file lifetime around unlink/rename.
- [ ] Reconcile the locking design with scheduler/preemption and interrupt-state contracts before enabling concurrent execution paths.
- **Acceptance:** documented locking proves which mutable state is protected, and the test setup exercises the same contract as production call sites. Otherwise keep concurrency explicitly unsupported.

## P6 — Complete operational/maintenance gaps

- [x] Removed the temporary cross-layer `block_device_init()` trace and forced `O0`/`noinline` attributes from `kernel/block.c`. The ProsenjitFS self-test retains its generic before/after checkpoints (`0xF004`/`0xF005`), so test-stage reporting remains without instrumenting the production block-init path. Exact-head CI is required for acceptance.
- [x] Host-side `mkprosenjitfs` uses the shared kernel formatter/format definitions on the disposable image generated by `scripts/build.sh`. It rejects non-regular targets via `lstat`/`O_NOFOLLOW`/`fstat`, validates the expected MBR/partition layout before formatting, and has SHA-256-backed refusal tests for special files and malformed regular images. Exact-head CI remains required for acceptance.
- [ ] Keep automated repair/reclamation disabled until a journal-first, conservative repair design includes pre/post-integrity verification, read-only fallback and recovery logging.
- [ ] Define a separate physical-device durability validation plan. Mock-device and QEMU tests are not physical power-loss certification.
- **Acceptance:** production diagnostics do not alter the filesystem control path unnecessarily; tools use explicit image paths and refuse unsafe targets; recovery is observable and non-destructive by default.

## Change and verification discipline

1. Inspect current source, the exact PR head and the current `main` base before editing.
2. Make the smallest change that addresses the demonstrated failing invariant; retain public interfaces unless a reviewed design requires a change.
3. Add/adjust a focused regression before broader runs.
4. Build and run the test matrix; keep logs and run IDs linked in the status matrix.
5. If CI fails, map the exact serial stage/assertion to source, fix that cause, and rerun. Never infer green from an earlier commit.
6. Do not format, overwrite, delete or repartition a real disk during discovery or tests. Use the deterministic mock device and repository-generated disposable image only.


## FS-26 — Ambiguous pre-commit journal-images flush (2026-10-25)

- [x] Added a deterministic mock-device regression for flush #2: journal header/images persist but flush reports `EIO` before a commit record is published.
- [x] Require failed write with zero bytes, current mount remaining writable, simulated crash/remount preserving the complete old payload, and a passing read-only integrity scan.
- [x] Exact PR #250 head `32541b0d67e9a077b10bf8896a0881bf862c01cd` passed Build/boot-test [#40168090685](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40168090685) and Security architecture [#40168090633](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40168090633); merged into `main` as `4cb57fc7e80b4a9cca6ecfb0a5cdf7b668c60055`. This is not physical-device durability certification.


## FS-27 — CRC-valid journal header with duplicate targets (2026-10-25)

- [x] Add a deterministic mock-device regression that creates a committed transaction stopped before home-block replay, duplicates two journal target LBAs, recomputes both header and commit checksums, and requires mount recovery to return `PROSENJITFS_ECORRUPT` before replay.
- [x] Exact-head Build/boot-test [#40171924978](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40171924978) and Security architecture [#40171924948](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40171924948) passed on `935d5ef715f6c81dc502269d584c7fa3f25cf728`; PR #255 merged as `d6e4f0998cbaa57d7234803011e3bbe21680b241`.
- Scope is structural validation on an in-memory mock device; no physical-device durability or general concurrency claim.


## FS-32 — Invalid journal target geometry (2026-10-25)

- [x] Added committed-journal corruption cases for a target equal to `total_blocks` and a target inside the journal region.
- [x] Recomputed header and commit checksums so each case exercises structural target validation.
- [x] Asserted recovery returns `PROSENJITFS_ECORRUPT` before replay and durable/volatile/dirty mock-media CRCs remain unchanged.
- [ ] Verify the exact PR head with Build/boot-test and Security architecture workflows before marking PASS.
- Scope remains deterministic mock-device recovery testing; physical hardware durability and general VFS concurrency are separate open items.
