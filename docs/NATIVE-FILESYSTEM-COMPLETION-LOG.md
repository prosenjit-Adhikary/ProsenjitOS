# Native Filesystem Completion Log

**Status:** In progress. The native filesystem is not marked complete. This log records the verified starting point and the work underway on 2026-10-30.

## Baseline and repository state

- Repository: `prosenjit-Adhikary/ProsenjitOS`.
- Latest `main` inspected: `eb19c13e03b52fa023e451ceb74cadc8c189b3fb` (tree `a7234c1b61caf659171a9e53707737f49467c699`).
- Exact-head hosted checks on `main`: Build and boot-test run `41055684858` passed; Security architecture run `41055685017` passed.
- Primary PR #305: open and draft, branch `audit/vfs-concurrency-offline-20261029`, head `b433cd1b838f869111b745792221063047d7c50d`, based on `fc8604aca130561182e0e1b50077654569ba5e80`. It has 57 commits ahead and 33 behind current `main` (merge base remains `fc8604aca130561182e0e1b50077654569ba5e80`). Build run `41025089705` and Security run `41025089699` passed on the PR head, not on a merge with current `main`.
- Current `main` already contains scoped fail-fast file and directory handle gates, atomic vnode reference counting, mount lookup admission/drain, namespace mutation admission, and the ProsenjitFS provider/integrity serialization work documented in FS-13. Those controls do not establish arbitrary concurrent teardown safety or general multi-threaded/SMP filesystem access. The filesystem matrix requires callers to serialize operations per mounted ProsenjitFS instance and records remaining object-lifetime work as not verified.

## Verified remaining-work checklist

- [x] Fetch current `main`, PR #305, current open PR list, compare history, changed-file list, and exact-head workflow results.
- [x] Inspect the current VFS handle gates, directory gate, mount lifecycle admission, vnode atomic reference operations, permission checks, path lookup, namespace admission, IPC locking, and scheduler interrupt fallback.
- [x] Check that the PR #305 patch applies to a snapshot of current `main` without conflicts. It applies cleanly to the eight PR files; this is not a Git merge or a build result.
- [x] Integrate the scheduler frame-eligibility fix and its deterministic policy regression into the latest `main` tree while retaining the PR branch history. The branch now includes current `main` as its merge base and preserves PR #305's history.
- [x] Run Build/boot-test and Security architecture checks on the integrated code head; inspect both workflow results and logs for the scheduler, VFS, ProsenjitFS recovery, and persistent-mount milestones.
- [x] Add deterministic callback-interleaving tests for actual in-flight file/directory I/O versus close and mount deactivation, rather than only forcing the gate state directly.
- [x] Exercise the CR3-switch failure rollback policy, including restoration of the previous RUNNING thread and cleanup when a candidate becomes ineligible.
- [ ] Verify provider lifetime and namespace/vnode ownership around lookup, rename, unlink, open handles, and mount teardown; document every guarantee and the supported CPU model.
- [ ] Complete the remaining FS-11 lower-level device read/write fault inventory and FS-12 adversarial/failure coverage listed in `docs/FILESYSTEM.md`.
- [ ] Keep physical power-loss certification explicitly unverified until tested on selected hardware. QEMU and mock-device tests do not satisfy this gate.
- [ ] Keep unsupported compatibility providers and advanced features explicit; do not report them as implemented.

## Work record — 2026-10-30

### Scheduler fallback fix from PR #305

- **Problem:** In the no-successor interrupt-scheduling branch, a BLOCKED thread could still own a runnable process. The old fallback checked process eligibility alone and could return the interrupted frame of a thread that had explicitly blocked. A similar unsafe fallback existed after a failed address-space switch.
- **Root cause:** The fallback did not require the interrupted thread itself to remain in RUNNING state.
- **Files:** `.github/workflows/build.yml`, `docs/SCHEDULER-AWARE-MUTEX-PREREQUISITES.md`, `docs/VFS-CONCURRENCY-OFFLINE-AUDIT.md`, `kernel/idt.asm`, `kernel/include/thread.h`, `kernel/kernel.c`, `kernel/thread.c`, `scripts/test_timer_irq_assembly.sh`.
- **Approach:** Port the shared resumability/fallback policy and boot regression from PR #305 onto current `main`; retain the production-only idle loop and test the side-effect-free policy boundary.
- **Tests:** The PR patch passes `git apply --check` against the local snapshot of current `main`. Local build tools (`gcc`, `make`, `nasm`, `bash`, QEMU, and WSL) are unavailable in this environment. The PR-head CI and current-main CI results above are prior exact-head evidence only; they do not verify an integrated commit.
- **Exact integrated-head CI:** `a11b9ac20da55104a9b97a5cb6c797b27a29a13c`; Security architecture run `41057503692` passed. Build run `41057503678` failed in step “Verify timer IRQ assembly frame and EOI invariants” before compilation: Bash rejected CRLF at `scripts/test_timer_irq_assembly.sh:11`. Build ISO and both QEMU boot steps were skipped. The source was corrected to LF and must be rerun on the next exact head.
- **Corrected integrated-head CI:** `dc614a22535320a9fe0fdcd27171ffa13cbf173e`; Build and boot-test run `41057739090` passed, including the timer-IRQ assembly invariant script, ISO build, ISO QEMU boot, and native BIOS QEMU boot. Security architecture run `41057739079` passed. The logs include `eligible-thread fallback policy test passed`, `blocked-thread interrupt scheduling regression passed`, `interrupt-frame scheduler save/restore test passed`, `scheduler address-space failure rollback test passed`, `ProsenjitFS format/mount/allocation/journal/recovery self-test passed`, and `ATA PIO -> MBR -> ProsenjitFS persistent mount test passed`.
- **Follow-up:** The first corrected build log exposed two new scheduler self-test serial strings containing a literal `\
` instead of a newline escape. Corrected those messages in `kernel/kernel.c`.
- **Latest exact-head verification:** `55181d8e84b0a57b2cbff89ea9388f30fff2c322`; Build and boot-test run `41058119821` passed, including the timer-IRQ source invariant, ISO build, ISO QEMU boot, and native BIOS QEMU boot. Security architecture run `41058119804` passed. Inspected job logs show the dead-process fallback and blocked-thread interrupt scheduling milestones, plus ProsenjitFS format/mount/journal/recovery and ATA→MBR→ProsenjitFS persistent-mount milestones.
- **Current result:** The scheduler frame-eligibility change and its boot regressions are verified on the exact code head above. The completion objective remains open because VFS teardown/lifetime interleavings, remaining failure-sweep gaps, and physical durability are still outstanding.
- **Risks/limitations:** The self-test does not execute the non-returning `sti; hlt` idle loop or force a real CR3-switch failure. This work does not add a scheduler mutex or establish SMP support.

### Latest-main integration and exact-head verification — 2026-10-30

- **Repository movement:** While integrating, `main` advanced from `eb19c13e03b52fa023e451ceb74cadc8c189b3fb` to `8db58c8d0d94562c0b3bee727a1c3e4bb4453a7b`. Its filesystem changes include FS-57 truncate flush-failure recovery tests and the corresponding completion-matrix update in `kernel/prosenjitfs.c` and `docs/FILESYSTEM.md`.
- **Branch integration:** Created merge commit `e0ee7b32922e5f94768b6590b90ecfe299682f7a` with both PR history and that latest `main` as parents. PR #305 remains open, draft, and unmerged; it has nine changed files and no commits behind its updated base.
- **Build/boot result:** On exact head `e0ee7b32922e5f94768b6590b90ecfe299682f7a`, Build and boot-test run `41058539033` passed all steps: desktop tests, timer IRQ assembly invariants, ISO build, ISO QEMU boot, and native BIOS QEMU boot. Job logs include the blocked-thread scheduling milestone and ProsenjitFS format/mount/journal/recovery and ATA→MBR→ProsenjitFS persistent-mount milestones.
- **Security result:** Security architecture run `41058539059` passed on the same exact head.
- **Progress-log placement correction:** An attempted update through the Contents API initially targeted its default branch (`main`). It was reverted by commit `8db58c8d0d94562c0b3bee727a1c3e4bb4453a7b`; the completion log is being updated explicitly on PR #305's branch. No runtime source changes from this task remain on `main`.
- **Post-log-update exact-head verification:** `5e8c7c9c29e981190d0e4c0c9d400a77590370cc`; Build and boot-test run `41058882194` passed, including timer IRQ invariants, ISO build, ISO QEMU boot, and native BIOS QEMU boot. Security architecture run `41058882217` passed. Inspected logs confirm the dead-process fallback, blocked-thread scheduling, ProsenjitFS recovery, and persistent ATA→MBR→ProsenjitFS milestones.
- **Still open:** FS-13's general concurrent teardown/object-lifetime guarantees remain unverified, FS-11/FS-12 still have listed test gaps, and FS-14 physical hardware durability is not tested.

### VFS callback-interleaving regressions — 2026-10-30

- **Problem/root cause:** Existing VFS self-tests forced `operation_busy` directly, so they did not prove that a provider callback actually runs under the handle/directory gate or that mount admission protects a live path walk.
- **Files:** `kernel/vfs.c`.
- **Implementation:** Re-enter mount deactivation from the mock lookup callback; re-enter read, seek, write, truncate and close from the mock file-read callback; and re-enter directory close from mock readdir. Each nested operation must return `EBUSY` while the outer provider callback completes normally.
- **Tests:** Added assertions to the existing deterministic `vfs_vnode_api_selftest`, which runs during the Build/boot workflow. On exact head `7c331f863d0fed5a859f716265917b21c538e4b4`, Build/boot-test run `41059775066` and Security architecture run `41059775168` passed; inspected logs include `VFS permission/security self-tests passed`. Local `git diff --check` passed.
- **Risks/limitations:** Callback re-entry is a deterministic overlap model for the fail-fast gates, not proof of arbitrary scheduler preemption, general multi-threaded safety, or SMP. Mount/provider teardown and vnode lifetime under arbitrary concurrent release still require further audit and tests.

### CR3-switch failure rollback coverage

- **Implementation:** Extracted the production queue rollback into `thread_interrupt_switch_failure_prepare()`. It restores a requeued previous thread to RUNNING, requeues an eligible candidate, retires candidates whose process is blocked/dead/quarantined, and reports whether the caller must park. Added deterministic boot assertions for runnable, blocked-previous, and newly ineligible candidate cases; both ISO and native BIOS workflows require the new serial milestone.
- **Files:** `kernel/thread.c`, `kernel/include/thread.h`, `kernel/kernel.c`, `.github/workflows/build.yml`.
- **Verification:** Build/boot-test run `41060596963` and Security architecture run `41060596744` passed on exact PR head `0c88ce58b7a6c8a79f2de81f28969b8600f79541`. Inspected ISO and native BIOS workflow logs confirm the interrupt address-space failure rollback milestone, VFS permission/security self-tests, ProsenjitFS recovery self-test, and persistent ATA→MBR→ProsenjitFS mount. The timer assembly invariant script and ISO/native BIOS QEMU boots also passed. The tests exercise the production rollback helper, but do not force a real hardware CR3 failure or enter the non-returning idle loop.

**Progress measure:** 7 of 11 explicit checklist items are complete (64%). This is checklist progress only; final acceptance criteria are not met.

## Verification boundary

No completion percentage is assigned until the checklist is measured against the exact integrated commit. Do not infer filesystem concurrency or hardware durability from a successful kernel boot alone.
