# Phase 1 Progress

Updated: 2026-11-28

## GitHub state
- Main baseline: 495291ecf1f2eda34d507e1c8ed09af641e9bb86.
- PR #642 open, unmerged, draft; branch `phase1/transactional-wait-reap-20261114` at `b5320ccda63456c33b6f0c0cab8c572fc0549b41`.
- PR #652 open draft; tracking branch `phase1/tracking-audit-20261128`; head before this update `0810654845f44f870fc755a5b93731bc56b77fca`.
- CI/CL, Build/QEMU/Security not run; runtime verification PENDING.

## Completion estimate
Not scored; source audit, implementation and regression review are incomplete.

## Current findings
- Main process_wait destroys handles/address space before thread stack reap; PR #642 adds aggregate read-only preflight before teardown.
- Main thread registry scans are unbounded; PR #642 adds bounded registry/run-queue validators and corruption tests.
- Main's native init loader is not a general spawn/exec API.
- PR #642 crash-record attach previously allowed NMI capture during slot scan after resetting the sequence; current head guards attach/detach and adds a callback-driven regression. Pending-record concurrency and full crash-matrix audit remain open.
- PR #642 now changes 64 files after its two workflow files were restored exactly to main; the earlier inventory was 66. Lifecycle depends on process/thread/scheduler, address-space/paging, PMM/heap, object/handle/IPC, usercopy/syscall/ABI, ELF/security/VFS references and kernel test integration. Boot protocol, storage/crash-record and workflow changes are separately reviewable; do not bundle automatically.
- Workflow diff was reviewed separately; both workflow files were restored exactly to main, verified by GitHub API, and are no longer in PR #642's changed-file list.

## Changes and tests
- Tracking handover now contains a complete 66-path subsystem grouping and dependency map plus partial zero-mutation review.
- Source/test changes on PR #642: exact per-entry handle-state comparisons replaced probabilistic fingerprints in two rollback-rejection fixtures; stage 1–5 rollback fixtures now snapshot PMM, stack-slot/quarantine, runnable-count and registry-count state; workflow policy files were restored to main. Latest verified PR head: `e5f8a24fa8fac336d4a44c0b2478919b29c6a465`; current crash-record attach/detach race hardening and peer-thread zero-mutation snapshots are recorded in the worklog. No tests executed; runtime verification PENDING.

## Next actions
1. Finish hunk-level audit of the remaining 64 PR #642 files and cross-subsystem dependencies.
2. Finish fixture-by-fixture zero-mutation/retry review; exact handle snapshots are improved, while page-table, owned-page, registry-slot and queue-neighbor snapshot coverage remains open.
3. Continue PMM/VMM/heap and ProsenjitFS crash-recovery source audit, then implement only confirmed gaps without duplicating PR #642.
4. Use focused PR structure after dependency review; leave all runtime gates PENDING.

## History review
GitHub compare reports PR #642 at 1,036 commits ahead, zero behind, with a 64-path diff. No history rewrite or merge was performed. The long commit chain is an open review/scope-structure concern.

Remaining rejection-fixture snapshot gaps: PTE and owned-page graph, complete thread registry slots, and run-queue neighbor links. No tests executed; runtime verification PENDING.



## Source-audit checkpoint — 2026-11-28

- Official main remains `495291ecf1f2eda34d507e1c8ed09af641e9bb86` (verified by repository commit search and PR #642 base).
- PR #642 exact head is `b5320ccda63456c33b6f0c0cab8c572fc0549b41`, open/unmerged/draft, 64 changed files. Its compare is 1,037 commits ahead / 0 behind; do not rewrite history. The full hunk review is still incomplete.
- PR #652 tracking branch exact head before this checkpoint: `d5edf0e4c6aa408e0b60cd3d230f3cdb1d1514c1`; both PRs were refreshed from GitHub API.
- Source review this session: bounded thread-registry identity/cycle/stack-page validation; bounded runqueue validation; aggregate spawn rollback and process wait/reap preflight; PMM free transactions; VMM destroy preflight/commit; heap allocator chain validation and poisoned growth rollback. No new defect was confirmed in this inspected slice.
- Cross-subsystem comparison: address-space destruction checks owned-page heap identity, page-table/data-page uniqueness, stack-page disjointness, and one PMM mixed-free commit. PMM and heap mutations preserve caller interrupt state for the documented single-CPU design; no SMP guarantee is claimed.
- Crash-record contract/source reviewed; attach/capture race fix and mock regression remain at PR #642 head. The dedicated two-boot fatal-write/readback plan explicitly remains unimplemented/unverified; ordinary QEMU and fake-I/O tests are not evidence of durable panic persistence.
- ProsenjitFS historical mainline design docs record merged multi-block torn-write matrices (FS-72/FS-73) and consistency preflight work; exact current source-to-contract and PR hunk review remains open.
- Tests executed this session: none. CI/CL, Build/QEMU, Security and runtime verification were not run; runtime remains PENDING.
- Current task: finish all 64 changed-file hunk/dependency review; close zero-mutation gaps; complete PMM/VMM/heap and ProsenjitFS source-to-contract review; implement only confirmed defects.
- Exact next action: review remaining PR #642 patches by subsystem, beginning with ATA/block/crash-record and panic handoff, then filesystem and boot scripts; compare each patch with main and relevant contracts. Keep #642 draft and do not merge.


## Latest implementation checkpoint — 2026-11-28

- PR #642 head verified at `ec1834f0e436040c34b54aefc54202d0efc296e5`; the only source change since `b5320cc` is one added Boot 2 crash-record recovery assertion: the saved timestamp must equal the documented early-boot zero value.
- The change is source/test coverage only. No tests or workflows were run. Runtime verification PENDING; PR #642 stays draft and unmerged.
- Latest exact next action: continue reviewing the remaining PR hunks and subsystem dependencies; after that, compare ProsenjitFS journal recovery source against its current canonical contract and test matrix.
