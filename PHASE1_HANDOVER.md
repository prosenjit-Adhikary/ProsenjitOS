# Phase 1 Handover

## Current state
Phase 1 audit has resumed using GitHub API as the sole development source. The prior Windows checkout is not being used. This branch currently adds tracking documents only; no kernel fix or acceptance-gate pass is claimed.

## Repository and branches
- Repository: `prosenjit-Adhikary/ProsenjitOS`.
- Verified main baseline at start: `495291ecf1f2eda34d507e1c8ed09af641e9bb86`.
- Tracking branch: `phase1/tracking-audit-20261128`, created from that main SHA.
- Existing lifecycle PR: #642, branch `phase1/transactional-wait-reap-20261114`; its description reports head `a1654c51a20a9d610ecde24f8b9361878762d303`. Refresh PR metadata before editing or citing current-head CI.
- Tracking branch latest known file commit: `247d8601bfb230063cbb087f8f34d359ea400678`; verify remote ref before proceeding.

## Comparative source audit — partial
Verified main source observations:
- `kernel/process.c:process_wait()` checks parent/child, zombie state, refcount and `thread_process_reapable()`, then calls `process_release_resources()` (handles then address space), sets DEAD, calls `process_unregister()`, and only after success writes the exit code to `exit_code`.
- `process_unregister()` locates process list membership, validates DEAD/root/refcount and thread live/current state, calls `thread_reap_process()`, then unlinks and releases object.
- `kernel/thread.c:thread_kernel_stack_valid()` checks slot range/bit, stack base/size, guard translations, each of four mapped pages and PMM allocation status.
These observations are not yet a full defect proof. Need inspect all functions, callers, PR #642 patch, tests, and workflow conditions.

## Required next audit steps
1. Refresh live PR #642 metadata and fetch its complete diff; compare every changed path against current main to identify fixes already merged or duplicated.
2. Inspect complete `kernel/process.c`, `kernel/thread.c`, headers, `kernel/address_space.c`, handle-table implementation, syscall/usercopy call chain, and relevant self-tests.
3. Trace spawn failpoints and rollback helpers. Verify whether cleanup is preflighted before first destructive mutation and whether rejection leaves all registries/resources unchanged.
4. Trace thread registry and run-queue traversal bounds, uniqueness, cycle handling, stack ownership alias prevention and teardown ordering.
5. Verify process wait output-pointer validation occurs before reap, and that failed copyout cannot lose exit status.
6. Map each confirmed defect to minimal tests and code changes; preserve one logical unit per branch/PR.
7. Inspect workflow triggers and required checks. Run Build/QEMU, Security and focused regressions on exact PR head; skipped/unavailable is not pass.
8. Update these records with verified SHAs and actual results after each meaningful checkpoint.

## Findings classification
No severity assigned yet because reachability and exploitability audit is incomplete. Tracking records and old PR body are not substitutes for proof.

## Verification
- Tests: none run by this continuation.
- Build/QEMU: pending.
- Security: pending.
- Last verified source baseline: main `495291ecf1f2eda34d507e1c8ed09af641e9bb86`.
- Last verified code-change SHA: none.
- Local unpushed changes: none used or incorporated.
