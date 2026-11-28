# Phase 1 Progress

Updated: 2026-11-28

## Verified GitHub refs
- Main: `495291ecf1f2eda34d507e1c8ed09af641e9bb86`.
- PR #642: `phase1/transactional-wait-reap-20261114`, head `c9e0635b9b64bc7cf4761b3674c056b9f6a1d769`, open/draft/unmerged, 64 changed files.
- PR #648: `phase1/boot-protocol-parser-tests-20261117`, head `971e3ddbabffe335272814954c7871eefbbf3f1b`, open/draft/unmerged. Parser-facing wire types now come from canonical `include/abi/boot.h`.
- PR #652 tracking head immediately before this refresh: `ed9156e114ae77f3956e5f8afa0eb2d48ec5cb12`.
- No PR merged. No CI/CL, Build/QEMU or Security workflow executed; runtime verification PENDING.

## This checkpoint
- PR #642 received a separate commit restoring the exact ELF64 userspace milestone required by current main's Build/QEMU workflow. Main source/workflow search confirms the required marker. This test-gate compatibility hunk was source-reviewed; workflow was not run.
- PR #642: extended the duplicate-target journal corruption regression. It now compares the exact bytes of both home targets and all journal blocks in durable and volatile media, compares the entire dirty bitmap byte-for-byte, and retains whole-medium/cache CRC checks.
- PR #648: source review confirms all direct parser consumers include the parser header; it imports canonical wire structs using a relative path resolving to `include/abi/boot.h`. Parser-specific early physical limit and maximum parsing policy remain local. Both host test scripts include `kernel/include`; no test was executed.
- PR #642 semantic source review continued across syscall ABI/dispatch, usercopy C/assembly, IPC transactional receive, security quarantine/admission, and build/assembly regression scripts. This is still a partial review of 64 changed files, not completion.

## Requirement matrix
| Area | Source review | Regression coverage | Runtime/CI |
|---|---|---|---|
| Process/thread lifecycle and registry | Prior transaction slice; unchanged files not re-reviewed | Partial; full process/queue/AS/PMM exact snapshots still open | PENDING |
| PMM/VMM/heap | Prior transaction slice; revisit only unmet criteria/new findings | Broader acceptance cross-check incomplete | PENDING |
| ProsenjitFS recovery | Journal/replay/mount paths and selected corruption/retry fixtures reviewed | New exact zero-mutation journal fixture; broader torn-write/ownership audit incomplete | PENDING |
| PR #648 Boot Protocol parser | Header consumers and aliases reviewed; one commit branch integration reviewed | Test source reviewed, not executed | PENDING |
| Other #642 hunks | Partial; reviewed paths recorded in worklog | Incomplete | PENDING |

## Remaining work / exact next action
1. Complete semantic review of every PR #642 hunk, marking each path reviewed against main, architecture, contracts and dependencies.
2. Close remaining lifecycle snapshot gaps: registry identities/links, runqueue order/neighbor links, address-space page-table/owned-page state, PMM ownership and object/handle references; ensure a rejected preflight can be retried without hidden mutation.
3. Finish ProsenjitFS corruption/replay/remount matrix, emphasizing exact disk/cache comparisons where feasible and allocation/metadata ownership.
4. Review all remaining PR #648 consumers and integration dependencies against current main.
5. Revisit PMM/VMM/heap only for uncovered documented gates or new defects.
6. Keep every PR open and draft. No tests/workflows run; runtime verification remains PENDING.
