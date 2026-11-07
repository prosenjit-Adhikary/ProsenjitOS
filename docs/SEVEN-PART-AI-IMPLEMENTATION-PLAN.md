# ProsenjitOS — Seven-Part AI Implementation Plan

Status: proposed execution plan. This document assigns work; it does not certify that any component is implemented.
Repository: https://github.com/prosenjit-Adhikary/ProsenjitOS
Companion architecture: [Compatibility-First Implementation Model](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md)
Last updated: 2026-10-22

## 1. Purpose and operating rules

This plan divides the implementation into seven bounded workstreams so that seven separate AI agents can contribute without inventing incompatible architectures, overwriting each other, or declaring work complete without evidence.

The project goal is an independent, bootable x86-64 ProsenjitOS with its own native kernel, native applications and filesystem, a unified desktop experience, and a staged user-mode Windows compatibility environment. Broad Windows compatibility is a long-term objective, not a claim that all Windows programs or drivers already work.

### Mandatory rules for every agent

1. **Inspect before editing.** Read the architecture model, relevant source, build files, tests, open PRs and recent history. Do not assume a feature exists because a filename, header, stub, README or earlier report says so.
2. **Establish a baseline.** Record the starting branch and commit SHA, working-tree state where available, relevant build/test commands, existing failures and runner availability. Do not attribute pre-existing failures to your change without evidence.
3. **One workstream, one branch, one PR.** Use a dedicated branch per task. Do not push to the default branch, merge your own PR, overwrite another agent's branch or change unrelated subsystems.
4. **Coordinate through contracts.** Before changing a public ABI, syscall, on-disk format, IPC message, driver interface or desktop protocol, identify every known consumer and document the compatibility impact. Prefer additive, versioned changes.
5. **Preserve existing work.** In particular, do not delete or replace the existing Desktop Manager/DML, Shared Desktop Integration ABI, graphics/windowing foundation, keyboard/mouse/clipboard/audio work, application handler or ProsenjitFS simply because a new plan describes them differently. Audit first; change only with a concrete reason, migration plan and regression tests.
6. **Implement, do not simulate.** Do not substitute a stub, mock, hard-coded success, fake screenshot, format detector or documentation-only change for the requested runtime behavior. Mocks are acceptable only for explicitly scoped unit tests and must be labelled.
7. **Test honestly.** Record exact command, target, commit SHA, exit code and relevant output. A host test does not prove guest behavior; a successful build does not prove boot; QEMU does not prove physical hardware support. Queued, cancelled, unavailable or runner-blocked tests are not passes.
8. **Fail closed.** Reject malformed binaries, packages, paths, messages and invalid capabilities safely. Never disable memory protections or security checks just to make a demo pass.
9. **No unsupported compatibility claims.** Distinguish detected, parsed, installed, launched, functionally tested and broadly compatible. Never claim all EXE/MSI/DLL/driver support.
10. **Respect licensing and provenance.** Evaluate third-party code and runtimes before integration; record license, notices, dependencies and changes. Do not copy code with unknown license status.
11. **Keep changes reviewable.** Prefer small, coherent commits and PRs. Explain design decisions and include regression tests.
12. **Report blockers instead of hiding them.** Use the status labels below and list the next concrete action.

### Status vocabulary

- **COMPLETE** — acceptance criteria passed with evidence for the exact commit.
- **PARTIAL** — some criteria pass; list those passed and what remains.
- **BLOCKED** — cannot proceed because a specific prerequisite/resource/decision is missing.
- **NOT TESTED** — implementation may exist, but required validation was not run.
- **NOT IMPLEMENTED** — no working implementation is present.
- **DEFERRED** — intentionally out of scope for this milestone, with rationale.
- **REGRESSION** — previously passing behavior is now failing; stop expansion and investigate.

Do not use “complete” as a synonym for “code written” or “compiles.”

## 2. Global architecture contract

Use the [Compatibility-First Implementation Model](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md) as the architecture authority unless a reviewed ADR explicitly supersedes it.

Conceptual flow:

```text
Native apps / Windows user-mode apps / packages
        |
Execution router + package transaction service
        |                         |
Native loader/backend      Windows compatibility backend
        |                         |
        +---- shared user-space services / desktop ABI ----+
                              |
                  versioned syscall ABI + capability IPC
                              |
                    native ProsenjitOS kernel
                              |
            VFS / ProsenjitFS / native driver interfaces
                              |
                         hardware
```

Key boundaries:

- ProsenjitOS remains an independent native OS, not a Linux distribution or Linux-kernel wrapper.
- Same-ISA machine code can execute on the CPU, but Windows API, runtime, registry, services and kernel behavior must be implemented or adapted.
- Compatibility runtimes and desktop policy belong in user space unless a reviewed architectural decision states otherwise.
- The native kernel remains authoritative for memory, processes, I/O, security and device resource ownership.
- Native applications and compatible Windows applications must share one desktop integration contract; do not create a competing desktop.
- Windows kernel-driver support is a separate high-risk research track. Do not load arbitrary Windows .sys drivers directly into the native kernel.
- Future POSIX/Linux application support is separately scoped and must not silently introduce a Linux-kernel dependency.

## 3. Workstream map and dependency order

| Part | Workstream | Primary responsibility | Depends on |
|---|---|---|---|
| 1 | Kernel, boot and core interfaces | Boot path, memory, interrupts, processes, syscalls, security foundations | Existing baseline |
| 2 | Native hardware and drivers | Device discovery, display, input, audio and driver model | Part 1 interfaces |
| 3 | Native filesystem and storage | Block I/O, VFS, ProsenjitFS, persistence and recovery | Part 1 memory/I/O contracts |
| 4 | Native apps, execution router and packages | Native loader, backend routing and safe package transactions | Parts 1 and 3 |
| 5 | Windows user-mode compatibility | PE loader/runtime/API implementation and minimal real Windows app proof | Parts 1, 3, 4 and Part 6 contracts for GUI |
| 6 | Shared desktop integration | Preserve and extend current desktop/ABI; native and compatible GUI path | Existing desktop code, Part 2, Parts 4–5 |
| 7 | Integration, security and release | Cross-part audit, regressions, reproducible build and release gates | Evidence from Parts 1–6 |

This table describes ownership, not permission to make unreviewed cross-part changes. If a prerequisite is missing, the dependent agent should complete a source-backed audit, define the exact interface/blocker, and work only on independent tasks until the prerequisite is accepted.

## 4. Shared hand-off contract for all agents

Every PR description must include:

- Workstream/part number and objective.
- Base branch and base commit SHA.
- Exact files and interfaces changed.
- Source-backed findings before changes.
- Design choices and alternatives considered where the choice is significant.
- Dependencies and consumers affected.
- Build/test commands, target, exit code, commit SHA and evidence links/log excerpts.
- Tests not run and why.
- Security and licensing considerations.
- Known limitations, follow-up issues and rollback strategy.
- Acceptance checklist with each item marked COMPLETE, PARTIAL, BLOCKED, NOT TESTED, NOT IMPLEMENTED or DEFERRED.

Before starting implementation, each agent should check open PRs and branches to avoid duplicating work. Before asking for review, it should rebase/update against the intended base safely and rerun affected tests. Never force-push over another agent's work.

---

# Part 1 — Kernel, Boot and Core Interfaces

## Mission

Establish and harden the native kernel foundation on which every other subsystem depends. Work only from the actual repository implementation; do not rewrite the kernel wholesale or invent a new architecture without evidence.

## Audit scope

Inspect the bootloader/entry point, linker script, CPU initialization, exception and interrupt paths, GDT/IDT or architecture-equivalent structures, physical and virtual memory management, heap/allocator, address-space switching, process/thread model, scheduler, syscall entry/dispatch, IPC, timer, locking/atomics, user/kernel boundary, capability/permission checks, device resource APIs, build configuration and QEMU setup. Identify which pieces are real, partial, stubbed or untested.

Record the current boot sequence and control-flow path from image entry to kernel initialization. Record existing ABI versions and all known consumers before altering them.

## Implementation scope

1. Fix reproducible boot/build defects and unsafe boundary conditions first.
2. Harden memory allocation and mapping: checked arithmetic, alignment, bounds, ownership, mapping permissions and failure cleanup.
3. Harden interrupt/exception and syscall paths, including invalid user pointers, invalid syscall numbers, malformed arguments, privilege transitions and error reporting.
4. Verify process/address-space and thread lifecycle behavior that other workstreams need. Do not claim scheduler or isolation features beyond the implementation and tests.
5. Define or document stable kernel interfaces needed by the storage layer, driver layer, execution router and user-space services.
6. Add diagnostics that help identify boot failures without flooding output or exposing secrets.
7. Keep architecture-specific code behind explicit architecture guards; do not imply support for CPUs or boot modes that are not built/tested.

## Tests and acceptance

- Reproducible clean build for the documented target.
- Existing test suite results recorded before and after changes.
- Unit tests for boundary checks, allocator failures, invalid syscall inputs and lifecycle errors where feasible.
- QEMU boot test on a declared machine configuration if available.
- Evidence of the exact commit, command, exit code and logs.
- Interface documentation sufficient for Parts 2–5 to integrate without guessing.

**Not sufficient:** successful compilation alone; a stub syscall; a screenshot; an old QEMU log from a different commit.

## Forbidden scope creep

Do not implement Windows APIs, duplicate the desktop, replace ProsenjitFS, add a Linux kernel, or modify unrelated app/runtime code. Do not weaken isolation to simplify the compatibility layer.

## Prompt to give AI Agent 1

> You own **Part 1: Kernel, Boot and Core Interfaces** of ProsenjitOS. Repository: https://github.com/prosenjit-Adhikary/ProsenjitOS. Read docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md and docs/SEVEN-PART-AI-IMPLEMENTATION-PLAN.md first. Audit actual source, build files, tests, branches and open PRs before editing. Record the base commit and baseline results. Inspect boot flow, CPU initialization, memory mapping/allocation, interrupts/exceptions, process/thread lifecycle, scheduler, syscalls, IPC, security boundaries and device interfaces. Fix only source-supported defects and implement the missing kernel foundations required by dependent parts. Preserve existing architecture and interfaces unless a reviewed, documented migration is necessary. Add negative/boundary tests, run the real build and QEMU tests when available, and record exact commands, exit codes, commit SHA and logs. Work on a dedicated branch and PR; do not commit directly to the default branch, merge your PR, overwrite another agent or delete unrelated work. Label each criterion COMPLETE/PARTIAL/BLOCKED/NOT TESTED/NOT IMPLEMENTED/DEFERRED. A compile-only result is not proof of boot or runtime correctness. Deliver a source-backed audit, code, tests, ABI notes, limitations and PR URL.

---

# Part 2 — Native Hardware Discovery, Drivers and I/O

## Mission

Build a truthful, incremental native hardware stack that works through Part 1's actual interfaces. Start with reproducible virtual hardware and add physical devices only where the controller and device implementation are real.

## Audit scope

Inspect PCI/device enumeration, bus and controller code, interrupt registration, MMIO/PIO helpers, DMA APIs and constraints, device-resource ownership, framebuffer/display path, graphics/windowing boundary, keyboard and pointer paths, USB/HID if present, audio interfaces, network/storage device models, logging and emulator configuration. Verify each dependency against Part 1 source.

## Implementation scope

1. Produce a supported-device inventory containing bus/type, vendor/device IDs where available, backend/driver, required kernel interfaces and test status.
2. Implement safe discovery and explicit reporting of supported, unsupported and failed devices.
3. Implement keyboard and pointer input for the specific virtual or physical devices actually supported.
4. Implement the simplest supported display path first; distinguish firmware/framebuffer output from a live compositor and from GPU acceleration.
5. Add audio only when an actual output backend exists, with ownership and lifecycle cleanup.
6. Validate device IDs, resource ranges, MMIO mappings, DMA buffers, lengths and interrupt lifecycle. Do not trust arbitrary device/package metadata.
7. Make driver initialization and failure recovery deterministic; one failed device must not falsely mark all device support as complete.
8. Preserve the existing desktop/windowing interfaces. Driver code should provide device services; it must not create a second desktop protocol.

## Tests and acceptance

- Tests for malformed/out-of-range resource descriptors and device initialization failures.
- QEMU tests with the exact emulated devices declared.
- Evidence that input events actually reach the guest path and display output actually works; record limitations of console-only or framebuffer-only paths.
- Audio claims require actual tested output path and evidence.
- Physical-device support is only claimed after testing the named device or a clearly documented equivalent.

**Not sufficient:** a device ID parser, a mock event, a framebuffer structure, or an untested driver stub.

## Forbidden scope creep

Do not load arbitrary Windows kernel drivers, perform unbounded DMA, assume all GPUs support one interface, or delete/replace existing desktop work.

## Prompt to give AI Agent 2

> You own **Part 2: Native Hardware Discovery, Drivers and I/O**. First read both architecture documents and inspect existing source, especially Part 1's real interfaces and current desktop/input/graphics/audio work. Verify what is implemented versus stubbed. Build a source-backed device inventory, then implement a small, safe set of real device paths supported by the repository and QEMU configuration. Validate memory/resource bounds, interrupts, DMA ownership, initialization failures and cleanup. Preserve the existing Shared Desktop ABI; do not create a competing compositor. Do not claim universal GPU, USB, Wi-Fi, Bluetooth or audio support, and do not load Windows .sys drivers into the native kernel. Use a dedicated branch/PR. Run actual build, negative tests and guest tests where available; record exact emulated/physical device, commands, exit codes, commit SHA and logs. Deliver code, supported-device matrix, limitations and PR URL. Mark untested hardware NOT TESTED.

---

# Part 3 — Native Filesystem, VFS and Storage

## Mission

Make the native storage path reliable, bounded, recoverable and testable while preserving existing ProsenjitFS work and on-disk compatibility.

## Audit scope

Inspect block-device interfaces, partition/volume discovery, VFS operations, ProsenjitFS format, mount/unmount, file and directory semantics, allocation maps, metadata, caching, locking, permissions, error handling, persistence, recovery, image tooling and tests. Determine which paths are actually called by the running OS.

## Implementation scope

1. Document the existing on-disk format and compatibility/versioning rules before changing it.
2. Validate block indices, offsets, lengths, arithmetic, metadata pointers, directory entries and allocation state before use.
3. Implement or harden open/read/write/seek/close, create/delete, directory traversal and mount/unmount only to the extent supported by the existing design.
4. Define consistency guarantees honestly. If journaling, atomic updates or crash recovery are not implemented, state that explicitly.
5. Protect against malformed or truncated images, allocation exhaustion, repeated mount/unmount, stale handles, concurrent operations and partial writes.
6. Add a safe formatting workflow that requires a disposable target and explicit confirmation at the tool/UI layer; never format arbitrary host disks in tests.
7. Integrate with package staging only through documented VFS/storage interfaces.
8. Provide diagnostics and recovery behavior that do not silently convert I/O failure into success.

## Tests and acceptance

Use disposable disk images. Include valid and corrupt image cases, boundary reads/writes, truncated blocks, invalid metadata, disk-full conditions, repeated mount/unmount, interrupted operations and persistence across guest reboot when supported. Verify file contents and metadata, not merely return codes. Never run destructive tests against real user data.

## Forbidden scope creep

Do not silently change the filesystem format, break old images, implement package management inside the filesystem, or claim journaling/encryption/snapshot support without source and test evidence.

## Prompt to give AI Agent 3

> You own **Part 3: Native Filesystem, VFS and Storage**. Read the architecture and seven-part plan, then audit the actual ProsenjitFS/VFS/block-device code, disk format, mount path and tests. Preserve existing work and format compatibility unless a versioned migration is justified and tested. Harden bounds, metadata validation, error propagation, permissions, concurrency, persistence and recovery according to the existing design. Use disposable test images only. Test malformed/truncated images, boundary operations, allocation exhaustion, repeated mount/unmount, interrupted writes and reboot persistence where available. Do not claim journaling, crash safety or encryption unless implemented and proven. Coordinate interfaces with Part 1 and Part 4; use a dedicated branch/PR and avoid unrelated edits. Report exact commands, exit codes, commit SHA, test image configuration, logs, format limitations and PR URL. Mark untested guest behavior NOT TESTED.

---

# Part 4 — Native Executables, Execution Router and Package Transactions

## Mission

Provide a trustworthy application path that distinguishes supported native binaries from Windows PE binaries and safely installs/uninstalls packages without confusing file detection with execution.

## Audit scope

Inspect current app handler, executable formats, native loader, process creation, command-line/environment handling, file associations, package tools, trust/signature handling, filesystem staging, permissions and existing UI integration. Trace a launch request from entry point to process creation. Identify whether the code is a real loader or only a parser/router stub.

## Implementation scope

1. Implement an explicit execution router with backend availability checks, architecture/ABI validation and actionable unsupported-format errors.
2. Native binaries must launch through the real native loader/process interface.
3. PE files must route to Part 5 only when a real compatible backend is available. Never label PE as native ELF or infer successful execution from extension.
4. Reject malformed headers, invalid lengths, unsupported architectures, missing dependencies and unsafe path resolution.
5. Define package metadata/versioning and validate package paths to prevent traversal, unsafe symlink handling and writes outside the installation root.
6. Stage installs before activation; verify hashes/signatures where available; record provenance; enforce least privilege; commit/rollback based on actual storage guarantees.
7. Separate package installation from executing installer scripts/actions. Do not automatically run downloaded installers as administrator.
8. Support safe uninstall and upgrade without deleting unrelated user data. Keep per-app data and profiles scoped.
9. Provide explicit launch status: detected, validated, backend selected, started, exited or failed, with a useful error and exit status.

## Tests and acceptance

Test valid and malformed native/PE headers, unsupported architecture, missing backend, permission denial, path traversal, corrupted packages, interrupted installation, rollback, upgrade/uninstall and cleanup. Guest tests are required for guest execution claims. Ensure no test can accidentally modify host paths or real user data.

## Forbidden scope creep

Do not implement Windows APIs here, pretend PE launch succeeded, silently run installers with elevated privileges, or change Part 5's backend contract without coordination.

## Prompt to give AI Agent 4

> You own **Part 4: Native Executables, Execution Router and Package Transactions**. Audit the existing application handler, native loader, process APIs, package manager, VFS and permissions before editing. Trace real launch/install flows and distinguish implemented code from detection-only stubs. Implement safe native execution routing and package transactions using verified Parts 1 and 3 interfaces. Route PE files only to Part 5 when that backend exists; format recognition is not execution. Validate architecture, headers, dependencies, package paths, privileges and rollback. Never auto-run downloaded installers elevated or write outside the target install root. Add positive and negative tests including malformed files, wrong architecture, missing backend, path traversal, corrupt package, interrupted install, rollback and uninstall. Work on a dedicated branch/PR, document interface changes, run actual tests and report exact evidence. Deliver a support-state matrix and PR URL; do not claim execution unless a real process launched.

---

# Part 5 — Windows User-Mode Compatibility

## Mission

Prove a small real Windows application path before expanding the API surface. Reuse existing compatibility technology if a feasibility study supports it and licensing/dependencies permit it.

## Audit and feasibility

Inspect the current PE parser/loader, process/memory model, user/kernel interfaces, file and network services, IPC, desktop ABI and execution router. Evaluate Wine or other established projects against actual ProsenjitOS constraints. Record required system interfaces, dependencies, porting cost, licensing, notices and security implications. Do not assume Wine is a drop-in binary or quietly add a Linux-kernel dependency.

## Implementation stages

1. **Feasibility decision:** document reuse/port/in-house alternatives and evidence.
2. **Minimal PE32+ parser:** x86-64 PE headers and sections, checked arithmetic, relocations/import validation as required by the chosen path, safe mapping permissions and explicit unsupported cases.
3. **First vertical slice:** launch one known, harmless x86-64 Windows console PE through the selected real backend; produce deterministic output and exit status in QEMU or another documented guest setup.
4. **Runtime expansion:** add tested process startup, memory, handles, synchronization, threads, time, files/paths, command line, environment and error semantics in small increments.
5. **Libraries and APIs:** maintain an API support inventory with implementation state, observable behavior, tests, security implications and known limitations.
6. **GUI integration:** only through Part 6's existing Shared Desktop ABI; no parallel compositor.
7. **Installer/runtime support:** separately scope MSI actions, .NET, services and complex runtimes. Container/file-format detection does not prove an installer is executable.

## Security requirements

Use checked arithmetic, bounded parsing, W^X by default, safe DLL search, dependency-loop detection, per-process address spaces and least privilege. Treat the PE program as untrusted. Do not claim a security sandbox unless escape/permission-negative tests exist. Never load arbitrary Windows kernel drivers into the native kernel.

## Tests and acceptance

A real guest launch must show the exact binary hash/provenance, backend/version, QEMU machine configuration, output, exit code and logs for the tested commit. Negative tests must reject malformed/truncated PE, bad sections/imports, overflow, wrong architecture and missing dependencies safely. Parser tests alone do not pass the vertical-slice gate.

## Forbidden scope creep

Do not claim all EXE/MSI/DLL programs work, do not invent successful output, do not bypass native security, and do not change shared interfaces without coordination.

## Prompt to give AI Agent 5

> You own **Part 5: Windows User-Mode Compatibility**. Read both project documents, audit actual source and test history, and verify the kernel/process/VFS/app-router/desktop contracts before coding. First perform a documented feasibility comparison of reusing/porting Wine or another established runtime versus a narrower implementation, including dependencies, license obligations and required native adapters. Then build the smallest real end-to-end proof: one known harmless x86-64 PE console binary launches, performs a deterministic operation and exits through the chosen backend in a documented guest environment. Implement bounds-checked parsing, correct memory protections, dependency validation and explicit unsupported API behavior. Expand APIs only with tests and a support matrix. GUI must use Part 6's existing desktop ABI. Never load arbitrary Windows kernel drivers or claim universal EXE/MSI/DLL support. Record binary hash, backend, guest configuration, commands, exit code, logs and commit SHA. Use a dedicated branch/PR and report blockers honestly. A PE parser or compile-only result is not execution proof.

---

# Part 6 — Shared Desktop, Graphics and User Experience

## Mission

Preserve and integrate existing desktop work so native and compatible applications share one user experience. This part must start with a source audit, not a desktop rewrite.

## Mandatory preservation rule

Keep existing Desktop Manager/DML, Shared Desktop Integration ABI, graphics/windowing foundation, keyboard/mouse/clipboard/audio integration and related existing work by default. Inspect actual source, branches, PRs, versions and tests before editing. Names and documents do not prove runtime readiness. Do not delete or replace components simply because they are incomplete.

## Audit scope

Trace boot-to-session startup, display/framebuffer path, compositor/window manager if present, window lifecycle, surfaces/frame presentation, keyboard/pointer event flow, focus, resize, clipboard, audio sessions, notifications, file dialogs, process disconnect cleanup, native application integration and guest test path. Separate host-side models from code that actually runs in the guest.

## Implementation scope

1. Produce an inventory: existing code, consumer, ABI/version, tests, runtime evidence, missing behavior and risk.
2. Reuse one documented, versioned Shared Desktop ABI. If one already exists, extend it compatibly rather than creating a competing one.
3. Define ownership and lifecycle rules for windows, surfaces, input clients, clipboard requests and audio sessions.
4. Enforce client identity, permissions, size/type limits, quotas and cleanup on process exit/disconnect.
5. First prove a native application can use the real desktop path.
6. Then connect a minimal Windows-compatible GUI application through a thin adapter to the same ABI, only if Part 5 has a real GUI path.
7. Test invalid events, disconnected clients, stale handles, oversized clipboard data, permission denial, resize and cleanup.
8. If a required compositor/display/input component is absent, identify the precise gap and implement the smallest missing component consistent with the architecture. Do not disguise a mock as a live compositor.

## Acceptance gates

- Existing desktop work inventoried and preserved.
- One shared ABI and no duplicate compositor/session/window protocol.
- Native app reaches the real desktop path with test evidence.
- For Windows GUI claims: a real guest application creates/destroys a window, presents frames, receives keyboard/pointer events, handles resize and exits cleanly through the same desktop path under QEMU.
- Clipboard/audio permission tests pass if those features are claimed.
- Physical graphics/audio claims require separate named-device tests.

## Forbidden scope creep

Do not replace the desktop architecture without an ADR, migration plan and regression tests. Do not equate framebuffer output with a fully working desktop. Do not claim GUI support from a mock screenshot or host-only unit test.

## Prompt to give AI Agent 6

> You own **Part 6: Shared Desktop, Graphics and User Experience**. Read both project documents. Your first task is a source-backed audit of the existing Desktop Manager/DML, Shared Desktop ABI, graphics/windowing, keyboard/mouse, clipboard, audio and app integration work, including relevant branches/PRs and tests. Preserve existing code by default; do not delete or replace it or create a competing compositor. Identify the exact gap between current code and a real guest desktop path. Extend the existing versioned ABI compatibly, enforce ownership/permissions/limits and cleanup, and test native applications first. Connect a Windows-compatible GUI application only through the same ABI and only when Part 5 supports it. Run actual guest tests where possible and record exact commands, guest configuration, exit codes, logs and commit SHA. Distinguish ABI/model tests, live compositor tests, native GUI tests, Windows GUI tests and physical hardware tests. Use a dedicated branch/PR. Deliver the inventory, gap matrix, code, regression tests, limitations and PR URL.

---

# Part 7 — Cross-Part Integration, Security and Release

## Mission

Be the integration gatekeeper. Verify evidence from Parts 1–6, resolve conflicts through reviewed PRs and report the real release state. Do not assume other agents' claims are true.

## Audit scope

Inspect all relevant branches and PRs, base/head commits, changed files, interface diffs, build and CI results, test logs, artifacts, dependency declarations, ABI versions, security boundaries, documentation and open blockers. Confirm that each result applies to the exact code being integrated.

## Integration sequence

1. Create a component/interface matrix mapping source paths, owners, dependencies, versions and acceptance evidence.
2. Verify branch bases and identify duplicate/conflicting edits before merging. Never overwrite a branch or merge a PR without authorization/review.
3. Establish a reproducible clean-checkout build and capture the exact command/environment.
4. Run unit/static checks and relevant regression tests for kernel, storage, app routing, package transactions, compatibility runtime and desktop.
5. Run staged end-to-end tests when dependencies permit:
   - boot the declared guest image;
   - enumerate only the supported virtual devices;
   - access a disposable storage image;
   - launch a native application;
   - exercise the real shared desktop path;
   - launch the known PE console binary through the real Windows backend;
   - exercise compatible GUI only if all required components exist.
6. Run malformed-input and security tests for parsers, package paths, permissions, process boundaries, resource cleanup and rollback.
7. Improve CI only using commands the repository can actually execute. A workflow file existing does not prove a workflow passed.
8. Generate release notes and a readiness matrix. State blockers explicitly; do not paper over failed tests.

## Release acceptance

- Clean reproducible build from an exact commit.
- Boot path tested in a declared environment.
- Native execution and storage tests pass where claimed.
- Windows console compatibility is marked supported only if the real vertical slice passed.
- Desktop readiness distinguishes ABI, live compositor, native app integration, Windows GUI integration and physical-device support.
- Every claimed driver/device has exact supported hardware/backend and evidence.
- Security-sensitive parser/package/process boundaries have negative tests.
- Test logs and artifact provenance are linked to the exact commit.
- Known gaps, deferred work, recovery/rollback limitations and compatibility scope are published.

## Prompt to give AI Agent 7

> You own **Part 7: Cross-Part Integration, Security and Release**. Read the architecture and seven-part plan, then independently inspect the actual PRs, branches, diffs, commit SHAs, build/test evidence and blockers for Parts 1–6. Do not trust status labels without evidence. Build a source-backed dependency/interface matrix, detect ABI drift and conflicts, and propose minimal fixes through separate PRs. Never merge automatically, overwrite another agent's branch or push to the default branch. Run a clean-checkout build, relevant regression/security tests and QEMU end-to-end paths only when the environment is available. Test boot, storage, native launch, real desktop integration and the minimal Windows PE console proof as separate gates. Distinguish host tests from guest tests and QEMU from physical hardware. Record exact commands, environment, exit codes, logs, artifact hashes and tested commit SHA. Publish a release-readiness report with COMPLETE/PARTIAL/BLOCKED/NOT TESTED/NOT IMPLEMENTED/DEFERRED status, known security risks and next actions. Do not declare ProsenjitOS complete while a required critical gate is untested or blocked.

---

## 5. Integration checkpoints and stop conditions

### Checkpoint A — Baseline accepted
Part 1 records the build/boot baseline and real kernel contracts. Other agents can audit in parallel, but they must not invent interfaces.

### Checkpoint B — Kernel contracts stable enough
Parts 2 and 3 verify their actual kernel dependencies. If a required API is absent, open a focused interface proposal or blocker issue rather than implementing a private incompatible substitute.

### Checkpoint C — Native vertical slice
Parts 1, 3 and 4 demonstrate boot + disposable storage + a real native application path. Do not treat this as proof of Windows compatibility.

### Checkpoint D — Windows console vertical slice
Part 5 demonstrates one real PE console binary using a documented backend. If blocked, report exact missing interfaces/dependencies and continue only independent tasks.

### Checkpoint E — Desktop integration
Part 6 proves the native desktop path first. Windows GUI support is a separate gate and cannot be inferred from console compatibility.

### Checkpoint F — Release candidate
Part 7 verifies exact commits, tests and artifact provenance. Failed or untested critical gates block a full release claim.

### Stop and investigate immediately if

- a change causes a previously passing boot/build/test to fail;
- an ABI or on-disk format is changed without consumer/migration analysis;
- a test needs security checks disabled to pass;
- an agent's change deletes or duplicates existing desktop/filesystem work without justification;
- evidence belongs to a different commit than the PR being reviewed;
- an agent reports hardware or Windows compatibility without the named test evidence;
- two agents edit the same files or public interface without coordination.

## 6. Recommended documentation tree

Use existing files when they already serve the purpose. Avoid duplicate sources of truth.

- `docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md` — architecture boundaries and acceptance model.
- `docs/SEVEN-PART-AI-IMPLEMENTATION-PLAN.md` — workstream ownership and detailed prompts.
- `docs/architecture/decisions/` — individual ADRs for major choices, with status and consequences.
- `docs/status/implementation-matrix.md` — current status, evidence commit and blockers.
- `docs/status/compatibility-matrix.md` — tested APIs/apps/runtimes/devices and limitations.
- `docs/testing/verification-guide.md` — reproducible commands, guest setup and evidence requirements.

Create new files only after checking for an existing equivalent. Keep historical decisions and supersede them with new ADRs rather than silently rewriting the history.

## 7. Status matrix template

| Component | Source path(s) | State | Interface/version | Tests run | Exact commit | Evidence | Blockers/risks |
|---|---|---|---|---|---|---|---|
| Boot/kernel | Fill from audit | NOT TESTED until proven | Record actual ABI | Exact commands | SHA | CI/QEMU logs | List |
| Native drivers | Fill from audit | NOT TESTED until proven | Record driver API | Device-specific | SHA | Guest/device logs | List |
| ProsenjitFS/VFS | Fill from audit | NOT TESTED until proven | Format/API version | Image tests | SHA | Test artifacts | List |
| Native loader/router | Fill from audit | NOT TESTED until proven | Executable ABI | Positive/negative | SHA | Launch logs | List |
| Windows backend | Fill from audit | NOT TESTED until proven | Backend/API level | PE guest proof | SHA | Binary hash/output | List |
| Shared desktop | Fill from audit | NOT TESTED until proven | Desktop ABI version | Native/GUI guest tests | SHA | Guest logs | List |
| Release integration | Fill from audit | NOT TESTED until proven | Combined image/version | Clean build and regressions | SHA | CI/artifacts | List |

This is a template, not a claim that any row has been verified.

## 8. Final instruction to all seven agents

Deliver the smallest useful real increment, not the largest speculative rewrite. The goal is an independent OS that can be built, booted, tested and extended without losing prior work. Architecture documents guide implementation; they do not replace it. Keep the native kernel independent, preserve the existing desktop and filesystem, define stable contracts, prove each vertical slice with exact evidence and be explicit about what still does not work.
