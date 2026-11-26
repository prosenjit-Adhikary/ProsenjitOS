# Pull Request Implementation Status and Evidence Standard

Status: mandatory repository-wide engineering rule.

## Purpose

Every ProsenjitOS pull request must leave an accurate, self-contained record of what was actually implemented, what remains, and what evidence supports each claim. This is required for future human maintainers and AI agents to resume work without guessing, repeating work, or treating a proposal as an implementation.

This rule applies to **every PR**, regardless of size, subsystem, author, draft/open/closed state, or whether the PR is code-only, tests-only, documentation-only, or a partial milestone.

## Required PR documentation

Every PR must update an appropriate existing subsystem document or add a focused PR progress document when no suitable document exists. The record must include:

1. **Identity and scope**
   - PR number, title, branch, base branch, current HEAD SHA, date of update.
   - Intended scope, explicit non-goals, dependencies, and affected subsystem boundaries.
2. **Implementation status**
   - Requirements marked individually as `IMPLEMENTED`, `PARTIAL`, `NOT IMPLEMENTED`, or `BLOCKED`.
   - Source files and relevant functions/types for each implemented item.
   - Distinguish design/specification, source implementation, host tests, and runtime integration. A document or test stub is not an implemented feature.
3. **Remaining work and blockers**
   - Exact missing behavior, prerequisite, owner boundary, and next safe action.
   - Explain whether a blocker is code, architecture, hardware, toolchain, runner, CI, or external dependency.
   - Never hide incomplete requirements under broad labels such as "mostly done".
4. **Verification evidence**
   - Exact commit SHA tested.
   - Build/test commands or workflow/run links, test names, and PASS/FAIL/PENDING/NOT RUN results.
   - QEMU/real-hardware results where applicable, including boot mode and observed markers.
   - Sanitizer/static-analysis results where applicable.
   - A test created but not executed must be marked NOT RUN; a source inspection must never be reported as runtime verification.
5. **Regression and safety**
   - Existing behavior and merged work that must remain intact.
   - Regression coverage added and results.
   - Security, ownership, ABI, compatibility, and failure-path considerations relevant to the change.
6. **Completion decision**
   - Explicit completion-gate checklist.
   - State whether the PR is ready, not ready, or blocked, with reasons.
   - Keep a PR open and draft while required gates are incomplete. Never merge automatically.

## Required status vocabulary

Use these meanings consistently:

- **IMPLEMENTED** — code exists in the current PR HEAD and meets the stated requirement by source inspection.
- **PARTIAL** — some behavior exists, but one or more stated acceptance conditions are missing.
- **NOT IMPLEMENTED** — no production implementation exists.
- **BLOCKED** — work cannot safely proceed until a named prerequisite is satisfied.
- **PASS** — the stated test actually ran successfully against the exact recorded commit.
- **FAIL** — the stated test actually ran and failed; include the failing step and evidence.
- **PENDING** — submitted/queued/running but no final result is available.
- **NOT RUN** — not executed; give the reason where known.
- **NOT APPLICABLE** — genuinely outside the PR's scope, with a short reason.

Never infer PASS from a green unrelated workflow, source review, test existence, or an earlier commit. If HEAD changes, evidence tied to the previous SHA is historical and does not verify the new HEAD.

## Progress update cadence

- Update the PR record in the same logical implementation milestone commit, and whenever scope, implementation state, blockers, or verification evidence changes.
- Before ending a work session, record the exact current HEAD and a concise handoff: completed, remaining, blocked, tests, and next action.
- When a PR is resumed, read its progress record and verify that the recorded HEAD still matches the live PR before relying on its status.
- When the live PR HEAD differs, mark previous CI evidence as historical and refresh the status after inspecting the new HEAD.
- For long-running PRs, retain dated progress entries rather than erasing important historical results. Keep the current status summary easy to find at the top.

## Minimum completion gates

A PR is complete only when all applicable acceptance criteria are implemented, regression tests pass, required security/ownership/ABI checks pass, required build and runtime verification pass on the exact latest HEAD, and documentation accurately reflects the final state. Review requirements are governed by the repository's current policy; independent review or inline review threads are not an additional universal gate unless explicitly required by the project owner or repository protection.

## PR progress record template

Use this structure in the relevant subsystem document or a dedicated PR record:

```markdown
## PR #N — implementation status (YYYY-MM-DD)

- State: OPEN/DRAFT/CLOSED/MERGED
- Branch / base:
- Current HEAD:
- Last verified HEAD:
- Scope:

### Requirement matrix
| Requirement | Status | Source/evidence | Remaining work |
|---|---|---|---|

### Implemented in this update
- ...

### Remaining / blocked
- ...

### Verification
| Check | Result | Exact SHA | Evidence |
|---|---|---|---|

### Completion gates
- [ ] ...
- [ ] ...

### Next safe action
- ...
```

## AI and automation instructions

AI agents and automation must follow this standard as an execution requirement, not optional reporting style. Before editing, inspect the live PR and existing progress record. Reuse existing documents and implementations; do not create duplicate tracking files unnecessarily. Update the record after changes and report only verifiable facts. Do not claim a PR is complete while any applicable gate is PARTIAL, NOT IMPLEMENTED, BLOCKED, FAIL, PENDING, or NOT RUN.
