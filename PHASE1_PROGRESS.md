# Phase 1 Progress

Updated: 2026-11-28

## GitHub state
- Main baseline: 495291ecf1f2eda34d507e1c8ed09af641e9bb86.
- PR #642 open, unmerged, draft; branch `phase1/transactional-wait-reap-20261114` at `e5f8a24fa8fac336d4a44c0b2478919b29c6a465`.
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
- Source/test changes on PR #642: exact per-entry handle-state comparisons replaced probabilistic fingerprints in two rollback-rejection fixtures; stage 1–5 rollback fixtures now snapshot PMM, stack-slot/quarantine, runnable-count and registry-count state; workflow policy files were restored to main. Latest verified PR head: `ac4c54be132bab45cd7f840f85aadc12e927c275`. No tests executed; runtime verification PENDING.

## Next actions
1. Finish hunk-level audit of the remaining 64 PR #642 files and cross-subsystem dependencies.
2. Finish fixture-by-fixture zero-mutation/retry review; exact handle snapshots are improved, while page-table, owned-page, registry-slot and queue-neighbor snapshot coverage remains open.
3. Continue PMM/VMM/heap and ProsenjitFS crash-recovery source audit, then implement only confirmed gaps without duplicating PR #642.
4. Use focused PR structure after dependency review; leave all runtime gates PENDING.