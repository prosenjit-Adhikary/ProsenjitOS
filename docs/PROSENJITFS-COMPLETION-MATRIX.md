# ProsenjitFS Completion Matrix — 2026-10-22

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
| Block bitmap and allocation ownership | Integrity checker marks metadata references, compares allocated bits to ownership, checks metadata bits and out-of-range tail bits, and reports unowned allocated data blocks in report mode. | Exercise all bitmap boundaries, duplicate ownership, read failures, and cache/durable-media immutability on the final combined branch. |
| Inode bitmap and inode metadata | Merged PR #179 validates allocation state, inode checksums/types, parent range, bitmap tail bits, and inode sizes against volume capacity; checksum-valid oversized-inode regression passes. | Continue adversarial generation/checksum cases and stable report-detail review as follow-up hardening. |
| Directories and names | Checker validates directory-index and entry bounds, name length/UTF-8/hash/checksum, duplicate names in a bucket, child type/parent, duplicate references, and parent chains to root. | PR #183 adds a checksum-valid directory-entry self-cycle regression, checking scan-side media immutability and clean rescan after restoration. PR #186 adds a checksum-valid duplicate-name regression in one hash bucket; await CI on the rebased branch. Broader repeated-reference and corruption-at-read-boundary cases remain follow-up. Review symlink namespace semantics against the actual API. |
| File extents and block ownership | Merged PR #179 validates bounded extent chains, logical ordering/arithmetic, volume-capacity bounds, block ownership and allocated-byte totals. PR #181 adds pre-iteration logical/physical range validation and a checksum-valid oversized-block-count regression; build/boot and security workflows passed on exact PR head `a237ebd02e77609b47258ff18a4677857bbd8350`. | PR #182 adds checksum-valid zero-length and out-of-volume physical-start regressions; build/boot and security workflows passed on exact PR head `666a7e2f96f6c5dd0c7b11d1cf16602b08dafbaa`. PR #185 merged as `0e8e996f55df9da7840502d4dba4e5d0cb0934a8`; its exact head passed build/boot-test #7791 and security architecture #7328. The checksum-valid duplicate physical-block ownership regression checks read-only scan state and a clean rescan after restoration. Continue broader cross-inode aliasing and adversarial overlap review. |
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
- Integrity regressions cover report-only orphan blocks/inodes, checksum-valid out-of-capacity extents and oversized inode sizes, deterministic reports, and read-only media invariance. Journal regressions cover reused commit slots, contradictory commit metadata, write/flush failure boundaries, and recovery restart.
- Mock-device/QEMU tests do not prove physical hardware power-loss durability.
- Automatic repair and orphan reclamation remain deliberately deferred; a report-only candidate is not safe to free without a separately reviewed, crash-safe and resumable repair design.
