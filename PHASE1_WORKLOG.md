# Phase 1 Worklog

Chronological entries. No CI/CL is run by this agent under the current directive.

## 2026-11-28 — P1-AUDIT-001
- **Problem:** Resume Phase 1 audit against authoritative GitHub source and compare main with PR #642.
- **Verified baseline:** main `495291ecf1f2eda34d507e1c8ed09af641e9bb86`; PR #642 branch `phase1/transactional-wait-reap-20261114` at `a1654c51a20a9d610ecde24f8b9361878762d303`.
- **Source facts:** main `process_wait()` releases handles/address space before `process_unregister()` and `thread_reap_process()`; main registry scans in `thread_process_has_live()`, `thread_process_reapable()`, and `thread_reap_process()` are unbounded. PR #642 adds aggregate preflight and bounded/validated registry checks; these are existing candidate fixes, not duplicates to reimplement.
- **Files changed:** Initial three tracking docs on `phase1/tracking-audit-20261128`; latest progress checkpoint in this PR.
- **Implementation:** Tracking branch created from main. Commits: `208380e4ca06b2e431bd2bfd2256d32b31a51821` (progress), `247d8601bfb230063cbb087f8f34d359ea400678` (worklog), `8fc3c40ca5f2ed352e1de71de44782ebeadda564` (handover), `c862564599de300fbae603e65d17009236cd2767` (comparative-audit handover), `5984d491ef60b0f55d3655595c5133b823598b1c` (latest progress checkpoint).
- **Push confirmation:** GitHub contents API accepted each commit; branch ref must be queried for final current head.
- **Tests:** None executed; user assigned CI/CL to separate workflow.
- **Remaining:** Complete comparative review of PR tests/dependencies; assess native launch gate; audit PMM/VMM/heap/ProsenjitFS crash recovery; implement remaining confirmed defects and prepare PRs with exact-head runtime verification pending.

## 2026-11-28 — P1-AUDIT-002
- **Problem:** Check whether the thread registry can hang lifecycle/reap processing and whether PR #642 already fixes it.
- **Root cause:** Main lifecycle helpers traverse the registry without a cycle/count check; `thread_process_reapable()` calls the live-thread scan first. A cyclic list can keep these checks from returning.
- **Classification:** Provisional Medium availability hardening under corrupted-registry conditions; scope of corrupt-pointer reachability requires continued review.
- **Correction:** No correction applied to main. PR #642’s branch already contains registry validation, fail-closed liveness check, bounded walk and cycle/corruption regressions. Avoid duplicating; audit its exact tests and overall patch scope.
- **Regression tests:** None run. PR test presence is not execution evidence.
- **Verification:** Source comparison through GitHub API only; runtime pending.


## 2026-11-28 — P1-DEPENDENCY-003
- **Problem:** Classify the 66-file PR #642 and establish lifecycle dependency boundaries before extraction.
- **Verified input:** PR #642 exact branch head a1654c51a20a9d610ecde24f8b9361878762d303; changed-file inventory contains 66 paths. Full inventory is grouped in PHASE1_HANDOVER.md.
- **Review result:** Process/thread lifecycle directly depends on scheduler/context-switch/IDT, address space/paging, PMM/heap, object/handle/IPC, syscall/usercopy/ABI, ELF/admission/VFS refs and boot self-test integration. Boot protocol producer/parser, storage/crash-record code, and workflow policy changes are independently reviewable; no lifecycle dependency is yet established for storage/crash records. Build/security workflow changes alter runner/concurrency configuration and should not be silently included in a lifecycle correctness decision.
- **Corruption test evidence:** PR branch contains registry identity/owner/stack corruption tests, bounded run-queue validation, stage 1-5 rollback cleanup assertions, additional preflight rejection/retry cases, and aggregate wait/reap preflight. Some fixtures use scalar snapshots/fingerprints rather than full graph snapshots; exact assertion coverage remains open. No tests executed.
- **Files changed:** PHASE1_HANDOVER.md and PHASE1_PROGRESS.md on tracking branch only.
- **Commit:** Handover map eba2a13327269d62e71830723596beb7cf65ea4f; progress checkpoint b447ea0a8ded867955a65a8214c5fa83fe8759e5.
- **Push status:** GitHub contents API accepted commits; verify actual ref after this worklog commit.
- **Tests:** None. Runtime verification PENDING; no CI workflows triggered.
- **Next:** Finish PR hunk/dependency review and zero-mutation assertions, then proceed with PMM/VMM/heap and ProsenjitFS source audit.


## 2026-11-28 — P1-ROLLBACK-004
- **Source change:** Replaced FNV handle-table fingerprints in the unpublished and queued spawn-rollback rejection fixtures with exact per-entry comparisons of object pointer, generation, rights, and reserved bits.
- **Reason:** A hash fingerprint can collide and does not prove zero mutation of every handle entry. The stronger assertion directly checks all fields used by handle-table validation and teardown.
- **PR:** #642, branch `phase1/transactional-wait-reap-20261114`, exact current head `462ef15fe32bf75e163cc2b84a50597395e62e0e`; PR remains open and draft.
- **Cross-check:** This reuses the existing rollback preflight tests; it adds no new lifecycle implementation and does not alter the workflow files.
- **Tests:** Not run by instruction. Runtime verification PENDING; no CI workflows triggered.
- **Remaining:** Full 66-file/hunk and dependency review, broader resource/registry zero-mutation assertions, PMM/VMM/heap/ProsenjitFS source audit, and independent exact-head verification.
- **Tracking branch base before this update:** `d4dd0adef60e0b91699cb4505d9496a938907df2`.
