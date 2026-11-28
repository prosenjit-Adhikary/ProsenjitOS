# Phase 1 Progress

Updated: 2026-11-28

## GitHub state
- Main baseline: 495291ecf1f2eda34d507e1c8ed09af641e9bb86.
- PR #642 open/unmerged; branch phase1/transactional-wait-reap-20261114 at a1654c51a20a9d610ecde24f8b9361878762d303.
- PR #652 open draft; tracking branch `phase1/tracking-audit-20261128`. Branch head at the start of this checkpoint: `d4dd0adef60e0b91699cb4505d9496a938907df2`.
- CI/CL, Build/QEMU/Security not run; runtime verification PENDING.

## Completion estimate
Not scored; source audit, implementation and regression review are incomplete.

## Current findings
- Main process_wait destroys handles/address space before thread stack reap; PR #642 adds aggregate read-only preflight before teardown.
- Main thread registry scans are unbounded; PR #642 adds bounded registry/run-queue validators and corruption tests.
- Main's native init loader is not a general spawn/exec API.
- PR #642 now changes 64 files after its two workflow files were restored exactly to main; the earlier inventory was 66. Lifecycle depends on process/thread/scheduler, address-space/paging, PMM/heap, object/handle/IPC, usercopy/syscall/ABI, ELF/security/VFS references and kernel test integration. Boot protocol, storage/crash-record and workflow changes are separately reviewable; do not bundle automatically.
- Workflow diff was reviewed separately; both workflow files were restored exactly to main, verified by GitHub API, and are no longer in PR #642's changed-file list.

## Changes and tests
- Tracking handover now contains a complete 66-path subsystem grouping and dependency map plus partial zero-mutation review.
- Source/test change on PR #642: exact per-entry handle-state comparisons replaced probabilistic fingerprints in two rollback-rejection fixtures; workflow policy files were restored to main. Latest verified PR head: `58bcf8f808baa549cf3180af1a21e558c1afd562`. No tests executed.

## Next actions
1. Finish hunk-level audit of the remaining 64 PR #642 files and cross-subsystem dependencies.
2. Finish fixture-by-fixture zero-mutation/retry review; exact handle snapshots are improved, while page-table, owned-page, registry-slot and queue-neighbor snapshot coverage remains open.
3. Continue PMM/VMM/heap and ProsenjitFS crash-recovery source audit, then implement only confirmed gaps without duplicating PR #642.
4. Use focused PR structure after dependency review; leave all runtime gates PENDING.