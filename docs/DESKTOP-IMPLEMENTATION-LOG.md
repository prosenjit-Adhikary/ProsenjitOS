# Desktop Implementation Work Log

## 2026-10-16 — DML source audit and selection-guard fix

### Scope
- Repository: `prosenjit-Adhikary/ProsenjitOS`
- Area: userspace Desktop Management Layer (DML)
- Source inspected: `user/desktop/desktop_manager.c`, `user/desktop/desktop_manager.h`, `tests/desktop_manager_test.c`, `scripts/test_desktop_manager.sh`, and `docs/DESKTOP-IMPLEMENTATION-PLAN.md`

### Checks and findings
- [x] Confirmed the DML is a userspace registry/preflight component, not a desktop-session launcher.
- [x] Audited manifest ID/path validation, dependency lookup/cycle handling, feature compatibility, and bounded registry count.
- [x] Found that `pros_dml_select()` had an invalid-manager guard without a `return PROS_DML_EINVAL`; execution could continue into preflight for an uninitialized or corrupt manager.
- [x] Added the missing error return.
- [x] Added regression cases for selection with an uninitialized manager and with a manager count above `PROS_DML_MAX_DESKTOPS`.
- [x] Updated the implementation plan to record the fix and the verification status.

### Changes committed
- `195f2b70c57f770fd3958ea3b0cdb8e46710aa33` — `fix: reject invalid desktop manager before selection`
- `d4d0c47cb235efd48bbdf3d106812cb575d2021f` — `test: cover invalid desktop manager selection state`
- `ebc98ad441aca035c59fe4115afdf1883d057e48` — `docs: record desktop manager fix and pending verification`
- `309e8b51a6843ebf971623eca50b609f7e948720` — add this desktop work log
- `5b83cf57d2d8bfa51b0147c235a99a2fbff61746` — record userspace/process/display/input blockers
- `09bb7183a4d0a64a2b9837f64800139b35913be5` — make the desktop gate accept expected QEMU timeout only when required serial milestones exist, run the DML unit-test target, and label the gate as prerequisite-only
- `cee18511ba6496e3a38be282bfd8b7e69b75094b` — report successful DML unit-test execution in the gate report
- `cd413b6ccdb7c82cf452d6896e08adb10a12bc09` — validate desktop shell scripts and run `make desktop-unit-test` in CI before the OS build; verify `cc`, `make`, and `timeout` prerequisites

### Additional architecture audit
- [x] Read `user/README.md`: it explicitly says a working userspace ELF loader, init/service manager, Xorg/Wayland runtime, and XFCE runtime are not present.
- [x] Read `include/abi/display.h` and `include/abi/input.h`: these currently define version constants and event/info structures, but the headers alone do not provide a usable userspace service or prove an end-to-end event path.
- [x] Read `kernel/display.c`: it implements a kernel-side driver/backend binding and present path with a self-test; this is not by itself a userspace display server or a GUI session.
- [x] Read `include/abi/syscall.h` and the dispatcher in `kernel/syscall.c`: the current public syscall set covers version, test completion, handle operations, and IPC; it has no process spawn/exec, display, or input syscall in the inspected code.
- [x] Read `kernel/include/driver_class.h`: an input-driver read callback contract exists, but that contract alone does not demonstrate a concrete keyboard/pointer driver or userspace event delivery.
- [x] Confirmed that guessed `user/init.c`, `user/service_manager.c`, and `kernel/input.c` paths are absent from the default branch. This is a path check, not proof that no related code exists anywhere else.
- [ ] Find and verify the actual syscall/process startup and concrete keyboard/input implementation paths before designing any service integration.

### Verification status
- Source-level DML fix: implemented.
- Regression tests: added, **not yet executed**.
- Host build/test: pending. CI has now been updated to run `make desktop-unit-test` and shell syntax checks before the OS build, but the new workflow commit has no reported check status yet. The available execution container could not resolve `github.com`, so it could not clone the repository to run the test target.
- Debian/self-hosted runner, full OS build, QEMU boot, and GUI runtime: not verified in this work session.
- Latest CI inspection (2026-10-16 12:21 UTC): build run `38899902507` and security run `38899902468` for commit `f8095673af2eb4ea2d0de2a08c853df3d3b36abc` are still `queued`; the preceding workflow runs for commits `2cd953f2d490a3226efed87e4947c01d42dc3477` and `58671d539e20a688e64a854d6a96873c2768d319` are also queued. The user confirms the Debian system and runner are active. Local service/process activity and GitHub job assignment are distinct states: the queued jobs have not yet produced test output, so this is not evidence of a source/build failure. Re-check the latest job assignment before attributing the queue to the runner.
- The current blocking dependency is a verified userspace execution + init/service path and concrete display/input delivery, not additional desktop manifest metadata.
- No claim is made that the desktop environment is complete.

### Next steps
1. Run `make desktop-unit-test` on the Debian system or a functioning runner.
2. Fix any compile/test failures before proceeding.
3. Trace the real process creation/ELF entry path and locate the actual keyboard/input driver and syscall interfaces from the current source tree.
4. Define and test a minimal userspace service lifecycle only after its real process and IPC contracts are identified.
5. Run the relevant project build and QEMU smoke test; retain exact output and tested commit.
6. Do not mark XFCE or boot-to-desktop complete until a real session with rendering, keyboard/pointer input, application launch, and logout has been demonstrated.

### 2026-10-16 — CI QEMU crash triage
- [x] Confirmed the self-hosted runner did claim and execute the workflow; toolchain verification, desktop-manager unit tests, and ISO build all completed successfully.
- [x] Captured the actual blocking runtime failure from build run [38901707980](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/38901707980): QEMU faults while entering the ProsenjitFS self-test, with page-fault instruction-fetch error code `0x11`, `RIP=0`, and the CPL3 success milestone absent.
- [x] Made ProsenjitFS self-test diagnostics volatile and fenced early checkpoints; moved the mock block device and filesystem state out of the large self-test stack frame.
- [x] Re-ran CI after each diagnostic change. The latest observed checkpoint is `0xF004` (immediately before `block_device_init()`); the fault is still present, so the root cause is **not fixed yet**. The checkpoint narrows the failure to the early self-test setup/call-return path; do not infer a confirmed stack-overflow cause.
- [x] Removed the duplicate CPL3 marker check and stopped uploading the reproducible ISO/raw image to reduce Actions storage use.
- [ ] Resolve the early ProsenjitFS self-test null instruction-fetch fault, then rerun QEMU and native BIOS tests.
- [ ] GitHub Actions artifact storage quota is still exhausted, so even the reduced diagnostics artifact upload is rejected; this is separate from the QEMU failure and does not itself fail the job because upload uses `continue-on-error`.
- [ ] Desktop runtime remains incomplete: no verified userspace init/service launch path, display/input service, Xorg/Wayland runtime, or XFCE session.

### 2026-10-16 — Follow-up QEMU result (run 38901862775)
- [x] Polled the completed build job [38901862775](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/38901862775) on commit `1210267e73b0c48e8dc9c9692761844bac265b3b`.
- [x] Confirmed desktop script syntax checks, DML unit tests, and ISO build passed on the self-hosted runner.
- [x] Confirmed the boot test still fails before the required IPC/CPL3 milestones. The serial exception report again records diagnostic stage `0xF004`, `RIP=0`, page-fault error code `0x11`, and `CR2=0`.
- [x] Confirmed native BIOS testing was skipped because the ISO QEMU step failed. This run does not establish a successful kernel boot or a working desktop.
- [ ] Next debugging step: isolate the `block_device_init()` call path with instrumentation that does not assume the root cause. The stage is set immediately before the call, so current evidence narrows the failure to that call/return boundary but does not prove which instruction or mechanism caused it.
- [ ] Actions artifact upload still fails because the repository's Actions storage quota is exhausted. This is a separate diagnostic-artifact issue and does not explain the QEMU exception.

### 2026-10-16 — Instrument the block-device initialization boundary
- [x] Added temporary, narrowly scoped checkpoints inside `block_device_init()` only when the ProsenjitFS self-test is at stage `0xF004`: entry (`0xF041`), argument validation (`0xF042`), field initialization (`0xF043`), and immediately before return (`0xF044`).
- [x] Exposed the existing volatile diagnostic stage to the block layer for this temporary trace. Normal block-device calls do not change the diagnostic value.
- [ ] CI must run against these commits before interpreting the new stage. The next QEMU serial diagnostic should distinguish failure before function entry, during field setup, or at the return boundary.
- [ ] This is diagnostic instrumentation, not a claimed fix. Remove the cross-layer trace once the failing instruction/call boundary is established, then rerun both ISO and native BIOS tests.

### 2026-10-16 — QEMU reaches block-init return boundary
- [x] Inspected build run [38902318553](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/38902318553): toolchain, DML tests, and ISO build passed; QEMU boot failed and native BIOS was skipped.
- [x] The diagnostic stage reached `0xF044`, immediately before `block_device_init()` returns. The exception still reports page-fault instruction-fetch error `0x11`, `RIP=0`, and `CR2=0`.
- [x] Added temporary snapshots of the return address and RSP from inside `block_device_init()`, and exception output to report them when the fault occurs in this diagnostic window. Relevant commits: `d1d490df82adfac1d4601172673509bfc6ec0e6f`, `929cb202118dc290a3568220649284c9ecfc7596`, and `b4f3848f6ff1089ec993c511ce218001a2ba0a9a`.
- [ ] The run for commit `d1d490df` also failed at QEMU; it predates the exception-output change, so it cannot reveal the saved return/RSP values. A newer run for commit `b4f3848f` is queued and must be inspected before changing the call/return path.
- [ ] Temporary cross-layer diagnostics must be removed after the failure mechanism is established. Do not call the QEMU issue fixed until all required ISO milestones and native BIOS milestones pass.
- [ ] Desktop runtime remains incomplete; passing DML tests and ISO build do not establish userspace service launch, display/input delivery, or an XFCE session.

### 2026-10-16 — Return-address snapshot follow-up
- [x] Inspected build run [38903052073](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/38903052073): DML tests and ISO build passed; QEMU boot failed; native BIOS was skipped.
- [x] The failure report showed `block_device_init()` reached stage `0xF044`, but the recorded return address was `0x0`; the stack pointer snapshot was `0xD5988`. This is suspicious but not conclusive because the optimized function's frame/return-address capture itself needed validation.
- [x] Added a targeted follow-up: mark `block_device_init()` noinline and compile that function at `O0` for diagnostic reliability, capture entry and pre-return return addresses plus RSP/frame snapshots separately, and print both sets on exception.
- [ ] CI must validate this diagnostic build before deciding whether the return slot is already zero at entry or changes during the function. This is not yet a functional fix.
- [ ] Keep the temporary instrumentation short-lived; once the fault is isolated, remove it and verify ISO and native BIOS boots. Desktop runtime remains incomplete.

### 2026-10-16 — Confirmed caller-return capture is not reliable yet
- [x] Inspected run [38903071895](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/38903071895), commit `b4f3848f`: DML tests and ISO build passed; QEMU boot failed; native BIOS was skipped.
- [x] The exception report still shows `RIP=0`, diagnostic stage `0xF044`, and the previous return-address capture as zero. This run contains the earlier implementation, not the new noinline/O0 entry-versus-exit comparison.
- [x] The refined diagnostic commits `f457fc64`, `dd07dda6`, and `fcce255f` are queued behind the active self-hosted runner job. Avoid adding further commits until those queued runs yield the entry/exit snapshots, to reduce redundant CI queueing.
- [ ] Next decision depends on entry/exit return and frame values from the refined run. No root-cause fix is confirmed; QEMU boot remains failing and desktop runtime remains incomplete.


### 2026-10-16 — Isolate page-fault entry and stop recursive exception reporting
- [x] Latest run `38903470923` confirms the QEMU log repeatedly emits the exception-vector prefix before reporting `RIP=0`; earlier diagnostic snapshots are still all zero, so they do not identify the original call/return fault.
- [x] Assigned page fault (#PF) its own TSS IST3 stack, separate from the double-fault and syscall stacks, to avoid relying on a possibly damaged current stack for first-fault reporting.
- [x] Added a re-entry guard to `exception_dispatch()`: if reporting triggers another exception that reaches the dispatcher, the kernel halts instead of recursively logging and corrupting the same context. The guard is cleared only on the existing recoverable-return paths.
- [ ] CI must verify that these changes assemble, link, and produce a stable first-exception report. This is diagnostic hardening, not a confirmed root-cause fix. QEMU and native BIOS boot remain unverified/failed; desktop runtime is still incomplete.

### 2026-10-17 — DML lifecycle hardening follow-up

- [x] Made registration explicitly initialize each new entry's reserved bytes and lifecycle state to PROS_DML_SESSION_STOPPED, rather than relying only on manager-wide initialization.
- [x] Hardened transition range validation with explicit signed comparisons before any state lookup/update.
- [x] Added regression assertions rejecting FAILED -> RUNNING and enum values above and below the defined state range.
- [x] Updated the implementation plan to describe the current DML scope and latest verification boundary.
- [ ] Run make desktop-unit-test against the exact latest commit on the user's Debian system or an available self-hosted runner.
- [ ] Run the project build and QEMU prerequisite gate only after the DML unit test passes; preserve the existing kernel/ProsenjitFS QEMU failure as a separate blocker unless fresh evidence shows it is resolved.
- [ ] Latest edits were written through GitHub's repository API. This session has no attached shell/SSH execution channel, and the local clone attempt was blocked by DNS, so these newest changes have not been compiled or run here.
- **Status:** source changes committed; runtime and latest-host-test verification pending. Do not mark the desktop environment complete.

### 2026-10-17 — Guard dependency traversal against corrupted metadata

- [x] Added bounds checks in both dependency preflight and dependency-order traversal before iterating a stored manifest's dependency array.
- [x] Added regression coverage that corrupts a registered manifest's dependency_count beyond the fixed array capacity and expects PROS_DML_EINVAL from both APIs.
- [x] Documented the corrupted-metadata guard in the dependency-order API contract.
- [ ] Compile and execute the updated unit tests on the exact head commit; GitHub workflow lookup returned no runs for the current head, and no shell/SSH execution channel is attached to this chat.
- **Status:** implementation and regression test are committed; compile/test/QEMU verification remains pending.

### 2026-10-17 — Backend-driven DML session orchestration

- [x] Added `user/desktop/dml_runtime.h` and `.c`: a serialized, single-active-root orchestrator that consumes dependency-first order, calls a narrow platform start/stop backend, tracks process handles, rolls back partial startup, stops in reverse order, and retains failed shutdowns for retry.
- [x] Added runtime error codes and a FAILED -> STOPPING recovery transition for retrying a failed shutdown.
- [x] Added `tests/desktop_runtime_test.c` with a mock backend covering dependency start order, failed middle-start rollback, retry after clean rollback, reverse-order shutdown, failed-stop retention, and stop retry.
- [x] Updated `make desktop-unit-test`'s script to compile and run both registry and runtime suites; updated DML plan, user-mode README, and `docs/DML-RUNTIME.md` to state the exact adapter boundary.
- [ ] Execute `make desktop-unit-test` on the exact branch head. No runner was used or occupied; this chat has no shell/SSH execution channel, so no compile/test result is available yet.
- [ ] A production backend still needs a real process spawn/exec and wait/reap API, plus package-root resolution and security policy. The current public syscall ABI does not expose spawn/exec; do not wire the mock backend into boot or claim that a desktop session can run.
- [ ] Display/input service integration, compatible window-system runtime, XFCE session, full OS build, QEMU, and boot-to-desktop verification remain incomplete.
- **Status:** userspace orchestration logic and mock regression suite added as source; compilation, execution, real backend integration, and GUI runtime are unverified.

### 2026-10-17 — DML runtime ownership and state-integrity hardening

- [x] `pros_dml_runtime_init()` now refuses to attach a fresh runtime to a manager containing non-STOPPED registered sessions; without the previous runtime's process handles, attaching could orphan live processes or reset meaningful failure state.
- [x] Runtime validation now rejects active-order entries outside the registry, unregistered entries, and duplicate indices before any start/stop operation.
- [x] Added regression assertions for runtime reinitialization during an active session and corrupted active-index state.
- [x] Reconciled the desktop implementation plan, runtime contract, and roadmap with the current source scope.
- [ ] Run `make desktop-unit-test` against the current branch head when the user releases the runner; no runner was used by this source-only batch.
- **Status:** new source and test cases committed; no claim of compile/test success. A real process backend, init/service launch path, display/input delivery, window-system/XFCE runtime, and QEMU boot-to-desktop proof remain pending.

### 2026-10-17 — OfficeWindows runner repair and ProsenjitFS boot-fault diagnostics

- [x] Replaced the missing-Bash dependency for DML validation with a native PowerShell harness.
- [x] Confirmed the harness found no installed compiler/VS C++ toolchain, downloaded the official Zig 0.17.0 Windows x64 archive, verified its pinned SHA-256, and compiled and ran both DML test executables successfully on the designated `OfficeWindows` runner.
- [x] Confirmed the `office` Debian build job passes toolchain checks and ISO build, but QEMU still faults before the IPC/CPL3 milestones: page-fault vector `0xE`, error `0x11`, `RIP=0`, `CR2=0`. Native BIOS QEMU is correctly skipped after ISO boot failure.
- [x] Identified that the temporary block-device initialization trace could publish stage `0xF041` before the non-volatile diagnostic values became visible to the exception handler. Changed diagnostic fields to volatile and added a compiler memory barrier before publishing the stage.
- [x] Added the block-device init arguments (device, callbacks, private data, sector size/count) to the fault report so the next designated `office` QEMU run can narrow the fault without guessing.
- [x] Reviewed the latest failing QEMU trace: argument registers still contain the expected `block_device_init` inputs, but all callee-side snapshots are zero despite stage `0xF041`. Added caller-side snapshots (call target, stack/frame, caller return, device/callback/private pointers and geometry) to distinguish a bad call/stack transition from a fault inside the callee on the next `office` run.
- [ ] Re-run the build/boot workflow and inspect the caller-side diagnostics. The fault is not yet fixed; do not claim the OS boot or ProsenjitFS runtime self-test passes until the QEMU milestones pass.
- **Status:** DML unit tests pass on OfficeWindows. Kernel/ProsenjitFS QEMU fault remains open; Debian/QEMU runtime verification is only through the designated `office` self-hosted runner, not the user's offline Debian system.

### 2026-10-17 — DML registry-layout integrity hardening

- [x] Hardened `manager_valid()` to enforce the registry's contiguous-prefix invariant: every slot below `count` must be registered, and every slot at/above `count` must be unregistered.
- [x] Added regression cases for a hidden registration outside `count` and a count that claims an unregistered slot. These cases verify malformed public storage is rejected before selection or a later registration can overwrite it.
- [ ] The new regression tests require a fresh run on `OfficeWindows`; no test pass is claimed for this commit until its DML job completes.
- **Status:** source-level hardening and regression coverage committed. Real process-spawn backend, package installation/verification, display/input delivery, desktop compositor/window-system, and boot-to-GUI remain separate unfinished integration gates.


### 2026-10-17 — DML runtime rejects malformed registry layout

- [x] Found a gap between the registry and runtime validation contracts: `pros_dml_manager` is public storage, and `pros_dml_runtime_init()` previously checked only its magic/count before skipping unregistered slots. A corrupted manager with a hole in the published prefix (or a hidden registration outside the prefix when count was zero) could be accepted as a runtime binding.
- [x] Added runtime-local contiguous-prefix validation and reused it in runtime validation and initialization. This keeps the runtime boundary defensive even before a manifest lookup occurs.
- [x] Added regression cases for a count claiming an unregistered slot and for a hidden registration beyond count.
- [ ] Run `make desktop-unit-test` on the exact updated head using `OfficeWindows`. No compilation or test result is claimed until that job actually executes and its logs are inspected.
- **Status:** source and regression test committed; designated runner verification pending. The production spawn/exec/wait/reap backend and graphical session remain incomplete.


### 2026-10-17 — Runtime-operation regression for mutated registry

- [x] Extended the malformed-registry regression to mutate the manager after a session is active and verify that runtime shutdown refuses to operate on a manager whose published count no longer matches its registered prefix.
- [x] The test restores the original manager state after the rejection so the mock session can still be stopped normally; it does not fake successful cleanup.
- [ ] Re-run the DML suite on `OfficeWindows` for the updated head. No test result is claimed from source inspection alone.


### 2026-10-17 — Canonical DML registration flags

- [x] Tightened registry validation to require `registered == 1` inside the published prefix and `registered == 0` outside it; nonzero-but-invalid flag values are no longer treated as valid registrations.
- [x] Added a regression that corrupts a registered slot to value `2` and confirms selection rejects the manager before restoring the valid flag.
- [ ] Re-run the complete DML suite on `OfficeWindows`; this commit has no test result until the designated job runs and logs are inspected.
