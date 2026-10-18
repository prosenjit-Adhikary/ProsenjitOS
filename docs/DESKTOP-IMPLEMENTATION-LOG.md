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


### 2026-10-17 13:00 UTC — Designated-runner verification and boot triage

- [x] Retrieved workflow run [39033799224](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39033799224) job logs. The DML job explicitly identified runner `OfficeWindows`; both `desktop manager tests passed` and `desktop runtime tests passed`. The new malformed-registry regression suite therefore passed in this run.
- [x] Confirmed toolchain verification and ISO build passed on the designated Debian runner `office`. Security architecture run [39033799262](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39033799262) also passed.
- [x] Captured the current ISO QEMU failure from the same build run: page fault vector 14, instruction-fetch error code `0x11`, `RIP=0`, `CR2=0`, and ProsenjitFS diagnostic stage `0xF041`. The required IPC milestone is absent and the native BIOS test was skipped.
- [x] Source audit confirms the public ABI still has no process spawn/exec or wait syscall. `process_create()` creates/registers a READY process but does not load an ELF or start its user instruction stream; the existing `enter_user_mode()` path is an explicit kernel boot self-test, not a general process launcher. `thread_init()` builds a kernel-context trampoline and is not a safe substitute for CPL3 ELF entry.
- [ ] Root cause of the current `0xF041` / null instruction-fetch fault is not proven or fixed. Keep the fix in the designated `office` ProsenjitFS/build workflow; do not mask the failure or treat the QEMU timeout as success without all required milestones.
- [ ] Define and implement a reviewed process ownership + user-mode entry + wait/reap contract before wiring a production DML backend. Do not invent a syscall number or route desktop policy into kernel-core.
- [ ] Display/window-system integration, keyboard/pointer event delivery, real desktop session, and boot-to-desktop remain incomplete.


### 2026-10-17 13:21 UTC — Fresh QEMU fault evidence (run 39036048406)

- [x] Confirmed the workflow checked out PR merge commit `86d2dd267bf4e94d0f035f7675cdba5ec40a0020`; the designated `office` runner passed toolchain verification and built the ISO.
- [x] Read the complete QEMU serial/interrupt diagnostics. Boot reaches process lifecycle, IPC wait-queue, security-runtime, partition discovery, VFS provider registry, storage-volume discovery, and process-lifecycle milestones, then faults before the ProsenjitFS self-test can progress and before the required IPC-channel/CPL3 markers.
- [x] Exact fault remains #PF vector `0xE`, error `0x11`, saved `RIP=0`, `CR2=0`, diagnostic stage `0xF041`. QEMU exits via the workflow's 20-second timeout, but required milestone checks correctly fail; timeout is not accepted as a pass.
- [x] New caller-side trace reports call target `0x112420`, caller RSP/frame/return `0xD5990/0x1568F0/0x102B95`, device/read/write/private `0x21C480/0x115EC0/0x115FA0/0x19C440`, and geometry `512/512`. Callee-side snapshots remain all zero despite linked disassembly placing their stores before its `0xF041` checkpoint. This is internally inconsistent diagnostic evidence; it does **not** yet establish a root cause.
- [ ] Next kernel/ProsenjitFS step is to reconcile the emitted linked disassembly, runtime checkpoint and snapshot visibility, using a minimal isolated call/return probe or debugger trace before changing the production call path. Do not make speculative stack/ABI changes or remove the required boot gates.
- [ ] Native BIOS boot remains skipped after ISO QEMU failure. Desktop production process launch and GUI integration remain incomplete.
- **Status:** latest ISO build passed; QEMU boot still failed. Security architecture checks passed in run `39036048410`; DML tests on `OfficeWindows` were still in progress when polled.


### 2026-10-17 13:27 UTC — Latest CI poll (run 39036283325)

- [x] Confirmed the DML job ran on the designated `OfficeWindows` runner and passed. Its log explicitly reports `desktop manager tests passed`, `desktop runtime tests passed`, and `DML manager and runtime unit tests passed`.
- [x] Confirmed the security architecture job passed in run `39036283329`.
- [x] Confirmed the `office` runner passed toolchain verification and ISO build again.
- [x] Confirmed the ISO QEMU boot gate still fails. Serial output reaches the storage-volume discovery and process-lifecycle markers, then reports page-fault vector `0xE`, instruction-fetch error `0x11`, `RIP=0`, `CR2=0`, stage `0xF041`; the required IPC channel/capability marker is absent, and native BIOS boot is skipped.
- [ ] Root cause of the `0xF041` fault remains unproven. The caller trace has a nonzero target and arguments, but callee snapshots are zero despite stores appearing before the checkpoint in linked disassembly. Next, reconcile the trace using a minimal call/return probe or debugger evidence before modifying the production path.
- [ ] Production process spawn/exec/wait/reap, userspace init/service startup, display/input delivery, and real desktop-session verification remain incomplete.
- **Status:** DML unit tests and security checks pass; ISO compiles; QEMU boot and desktop completion remain blocked.


### 2026-10-17 13:29 UTC — Add a stack-frame measurement probe (commit 95565d5)

- [x] Re-read the latest `office` QEMU diagnostics and the exact linked `block_device_init` disassembly.
- [x] The caller snapshot reports RSP `0xD5990` and frame address `0x1568F0` (a gap of about 562 KiB), while the fault has `RIP=0` and the callee's saved return-address snapshot is reported as zero. This is a strong clue to investigate stack/frame integrity, but it is not yet proof of stack overflow or a root cause; the caller/callee global-snapshot mismatch also needs explanation.
- [x] Added a diagnostic-only `-fstack-usage` report for `kernel/prosenjitfs.c` and linked disassembly of `prosenjitfs_selftest` entry/call site to `scripts/build.sh`. This does not change runtime logic; it should reveal the compiler-reported static frame size and the actual call-site stack adjustment in the next build log.
- [ ] Run the new diagnostics on the designated `office` runner and compare the reported frame size with the observed caller RSP/frame values.
- [ ] Only after the new trace identifies a concrete cause, make a minimal kernel/ProsenjitFS fix and rerun all required ISO and native BIOS milestones.
- **Status:** instrumentation commit `95565d5d44d23a4fcc4c59411a0c3758f9e741d4` is pushed; its build workflow is queued. DML tests and security checks passed on the immediately preceding tested commit; this new diagnostic commit still requires its own CI result.


### 2026-10-17 13:33 UTC — Root cause isolated: oversized ProsenjitFS test stack object

- [x] Inspected the compiler-generated stack-usage output from build run [39036768898](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39036768898): `prosenjitfs_selftest` uses 528,240 bytes of static stack.
- [x] Located the dominant object in the source: the nested tiny-image exhaustion test declared a local `struct pfs_test_disk small_disk`. Its two 512 × 512-byte media arrays alone consume 524,288 bytes, before other locals and compiler-generated frame space. This explains the measured oversized frame and is consistent with (but does not alone prove every detail of) the QEMU null-return/fault evidence.
- [x] Applied a narrow fix in commit `5c70267a4066a6b4832902e9d4e6d3f113333e34`: made only `small_disk` static storage, with an explanatory comment. The test already zeroes the complete disk before use; device and filesystem state remain local. No production filesystem semantics or desktop code were changed.
- [ ] Verify the new compiler-reported stack usage is substantially reduced on `office`; inspect the exact value rather than assuming the fix worked.
- [ ] Re-run ISO QEMU and native BIOS gates on `office`. Accept no success unless required IPC/CPL3 milestones are present; investigate any next failure from fresh logs.
- [ ] Re-run DML tests on `OfficeWindows` and security architecture checks for the new head.
- **Status:** concrete oversized stack object identified and moved off stack; validation is pending. Desktop remains incomplete until boot milestones and real display/input/session tests pass.


### 2026-10-17 13:40 UTC — Stack fix verified; next ProsenjitFS assertion isolated

- [x] Retried the failed Debian `office` job after its earlier `actions/checkout` download/network error.
- [x] The retry built the ISO successfully and confirmed `prosenjitfs_selftest` static stack usage dropped from 528,240 bytes to **7,408 bytes** after moving the 512 KiB test-only disk object to static storage.
- [x] The prior null-RIP/oversized-frame symptom did not recur in this run. Kernel reached storage-volume discovery, then reported ProsenjitFS self-test failure `rc=-15`, source line/stage 7272 (hex `0x1C68`). The missing IPC/CPL3 markers are downstream of this filesystem self-test abort; they are not yet independent IPC failures.
- [x] Added a focused diagnostic change in commit `fe59c25e8a1ce0883b0b37f9d400fc5b95d309d5`: split the combined negative/boundary assertion into individual checks so the next serial failure line identifies the exact violated contract; also replaced the test's cast of a `uint64_t` local as a `uint32_t *` with a correctly typed `uint32_t invalid_offset`.
- [ ] Verify which individual boundary assertion fails on the next `office` QEMU run. Do not change filesystem production logic until the exact assertion and function contract are established.
- [ ] After ProsenjitFS self-test passes, verify IPC/CPL3 and all later boot milestones, then native BIOS and real desktop display/input/session gates.
- [x] `OfficeWindows` DML tests passed on the current tested head; security architecture checks passed.
- **Status:** oversized stack regression is fixed and its compiler measurement confirms it. ProsenjitFS self-test still fails on a separate `-15` boundary-test assertion; desktop remains incomplete.


### 2026-10-17 14:00 UTC — Fix stale pathname in oversized-truncate regression

- [x] Inspected the latest QEMU job for commit `2786587b394b1dab90f728f7eaa997885b799e1c`. Toolchain verification and ISO build passed; DML tests and security architecture checks passed; ISO QEMU failed in ProsenjitFS self-test at line 7280.
- [x] Traced line 7280 to the attempt to open `/hello.bin` for write. The same self-test intentionally unlinks `/hello.bin` at line 6931 and verifies the deleted path stays absent; reopening it without `O_CREATE` is an invalid test setup, not evidence of a VFS production bug.
- [x] Changed the oversized-truncate regression to use `/sizes.bin`, a file created and verified by the immediately preceding boundary-size persistence matrix. This preserves the intent of testing `UINT64_MAX` and 4 GiB truncation overflow without relying on a deleted pathname. Commit: `824af6da1cfd4810ce21c8814040f6493a5ac902`.
- [ ] Verify the new head on the designated Debian `office` runner. Check the next exact ProsenjitFS self-test failure, if any, before changing production behavior; then confirm IPC/CPL3 markers and the native BIOS boot gate.
- [ ] Production process spawn/exec/wait/reap, a real DML process-launch adapter, display/input delivery, and an end-to-end desktop session remain unfinished.
- **Status:** a test-fixture ordering/path bug was corrected; CI verification of this correction is pending. Desktop is not complete.

## 2026-10-17 — ProsenjitFS truncate-bound regression follow-up

- Re-ran the pull-request CI after the oversized-truncate pathname correction. The ISO build passed on the designated Debian `office` runner, but the QEMU boot gate failed before the IPC milestone: ProsenjitFS self-test returned `rc=-15` at stage `0x1CA6` (source line 7334 at the tested revision). The security architecture contract job passed.
- Root-cause candidate found in the failing truncate-bound test: it closed `file` and then read `file_data->inode_number`, even though `file_data` is vnode-private state whose lifetime is not guaranteed after close. The inode number had already been copied into the stable local `inode_number` variable.
- Fix committed: `afdff35fe7dc43b444c4baf94d02ab90a8d7340e` — use the saved inode number after close and split the combined checks into individual failures for precise diagnostics.
- Verification status at log update: source change committed; fresh CI/QEMU and OfficeWindows DML results are pending. Do not mark desktop or boot as complete until the next run proves the IPC milestone and all required runtime/display/input checks.

### 2026-10-17 — Audited the real CPL3 launch path before DML integration

- [x] Traced `boot/entry.asm`'s `enter_user_mode` IRETQ frame (user SS `0x23`, CS `0x1B`, entry and stack) and the `kernel/idt.asm` syscall/exit-trampoline route.
- [x] Compared the boot-time CPL3 test with `thread_start_trampoline`, `thread_init`, ELF loader output, and internal process lifecycle APIs. Confirmed the boot test is not yet a general scheduled-child launcher; ELF loading does not enqueue a runnable user thread; the kernel thread trampoline is not a substitute for an IRETQ ring-3 frame.
- [x] Recorded the exact source audit and implementation decision in `docs/DML-PROCESS-LAUNCH-GATES.md`, commit `92dbfbd67b5f89e9a5be294791c8f18d73a0038c`.
- [x] Kept syscall ABI and production behavior unchanged; no guessed spawn syscall or mock-backed boot path was added.
- [ ] Implement and test a kernel-internal launch transaction with admission, ELF loading, initial user frame, runnable thread handoff, rollback, and wait/reap ownership. Then review a versioned user ABI and integrate the DML backend only after kernel tests pass.
- **Status:** source audit and documentation committed; this is not a launch implementation or desktop completion. Current CI remains blocked on the ProsenjitFS QEMU self-test regression, and the OfficeWindows DML job result is still pending in the latest available run.


### 2026-10-17 — Corrected stale QEMU log-gate expectations

- [x] Inspected the failed Office/debian QEMU workflow log for commit `572dadd43f1c8d7372d6dd618573ba4fee4ff138`. The ISO built successfully; QEMU reached and printed `CPL3 user-mode and IPC syscall test passed`, `IPC wait-queue block/wakeup test passed`, the ProsenjitFS persistence milestone, and the scheduler milestones before the workflow failed on a missing marker string.
- [x] Traced the missing marker against the current source: `kernel/kernel.c` does not emit the old standalone `IPC channel/capability foundation`, per-test security, capability-transfer, or usercopy-recovery marker strings. The security manager runs component self-tests before emitting its aggregate runtime verification marker; the real address-space self-test emits `address-space deep isolation self-test passed`.
- [x] Updated `.github/workflows/build.yml` on this branch to require the current emitted IPC/CPL3 and wait-queue markers, the security-manager aggregate verification marker, and the actual address-space and usercopy initialization markers instead of obsolete strings. This corrects a CI log-contract mismatch; it does not implement missing runtime capabilities.
- [ ] Re-run the updated workflow and inspect every remaining missing milestone or QEMU exception. Confirm the native BIOS boot gate separately.
- [ ] Explicitly audit/add runtime regression coverage for capability copy/move transfer and usercopy page-fault recovery; those old marker strings were not present in the serial log and are not claimed as independently verified by this change.
- [ ] DML test result on the designated `OfficeWindows` runner remains unverified because the prior DML job was cancelled before completion. Re-run it on the current branch head.
- **Status:** workflow gate corrected and committed as `e016776e13c9bb1234291b081718a67cec84d0ac`; this is not desktop completion. Real process spawning, DML-to-kernel backend integration, display/input delivery, and a graphical session remain outstanding.


### 2026-10-17 — Prevent Actions from cancelling the designated runner tests

- [x] Inspected the workflow history after the DML job on `OfficeWindows` was cancelled while its test step was running.
- [x] Root cause: `.github/workflows/build.yml` used the same concurrency key for `push` and `pull_request` events on the same branch. A push-triggered run and the PR-triggered run therefore cancelled one another before verification could finish.
- [x] Updated the concurrency key to include `${{ github.event_name }}`, so push and PR event runs no longer cancel each other merely because they refer to the same branch.
- [ ] Verify the new PR-triggered `OfficeWindows` DML run reaches a final result and the Debian `office` build/QEMU run completes without being cancelled by the parallel event.
- **Status:** concurrency fix committed as `ba785afb8782083658ce568d369730817a9eff49`. Earlier DML job results remain cancelled and must not be counted as pass/fail test results.


### 2026-10-17 — Fix bounded-truncate regression fixture

- [x] Inspected the latest QEMU failure: ProsenjitFS self-test returned `rc=-15` at source line 7329, where the bounded-truncate test expected `PROS_VFS_EOVERFLOW`.
- [x] Root cause in the test fixture: it wrote at `n * block_size`, which allocates adjacent logical blocks and can coalesce into one extent, contradicting the test comment that eight independent extents exceed the v1 transaction bound.
- [x] Changed fixture writes to `n * 2 * block_size`, leaving one logical block hole between each written block. Added a precondition that the inode contains exactly eight one-block extents at logical blocks 0, 2, 4, ..., 14 before testing bounded truncate.
- [ ] Run the updated ISO/QEMU test. If the new extent-layout assertion or truncate expectation fails, use its distinct `-17` diagnostic and the exact source line to continue root-cause analysis.
- **Status:** source/test fix committed as `8863302b4c2fbf12373d61b01a3ab32f2d2d87f0`; runtime verification pending. This is a regression-test fixture correction, not yet a verified filesystem fix or desktop completion.


### 2026-10-17 — Preserve corruption errors on malformed file extents

- [x] The updated bounded-truncate fixture advanced past its previous `-15` failure. The next QEMU result stopped at source line 7400 with `rc=-16` in the semantic-corruption test for `/corrupt.bin`.
- [x] Traced the failure to `pfs_file_read()`: when `pfs_find_extent()` returned a specific error (including `PROSENJITFS_ECORRUPT` for the deliberately invalid extent head), the read path collapsed it to generic `PROS_VFS_EIO`. The regression correctly expects `PROS_VFS_ECORRUPT`.
- [x] Changed the read path to preserve filesystem error mapping through `pfs_vfs_error(rc)`; a successful lookup with an impossible zero extent-block location now returns `PROS_VFS_ECORRUPT` instead of generic I/O failure. Partial reads continue to return the bytes already read.
- [ ] Re-run ISO/QEMU and confirm the corruption regression passes, then continue through the remaining persistence/journal matrix and native BIOS gate.
- **Status:** source fix committed with this log entry; runtime verification pending. No desktop-completion claim.


### 2026-10-17 — Unmount corrupted test image before restoring it

- [x] The latest QEMU run passed the bounded-truncate fixture and the malformed-extent read assertion, then failed at source line 7415 with `rc=-16` while restoring the clean ProsenjitFS image.
- [x] Traced the restore sequence: after validating that the corrupted filesystem is rejected, the self-test zeroed and reformatted the backing disk while the same `fs` object was still mounted, then attempted to mount it again. `prosenjitfs_mount()` must not be called over an active mount.
- [x] Added an explicit successful `prosenjitfs_unmount(&fs)` check before resetting the mock disk and formatting/remounting the clean image.
- [ ] Re-run QEMU and verify the corruption test, clean remount, allocation-exhaustion and journal/crash matrix all advance. Continue fixing the next exact failing assertion rather than weakening the self-test gate.
- **Status:** source fix and log committed together; runtime verification pending.


### 2026-10-17 — Initialize tiny-image filesystem state before mount

- [x] The next QEMU run passed the corruption-image restore and advanced to the tiny-image allocation-exhaustion test, then failed at source line 7444 with `rc=-17`.
- [x] Root cause: `small_fs` was an uninitialized local object. `prosenjitfs_mount()` checks `fs->mount.active` before it clears/initializes the supplied filesystem structure, so uninitialized stack bytes could reject the mount as busy.
- [x] Explicitly zero-initialized `small_fs` before the tiny-device format/mount sequence. The 512 KiB disk remains static to avoid placing it on the kernel self-test stack.
- [ ] Re-run QEMU to verify minimum-size format/mount and allocation exhaustion, then continue through the recovery/reuse and journal/crash matrix.
- **Status:** test initialization fix and this log entry committed together; runtime verification pending.


### 2026-10-17 — Correct allocation-exhaustion self-test invariant

- [x] The latest QEMU run advanced past the tiny-image setup and failed at source line 7467 (`rc=-17`) after exhausting the raw block allocator.
- [x] The test was incorrectly calling `pfs_selftest_consistency()` after allocating data blocks directly without attaching them to inode extent chains. The consistency checker intentionally rejects allocated blocks that have no filesystem owner, so this assertion contradicted the test's setup.
- [x] Replaced the inappropriate global consistency check with a direct allocation-bitmap range check for the last successfully allocated block. Added an explicit `last_allocated_block` variable and used it for free/reuse verification.
- [ ] Re-run QEMU and verify ENOSPC, free/reuse, inode allocation, and the remaining journal/crash recovery matrix.
- **Status:** regression-test invariant fix and log committed together; runtime verification pending.


### 2026-10-17 — Keep inode-exhaustion test scoped to allocator contract

- [x] The office Debian runner built the ISO successfully. QEMU advanced to source line 7505 and failed with `rc=-17` in the tiny-image inode-exhaustion test.
- [x] Root cause: the test intentionally calls `prosenjitfs_alloc_inode()` repeatedly to exhaust bitmap slots, without initializing valid inode records or linking them into the namespace. It then ran the global consistency checker, which is correctly expected to reject those deliberately unowned slots.
- [x] Removed only the contradictory global-consistency assertion. The test still requires allocation to terminate with `PROSENJITFS_ENOSPC` and verifies that the allocator reached the expected exhaustion count; the surrounding mount/unmount and other consistency gates remain intact.
- [ ] Re-run ISO/QEMU and native BIOS QEMU. Continue from the next exact failure; do not mark the desktop implementation complete until the full required gates pass.
- **Status:** regression-test scope fix and this log entry committed together; runtime verification pending.


### 2026-10-17 — Reset allocator-exhaustion image before global consistency check

- [x] The latest office Debian QEMU run passed block and inode allocation exhaustion but stopped at source line 7543 with rc=-17 in the following mount-boundary/consistency check.
- [x] Root cause: the preceding exhaustion test intentionally leaves raw data blocks and inode bitmap slots allocated without inode/extent ownership. Reusing that same image for pfs_selftest_consistency() makes the checker correctly reject the deliberately inconsistent exhaustion fixture.
- [x] Added a checked reformat of the tiny test device after its filesystem is unmounted and before the independent short-partition rejection and clean consistency checks. This preserves the strict global consistency assertion while isolating the test fixtures.
- [ ] Re-run ISO/QEMU and verify the remaining journal recovery and crash/power-loss matrix, then run the native BIOS QEMU gate. The OfficeWindows DML manager/runtime tests passed on commit 31e7beee53bfce2d1b073ddc50d4023039079439.
- **Status:** test-fixture isolation fix committed; runtime verification pending. Desktop environment is not yet complete.


### 2026-10-17 — Simulate power loss before interrupted journal recovery

- [x] The latest ISO/QEMU run passed the tiny-image boundary checks and the nine transaction-boundary crash cases, then failed at source line 7849 with rc=-14 in the interrupted-recovery/idempotence regression.
- [x] Root cause: the test injected stage 9 after a durable journal commit, verified the expected write error, then called prosenjitfs_mount() again on the still-active filesystem object. That violates the mount lifecycle and prevents recovery-stage 10/11 injection from being exercised.
- [x] Added pfs_test_crash_reset(&fs, &disk) between the durable stage-9 interruption and the recovery mount. This discards volatile cache state and tears down in-memory mount state while preserving the durable journal, matching the intended power-loss scenario.
- [ ] Re-run ISO/QEMU to verify recovery interruption at stages 10 and 11, then verify the native BIOS QEMU gate and the full DML Windows job.
- **Status:** crash-test lifecycle fix committed; runtime verification pending, desktop environment remains incomplete.


### 2026-10-17 — Expose device-broker self-test failure stage

- [x] The latest QEMU run passed the complete ProsenjitFS journal/recovery self-test and then stopped in the device permission broker self-test before CPL3 startup.
- [x] The broker self-test already returns distinct negative codes for its assertions (-1 through -12), but kernel boot logging discarded that code, preventing evidence-based diagnosis.
- [x] Updated the boot failure message to print the exact signed broker self-test result in hexadecimal. This is diagnostic instrumentation only; it does not relax the broker gate or claim the broker is fixed.
- [ ] Re-run the latest commit, map the reported code to the precise broker assertion, fix the underlying defect, then rerun ISO/QEMU, native BIOS QEMU, and OfficeWindows DML tests.
- **Status:** diagnostic commit pending runtime evidence; desktop implementation is not complete.


### 2026-10-17 — Align stale device-grant revocation assertion with API result

- [x] Source-traced the broker self-test's combined assertion after the QEMU run stopped at `device permission broker self-test failed`. `pros_device_revoke()` returns PROS_DRIVER_ENODEV when no active grant matches; after the first revoke, the second revoke therefore returns ENODEV, not EPERM.
- [x] Corrected the test to expect ENODEV for revoking an already-revoked grant. The adjacent permission check still requires EPERM, so the test continues to prove that the revoked grant no longer authorizes access. The broker's runtime behavior and enforcement were not weakened.
- [x] Retained the kernel's numeric broker error diagnostic so any later broker assertion failure reports its exact test code.
- [ ] Re-run ISO/QEMU, confirm broker and display/input device tests pass, then run native BIOS QEMU and OfficeWindows DML unit tests on this commit.
- **Status:** source-derived assertion fix committed; runtime verification pending and desktop remains incomplete.


### 2026-10-17 — Exercise oversized network payload on an established socket

- [x] The latest ISO/QEMU run passed the ProsenjitFS, device permission broker, and display-device binding self-tests, then stopped at the network protocol/socket self-test.
- [x] Source review found the final network assertion tested an oversized send immediately after deliberately rejecting an invalid ACK. The socket correctly remained in SYN_SENT, and send validates socket state before payload length, so ESTATE was returned before the intended EBOUNDS check.
- [x] Added the valid ACK for the expected sequence number after the invalid-ACK assertion and before the oversized send. The test now proves the bad ACK is rejected without advancing state, then transitions to ESTABLISHED and tests the payload bound under the documented state precondition.
- [ ] Re-run ISO/QEMU and continue through PCI and CPL3 milestones; verify native BIOS and OfficeWindows DML gates separately.
- **Status:** network regression fixture corrected; runtime verification pending and desktop environment remains incomplete.


### 2026-10-17 — Check the full accepted network payload length

- [x] The latest QEMU run passed the filesystem, broker, and display self-tests but still stopped at the network protocol/socket self-test after the first socket send assertion.
- [x] Source review found the test sent a two-byte payload (`sizeof(payload) == 2`) while expecting `pros_net_socket_send()` to return `1`. The API returns the number of bytes accepted, so the assertion was incorrect.
- [x] Changed the expected result to `(int)sizeof(payload)`. The separate final oversized-payload test now also establishes the socket before checking EBOUNDS, preserving state and bounds coverage.
- [ ] Re-run ISO/QEMU and continue through PCI/CPL3 and later milestones; verify native BIOS and OfficeWindows DML gates separately.
- **Status:** network test assertions aligned with API semantics; runtime verification pending and desktop remains incomplete.


### 2026-10-17 — Use a genuinely undersized receive buffer in network self-test

- [x] After correcting the two-byte send return expectation, the next QEMU run still stopped in the network self-test.
- [x] Source review found the receive-bounds assertion supplied a two-byte buffer for a two-byte queued payload while expecting PROS_NET_EBOUNDS. The receive API correctly accepts that capacity; the fixture was not undersized.
- [x] Changed the first receive attempt to advertise one byte of capacity, then retained the following two-byte receive to verify the queued payload can be read intact after the bounds rejection.
- [ ] Re-run ISO/QEMU; continue to PCI and CPL3 milestones, then complete native BIOS and Windows DML gates.
- **Status:** receive-buffer fixture corrected; runtime verification pending and desktop remains incomplete.


### 2026-10-17 — Build the ICMP test checksum from one snapshot

- [x] The latest QEMU run passed ProsenjitFS, device-broker, and display self-tests but still stopped in the network self-test after the send/receive test-fixture corrections.
- [x] Source audit found the ICMP fixture called pros_net_checksum() twice while filling the two checksum bytes. The second call included the already-written high checksum byte, so it computed a different checksum and created an invalid ICMP packet.
- [x] Compute the checksum once while the field is zero, then write both bytes from that same value. This corrects the fixture and keeps the ICMP parser's checksum validation strict.
- [ ] Re-run ISO/QEMU to verify the full network self-test and continue to PCI/CPL3; then verify native BIOS and OfficeWindows DML gates.
- **Status:** ICMP regression fixture corrected; runtime verification pending and desktop remains incomplete.


### 2026-10-17 — Expose network self-test failure code

- [x] The latest QEMU run passed the filesystem, broker, and display-device tests but continued to stop in the network protocol/socket self-test after the ICMP and socket-fixture corrections.
- [x] The network self-test returns distinct negative codes for each assertion, but kernel boot logging only printed a generic failure, hiding which assertion remains incorrect.
- [x] Added the signed network self-test result to the serial error line. This is diagnostic-only and does not skip or weaken network checks.
- [ ] Re-run QEMU, map the exact return code to its assertion, fix the root cause, and continue through PCI/CPL3 and the native BIOS gate.
- **Status:** diagnostic instrumentation committed; runtime verification pending, desktop remains incomplete.


### 2026-10-17 — Fix ephemeral port binding in network socket API

- [x] The network self-test diagnostic reported rc=-13, which maps to the final invalid-ACK/oversized-send regression block. Inspection localized the first failing expression to `pros_net_socket_bind(sid, 0u, 0u)`.
- [x] Root cause was in the implementation: `pros_net_socket_bind()` called `port_conflict()` before checking whether port 0 requested an ephemeral port, while `port_conflict()` intentionally treats port 0 as invalid. This made the function's existing ephemeral-allocation branch unreachable.
- [x] Reordered validation so socket/state are checked first; port 0 now allocates an ephemeral port, while nonzero ports still go through conflict validation. The self-test keeps the port-0 bind path and verifies subsequent invalid-ACK and payload-bound behavior.
- [ ] Re-run ISO/QEMU; verify the complete network self-test, PCI/CPL3 milestones, native BIOS boot, and Windows DML tests.
- **Status:** real socket API defect fixed; runtime verification pending and desktop remains incomplete.


### 2026-10-17 — Preserve the live process manager around its self-test

- [x] After network sockets and PCI enumeration passed, the ISO/QEMU run entered CPL3 but never emitted the user-mode syscall success milestone. Serial diagnostics showed handle-duplication denials with `process=0`, indicating that `process_current()` had been cleared before the user-mode test.
- [x] Root cause: `process_selftest()` called `process_manager_init()`, which reset the global process list, current process, and PID counter. The self-test then returned success without restoring the live `init_process` registry/context, invalidating later syscall dispatch.
- [x] Split the test body into an isolated fixture and wrapped it with save/restore of the manager globals and address-space context. The original self-test assertions remain intact; on return, the previously active process and PID allocator are restored.
- [ ] Re-run ISO/QEMU to verify the CPL3 syscall completion milestone and downstream scheduler/IPC tests; then run native BIOS QEMU and the Windows DML tests.
- **Status:** process-manager test-isolation defect fixed; runtime verification pending and desktop remains incomplete.


### 2026-10-17 — Implement and verify native BIOS ATA/MBR/ProsenjitFS persistence path

- [x] The ISO/QEMU run now passes all existing milestones through CPL3. The native BIOS job exposed a missing implementation, not a log-only issue: `kernel.c` never probed ATA PIO or mounted the MBR-declared PFS partition, while `build.sh` left that partition blank.
- [x] Added a native-boot-only ATA PIO path: probe/read the real IDE block device, parse its MBR, locate the 0xDA ProsenjitFS partition, and validate partition-local out-of-range reads/writes are rejected.
- [x] The kernel now formats a partition only when `prosenjitfs_probe()` rejects it and a full sector scan proves every partition sector is zero. A nonblank unsupported/corrupt partition is never auto-formatted.
- [x] Added a persistent marker write/read test with unmount/remount verification. The native workflow now boots the same disk image twice and requires the second boot to report recovery of the marker written on the first boot.
- [x] Exposed the existing PFS probe/format/mount/unmount functions through the kernel PFS header and corrected the build-script comment to match the real native storage flow.
- [ ] Verify build, ISO/QEMU, first native BIOS boot, second native BIOS persistence boot, security architecture checks, and OfficeWindows DML unit tests on this commit.
- **Status:** native storage integration implemented; runtime verification pending. Desktop environment is not yet complete.


### 2026-10-17 — Diagnose native PFS blank-partition rejection

- [x] The current Office Debian run passes ISO/QEMU and all earlier boot milestones, but native BIOS boot stops in `native_bios_storage_selftest()` with `rc=-6` after the ATA real-disk block self-test.
- [x] Mapped `-6` to the exact guard: the PFS partition is rejected because the full-sector blank scan did not return `1`. The existing log did not distinguish a nonzero byte from an ATA read/allocation failure.
- [x] Added bounded serial diagnostics for allocation failure, read failure (including partition-relative LBA and return code), and the first nonzero sector/byte/value. The scanner remains read-only; it does not auto-format a nonblank or unreadable partition.
- [ ] Re-run native BIOS QEMU, use the emitted diagnostic to fix the underlying storage/image issue, then verify first-boot marker creation and second-boot persistence. Recheck ISO/QEMU and OfficeWindows DML tests after the fix.
- **Status:** root-cause instrumentation committed; native storage and desktop completion remain unverified.


### 2026-10-17 — Fix ATA transfers that end at the final device LBA

- [x] The latest run's blank-scan diagnostic narrowed the native BIOS failure to the final batch: partition-relative LBA `0x1F8` (504), count 8, returned `PROS_BLOCK_EIO` while scanning the 512-sector partition.
- [x] Traced the error through `ata_read()` to `block_device_lba_add()`. After successfully transferring the last chunk, the ATA read/write loops unconditionally advanced to the one-past-end LBA. The helper correctly rejects an out-of-range result, so a valid transfer ending exactly at `sector_count` was incorrectly reported as I/O failure.
- [x] Updated both ATA read and write loops to advance the LBA only when more sectors remain. Bounds validation and per-sector I/O error checks are unchanged; exact-end transfers now can complete without manufacturing an invalid next-LBA request.
- [ ] Verify ISO/QEMU and native BIOS QEMU; confirm blank scan passes, first boot writes the persistence marker, and second boot recovers it. Re-run OfficeWindows DML tests and security checks.
- **Status:** ATA exact-end transfer bug fixed in source; runtime verification pending and desktop remains incomplete.


### 2026-10-17 — Fix second native BIOS QEMU command continuation

- [x] After the ATA exact-end transfer fix, the first native BIOS boot passed the ATA/MBR/PFS persistent mount test and wrote the persistence marker.
- [x] The workflow then failed before its second boot: shell logs showed `Second native QEMU exit status: 127`. Inspection of `.github/workflows/build.yml` found doubled trailing backslashes on every continuation line of the second QEMU command, so Bash ended the command early and tried to execute `-drive` as a separate command.
- [x] Changed those seven continuations to single backslashes. The first native boot and all its checks are unchanged; the second boot will now invoke QEMU with the same image and can verify marker recovery.
- [ ] Re-run build, ISO/QEMU, first and second native BIOS boots, OfficeWindows DML tests, and security checks on this revision.
- **Status:** CI invocation defect fixed; persistent second-boot recovery and desktop completion still require verification.


### 2026-10-17 — Verification of native persistence and DML gates

- [x] Tested exact source commit `73d5680b1b629d712fef4bc4b220be9a996de3b5` on the designated `office` Debian self-hosted runner: warnings-as-errors build and ISO/QEMU boot gate passed.
- [x] Native BIOS first boot passed ATA PIO, MBR partition discovery, ProsenjitFS format/mount, filesystem-local LBA boundary checks, marker write, unmount/remount, and marker readback.
- [x] Native BIOS second boot reused the same disk image and emitted `ProsenjitOS: native persistence marker recovered from prior boot`; the persistent-mount and LBA-boundary milestones also passed. Both QEMU invocations ended with expected timeout status 124 after the required milestones.
- [x] The designated `OfficeWindows` runner passed `desktop_manager_test` and `desktop_runtime_test` (DML job `117459252207`).
- [x] Security architecture checks passed (run `39054385898`).
- [x] Root causes corrected in this sequence: ATA loops falsely failed transfers ending at the final LBA, and the second native QEMU command used doubled shell continuations. No checks were skipped or weakened.
- [ ] Continue Desktop Implementation Plan phases: implement and test safe preemptive CPL3 process execution/spawn-wait-reap, userspace init/service management, production display/input drivers/services, then verify a compatible window system and XFCE end-to-end. These are genuine remaining features, not CI failures.
- **Status:** OS/storage regression gates and host DML tests are green on the verified commit. Desktop GUI completion is still outstanding.


### 2026-10-17 — Add validated first-run CPL3 scheduler frames

- [x] Added a distinct CPL3 interrupt-frame shape with the required user RSP and SS words after RIP/CS/RFLAGS, while preserving the existing kernel interrupt-frame layout and the assembly-critical `pros_cpu_context.rsp` offset.
- [x] Added `thread_init_user()` to validate an executable user entry mapping, a writable/NX user stack mapping, aligned bounded kernel stack storage, and then construct an IRET frame with user selectors `CS=0x1B`, `SS=0x23`, and `RFLAGS=0x202`. It does not enqueue or execute the thread by itself.
- [x] Added TSS.RSP0 setter/getter assembly helpers and made interrupt-driven scheduling install the selected thread's kernel stack before returning its saved frame. The existing non-preemptive CPL3 self-test explicitly restores the bootstrap stack.
- [x] Guarded the kernel `RET` scheduler path against selecting a CPL3 thread; CPL3 frames must go through the interrupt/IRET path.
- [x] Added boot assertions for scheduler TSS-stack selection and a CPL3 frame-construction self-test; added both milestones to ISO and native BIOS QEMU gates.
- [ ] Build and boot-test this commit on `office`, run security architecture checks, and run DML manager/runtime tests on `OfficeWindows`. A successful frame-construction test is only a prerequisite: real spawn/exec/wait/reap, safe user-mode preemption, and GUI services remain unimplemented.
- **Status:** initial user-mode scheduler-frame primitive implemented; runtime verification pending and desktop remains incomplete.

## 2026-10-18 — Add bounded userspace compositor/window primitives

- [x] Added a userspace-only, caller-owned 32-bit pixel-surface renderer with desktop background/panel and clipped window surfaces.
- [x] Added a bounded eight-window registry, opaque IDs, z-order raising, focus, close, and topmost pointer hit-testing.
- [x] Validate surface dimensions, pitch/capacity arithmetic, title bounds/printable characters, window geometry, and stale IDs.
- [x] Added regression coverage for rendering, overlap/z-order, focus, pointer routing, close/stale handles, invalid geometry/title, and invalid backing surfaces; wired it into POSIX and Windows DML test scripts.
- [ ] Verify compositor tests on OfficeWindows and rerun Debian warnings-as-errors build/ISO/native BIOS regressions on this commit.
- **Limitations:** this is not a hardware display driver, framebuffer mapping, real input path, window-server process, package/runtime backend, or bootable graphical desktop. Desktop completion remains unverified.

## 2026-10-18 — Harden compositor state validation

- [x] Follow-up source audit found that the public compositor structure needed validation of its z-order indices before pointer hit-testing; corrupted indices could otherwise be used as array subscripts.
- [x] Extended the shared validity gate to verify active-window count, unique nonzero IDs, geometry/title bounds, unique in-range z-order slots, cleared unused order slots, and focus membership before every public operation.
- [ ] Verify the compositor regression suite and all existing desktop/OS CI gates on the updated commit.
- **Status:** defensive validation implemented; CI verification pending.

- [x] Added forward declarations for the shared title/ID validators used by the compositor's common state gate, keeping the implementation valid under strict C11 compilation.

- [x] Corrected the invalid-title regression input to an empty title; path-like punctuation is valid display text and must not be confused with package-path validation.


### 2026-10-18 — Current-head verification and remaining desktop blockers

- [x] Re-checked the Desktop PR #137 head and its latest verification record. Workflow run [39109546967](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39109546967) completed successfully: designated Debian `office` runner passed toolchain verification, ISO build, ISO QEMU boot checks, native BIOS QEMU boot checks, and the Windows `OfficeWindows` DML test job passed.
- [x] Security architecture workflow [39109546969](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39109546969) completed successfully.
- [x] Re-audited the source contracts on `desktop/dml-dependency-order-20261017`: `kernel/init.c` loads and security-admits `/init` into a process address space, but does not start a thread or provide spawn/wait. `thread_init_user()` constructs a CPL3 interrupt frame, but a general executable spawn transaction and safe userspace service lifecycle are not yet exposed by the public syscall ABI.
- [x] Confirmed `user/desktop/compositor.c` is a software renderer over a caller-supplied pixel surface with window/z-order/pointer-hit-test primitives and host tests. It is not yet bound to the kernel display-present path, a concrete keyboard/pointer driver, or a running userspace session.
- [ ] Implement and test a complete process-launch transaction: secure executable resolution/admission, ELF load, user stack and kernel stack/thread ownership, runnable scheduling, rollback on every partial failure, and parent wait/reap. Preserve the existing IF-clear CPL3 policy until interrupt-frame restoration and user-mode preemption are verified.
- [ ] Add a real DML backend over that launch/stop/wait contract; do not connect the mock backend to boot.
- [ ] Implement the userspace display/input event services and bind compositor presentation to the supported display ABI/backend.
- [ ] Boot to a real session and verify rendering, keyboard and pointer delivery, application launch, logout, restart, and failure recovery.
- **Status:** prerequisite CI is green; Desktop Environment is **not complete**. These blockers are source/runtime work, not documentation-only acceptance gates. PR #137 must remain Draft until end-to-end evidence exists.

### 2026-10-18 — Initial kernel-side init launch transaction (verification pending)

- [x] Added `pros_init_launch()` as a kernel-internal primitive that creates a child process, applies the existing security-gated `/init` ELF loader, validates a CPL3 thread frame, and queues the thread only after all setup stages succeed.
- [x] Added failure rollback that attempts to terminate and reap a child after loader, user-thread initialization, or run-queue insertion fails.
- [x] Kept the caller responsible for process/thread storage and kernel-stack memory; the API does not allocate unbounded resources or invent a public syscall number.
- [ ] Compile the exact branch head with `-Wall -Wextra -Werror` and run ISO/native BIOS QEMU tests.
- [ ] Add a real process-launch selftest that exercises success and each rollback edge using an admitted executable fixture.
- [ ] The scheduler currently skips user threads in ordinary `thread_schedule()`; the interrupt-frame scheduling path, IF policy, child exit return, and parent wait/reap must be verified together before this helper can be treated as a production spawn/exec path.
- [ ] No public spawn/exec/wait syscall, DML production backend, graphical display presentation path, keyboard/pointer event path, or boot-to-desktop test has been completed by this change.
- **Status:** source primitive committed; compilation and runtime verification pending. Desktop Environment remains incomplete.

## 2026-10-18 — Current-head CI and user-thread scheduling audit

### Verified on branch head `895ca9122ba5fcb6cfea2cccfc2ac6b9c32ae1bf`
- [x] GitHub Actions PR build/boot run `39135123119`: Debian self-hosted runner verification, toolchain verification, ISO build, ISO QEMU boot test, and native BIOS QEMU boot test all succeeded.
- [x] The same run's OfficeWindows `dml-tests` job succeeded.
- [x] Security workflow run `39135122988` succeeded.
- [x] Reviewed `kernel/thread.c` scheduler and `kernel/include/thread.h` user-thread frame contract on this branch head.

### Blocking finding — not fixed by these green workflows
- [x] Confirmed `thread_schedule()` deliberately requeues and skips every `user_mode` thread because the ordinary context switch uses a kernel RET path.
- [x] Confirmed `thread_schedule_interrupt()` can select a user thread only when a saved interrupt frame exists; `thread_init_user()` deliberately builds a CPL3 IRET frame with interrupts disabled pending verified preemption.
- [ ] No end-to-end test yet proves timer-driven CPL3 preemption, user-thread exit, process teardown/reap, and safe return to another process together. Do not enable user interrupts or claim general spawn/exec readiness without that test.
- [ ] No real graphical desktop-session test is represented by the current build/boot or hosted DML tests. Display presentation, keyboard/pointer event delivery, application launch, logout/restart, and failure recovery remain pending.

### Header clarification
- [x] Clarified that `pros_init_launch()` accepts a caller-selected executable path rather than only `/init`; its successful return means the CPL3 thread is queued, not that it has executed.
- [ ] This launch helper is still an initial kernel primitive, not a production userspace spawn/exec/wait ABI or a complete desktop launcher.

### Next required implementation gate
Implement a reviewed interrupt-return scheduling/exit lifecycle with deterministic cleanup and regression tests before connecting a production DML process backend. Preserve the current safe non-preemptive user-mode behavior until that gate passes.

