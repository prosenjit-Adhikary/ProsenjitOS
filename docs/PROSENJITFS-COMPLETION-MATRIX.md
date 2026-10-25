## Directory-index owner identity — source/test added; CI pending (2026-10-25)

- `pfs_read_dir_index()` now receives the expected inode number and rejects a checksum-valid directory index whose embedded `inode` field names a different in-range inode.
- Propagated the known owner through create/mkdir/unlink/rmdir/rename, lookup, readdir, and integrity-check callers, including self-test helper paths.
- Regression mutates the root index owner while recalculating the CRC, requires `PROSENJITFS_ECORRUPT`, then restores the original index via the journal transaction interface and runs a clean integrity scan.
- The preceding source revision `88b7873f12c29678a9fc93bb9be8012e8dac2205` passed Build/boot-test #8544 (run [#8544](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40163951177)); this new source change requires fresh exact-head checks.

## Inode-record admission update — 2026-10-25

- Added v1 inode semantic validation to prosenjitfs_read_inode() after checksum validation: known type/flag values, nonzero generation, parent index bounds, size within the data area, and block-aligned allocated-byte accounting bounded by volume capacity.
- Added seven checksum-valid malformed-record regressions. Each mutation is written via the existing fixture-capable inode writer, then the valid root record is restored before assertion results are evaluated; the suite ends with a clean integrity scan.
- The accepted v1 disk format is unchanged. Extent and directory references remain the responsibility of dedicated readers.
- Verification status: PENDING exact-head Build/boot-test and Security architecture workflows. Do not mark FS-23 green before both pass.

## Current integration update — allocator bitmap read-error coverage (2026-10-25)

- Added test cases in `kernel/prosenjitfs.c` for read failures in the inode bitmap finder and public inode allocator, plus all four data bitmap lookup/query paths.
- Acceptance checks include filesystem `EIO` classification, unchanged result sentinels, consumed one-shot read-failure injection, writable mount state, unchanged durable/volatile/dirty mock-media CRCs, and successful integrity scanning after each sweep.
- Source commit `c7218d60a8b5b499f2baeec4170c8642c6c11f55`; exact PR #242 integration CI is still required. Do not treat the earlier PR #218/#219 branch states as verification of this port.

## Latest verified follow-ups — 2026-10-25

- [x] **FS-18 home-block flush failure:** inject transaction flush #4 after commit publication; require zero bytes on failure, read-only current mount, committed payload replay after simulated power loss, and clean integrity scan. Exact PR #239 head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf` passed [Build/boot-test #40156447788](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447788) and [Security #40156447759](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447759); merged as `1b87449daf9555d15723dea284d3725b87e354d0`. Mock-device/QEMU only.
- [x] **FS-19 allocator geometry hardening:** the original PR #237 exact head `3d76a1fdbc2a58518bc914392caf5c846be04ce2` passed [Build/boot-test #40119726528](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119726528) and [Security #40119726499](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119726499). Its changes are ported onto newer main with FS-18 and reconciled rmdir recovery checks in source commit `f533f1a3f8c2a6dd2c0f6891c8323c68562b4fd1`; **the integrated commit still requires fresh exact-head CI**.
- Geometry tests cover block sizes 0, 511, 513, 4097 and 8192, sentinel/media invariance, bounded bitmap exhaustion, and final valid allocator bits. Accepted on-disk v1 format is unchanged.
- [x] **FS-20 allocator bitmap read failures:** tests cover inode/data bitmap lookup and query read errors, output sentinels, unchanged durable/volatile/dirty CRCs, writable mount state and post-fault integrity. - [x] Exact PR #242 head `96418d7bf512ee49e42eb325d0864738781891ab` passed [Build/boot-test #40160091435](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091435) and [Security architecture #40160091518](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091518). Build includes ISO and native BIOS QEMU boot checks; no physical-device durability or general concurrency claim.
- [x] **FS-21 VFS path bounds and resolution:** complete path-length limit and tests for empty/overlong path, repeated separators, `.`/`..`, root escape clamp, missing intermediate directory, non-directory traversal and result-sentinel preservation. - [x] Exact PR #242 head `96418d7bf512ee49e42eb325d0864738781891ab` passed [Build/boot-test #40160091435](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091435) and [Security architecture #40160091518](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091518). Build includes ISO and native BIOS QEMU boot checks; no physical-device durability or general concurrency claim.
- Build/boot-test #40159887034 **failed at ISO compilation** with `kernel/prosenjitfs.c:10624:18: error: 'found_lba' undeclared`. The misplaced cleanup was removed from the unlink-read sweep and moved to the actual rmdir-write sweep. A fresh exact-head build/boot and security pass is required.

# ProsenjitFS Completion Matrix

## Part 2 follow-up — VFS read output-count contract (2026-10-24)

- [x] Audited `vfs_file_read()`: unlike the write fix in this branch, it cleared `bytes_read` only after argument and permission checks. A rejected read could therefore leave a stale caller-provided count.
- [x] Changed `kernel/vfs.c` to clear a non-null output count before validation/dispatch; added regressions for permission-denied and filesystem-`EIO` reads using a nonzero sentinel.
- [x] Exact PR-head Build/boot-test #40119299299 and Security architecture #40119299311 passed on `89a35608bcd278b01977f5abded40f2208208d9e`; PR #235 merged as `d82d1010f3c58696c15dfb09c326ba2b6d612e43`.
- Scope: VFS output contract only; no on-disk format change, no crash-atomicity or hardware-durability claim.


## Part 2 execution update — 2026-10-24

- Starting main HEAD: `e875810769cb022f1a450bce86027966a3982fcc`.
- Change under verification: `kernel/vfs.c` now clears `bytes_written` before validating/dispatching a write, preventing stale caller state after any write error. A focused VFS self-test asserts that a callback returning `PROS_VFS_EIO` leaves the count at zero.
- Status: **verified** on exact PR head `89a35608bcd278b01977f5abded40f2208208d9e`: [Build/boot-test #40119299299](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119299299) and [Security architecture #40119299311](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119299311) passed; merged as `d82d1010f3c58696c15dfb09c326ba2b6d612e43`.
- This is VFS contract hardening in the existing ProsenjitFS integration, not a new filesystem format or claim of crash atomicity.


## Current source-backed audit — 2026-10-24 (authoritative snapshot)

**Repository:** `prosenjit-Adhikary/ProsenjitOS`  
**Audited baseline:** `main` at `cfe04a8520ec917a90b284bbc726a8bd0cc83a2d`  
**Audit branch:** `audit/prosenjitfs-part1-20261024`

### Verified baseline

- Main-branch Build/boot-test run [#8426](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40083299324) passed at the audited commit. Its completed steps include toolchain validation, desktop/DML prerequisite tests, ISO build, ISO QEMU boot and native BIOS QEMU boot.
- Main-branch Security architecture run [#7962](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40083299325) passed at the same commit.
- The workflow's required serial markers include partition discovery, VFS provider-boundary self-test, storage volume discovery, and the ProsenjitFS format/mount/allocation/journal/recovery self-test. These establish the baseline at this exact main commit; they do not prove unmerged test branches.
- Source inspection confirms a freestanding C filesystem in `kernel/prosenjitfs.c`, shared block/VFS contracts in `kernel/include/block.h` and `kernel/include/vfs.h`, and a version-1 disk format in `include/prosenjitfs_format.h`. The current provider includes formatting, probing/mounting, persistent allocation, inodes/extents, hash-indexed directories, VFS file/namespace operations, redo-journal recovery and a read-only integrity-report API.
- Existing design notes explicitly limit the integrity scan to callers that serialize it against mutations. The `io_lock` in filesystem state must not be treated as proof of global VFS/SMP safety.
- Existing design notes distinguish deterministic mock-device crash injection from physical-device power-loss validation. No automatic destructive repair or orphan reclamation is currently exposed by the integrity checker.

### Required status categories

| Category | Audited finding |
|---|---|
| **Verified complete (baseline only)** | The audited main commit builds and passes both ISO QEMU and native BIOS QEMU workflow gates; Security architecture check passes. The existing format/mount/allocation/journal/recovery self-test passes on that baseline. |
| **Partially implemented** | Systematic per-operation read/write/flush fault injection is being added incrementally. Current VFS locking is not a general concurrency guarantee. The structured integrity scan reports problems but does not repair them. |
| **Missing / deferred** | A host-side `mkprosenjitfs` utility and VFS-visible fallocate policy are documented as future work. Conservative repair/reclamation, physical-device power-loss testing, and concurrent mutation/scan guarantees remain deferred. |
| **Verified on PR branch (not yet merged)** | PR #233's earlier head `74b8cedf5b96055823b9a9a5d4a963b8d2f1a3b0` failed at stage `0x28E1` / line 10465 (`-27`), checking expected EIO, unchanged durable/volatile/dirty mock-media CRCs and writable mount state. Source audit found that `pfs_make_vnode()` leaked filesystem `EIO (-2)` into VFS, where `-2` is `PROS_VFS_ENOSYS`. Fixed by retaining filesystem status inside the helper and translating at VFS call boundaries; split assertion into predicate-specific diagnostics. Current exact head `84d2b84948065011cf0c94291e8bcaf5d9f7079c` passed [Build/boot-test #40116501666](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40116501666) and [Security architecture #40116501659](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40116501659). Logs confirm the ProsenjitFS self-test marker and ATA PIO -> MBR -> ProsenjitFS persistent mount marker; both QEMU runs terminated with expected timeout 124 after required markers. PR #233 has now merged as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`. Exact-head Build/boot-test #40116501666 and Security #40116501659 both passed; post-merge main Build/boot-test #40116980733 and Security #40116980685 are pending. |
| **Blocked by unavailable tooling** | This session has no attached local repository/interactive shell for independent local compilation. GitHub Actions is accessible and did run PR #233 on self-hosted runner `home`; the failure is a test failure, not evidence of runner unavailability. |
| **Not yet verified** | PR #233's append/truncate read-failure sweep passed on its exact head and has now merged into `main` as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`. Create/overwrite-write and rmdir-write PRs must pass their exact heads before being counted green. General concurrency and physical durability remain unverified. |

### Immediate next action

1. **Completed and verified on PR #233:** `pfs_make_vnode()` returns filesystem-native errors; VFS callbacks translate them via `pfs_vfs_error()`. The append sweep has distinct diagnostic returns for operation status, durable/volatile/dirty CRC and read-only state.
2. Exact head `84d2b84948065011cf0c94291e8bcaf5d9f7079c` passed Build/boot-test #40116501666 (ISO QEMU + native BIOS QEMU; required filesystem and ATA/partition serial markers present) and Security architecture #40116501659.
3. Next, review the still-open create/overwrite-write and rmdir-write sweeps. Keep general concurrency and physical-device durability explicitly separate from mock-device/QEMU acceptance.
4. Use `docs/PROSENJITFS-IMPLEMENTATION-PLAN.md` and `docs/PROSENJITFS-TEST-MATRIX.md` as the dependency-ordered next-work checklist.

## Historical verification notes — 2026-10-22 (superseded by the 2026-10-24 snapshot)

- PR #193 fixed a diagnostic contract bug in the report-only integrity scanner: allocated inode bitmap + FREE inode record previously returned `-20000 - inode_number`, which the public wrapper incorrectly decoded as a source line. It now returns corruption with the actual checker source line and has a targeted regression. PR #193 merged as `0e61bc728f1eb0fe5cf3a701de806d98a04bfeb0`.
- Exact PR head `7a3ff53146aa9685d3becc0d304b62f16e8d01d4` passed build/boot-test [#39783774495](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39783774495) and security architecture [#39783774327](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39783774327). Build steps including ISO build, filesystem diagnostics, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Follow-up documentation commit `3580b1bb1b97cc1deffc8b30ecbad352356fe8ed` passed build/boot-test [#39784256277](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39784256277) and security architecture [#39784256637](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39784256637). On this exact current-main descendant, toolchain verification, desktop/DML tests, ISO build, ProsenjitFS diagnostics, ISO QEMU boot, and native BIOS QEMU boot all passed.
- PRs #170 and #179 are merged. PRs #172 and #173 were closed as superseded by #179, not independently merged. Issue #166 remains open for staged integrity/recovery follow-up.
- PRs #167 and #168 contain earlier, overlapping versions of the read-only/structured integrity API; their feature scope has since been consolidated and extended through #179 and #193. These stale branches conflict with current main and should not be used as merge candidates.
- PR #142 remains a diagnostic-only branch with a recorded native BIOS QEMU boot failure on its own head. Do not treat passing descendant-main boot tests as proof that this separate branch is fixed.

- Current `main` commit `141f63ae7c79ab85252fab84f8fc44bc05bfdf8d` (`docs(prosenjitfs): record green feature-mask verification`) passed build/boot-test [#39782771424](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39782771424) and security architecture [#39782771420](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39782771420).
- The build/boot job ran on self-hosted runner `office`. Verified steps: toolchain validation, desktop script checks, DML unit tests, ISO build, ProsenjitFS block-init diagnostics, ISO QEMU boot, and native BIOS QEMU boot all passed.
- The merged integrity/journal work is consolidated through PR #179 (`4bb5a241054b8e7b661672fc9ee64b0a30919756`); later merged hardening includes checksum-valid extent, bitmap-tail, directory-bucket and duplicate-name corruption regressions documented below.
- PRs #170 and #179 are merged. PRs #172 and #173 were closed as superseded by #179, not independently merged. Issue #166 remains open for staged integrity/recovery follow-up.
- PR #142 remains a diagnostic-only branch with a recorded native BIOS QEMU boot failure on its own head. Do not treat passing descendant-main boot tests as proof that this separate branch is fixed.

## Purpose and baseline

This is a source-grounded tracking document, not a declaration that ProsenjitFS is production-complete. The inspection baseline for the integrity-scan implementation is branch
`hardening/prosenjitfs-report-only-orphan-inodes` (source file SHA observed during inspection:
`ed78325c8539de09daca7968a9b18d73323a162c` for `kernel/prosenjitfs.c`).

Integration status as of 2026-10-22:
- PR #170 (orphan block reporting) was merged into `main`.
- The combined integrity/inode/extent validation and journal recovery hardening was merged through PR #179.
- Issue #166 remains open for follow-up on the repair roadmap; automatic orphan reclamation is deliberately deferred.

Do not reclaim orphan objects or describe unrun tests as passing.

## Completion matrix

Status meanings: **Implemented in inspected source** means code exists, not that the full acceptance gate has passed. **Needs verification** means this inspection did not establish a passing result on the exact final commit. **Deferred** means the feature is not part of the current safe scope.

| Subsystem | Source-grounded status | Remaining acceptance work |
|---|---|---|
| On-disk format / superblock geometry | Implemented in existing ProsenjitFS; integrity checker validates key region ordering, bounds, bitmap capacity, inode-table capacity, and root inode. | Corrupt each geometry/feature/version case and verify mount rejects safely; run the current full matrix. |
| Mount and journal recovery ordering | Merged PR #179 includes journal-slot invalidation, contradictory commit rejection, fail-closed handling, and recovery-restart regression coverage; consolidated build/boot and security workflows passed. | Continue adversarial recovery/failure-injection review and real-device durability validation; QEMU/mock-device tests cannot prove physical power-loss behavior. |
| Block bitmap and allocation ownership | Integrity checker marks metadata references, compares allocated bits to ownership, checks metadata bits and out-of-range tail bits, and reports unowned allocated data blocks in report mode. | PR #191 merged as `a843f4ac1d62ef27c6fe3a06d03359783a50b5c2`; its exact head passed build/boot-test #7824 and security architecture #7360. Checksum-valid block-bitmap and inode-bitmap tail-bit regressions verify out-of-range allocation bits are rejected without scan-side media mutation, then pass after restoration. Continue boundary and read-failure coverage. |
| Inode bitmap and inode metadata | Merged PR #179 validates allocation state, inode checksums/types, parent range, bitmap tail bits, and inode sizes against volume capacity; checksum-valid oversized-inode regression passes. | Continue adversarial generation/checksum cases and stable report-detail review as follow-up hardening. |
| Directories and names | Checker validates directory-index and entry bounds, name length/UTF-8/hash/checksum, duplicate names in a bucket, child type/parent, duplicate references, and parent chains to root. | PR #184 adds a checksum-valid directory-entry self-cycle regression, checking scan-side media immutability and clean rescan after restoration. PR #187 merged as `6e9e70676617d8f128ca03134cd1c8ebe0eca829`; its exact head passed build/boot-test #7802 and security architecture #7338. The checker now rejects entries whose hash does not map to their containing bucket, with a checksum-valid wrong-bucket regression and read-only media-state checks. PR #192 merged as `2abc3feffba5a5d2941b8580e7af06c3f5427c99`; its exact head passed build/boot-test #7831 and security architecture #7367. It creates two distinct filenames that intentionally collide in the same directory bucket, corrupts one entry into a duplicate name with a valid checksum, and verifies read-only rejection and clean rescan after restoration. Broader repeated-reference and corruption-at-read-boundary cases remain follow-up. Review symlink namespace semantics against the actual API. |
| File extents and block ownership | Merged PR #179 validates bounded extent chains, logical ordering/arithmetic, volume-capacity bounds, block ownership and allocated-byte totals. PR #181 adds pre-iteration logical/physical range validation and a checksum-valid oversized-block-count regression; build/boot and security workflows passed on exact PR head `a237ebd02e77609b47258ff18a4677857bbd8350`. | PR #182 adds checksum-valid zero-length and out-of-volume physical-start regressions; build/boot and security workflows passed on exact PR head `666a7e2f96f6c5dd0c7b11d1cf16602b08dafbaa`. PR #185 merged as `0e8e996f55df9da7840502d4dba4e5d0cb0934a8`; its exact head passed build/boot-test #7795 and security architecture #7331. The checksum-valid duplicate physical-block ownership regression checks read-only scan state and a clean rescan after restoration. PR #188 merged as `e5af0af4df3cf0d67eadbcedf799fb384f22d5de`; its security architecture check passed (run #7347). The exact PR build job was marked cancelled despite the listed ISO/native BIOS steps completing successfully, so do not count that run alone as a clean pass. The descendant `main` commit `569fb12b786a1bef4ba7ef8a13357b66af66707c` subsequently passed build/boot-test #7834, including ISO build, ISO QEMU boot, and native BIOS QEMU boot. The cross-inode regression is therefore covered by a passing descendant build, but exact-head PR verification remains distinct. PR #189 merged as `2029eb371c6865a839f282b0538c59030f3c8e2c`; its exact head passed build/boot-test #7814 and security architecture #7350. The checksum-valid overlapping-logical-range regression confirms corruption is rejected without scan-side media mutation. PR #190 merged as `0026779c6006a72f21c1a824d2fd8ec403a05a17`; its exact head passed build/boot-test #7818 and security architecture #7354. The checksum-valid extent-chain self-cycle regression confirms repeated extent metadata is rejected without scan-side media mutation. Continue broader adversarial overlap review. |
| Orphan data-block detection | Implemented as report-only candidate detection in the inspected branch. Report mode completes the bounded bitmap scan and returns corruption when candidates exist. | Preserve zero mutation of durable, volatile, and dirty state on all outcomes; no reclamation without a separately proven crash-safe design. |
| Orphan inode detection | Implemented as report-only candidate detection for allocated, structurally valid non-root inodes with no directory reference and a parent chain reaching root. | Test unusual namespace/link semantics and ensure candidates are never treated as safe-to-free solely from this report. |
| Structured integrity report | Existing report includes status, first error location, inode and bitmap scan counts, and first/count of orphan block and inode candidates. | Ensure error classification is stable and documented; verify bounded memory and report initialization for every early-return path. |
| Create/read/write/append/truncate/delete | Core mutation paths and deterministic self-tests exist in the current implementation. | Run normal, zero-length, block/extent-boundary, max-size, invalid-input, persistence and remount tests on the exact final commit. |
| Directory create/remove/rename/path traversal | Core directory and mutation paths exist. Rename and unlink now have read/write failure sweeps; rmdir has a cyclic-chain classification regression and read-failure sweep. Exact PR heads passed ISO QEMU, native BIOS QEMU, and Security architecture checks. | Continue mutation rollback/persistence tests for mkdir/create/truncate/append and remaining path/name edge cases; test allocation failures and all transaction boundaries. |
| Journal commit / replay / flush failures | Merged PR #179 invalidates reused commit slots, rejects contradictory matching-ID commit records, forces uncertain post-commit failures read-only, and tests recovery restart after interrupted replay. Combined build/boot and security workflows pass. | Maintain the physical power-loss caveat for simulated QEMU/mock-device tests; add real-device validation separately. |
| Failed I/O and resource cleanup | Rename and unlink read/write failure sweeps and rmdir read-failure sweep are merged; they check error propagation, mount/media invariants where applicable, remount recovery, payload preservation, and report-only integrity. A real EIO-to-corruption misclassification in unlink extent reclamation was fixed. | Extend systematic short/read/write/flush injection to create, mkdir, rmdir writes, truncate, append, and all journal boundaries; continue cleanup/resource-exhaustion tests. |
| Concurrency and serialization | Integrity API explicitly requires caller-side serialization against concurrent mutation. | Confirm all callers obey this precondition; do not claim the scan is internally concurrency-safe. |
| Read-only/degraded mount behavior | Degraded-superblock read-only behavior is a stated architecture constraint in Issue #166. | Test that uncertain metadata/recovery states remain read-only and cannot mutate media. |
| VFS/provider and boot integration | Code-bearing PRs #223–#226 passed exact-head Build/boot-test (including ISO QEMU and native BIOS QEMU) and Security architecture CI; documentation logs and matrix were refreshed afterward. | Re-run full CI on any subsequent code changes; continue provider-boundary, resource cleanup, and hardware-backed validation. |
| CoW, snapshots, reflink, ACLs/xattrs, links, quotas, compression, dedup, online resize, relocation | Listed as long-term targets in README, not established as implemented by this audit. | Explicitly defer until design, API/on-disk compatibility, recovery semantics and tests are implemented. |
| Repair / orphan reclamation | **Deferred for safety.** The inspected scan is read-only and report-only. | Require proven reachability/link semantics, journaled or otherwise crash-safe repair, resumable interrupted repair, explicit operator-controlled mode, dry run, and a clean follow-up scan before implementation. |

## Safe execution sequence

1. The report-only integrity and journal-hardening branches have been reconciled and merged through PR #179.
2. Continue fixing source-proven defects with a regression test for each behavior change.
3. Run narrow filesystem tests, then the warning-clean build, security workflow, ISO QEMU boot, native BIOS QEMU boot, remount/persistence and recovery/fault-injection suites.
4. Tie every verification result to an exact commit SHA and inspect actual workflow logs.
5. Do not enable destructive repair until the repair safety gates above are satisfied.

## Verification record

- Consolidated change PR #179 was merged to `main` as commit `4bb5a241054b8e7b661672fc9ee64b0a30919756`.
- Exact pre-merge head `3cb91f739a839bf05461b754b698b4f3c7341b18` passed [build and boot-test #7775](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39772556092) and [security architecture #7311](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39772555954).
- The build/boot workflow passed desktop/DML unit tests, ISO build, ProsenjitFS block-init diagnostic, ISO QEMU boot, and native BIOS QEMU boot.
- Follow-up PR #181 was merged as `0fb37a3abd8079e08e4ac9d7918b355fd57b45b4`; its exact head passed [build/boot-test #7780](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39774423290) and [security architecture #7317](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39774423437). The added oversized-block-count regression verifies corruption is rejected without scan-side media mutation.
- Follow-up PR #182 was merged as `4654c889b0b6c59b2f656c60a285e25700bcb6f2`; its exact head passed [build/boot-test #7787](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39775368199) and [security architecture #7323](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39775368156). Added zero-length and out-of-volume physical-start corruption cases, each checked for scan-side media immutability and a clean rescan after restoration.
- Follow-up PR #185 merged as `0e8e996f55df9da7840502d4dba4e5d0cb0934a8`; exact head passed [build/boot-test #7795](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39776334832) and [security architecture #7331](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39776334904). It verifies checksum-valid duplicate physical-block ownership is rejected without scan-side media mutation.
- Follow-up PR #187 merged as `6e9e70676617d8f128ca03134cd1c8ebe0eca829`; exact head passed [build/boot-test #7802](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39777353077) and [security architecture #7338](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39777353159). It validates directory hash-bucket membership and checks a checksum-valid misplaced-entry fixture.
- Follow-up PR #188 merged as `e5af0af4df3cf0d67eadbcedf799fb384f22d5de`; exact head passed [build/boot-test #7807](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39778043437) and [security architecture #7343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39778043379). It verifies cross-inode physical-block aliasing is rejected by the global ownership scan.
- Follow-up PR #189 merged as `2029eb371c6865a839f282b0538c59030f3c8e2c`; exact head passed [build/boot-test #7814](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39778530084) and [security architecture #7350](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39778530132). It verifies a checksum-valid logical overlap is rejected and the report-only scan does not mutate mock-media state.
- Follow-up PR #190 merged as `0026779c6006a72f21c1a824d2fd8ec403a05a17`; exact head passed [build/boot-test #7818](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779110024) and [security architecture #7354](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779110075). It verifies a checksum-valid self-cycle in an extent chain is rejected.
- Follow-up PR #191 merged as `a843f4ac1d62ef27c6fe3a06d03359783a50b5c2`; exact head passed [build/boot-test #7824](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779654470) and [security architecture #7360](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779654485). It verifies invalid block- and inode-bitmap tail bits are rejected and that the scan does not mutate mock media.
- Follow-up PR #192 merged as `2abc3feffba5a5d2941b8580e7af06c3f5427c99`; exact head passed [build/boot-test #7831](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39780459233) and [security architecture #7367](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39780458900). It covers duplicate directory names in an intentionally colliding hash bucket and confirms the report-only scan does not mutate mock media.
- Integrity regressions cover report-only orphan blocks/inodes, checksum-valid out-of-capacity extents and oversized inode sizes, deterministic reports, and read-only media invariance. Journal regressions cover reused commit slots, contradictory commit metadata, write/flush failure boundaries, and recovery restart.
- Mock-device/QEMU tests do not prove physical hardware power-loss durability.
- Automatic repair and orphan reclamation remain deliberately deferred; a report-only candidate is not safe to free without a separately reviewed, crash-safe and resumable repair design.


## Verification update — 2026-10-22 13:07 UTC

- Main merge commit: `0e61bc728f1eb0fe5cf3a701de806d98a04bfeb0`.
- Exact PR head `7a3ff53146aa9685d3becc0d304b62f16e8d01d4` passed build/boot-test [#39783774495](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39783774495) and security architecture [#39783774327](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39783774327).
- Follow-up documentation commit `3580b1bb1b97cc1deffc8b30ecbad352356fe8ed` passed [build/boot-test #39784256277](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39784256277) and [security architecture #39784256637](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39784256637), including successful ISO and native BIOS QEMU boots.
- The regression now guarantees that allocated-bitmap/free-inode corruption is not mistaken for a synthetic source-line encoding and that the report has a bounded source-line location.
- This is a green exact-PR-head milestone, not a declaration of production-complete filesystem behavior. Remaining acceptance work includes injected read/write/flush failures, mutation rollback and persistence/remount coverage, report error-classification review, adversarial namespace/link semantics, caller serialization review, and physical-device durability validation.
- Automatic orphan repair/reclamation remains deferred. QEMU and mock-device tests do not prove physical power-loss durability. PR #142's own branch remains independently unverified/failing.

- Current main: `141f63ae7c79ab85252fab84f8fc44bc05bfdf8d`.
- Build and boot-test [#39782771424](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39782771424): **PASS** on self-hosted runner `office`; desktop script validation, DML tests, ISO build, ProsenjitFS block-init diagnostics, ISO QEMU boot, and native BIOS QEMU boot all succeeded.
- Security architecture checks [#39782771420](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39782771420): **PASS** on the same commit.
- The unknown-feature regression rejects unsupported v1 feature bits even when both redundant superblocks have valid CRCs, then restores the fixture.
- This is a green build/security/boot milestone, not a declaration of production-complete filesystem behavior. Remaining acceptance work includes broader injected read/write/flush failures, persistence/remount and rollback testing for each mutation path, report initialization/error-classification checks, adversarial namespace/link semantics, caller serialization review, and physical-device durability validation.
- Automatic orphan repair/reclamation remains deferred. QEMU and mock-device tests do not prove physical power-loss durability. PR #142's own branch remains unverified/failing independently even though descendant `main` boots successfully.


## Verification update — 2026-10-22 13:35 UTC

- PR #194 merged as `3d8a40d0a6ee5fa1189abe3e1b9bda774350d140`. Exact PR-head build/boot-test #39786642331 and security architecture #39786642131 passed. It added mock-device read fault injection and verified that an inode-bitmap read failure is reported as `PROSENJITFS_EIO`, with durable/volatile/dirty mock-media state unchanged.
- PR #195 merged as `02063bac9d92dad5781230c7898d27b73544c191`. Exact final PR head `8ed82f7bf1ee074cdae9e40be7362519671935e5` passed build/boot-test #39787466192 and security architecture #39787466220. Main merge commit build job #39787804429 and security #39787804339 also completed successfully; build included ISO QEMU and native BIOS QEMU boot.
- Integrity scan now preserves `PROSENJITFS_EIO` across inode bitmap, inode, parent-chain, directory-index/entry, extent and block-bitmap reads instead of collapsing device failures into corruption.
- Test-coverage limitation: the deterministic injected read-failure regression directly targets the inode-bitmap read path. The other propagated read paths still need focused injected fault cases. Broader write/flush fault coverage, mutation rollback/remount, adversarial namespace/link cases, caller serialization and physical-device durability remain acceptance work.
- Automatic orphan repair/reclamation remains deferred. Mock-device and QEMU tests do not prove physical power-loss durability. This is incremental hardening, not a claim of production-complete filesystem behavior.


## Verification update — 2026-10-23: integrity-scan read-failure sweep

- Commit `2ddc86ac087a90be018d8ab14a877d26be4bb060` expands the integrity scanner's mock-device read-failure regression from a single injected read to a sequential sweep over each read position reached by a clean scan.
- For each injected read failure, the regression requires `PROSENJITFS_EIO` in both the return value and report status, and verifies durable, volatile and dirty mock-media state remains unchanged. The sweep stops when the selected read position is beyond the scan's read sequence.
- Exact commit build/boot-test [#39927032201](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39927032201): **PASS**. Toolchain validation, desktop/DML tests, ISO build, block-init diagnostics, ISO QEMU boot and native BIOS QEMU boot all passed; both boot logs contained the ProsenjitFS format/mount/allocation/journal/recovery self-test success marker, and the native BIOS log also contained the ATA/MBR/ProsenjitFS persistent-mount marker.
- Exact commit security architecture checks [#39927032230](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39927032230): **PASS**.
- This closes the narrow acceptance gap for sequential read-failure classification during the report-only integrity scan. It does **not** close all ProsenjitFS acceptance work: broader mutation rollback/persistence-remount coverage, write/flush failure paths, adversarial namespace/link semantics, caller-serialization review and physical-device power-loss durability still require independent verification. QEMU/mock-device success is not proof of real-hardware durability.


## Verification update — 2026-10-23: degraded-mount mutation rejection

- Commit `433c93ff864b0fb114119ddfe859cf421d01d6fb` extends the degraded-superblock self-test in both directions: primary invalid / backup valid, and backup invalid / primary valid.
- Each direction now attempts file creation and directory creation on the read-only degraded mount. Both operations must return `PROS_VFS_EPERM`; durable disk, volatile cache and dirty-block checksums must remain unchanged.
- Exact commit build/boot-test [#39928003595](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39928003595): **PASS**. ISO build, ISO QEMU boot and native BIOS QEMU boot all succeeded, including the ProsenjitFS format/mount/allocation/journal/recovery marker and the ATA/MBR persistent-mount marker.
- Exact commit security architecture checks [#39928003550](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39928003550): **PASS**.
- This verifies the two degraded-superblock recovery directions reject create/mkdir mutation without mock-media changes. It does not prove all VFS mutation operations, all I/O fault paths, or real-device power-loss durability.


## Verification update — 2026-10-23: rename commit-write failure and remount

- PR #197 adds a regression for a failed journal commit-record write during `vfs_rename`.
- The test simulates power loss and remount, checks that the original path and payload remain intact, confirms the destination path is absent, and runs the report-only integrity check.
- Exact PR head `13cf8cc0201ba642be8960475f866d3a732029cb` passed build/boot-test [#39946623102](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946623102), including ISO build, ISO QEMU boot and native BIOS QEMU boot. Security architecture [#39946623071](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946623071) also passed.
- Scope: this closes one focused namespace-rollback/remount acceptance case under the mock block device. Other mutation classes and write/flush failure boundaries still need a matrix-driven review; mock-device/QEMU coverage is not proof of physical power-loss durability.


## Verification update — 2026-10-23: unlink commit-write failure and remount

- PR #198 adds a regression for a failed journal commit-record write during `vfs_unlink` on a populated file.
- The test simulates power loss and remount, checks the original path and payload remain intact, and runs the report-only integrity scan.
- Exact PR code head `ed197bf77328ae419f8f45480640da0063641537` passed build/boot-test [#8194](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946938083), including ISO build, ISO QEMU boot and native BIOS QEMU boot. Security architecture [#7730](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946938090) also passed.
- Scope: this closes one focused unlink rollback/remount case under the mock block device. Other mutation classes and read-error classification still need review; mock-device/QEMU coverage is not proof of physical power-loss durability.


## Verification update — 2026-10-23: unlink read-error classification

- Fixed `pfs_vnode_unlink()` so an invalid entry LBA returns corruption while a block-device read failure propagates as `PROS_VFS_EIO`.
- Added a deterministic fault-injection case for the directory-entry read; it verifies unchanged durable/volatile/dirty media, preserved file bytes, a writable mount, and a clean integrity scan.
- Exact code head `1c051dc7f4d2561352e7728e1b0da86cadfa3c54` passed build/boot-test [#8199](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947309841), including ISO and native BIOS QEMU boot. Security architecture [#7735](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947309844) also passed.
- This closes one specific I/O-error-classification defect. Caller-side scan/mutation serialization, broader mutation rollback coverage, and physical-device durability remain open acceptance work.


## Verification update — 2026-10-23: truncate commit-write failure and remount

- PR #200 adds a populated-file regression for a journal commit-record write failure during shrink-to-zero.
- The test verifies `PROS_VFS_EIO`, fail-closed read-only state, simulated power loss/remount, preservation of the original file length and bytes, and a clean report-only integrity scan.
- Exact PR code head `2c8409065afb624f6bf115fb0aece5095f777355` passed build/boot-test [#8205](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947772776), including ISO and native BIOS QEMU boot. Security architecture [#7741](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947772767) also passed.
- Scope: this closes one focused truncate rollback/remount case under mock-device fault injection. Broader mutation-path coverage and physical-device durability remain separate acceptance work.


## Verification update — 2026-10-23: mkdir commit-write failure and remount

- PR #201 adds a commit-record write-failure regression for `vfs_mkdir` across its multi-image metadata update.
- The test verifies `PROS_VFS_EIO`, fail-closed read-only state, simulated power-loss remount, absence of the uncommitted directory, and a clean report-only integrity scan.
- Exact PR code head `af7cbf0187aa717d483d146878051bd5c2cf584a` passed build/boot-test [#8211](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948217344), including ISO and native BIOS QEMU boot. Security architecture [#7747](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948217395) also passed.
- Scope: this closes one focused mkdir rollback/remount case under mock-device fault injection. Other mutation cases and physical-device durability remain separate acceptance work.


## Verification update — 2026-10-23: rmdir commit-write failure and remount

- PR #202 adds a commit-record write-failure regression for `vfs_rmdir` on an existing empty directory.
- The test verifies `PROS_VFS_EIO`, fail-closed read-only state, simulated power-loss remount, persistence/openability of the original directory, and a clean report-only integrity scan.
- Exact PR code head `f137d83af1df8fdfbff9f3c21672d65827d198a9` passed build/boot-test [#8216](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948709380), including ISO and native BIOS QEMU boot. Security architecture [#7752](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948709352) also passed.
- Scope: this closes one focused rmdir rollback/remount case under mock-device fault injection. Other mutation cases and physical-device durability remain separate acceptance work.


## Verification update — 2026-10-23: file-create commit-write failure and remount

- PR #203 adds a commit-record write-failure regression during file creation through `vfs_file_open(..., PROS_VFS_O_CREATE)`.
- The test verifies `PROS_VFS_EIO`, fail-closed read-only state, simulated power-loss remount, absence of the uncommitted file, and a clean report-only integrity scan.
- Exact PR code head `4f25d7d711d799bac1974049dc7972e57eb52378` passed build/boot-test [#8221](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39949042895), including ISO and native BIOS QEMU boot. Security architecture [#7757](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39949043002) also passed.
- Scope: this closes one focused file-create rollback/remount case under mock-device fault injection. Physical-device durability and other mutation/failure combinations remain separate acceptance work.


## Verification update — 2026-10-23: integrity-scan caller serialization audit

- Reviewed the integrity API declarations/documentation, all in-source scan callsites, and the CPL3 entry interrupt-flag contract.
- All in-source calls to `prosenjitfs_check_integrity_report()`/`prosenjitfs_check_integrity()` are within `prosenjitfs_selftest()`; repository search found no separate production kernel caller. The documented caller-serialization precondition is therefore not currently exercised by a production callsite.
- `pfs_io_lock()` only serializes append writes; it is not a filesystem-wide lock. `boot/entry.asm` keeps IF clear for CPL3 because timer preemption is not yet scheduler-safe.
- Current callsite audit is complete, but **general concurrent VFS safety remains unproven**. Before enabling timer preemption/SMP or production concurrent scans, add a reviewed locking boundary and tests for lock ordering/recursion and mutation-versus-scan exclusion.
- No automatic repair/reclamation was added. Physical-device power-loss durability remains outside mock-device/QEMU proof.


## Pending verification — recovery-test failure cleanup (2026-10-23)

- Source audit found an unexpected-failure branch in the stage-10/11 recovery interruption regression that zeroed `fs` directly rather than using `pfs_test_crash_reset()`. The helper also discards volatile mock-device writes, deactivates the mount, and frees transaction/scratch buffers.
- Commit `ce2d39f71aaf1f33545b67e4f3cb18c3fb3b611d` changes only that failure path to use the helper; it preserves the same failing assertion and does not weaken the regression.
- Follow-up documentation commit `372ed6eed56d3b7f8323753a08900160616a4c4d` records the source finding in `docs/DEBUG_LOG.md`.
- Build/boot-test run [#8227](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39956134203): **PASS** on self-hosted runner `office`; toolchain checks, desktop/DML unit tests, ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed. Both QEMU logs contain the ProsenjitFS format/mount/allocation/journal/recovery success marker; the native BIOS log also contains the ATA PIO, LBA-boundary, and ATA → MBR → ProsenjitFS persistent-mount markers.
- Security architecture run [#7763](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39956134201): **PASS**.
- Exact tested PR head: `f2df43395bbf2f07c56b6796df2818cc75aec220`. This validates the test cleanup and current automated gates only; it does not establish physical-device power-loss durability or production completion.


## Additional regression coverage in review — 2026-10-23

- PR #211 adds two adversarial namespace integrity tests: duplicate inode references across distinct directory entries, and duplicate valid names across distinct inodes. Both tests recompute valid entry checksums, require `PROSENJITFS_ECORRUPT`, verify durable/volatile/dirty mock-media CRCs are unchanged by the report-only scan, restore the original entry, and require a clean rescan.
- PR #212 adds a bounded read-failure countdown sweep around `vfs_rename`. For each read position reached, it requires rename failure, unchanged durable/volatile/dirty media CRCs, a writable mount, intact original path/payload, absent destination, and a clean integrity scan. The sweep stops at the first countdown not reached.
- PR #211 merged as `e8a27899d4fe6de72a6e0959fa9d04659ca34eec`; exact head `28446ed40aeb748e8e0a5b3172a3e86f799f4567` passed Build/boot-test [#39980888372](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39980888372) and Security architecture [#39980888146](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39980888146). PR #212 merged as `b6b6eaf409c9407a822b0a6016147087edb6c365`; fixed head `1204deed4e8899448422e5cc7702eca776de3c6f` passed Build/boot-test [#40072397646](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397646) and Security architecture [#40072397702](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397702).
- These are mock-device regression tests. They do not prove physical-device power-loss durability, full concurrent VFS safety, or production filesystem completion.


## Pending verification — rename read-failure sweep fix (2026-10-24)

- Build/boot run [#39981319506](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39981319506) failed the ISO QEMU ProsenjitFS self-test at source line 10605 (stage 0x296D, failure -21); build/ISO and prior diagnostic steps passed. Security architecture run [#39981319411](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39981319411) passed.
- Root cause: the rename read-failure sweep left the final not-reached read countdown armed while running the report-only integrity scan and unmount. Those follow-up operations could consume an injection intended only for vfs_rename.
- Fix in commit a7a7a5b7eaca35dfdf91788d44e37daef433f2a0 disarms the countdown before integrity/unmount only when the requested read position was not reached by rename. Failure-path assertions remain intact.
- Debug log updated in commit f497ca14d967a4107f6f686d241187dc87067a4a.
- **Status: verified** on fixed exact PR head `1204deed4e8899448422e5cc7702eca776de3c6f`: Build/boot-test [#40072397646](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397646) passed ISO QEMU and native BIOS QEMU; Security architecture [#40072397702](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397702) passed. PR #212 is merged.


## Verification update — rename write-failure sweep (2026-10-24)

- Added mock-device one-shot block-write failure injection and a bounded countdown sweep across `vfs_rename`.
- Each reached failure is followed by simulated power loss and remount; acceptance requires exactly one of the original/destination paths, intact payload bytes, and a clean report-only integrity scan.
- The first countdown not reached is disarmed before integrity/unmount checks so those operations cannot consume a rename-specific fault.
- Source commit: `68b04cad273d9521c271942ae828926b0bcb3ab6`; PR #222 merged as `2b14a2b67e7c3543e95140af033f603410c66bc5`.
- Exact PR head `c897fa782f805778b0de051ca2b338a39be7d7ce` passed Build/boot-test [#40072964418](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072964418), including ISO QEMU and native BIOS QEMU, and Security architecture [#40072964417](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072964417).
- This regression does not prove physical hardware durability or concurrent VFS safety.


## Pending verification — unlink read-failure sweep (2026-10-24)

- Added a bounded read-failure sweep around `vfs_unlink`. Each reached read fault must return `PROS_VFS_EIO`, preserve all mock-media CRCs, keep the mount writable, preserve the file payload, and pass the report-only integrity scan.
- The first not-reached countdown is disarmed before follow-up checks to avoid leaking an operation-specific fault into integrity/unmount.
- Source commit: `126a4786dbd9b0427625149cee3999efd4c6dbba`.
- Initial ISO QEMU run [#40073247679](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073247679) exposed a source-proven error-classification bug: `pfs_vnode_unlink()` collapsed `pfs_read_extent()` device I/O failures into corruption. Fix commit `ab3c659d4784f82fa83b87f54e7e1d84122d8dd6` preserves the original error. Fixed exact PR head `895ee35f7a2413e569b2d42cbc52ca0efc580928` passed Build/boot-test [#40073371338](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073371338), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073371343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073371343). PR #223 merged as `30acc248df1258fa6ac65a6b5eb8e8ef47e2ceb5`. This does not prove physical-device durability or general concurrent VFS safety.


## Pending verification — unlink write-failure sweep (2026-10-24)

- Added a bounded block-write failure sweep around `vfs_unlink`; each injected failure is followed by simulated power loss and remount.
- Acceptance allows either the original file with unchanged payload or the fully committed unlink, but rejects both/neither namespace inconsistencies, altered payload, or an integrity-scan failure.
- The first not-reached countdown is disarmed before follow-up checks.
- Source commit: `a18e2a55b36c6a445657bb9786dd172fb0eaf95c`.
- **Verified:** exact PR head `c536d9e03f2e21ee559d73ecb1d1a4d92e30a74b` passed Build/boot-test [#40073592946](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073592946), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073592829](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073592829). PR #224 merged as `714fa8e66d3a46991cb80446d769ab4538a63120`. Physical-device durability and general concurrent VFS safety remain unproven.


## Pending verification — rmdir cyclic directory-entry classification (2026-10-24)

- Fixed `pfs_vnode_rmdir()` to classify a bounded directory-chain cycle as corruption rather than I/O failure.
- Added a valid-checksum self-cycle regression that checks the VFS error result, no operation-side media mutation, restoration, clean integrity scan, and normal rmdir afterward.
- **Verified:** fixed exact PR head `5a2dd5d63813498b0bd83f2aa4b6fed1f69e8e2a` passed Build/boot-test [#40073950609](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073950609), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073950625](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073950625). PR #225 merged as `0cdaab0ca75c286e839494d37c24420f74a5c039`.


## Pending verification — rmdir read-failure sweep (2026-10-24)

- Added a bounded read-failure sweep around `vfs_rmdir`. Reached faults must propagate as I/O errors, preserve durable/volatile/dirty mock-media state, keep the mount writable, and leave the directory intact.
- After each injected failure, the directory is reopened and integrity-scanned; the first not-reached countdown confirms the sweep end and is disarmed before follow-up operations.
- Source commit: `aefcac64b5ee80cf58c62c04e0a312d8878ab8dd`.
- **Verified:** exact PR head `5522c822d9bce6d01ca6c0c06b572c8032e63c5e` passed Build/boot-test [#40074188166](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40074188166), including ISO QEMU and native BIOS QEMU, and Security architecture [#40074188142](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40074188142). PR #226 merged as `84a707c354065d6f1c9aa0f418cf043f6d55bdd3`.


## Verified — truncate block-write failure sweep (2026-10-24)

- Added a bounded write-failure sweep across `vfs_file_truncate` and simulated power-loss remount after each reached fault.
- Recovered content must be either the unchanged original 64-byte payload or a fully truncated empty file; intermediate EOF, altered bytes, or integrity failures are rejected.
- **Verified:** exact PR head `2586aa0403d5c984cb64b962c5bfdd1bf360cfe4` passed Build/boot-test [#40077700893](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40077700893), including ISO QEMU and native BIOS QEMU, and Security architecture [#40077700948](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40077700948). PR #228 merged as `e6c391ae009b94dd8645140cfe86a571167ff54f`.



## Verification update — home-block flush failure recovery (2026-10-25)

- PR #239 merged into `main` as `1b87449daf9555d15723dea284d3725b87e354d0` after exact-head Build/boot-test [#40156447788](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447788) and Security architecture [#40156447759](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447759) both passed.
- FS-18 now covers failure of transaction flush #4 after durable commit publication and during home-block persistence. The current mount must report zero written bytes and become read-only; simulated power-loss remount must replay the committed payload completely and pass integrity validation.
- Remaining work includes systematic coverage of other journal interruption points, broader malformed journal metadata, concurrency/object lifetime verification, and physical hardware durability testing. The current pass is mock-device/QEMU evidence, not hardware certification.
