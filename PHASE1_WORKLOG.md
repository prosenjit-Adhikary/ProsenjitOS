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


## 2026-11-28 — P1-WORKFLOW-SCOPE-005
- **Review:** Inspected both workflow diffs in PR #642. They removed the established runner selector, changed build timeout and concurrency cancellation, added branch push triggers, and added diagnostic steps. These are independent of kernel lifecycle correctness.
- **Change:** Restored `.github/workflows/build.yml` and `.github/workflows/security-design.yml` byte-for-byte to official main at `495291ecf1f2eda34d507e1c8ed09af641e9bb86`. PR #642 now changes 64 files; neither workflow appears in its changed-file list.
- **Verification:** GitHub ref and PR metadata both report head `58bcf8f808baa549cf3180af1a21e558c1afd562`; both workflow files compare equal to main; PR #642 remains open, unmerged, draft.
- **Tests:** None run. No workflow was triggered or awaited. Runtime verification PENDING.
- **Next:** Continue exact source/test review of the remaining PR files and dependency-ready PMM/VMM/heap/ProsenjitFS requirements.


## 2026-11-28 — P1-ROLLBACK-006
- **Source change:** Extended the existing stage 1–5 spawn rollback fixture with before/after checks for PMM free-page count, allocated and quarantined kernel-stack slot bitmaps, runnable-thread count, and process-registry cardinality.
- **Requirement:** Every failed construction/registration/enqueue stage must remove all resources and publication state.
- **PR #642 exact head:** `ac4c54be132bab45cd7f840f85aadc12e927c275`; branch ref and PR head match. The commit adds 18 lines to `kernel/process.c`.
- **Cross-check:** The snapshot uses the existing read-only stack allocator API and existing process/PMM/scheduler counters. No workflow files changed; PR remains draft.
- **Tests:** Not run under the no-CI/CL instruction. Runtime verification PENDING.
- **Remaining:** These scalar/bitmap checks improve leak detection but do not yet snapshot every page-table entry, ownership-list link, process/thread registry slot, or queue-neighbor link for each corruption rejection.


## 2026-11-28 — P1-CRASH-RECOVERY-007
- **Confirmed defect:** `pros_crash_record_attach_internal()` reset `crash_sequence` before scanning the two persistent slots while `pros_crash_record_capture()` remained callable from fatal exceptions, including NMI. A capture during slot I/O could create a pending sequence from the temporary zero value; attach then restored the persisted sequence/cursor, leaving the pending record stale.
- **Fix:** Attach claims the existing atomic capture guard through the entire slot scan and publishes the panic writer only after sequence/cursor discovery. Detach claims the same guard, clears panic readiness before state, and declines without mutation if capture/attach owns it.
- **Regression coverage added:** The crash-record mock block read attempts a capture during reattach; the fixture requires ENOTSUP, no pending record, retained persisted sequence, correct next-slot, and the same latest record. A second fixture proves detach while the guard is owned leaves the attachment/callback state intact.
- **PR #642:** exact current head `e5f8a24fa8fac336d4a44c0b2478919b29c6a465`; open, unmerged, draft.
- **Cross-check:** The crash-record integration audit states exception dispatch captures before fatal handoff and that NMI avoids storage I/O. The regression calls the actual attach path through the mock block-device reader.
- **Tests:** Not run under the explicit no-CI/CL instruction. Runtime verification PENDING.
- **Remaining:** Review pending-record operations against NMI interleaving and source-audit the full crash-write/recovery matrix; test code has not executed.


## 2026-11-28 — P1-HISTORY-REVIEW-008
- **Verified history shape:** GitHub compare from main `495291ecf1f2eda34d507e1c8ed09af641e9bb86` to PR #642 head `e5f8a24fa8fac336d4a44c0b2478919b29c6a465` reports 1,036 commits ahead, zero behind, and a 64-file tree diff. The compare response exposes at most 250 commit entries; its visible range includes repeated same-message commits.
- **Safety:** No history was rewritten, reset, or force-pushed. PR #642 remains draft/unmerged. Treat the commit graph as an outstanding review item; do not collapse or rewrite it without a separate, explicit branch strategy review.
- **Open PR cross-check:** PR #642 is the kernel/crash-record implementation branch; PR #652 is tracking documentation. No merge performed.
- **Next:** Continue exact hunk/dependency review with the complete current tree diff, and identify whether the long commit chain affects review/merge safety before proposing any branch restructuring.


## 2026-11-28 — P1-ROLLBACK-009
- **Source/test change:** Unpublished and registered peer-thread rejection fixtures now snapshot and compare the exact stack-slot allocation/quarantine bitmaps and process-registry cardinality, alongside existing free-page, runnable-count, exact handle-entry, process/thread and root checks.
- **PR #642 current head:** `b5320ccda63456c33b6f0c0cab8c572fc0549b41`; GitHub branch and PR refs match; PR remains draft/unmerged.
- **Tests:** Not run. Runtime verification PENDING.
- **Remaining zero-mutation gaps:** Page-table leaf snapshots, owned-page list contents, complete thread registry slots, and queue-neighbor links are not yet captured for every preflight rejection.



## 2026-11-28 — P1-SOURCE-REVIEW-010

- **Refs verified:** official main/base `495291ecf1f2eda34d507e1c8ed09af641e9bb86`; PR #642 head `b5320ccda63456c33b6f0c0cab8c572fc0549b41`, open/unmerged/draft, 64 files; PR #652 tracking head before this update `d5edf0e4c6aa408e0b60cd3d230f3cdb1d1514c1`.
- **Hunks/source reviewed:** PR #642 thread registry integrity and process stack/reap preflight, process spawn rollback preflight, address-space destroy/owned-page preflight and commit, PMM allocation/free batch and mixed-free paths, heap live-allocation authentication, plus the Phase 1 master and native-spawn contract sections.
- **Verified implementation evidence:** thread registry validates a bounded maximum of 64 trusted identity slots before following links, rejects cycles/aliases and duplicate stack pages, and validates allocator-slot agreement. Spawn rollback composes process, handle-table, address-space, stack and queue preflights before destructive cleanup. Address-space destroy validates private page-table/leaf ownership, heap-live metadata identity and cross-alias with registered kernel stacks before a mixed PMM release. PMM batch and mixed release validate the full set before changing bits; heap allocation liveness requires membership in a validated allocator chain.
- **Finding classification:** no new defect confirmed in this source slice. This is not a full PR review: remaining changed-file hunks, complete corruption fixture assertions, ProsenjitFS exact source comparison, and the 64-file dependency map still require review.
- **Tests:** none executed. No CI/CL, Build/QEMU or Security workflow was triggered or awaited. Runtime verification PENDING.
- **Next:** continue hunk review across ATA/block/crash-record/panic handoff, filesystem, boot and script changes; compare with main and authoritative subsystem contracts; patch only confirmed gaps.
- **Tracking branch commit will be recorded after all three documents are committed; PR #652 remains open and draft.


## 2026-11-28 — P1-CRASH-TEST-011

- **Gap:** The two-boot crash-record recovery fixture validated sequence, vector, error code, RIP and CR2 but did not assert the documented deterministic zero timestamp for its early-boot trigger.
- **Change:** Added `record.timestamp != 0u` to the existing Boot 2 failure predicate in `kernel/kernel.c`; reused the existing test path and marker. No production behavior changed.
- **PR #642:** commit/head `ec1834f0e436040c34b54aefc54202d0efc296e5`, verified through PR metadata and GitHub compare from prior head `b5320ccda63456c33b6f0c0cab8c572fc0549b41` (one commit, one changed file, one added and one removed line). PR remains draft/unmerged.
- **Evidence boundary:** Source contract states the gate runs before hardware IRQs and zero timestamp is valid; current test now checks the persisted value through `pros_crash_record_peek()` on Boot 2. No tests/workflows executed; runtime remains PENDING.
- **Remaining:** Complete all 64-file hunk/dependency review and compare exact ProsenjitFS recovery source with its canonical contract.


## 2026-11-28 — P1-DEPENDENCY-REVIEW-012 (refreshed)

- **Current refs:** main `495291ecf1f2eda34d507e1c8ed09af641e9bb86`; PR #642 `c9e0635b9b64bc7cf4761b3674c056b9f6a1d769`; PR #648 `971e3ddbabffe335272814954c7871eefbbf3f1b`; PR #652 prior head `ed9156e114ae77f3956e5f8afa0eb2d48ec5cb12`.
- **New PR #642 coverage:** duplicate-target journal corruption rejection now checks exact durable and volatile bytes for the two candidate home blocks and four journal blocks, exact dirty bitmap bytes, and full medium/cache CRCs. Diff since prior #642 head `17b4c629b0e3f521c960b73d2524475ebc168e20` is one file, `kernel/prosenjitfs.c`.
- **New #642 boot hunk:** commit `7a06c179c396b2485abad4d6f904def876bd45f0` restores the exact ELF64 userspace marker searched by current main's Build/QEMU workflow. Source search confirmed the workflow expectation; no workflow was run.
- **ProsenjitFS source review:** replay validates target ranges/uniqueness and journal exclusion, commit/header IDs and counts, checksums and reserved padding before applying home images. Replay failure retains the journal for retry; mount aborts before publishing a root on error. Existing mock-device tests cover interrupted write/replay, accepted-then-EIO, journal-clear write/flush errors, retry and integrity/remount controls. The full architecture and corruption matrix remains under review; no production defect confirmed in this pass.
- **PR #648 source review:** parser C, parser header, layout test and parser test consume the parser-facing header; it now imports canonical ABI structs. Scripts compile with `-Ikernel/include`; the relative include resolves to root `include/abi/boot.h`. Parser-specific limit aliases remain local. Source compatibility was checked; no test/build run.
- **Additional #642 semantic review this pass:** syscall ABI/dispatch, usercopy C/assembly, IPC receive reservation/cancel/commit flow, security quarantine and exec fixtures, and build/stack/timer/crash-record script hunks. This does not complete all 64 changed-file hunks.
- **Paths inspected in this pass (partial hunk review, not full-file signoff):** `include/abi/syscall.h`; `kernel/syscall.c`; `kernel/usercopy.c`; `kernel/usercopy.asm`; `kernel/ipc.c`; `kernel/intent.c`; `kernel/security_exec.c`; `kernel/security_hardening.c`; `kernel/prosenjitfs.c`; `boot/native_stage2.asm`; `scripts/build.sh`; `scripts/test_timer_irq_assembly.sh`; `scripts/test_kernel_stack_guard.sh`; `scripts/test_crash_record_two_boot.sh`. PR #648 consumer paths inspected: `kernel/include/pros_boot_protocol_v1.h`, `kernel/include/pros_boot_protocol_v1_parser.h`, `kernel/boot_protocol_v1.c`, `tests/boot_protocol_abi_test.c`, `tests/boot_protocol_v1_parser_test.c`, both boot protocol test scripts and `Makefile`.
- **Still open:** semantic review of all 64 #642 files; exact rollback snapshots for full process/thread registry and queue order, page-table/owned-page state, and PMM/object references; remaining ProsenjitFS corruption cases.
- **Tests/workflows:** not executed or triggered. Runtime verification PENDING.


## 2026-11-28 — P1-CURRENT-REFS-013

- **Live GitHub refs checked:** main `495291ecf1f2eda34d507e1c8ed09af641e9bb86`; PR #642 `c9e0635b9b64bc7cf4761b3674c056b9f6a1d769`; PR #648 `971e3ddbabffe335272814954c7871eefbbf3f1b`; PR #652 `1ff66b708a16b45f4800022aaec8f006727eabbe` before this documentation checkpoint. PR #642 remains open/draft/unmerged with 64 files; #648 and #652 remain open/draft/unmerged.
- **SHA reconciliation:** Earlier worklog sections record the heads that were current at their respective checkpoints. They are historical references, superseded by the live refs above; they do not identify the current heads.
- **Source reviewed this pass:** PR #642 `kernel/prosenjitfs.c` duplicate-target journal-mount rejection hunk; official-main `docs/FILESYSTEM.md` canonical journal/recovery contract. The fixture checks exact bytes for candidate home/journal sectors and the complete dirty map, retaining whole-medium/cache CRC comparisons. This pass found no additional production defect. The ProsenjitFS architecture/crash matrix and all-64-file semantic review remain open.
- **Tests/workflows:** none executed or triggered. Runtime verification remains **PENDING**.
- **Next:** continue semantic hunk review of remaining boot, ATA/block, crash/panic and filesystem changes; then inspect exact lifecycle rejection/retry snapshots and continue subsystem gaps. No PR merge, history rewrite, or workflow execution.
