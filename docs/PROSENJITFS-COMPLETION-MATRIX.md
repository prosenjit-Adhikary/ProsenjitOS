# ProsenjitFS Completion Matrix — 2026-10-22

## Purpose and baseline

This is a source-grounded tracking document, not a declaration that ProsenjitFS is production-complete. The inspection baseline for the integrity-scan implementation is branch
`hardening/prosenjitfs-report-only-orphan-inodes` (source file SHA observed during inspection:
`ed78325c8539de09daca7968a9b18d73323a162c` for `kernel/prosenjitfs.c`).

Relevant review work is still unmerged:
- PR #170: report-only orphan data-block candidates.
- PR #172: report-only orphan inode candidates; stacked on #170.
- PR #173: contradictory journal commit rejection and recovery hardening. This is a separate review branch and is **not assumed to be present** in the #172 source baseline.
- Issue #166: conservative integrity scan and staged self-healing.

Do not merge PRs, reclaim orphan objects, or describe unrun tests as passing as part of this work.

## Completion matrix

Status meanings: **Implemented in inspected source** means code exists, not that the full acceptance gate has passed. **Needs verification** means this inspection did not establish a passing result on the exact final commit. **Deferred** means the feature is not part of the current safe scope.

| Subsystem | Source-grounded status | Remaining acceptance work |
|---|---|---|
| On-disk format / superblock geometry | Implemented in existing ProsenjitFS; integrity checker validates key region ordering, bounds, bitmap capacity, inode-table capacity, and root inode. | Corrupt each geometry/feature/version case and verify mount rejects safely; run the current full matrix. |
| Mount and journal recovery ordering | Existing mount path invokes journal recovery before exposing the active mount; integrity API documents that scans require an active mount. | Verify the exact combined commit and all recovery-restart/failure-injection tests. PR #173 remains separate and open. |
| Block bitmap and allocation ownership | Integrity checker marks metadata references, compares allocated bits to ownership, checks metadata bits and out-of-range tail bits, and reports unowned allocated data blocks in report mode. | Exercise all bitmap boundaries, duplicate ownership, read failures, and cache/durable-media immutability on the final combined branch. |
| Inode bitmap and inode metadata | Checker validates allocation state, inode reads/checksums through existing helpers, inode types, parent range, and inode-bitmap tail bits. | Confirm all malformed inode/checksum/generation cases have focused regressions and correct stable report details. |
| Directories and names | Checker validates directory-index and entry bounds, name length/UTF-8/hash/checksum, duplicate names in a bucket, child type/parent, duplicate references, and parent chains to root. | Test malformed chains, cycles, repeated references, and corruption at every read boundary; review symlink namespace semantics against the actual API. |
| File extents and block ownership | Checker walks bounded extent chains, validates logical ordering and arithmetic for logical ends/allocated byte counts, marks extent and physical data blocks, and compares allocated bytes with inode metadata. | Add/confirm adversarial physical-range overflow, overlap, duplicate ownership, zero-length and maximum-boundary cases; verify every checked arithmetic path. |
| Orphan data-block detection | Implemented as report-only candidate detection in the inspected branch. Report mode completes the bounded bitmap scan and returns corruption when candidates exist. | Preserve zero mutation of durable, volatile, and dirty state on all outcomes; no reclamation without a separately proven crash-safe design. |
| Orphan inode detection | Implemented as report-only candidate detection for allocated, structurally valid non-root inodes with no directory reference and a parent chain reaching root. | Test unusual namespace/link semantics and ensure candidates are never treated as safe-to-free solely from this report. |
| Structured integrity report | Existing report includes status, first error location, inode and bitmap scan counts, and first/count of orphan block and inode candidates. | Ensure error classification is stable and documented; verify bounded memory and report initialization for every early-return path. |
| Create/read/write/append/truncate/delete | Core mutation paths and deterministic self-tests exist in the current implementation. | Run normal, zero-length, block/extent-boundary, max-size, invalid-input, persistence and remount tests on the exact final commit. |
| Directory create/remove/rename/path traversal | Core directory and mutation paths exist. | Verify parent/child invariants and failure rollback across interrupted transactions; include path/name edge cases. |
| Journal commit / replay / flush failures | Journal and recovery implementation exists. PR #173 describes focused fixes and injected-failure regressions, but it is a separate open PR. | Reconcile dependency/base SHAs; do not mark complete until tests pass on the intended combined commit and recovery remains restartable. |
| Failed I/O and resource cleanup | Checked block helpers and cleanup paths exist in the inspected code. | Inject short/read/write/flush failures where supported; check cleanup, dirty-state immutability, and error propagation. |
| Concurrency and serialization | Integrity API explicitly requires caller-side serialization against concurrent mutation. | Confirm all callers obey this precondition; do not claim the scan is internally concurrency-safe. |
| Read-only/degraded mount behavior | Degraded-superblock read-only behavior is a stated architecture constraint in Issue #166. | Test that uncertain metadata/recovery states remain read-only and cannot mutate media. |
| VFS/provider and boot integration | Existing VFS integration and ISO/native boot test infrastructure are documented in repository sources and PR verification notes. | Run current security, build, ISO QEMU, native BIOS QEMU and filesystem self-tests on the exact final SHA. |
| CoW, snapshots, reflink, ACLs/xattrs, links, quotas, compression, dedup, online resize, relocation | Listed as long-term targets in README, not established as implemented by this audit. | Explicitly defer until design, API/on-disk compatibility, recovery semantics and tests are implemented. |
| Repair / orphan reclamation | **Deferred for safety.** The inspected scan is read-only and report-only. | Require proven reachability/link semantics, journaled or otherwise crash-safe repair, resumable interrupted repair, explicit operator-controlled mode, dry run, and a clean follow-up scan before implementation. |

## Safe execution sequence

1. Preserve PR dependency order; keep #170/#172/#173 open and unmerged unless explicitly authorized.
2. Reconcile the journal-hardening branch with the report-only integrity branch without rewriting shared history.
3. Fix one source-proven defect at a time and add a regression before changing behavior.
4. Run narrow filesystem tests, then the warning-clean build, security workflow, ISO QEMU boot, native BIOS QEMU boot, remount/persistence and recovery/fault-injection suites.
5. Inspect the actual workflow logs and exact commit SHA. Update this matrix only with results tied to that SHA.
6. Do not enable destructive repair until the repair safety gates above are satisfied.

## Verification record

- Live repository and Issue #166 inspected on 2026-10-22.
- Inspected integrity-scan implementation in `kernel/prosenjitfs.c` on `hardening/prosenjitfs-report-only-orphan-inodes`.
- PR #170, #172 and #173 were found open/unmerged during inspection.
- No build, QEMU boot, runner job, or new test execution is claimed by this document.
