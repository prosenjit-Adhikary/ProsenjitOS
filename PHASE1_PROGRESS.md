# Phase 1 Progress

Updated: 2026-11-28

## Authoritative GitHub state
- Main: `495291ecf1f2eda34d507e1c8ed09af641e9bb86`.
- PR #642: `phase1/transactional-wait-reap-20261114`, head `17b4c629b0e3f521c960b73d2524475ebc168e20`, open/draft/unmerged, 64 files.
- PR #648: `phase1/boot-protocol-parser-tests-20261117`, head `971e3ddbabffe335272814954c7871eefbbf3f1b`, open/draft/unmerged. Its parser-facing header now reuses canonical `include/abi/boot.h`; branch is one commit behind current main and the PR must be rechecked against its merge base.
- PR #652: `phase1/tracking-audit-20261128`, prior head `2b08bd43e4a1d6afff5a8923bfd1ebc89fb3781b`; this update advances the tracking PR.
- No PR merged; no CI/CL, Build/QEMU or Security workflow run or triggered.

## Current status
- **Process lifecycle and registry:** previous source slice reviewed. Remaining zero-mutation snapshot gaps in rollback corruption fixtures are still open; do not re-review unchanged lifecycle code without a targeted reason.
- **PMM/VMM/heap:** previous transaction slice reviewed; no additional confirmed defect in that slice. Broader acceptance criteria remain open.
- **ProsenjitFS:** reviewed current journal recovery, transaction commit ordering, mount publication/cleanup, and relevant fail/retry fixtures against `docs/FILESYSTEM.md`. Recovery validates target bounds/uniqueness, journal exclusion, transaction IDs, checksums and reserved padding before replay. Replay errors retain the durable journal; remount retry and interrupted recovery stages are present. No production defect confirmed in this pass.
- **Confirmed test gap fixed:** PR #642's duplicate journal-target corruption fixture previously required `ECORRUPT` but did not assert zero medium mutation. It now snapshots and compares durable, volatile and dirty mock-device state around the rejected mount.
- **PR #648 integration fix:** removed duplicate Boot Protocol v1 wire struct definitions and reused canonical main ABI definitions. Parser-specific bounds/constants remain local aliases. No tests run.
- **64-file PR #642 review:** complete changed-file inventory exists, but semantic hunk review is not complete. Reviewed lifecycle/memory slices and targeted ProsenjitFS test hunk; remaining boot, ATA/block/panic, storage, security, IPC/VFS, scripts and documentation hunks still need independent semantic review.

## Requirement matrix
| Area | Source review | Regression coverage | Runtime/CI |
|---|---|---|---|
| Lifecycle/registry | Partial; prior reviewed slice | Partial; full zero-mutation snapshot audit open | PENDING |
| PMM/VMM/heap | Partial; prior reviewed slice | Not fully cross-checked against all contracts | PENDING |
| ProsenjitFS recovery | Partial; transaction/replay and selected corruption cases reviewed | Extensive existing matrix; semantic review incomplete | PENDING |
| Boot/ATA/block/panic/security/IPC/VFS/scripts | Incomplete | Incomplete | PENDING |

## Remaining work and next action
1. Continue semantic hunk review of all 64 #642 files; classify each hunk as lifecycle-critical, dependent, or independent and record reviewed paths.
2. Finish process rollback zero-mutation snapshots and retry review; add only targeted assertions for unproven invariants.
3. Continue ProsenjitFS corruption matrix review for torn header/commit/image/padding, mount abort cleanup, allocator/inode ownership, and remount recovery. Implement only confirmed gaps.
4. Cross-check the #648 canonical-header change against its merge-base consumers; then review remaining #648 PR dependencies without merging.
5. Revisit PMM/VMM/heap only for unmet architecture gates or new evidence.
6. Keep all PRs open/unmerged. No tests/workflows executed; runtime verification remains **PENDING**.
