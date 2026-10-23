# ProsenjitFS Completion Matrix — 2026-10-22

## Latest verification update — 2026-10-22 13:07 UTC

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
| Directory create/remove/rename/path traversal | Core directory and mutation paths exist. | Verify parent/child invariants and failure rollback across interrupted transactions; include path/name edge cases. |
| Journal commit / replay / flush failures | Merged PR #179 invalidates reused commit slots, rejects contradictory matching-ID commit records, forces uncertain post-commit failures read-only, and tests recovery restart after interrupted replay. Combined build/boot and security workflows pass. | Maintain the physical power-loss caveat for simulated QEMU/mock-device tests; add real-device validation separately. |
| Failed I/O and resource cleanup | Checked block helpers and cleanup paths exist in the inspected code. | Inject short/read/write/flush failures where supported; check cleanup, dirty-state immutability, and error propagation. |
| Concurrency and serialization | Integrity API explicitly requires caller-side serialization against concurrent mutation. | Confirm all callers obey this precondition; do not claim the scan is internally concurrency-safe. |
| Read-only/degraded mount behavior | Degraded-superblock read-only behavior is a stated architecture constraint in Issue #166. | Test that uncertain metadata/recovery states remain read-only and cannot mutate media. |
| VFS/provider and boot integration | Existing VFS integration and ISO/native boot test infrastructure are documented in repository sources and PR verification notes. | Run current security, build, ISO QEMU, native BIOS QEMU and filesystem self-tests on the exact final SHA. |
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
