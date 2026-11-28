# Phase 1 Handover

## Current state
GitHub API is the sole source used. No CI, Build, QEMU, Security, or merge operation has been initiated. Tracking-only PR #652 is open.

## Verified GitHub state
- Main: `495291ecf1f2eda34d507e1c8ed09af641e9bb86`.
- PR #642: open, unmerged; branch ref `phase1/transactional-wait-reap-20261114` verified at `a1654c51a20a9d610ecde24f8b9361878762d303`.
- PR #652: open draft, branch `phase1/tracking-audit-20261128`; latest verified head `8fc3c40ca5f2ed352e1de71de44782ebeadda564` (verify again before next write).
- PR #652 base is main at `495291ecf1f2eda34d507e1c8ed09af641e9bb86`.

## Comparative audit findings
### Main source facts
- `kernel/process.c:process_wait()` verifies caller/child/zombie/refcount and `thread_process_reapable()`, then closes handles/destroys address space, marks DEAD, calls unregister, and only then copies the saved exit code to `exit_code`.
- `process_unregister()` checks registry membership and object/address-space/thread predicates, invokes `thread_reap_process()`, then unlinks and releases the object.
- `kernel/thread.c:thread_kernel_stack_valid()` validates one record's slot bit, base/size, guard mappings, four page mappings and PMM allocated state.
- `thread_process_has_live()`, `thread_process_reapable()`, `thread_reap_process()`, and several queue/registry walks traverse linked lists without a cycle bound in main. Specifically, `thread_process_reapable()` calls the unbounded live-thread scan before its own registry scan. A cyclic registry can therefore prevent bounded rejection. This is a source-confirmed robustness defect under registry-corruption conditions; severity provisional Medium (kernel liveness/DoS), subject to checking all mutation and validation paths.
- Main's `process.c` does not contain the staged spawn rollback helper `process_spawn_discard_unregistered()` found in PR branch; general public spawn is not equivalent to the internal init loader and must not be inferred complete.

### PR #642 branch comparison
- The current PR branch contains aggregate preflight additions in `process_wait()` and the previously absent staged spawn rollback helpers.
- Its changed `thread.h` declares registry-integrity/cycle/stack allocator self-test interfaces and aggregate rollback/address-space-disjoint preflights; changed `address_space.c` adds page-table/ownership destruction preflight and mapping rollback safeguards.
- PR #642's description and changed-file set show extensive code/documentation changes, but do not by themselves prove every claim. Confirm the implementations/tests against exact branch files before reuse. Do not duplicate fixes already on that branch.
- Exact current PR head is the branch ref SHA above; historical CI in PR text is not current-head evidence.

## Tests / CI
- No tests run by this agent.
- User directive reserves all CI/CL execution for a separate workflow. Build/QEMU/Security remain PENDING and will not be initiated here.
- PR #642 is not merged. PR #652 is documentation-only and not merged.

## Next exact work
1. Finish comparing PR #642 versions of `thread_process_has_live`, `thread_process_reapable`, `thread_reap_process`, registry validation, and corruption self-tests against main.
2. Inspect spawn rollback test markers/functions and determine whether #642 already resolves each mainline gap.
3. Decide if source-confirmed residual work belongs on #642 or a narrowly scoped dependent PR; do not duplicate existing branch fixes.
4. Update progress/worklog/handover in #652 with exact evidence; prepare implementation PR(s) and leave all CI verification pending for the designated workflow.

No implementation change or test pass is claimed.
