# ProsenjitOS — Agent and Implementation Guidelines

These rules apply to every human contributor, coding agent, automated workflow, and implementation branch in this repository. They supplement (and do not replace) the subsystem specifications.

## 1. Read before changing code

Before starting a task:

1. Read this file and the relevant subsystem specification. For licensing, attribution, project-name, or release questions, also read `docs/LICENSING-AND-BRAND-POLICY.md` and `THIRD-PARTY-NOTICES.md`.
2. Read `README.md`, `docs/ARCHITECTURE.md`, and the relevant sections of `docs/ROADMAP.md`.
3. Inspect the current implementation, public headers, callers, tests, build rules, and recent history around the affected code.
4. Search for existing implementations and in-progress PRs/issues before creating duplicate work.
5. Identify dependencies, invariants, security boundaries, and the exact acceptance criteria.

For Linux/POSIX, Windows application, shared compatibility-service, or foreign-driver work, read `docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md` first, then the linked canonical subsystem documents it identifies. It is the master compatibility execution plan and coding-agent handoff contract.

For filesystem work, `docs/FILESYSTEM.md` is the single source of truth for design, status, gates, and evidence. Do not create a competing filesystem plan/status document. For other subsystems, update their existing canonical specification rather than creating duplicate status files.

## 2. Architecture and scope

- Preserve the documented ProsenjitOS architecture and subsystem boundaries.
- Prefer extending an existing, appropriate interface over adding a parallel framework.
- Keep compatibility implementations outside kernel core when the architecture specifies provider or user-space boundaries.
- Do not claim a feature is implemented merely because an interface, stub, design, or test fixture exists.
- Separate independent release gates (for example, filesystem format support versus USB transport and physical-device support).
- Do not silently expand the task scope. Record necessary follow-up work explicitly.

## 3. Research and third-party code

- Research mature upstream implementations, standards, official documentation, and established OS behavior before writing substantial low-level code.
- Prefer reuse of a suitable, maintained library over reimplementing a complex standard from scratch, when its architecture and license are compatible.
- Record upstream project, exact release/commit, source URL, license, required notices, configuration, local patches, and update procedure before vendoring.
- Review license compatibility and relevant patent/distribution obligations; escalate uncertainty instead of assuming permission.
- Do not copy source code from another OS or project without verifying its license and complying with its terms. Treat reference implementations as behavioral/architectural references unless copying is explicitly approved.
- Never remove attribution, copyright notices, or license files.
- The project owner's intended policy is that every modified/forked OS distribution derived from ProsenjitOS must retain `ProsenjitOS` as its exact primary OS name. This is only a proposed condition until the custom license is legally reviewed, rights-holder-approved, and formally adopted.
- The root `LICENSE` currently remains MIT; do not claim it enforces a same-name requirement. `LICENSE-CUSTOM-DRAFT.md` is a draft only. Do not replace the active license, imply the draft is effective, or merge a license change without explicit owner approval after legal review.
- Preserve all applicable original and third-party notices. Third-party terms remain independent and must be indexed in `THIRD-PARTY-NOTICES.md`; the proposed naming condition cannot override those terms.
- A modified distribution must not falsely claim to be an official, endorsed, or supported upstream release.

## 4. Correctness, security, and failure behavior

- Treat all external input, disk structures, device responses, file metadata, and user-space data as untrusted.
- Validate lengths, bounds, integer overflow, alignment, identifiers, state transitions, and resource ownership before use.
- Fail closed on malformed, unsupported, or ambiguous input. Do not guess or silently repair persistent data.
- Preserve least privilege, permission checks, capability boundaries, authentication/authorization, audit logging, and data-admission/security policy.
- Preserve object lifetime, reference counts, locking, cancellation, and teardown invariants. Do not introduce use-after-free, double-release, lock-order, or stale-handle hazards.
- Make writes and destructive operations explicit, bounded, and recoverable where the design requires it.
- Keep error codes and failure semantics consistent with the existing subsystem contract.
- Do not weaken security checks merely to make a test pass.

## 5. Implementation discipline

- Work in a dedicated feature branch; do not commit feature work directly to `main`.
- Make small, reviewable changes with focused commit messages.
- Inspect the diff after each logical change. Avoid unrelated formatting or broad refactors.
- Update headers, callers, build integration, documentation, and tests together when an interface changes.
- Do not add speculative APIs or dead code without a documented need.
- Keep generated files and build artifacts out of source control unless the project explicitly requires them.
- Do not overwrite another contributor's changes. Refresh the branch and reconcile conflicts before retrying a write.

## 6. Tests and verification

- Identify the relevant existing tests and add focused tests for every new behavior and important failure path.
- Include malformed-input, boundary, overflow, permission-denied, resource-exhaustion, and teardown tests where applicable.
- Run the narrowest relevant tests first, then the required regression/build/boot suite.
- Verify on the exact commit/PR head being reviewed. A successful run on an older commit is historical evidence only.
- Inspect workflow logs and test output; distinguish source failures, infrastructure failures, cancellations, skipped jobs, and unavailable runners.
- Never report a test as passed if it did not execute. Never report a build, boot, hardware, or end-to-end test based only on static inspection.
- Record the exact commit SHA, commands/workflow run IDs, outcomes, and unverified limitations.

## 7. Documentation and implementation log

For each meaningful checkpoint:

- Update the subsystem's canonical document with scope, design decisions, implementation status, acceptance criteria, and remaining gates.
- Append a dated entry to `docs/DEBUG_LOG.md` describing files changed, rationale, tests actually run, exact-head evidence, failures, and next action.
- Clearly label work as **planned**, **implemented**, **built**, **tested**, **verified**, or **not tested**. These states are not interchangeable.
- Record upstream dependencies and license decisions when third-party code is introduced.
- Keep the log factual and chronological; do not erase unresolved failures or historical evidence.

## 8. Pull requests and merges

- Prefer one coherent PR at a time for dependent work. Keep unrelated changes separate.
- Before opening a PR, check the diff, build impact, tests, documentation, and whether the branch is based on the current intended base.
- Before merging, inspect the complete PR diff, conflicts, required checks, review comments, and exact-head workflow results.
- Merge only when the required checks have actually passed and the change is compatible with the target branch. If checks are blocked by infrastructure, leave the PR open and report the limitation.
- After merging, verify the merge commit and confirm the intended changes are present on the target branch. Then update the log.
- Do not force-push, bypass required checks, or close another PR as a substitute for resolving its work without an explicit reason.

## 9. Status reporting

Every progress report must distinguish:
- what was inspected;
- what code or documentation changed;
- what was actually built/tested;
- exact commit/PR/workflow evidence;
- what remains and why.

Use measured, evidence-backed status. Do not invent percentages, completion claims, test results, hardware support, or merge success.

## 10. Task completion checklist

Before declaring a task complete, confirm:

- [ ] Scope and acceptance criteria match the canonical specification.
- [ ] Existing architecture and security invariants are preserved.
- [ ] Third-party code/license obligations are recorded, if applicable.
- [ ] Implementation, callers, build integration, and tests are consistent.
- [ ] Relevant tests ran successfully on the exact candidate head.
- [ ] Diff and workflow logs were reviewed.
- [ ] Canonical subsystem documentation and `docs/DEBUG_LOG.md` are updated.
- [ ] Remaining limitations and separate release gates are clearly stated.
- [ ] PR is merged only after all required gates pass; otherwise it remains open.

When in doubt, stop before a risky write or merge, document the uncertainty, and request clarification rather than guessing.
