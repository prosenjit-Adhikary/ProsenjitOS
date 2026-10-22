# ProsenjitOS Compatibility-First Implementation Model

Status: proposed engineering plan; implementation and acceptance must be tracked separately.
Scope: independent native x86-64 ProsenjitOS, Windows user-mode compatibility, unified desktop, and staged driver support.
Last updated: 2026-10-22.

## 1. Mission

Build an independent, bootable x86-64 OS with its own boot path, kernel, security model, native APIs, VFS and ProsenjitFS, while allowing the broadest practical range of existing Windows applications to run through a compatibility environment. Native and compatible applications should appear in one ProsenjitOS desktop session.

Core principle: execute compatible machine code directly on the CPU; implement or translate the operating-system environment the program expects; keep the native kernel as the final authority over resources and security.

## 2. Non-negotiable boundaries

- ProsenjitOS remains an independent OS, not a Linux distribution or a Linux-kernel wrapper.
- Compatibility runtimes, desktop, package manager and Windows API implementations remain in user space.
- The kernel validates user pointers, handles, capabilities, buffer sizes, permissions and device operations.
- Reuse open-source compatibility components where technically feasible and license-compliant; do not assume a component such as Wine is a drop-in binary for a new kernel. Prove its dependencies and porting requirements first.
- Do not claim that every Windows application or driver will work.
- File-format recognition is not proof of successful install, execution or device support.
- Arbitrary Windows kernel drivers must never be loaded directly into the native kernel.
- Reuse the existing versioned Shared Desktop ABI instead of inventing a competing desktop protocol.
- A test result is valid only for the exact commit and test actually executed.

## 3. Architecture

    Applications and packages
      |-- Native ProsenjitOS applications
      |-- Windows user-mode applications (PE/EXE/DLL; supported Win32/runtime APIs)
      |-- Future POSIX/Linux applications (ELF/POSIX, separately scoped)
      |
    Package transaction service and execution router
      |-- Native execution backend
      |-- Windows user-mode compatibility backend
      |     PE loader -> imports/relocations -> Windows API/runtime
      |     -> process/thread/file/registry/network abstractions
      |-- Future POSIX/Linux compatibility backend
      |
    Shared user-space OS services and Shared Desktop Integration Layer
      |-- windows, input, clipboard, audio, notifications, file dialogs
      |-- ownership checks, capability checks, resource limits
      |
    Stable public user/kernel ABI + capability-checked IPC/syscalls
      |
    Native ProsenjitOS kernel
      |-- processes/threads, address spaces, memory and security
      |-- I/O, VFS, ProsenjitFS, native network and device interfaces
      |
    Native drivers / hardware abstraction / controller implementations
      |
    x86-64 CPU and hardware

The CPU executes instructions that match its instruction set and execution mode. It does not provide Windows APIs, registry behavior, Windows services or Windows kernel semantics. Same-ISA code can execute directly, while the compatibility environment implements the required OS behavior. Other CPU architectures require emulation or binary translation.

## 4. Shared Compatibility Layer components

### 4.1 Execution router
- Route native formats to the native backend and PE files to the Windows backend only when it is available.
- Never infer trust from extension or format detection alone.
- Reject malformed metadata, unsupported architecture/ABI, missing runtime capabilities and invalid dependencies with explicit errors.
- Keep per-application compatibility data separated; record backend choice, launch errors and exit status.

### 4.2 PE loader and process startup
Implement in stages: bounds-checked PE32+ parsing for x86-64; section mapping with correct permissions; relocations; imports/exports; DLL dependency resolution; safe DLL search rules; process parameters; TLS; exception/unwind metadata; runtime initialization. Use checked arithmetic and reject malformed images. Writable-executable memory should be denied by default unless a documented runtime requirement is handled safely.

A header parser or format detector is not a loader. Acceptance requires a real test binary to start, execute, exit and produce the expected result in QEMU, plus negative malformed-file tests.

### 4.3 Windows API/runtime compatibility
Prioritize a small, tested API surface:
1. Base runtime and process startup.
2. Memory, synchronization, threads and timing.
3. File/path operations, handles and error mapping.
4. Command line, environment, locale and console.
5. Registry/configuration persistence.
6. Basic Win32 GUI mapped to the Shared Desktop Integration Layer.
7. Networking and common system libraries.
8. Installer/runtime prerequisites and selected services.
9. Advanced graphics, DRM, anti-cheat, specialized runtimes only as separate scoped projects.

Maintain an API inventory recording behavior, native adapter, error semantics, security implications, tests and known limitations. Translating function names alone is not enough; observable behavior must be compatible.

### 4.4 Existing technology feasibility spike
Evaluate Wine or another established user-mode compatibility project before writing a large API surface. Document required POSIX/Linux interfaces and libraries, what ProsenjitOS already provides, what needs a native adapter or port, build dependencies, license notices and test results. Do not silently add a Linux-kernel dependency or weaken the independent-OS requirement. If porting is infeasible, document the evidence and evaluate another implementation route.

### 4.5 Native service adapters
Create explicit adapters for VFS/file access, process/thread lifecycle, synchronization, network sockets, time/environment/error semantics, registry storage, graphics/window/input/clipboard/audio, notifications and file dialogs. All resource access remains mediated by native security and capability policy.

## 5. Windows driver strategy — separate and safety-first

A Windows kernel driver is machine code plus dependencies on Windows kernel contracts. CPU compatibility is not sufficient. Drivers may depend on IRPs, kernel objects, interrupts, DMA, memory semantics, PnP/power management, synchronization, signing and undocumented behavior. A proxy that imitates function names cannot guarantee correct behavior.

Support tiers:
- D0 — Detection only: parse INF/package metadata, architecture, hardware IDs, dependencies and signature status. This is inventory, not support.
- D1 — Native driver matching: match hardware IDs to a signed ProsenjitOS driver.
- D2 — Standard class/protocol support: native USB HID, USB mass storage, standard network/storage/audio/display protocols where hardware and controller support permit.
- D3 — Constrained user-mode device/service compatibility only where the device interface safely permits it.
- D4 — Research selected Windows kernel-driver compatibility only after a separate isolation and feasibility review. Define the required kernel ABI subset, IRP/object semantics, interrupt/DMA ownership, memory safety, PnP/power behavior, signing, fault containment and recovery. Virtualization or a separate compatibility environment may be necessary. This is not an initial release promise.
- Unsupported path: explain why the driver is unsupported and prefer a native driver or standard protocol when feasible.

Never select a driver by filename alone. Match hardware IDs, architecture, dependencies, signature/trust policy, version and device model. Installation stages files, verifies them, activates under policy, checks health and rolls back on failure. Package installation and driver binding/activation are separate operations.

## 6. Application installation transaction

1. Acquire a package from a declared source.
2. Validate structure, architecture and bounds.
3. Verify signature/provenance where available; record hash and trust evidence. Unknown does not automatically mean malicious.
4. Inspect requested privileges, services, drivers, persistence and network access.
5. Resolve dependencies without granting broad privilege.
6. Stage files in an isolated per-application area.
7. Create/update the compatibility profile and registry record.
8. Commit atomically only after storage succeeds; otherwise clean up or roll back.
9. Launch under least privilege and an explicit capability set.
10. Verify startup/health, report actionable errors and record the outcome.
11. Support uninstall and profile cleanup without deleting unrelated user data.

An MSI being detected as an OLE compound container does not prove it is a valid MSI. Format validation and executing installer actions are separate milestones.

## 7. Shared Desktop Integration — preserve and extend existing work

**Decision: keep the Desktop Environment work already in progress. Do not delete it or replace it with a second desktop architecture.** The desktop is a required part of the target OS and becomes the common user experience for native and compatible applications.

The existing Desktop Manager/DML, Shared Desktop Integration ABI, graphics/windowing foundation, and keyboard/mouse/clipboard/audio integration work are retained. Phase 0 must inspect their actual source, current branches/PRs, ABI versions, tests and integration state before changing them. Names or documentation alone do not establish that a component is implemented or complete.

Both native and Windows-compatible applications must use the existing versioned Shared Desktop ABI. The Windows compatibility backend adapts Windows GUI/input/clipboard/audio expectations to this shared contract; it must not create a competing compositor, window protocol, or parallel desktop session.

Minimum contracts: window create/destroy and ownership; focus; keyboard/pointer events and keymap; bounded frame submission/presentation; clipboard with explicit permissions and size/type limits; audio sessions with owner identity and lifecycle cleanup; notifications/file dialogs; process-exit/disconnect cleanup; quotas and explicit unsupported-feature errors.

The compositor/window manager and desktop policy stay in user space. Host-side model tests do not prove a live compositor, guest input delivery, physical graphics or audio support.

### Desktop integration acceptance gate

1. Audit and preserve existing desktop code; identify reusable components and real gaps from source and tests.
2. Define or confirm one versioned Shared Desktop ABI and document compatibility adapters against it.
3. Keep the desktop independently testable with native applications before introducing Windows GUI support.
4. Add one minimal compatible GUI application and prove window creation, frame presentation, keyboard/pointer delivery, resize and clean exit through the actual ProsenjitOS desktop path under QEMU.
5. Test clipboard/audio permissions and invalid/disconnected client handling; do not report physical GPU/audio support unless tested on the relevant hardware.
6. Extend existing components in place where practical. Introduce a replacement only after a documented technical reason, migration plan and regression coverage.

Desktop milestones must be tracked separately from Windows API/runtime milestones: a desktop ABI or host-side model can be complete while the live compositor or end-to-end guest integration is still incomplete.

## 8. Phased implementation and acceptance gates

### Phase 0 — Baseline and source audit
Pin the base commit; inventory current code, docs, ABIs, tests, PRs and runner availability. Reconcile the existing application handler, Desktop Manager/DML, Shared Desktop ABI, graphics/windowing foundation, keyboard/mouse/clipboard/audio work, kernel syscall/IPC, security, VFS and package transaction code. **Preserve existing desktop work by default**; identify its actual state from source and tests and find duplicates/conflicts before adding interfaces. Do not delete or replace the desktop foundation merely because the Windows compatibility architecture is being updated.

Acceptance: a source-backed gap matrix labels each item missing, partial, implemented-unverified, verified, blocked or deferred. Current-head CI state is recorded. The matrix explicitly separates (a) desktop ABI/model readiness, (b) live desktop/compositor readiness, (c) native application integration, and (d) Windows-compatible GUI integration.

### Phase 1 — Shared contracts and test harness
Freeze versioned backend ABI and errors; define compatibility profiles and capability requirements; add positive/negative host tests, malformed-input tests and reproducible commands.

Acceptance: ABI tests pass and invalid inputs fail closed.

### Phase 2 — Minimal end-to-end PE console program
Implement a bounded PE32+ startup path or a proven compatibility-runtime spike. Start a small known x86-64 PE console program with minimal dependencies; support exit status and deterministic output.

Acceptance: expected output/exit status observed in QEMU, malformed PE cases rejected, and process isolation/memory permissions checked. Format detection alone does not pass.

### Phase 3 — Minimal Windows user-mode runtime
Add tested process, memory, thread, synchronization, file and error semantics. Run a small independent test suite and DLL dependency tests.

Acceptance: selected baseline tests pass on the exact commit, including negative and sandbox tests. Do not claim universal compatibility.

### Phase 4 — Package and filesystem integration
Map Windows paths/handles to VFS under a defined policy; add per-app prefixes/configuration, registry store, dependency checks, install/uninstall rollback and trust metadata.

Acceptance: install-launch-exit-uninstall succeeds on a disposable test image; interrupted installation recovers cleanly; cross-app access is denied without permission.

### Phase 5 — Shared desktop integration
Connect a simple compatible GUI application to the existing shared desktop service. Test window ownership, input, resize/presentation, clipboard, audio ownership and cleanup.

Acceptance: an actual guest application opens and closes a window through the ProsenjitOS desktop path under QEMU; event routing and permission-negative tests pass. Physical hardware is a separate gate.

### Phase 6 — Compatibility breadth
Expand APIs according to demand and evidence. Treat 32-bit support, .NET, installers, advanced graphics and specialized runtimes as separately scoped targets. Publish a compatibility matrix.

Acceptance: each advertised application/runtime has reproducible versioned evidence and known limitations.

### Phase 7 — Driver and hardware compatibility
Prioritize native drivers and standard device classes. Add driver-package inventory/matching and rollback. Research selected Windows driver compatibility only after isolation, memory, interrupts, DMA, PnP and recovery prerequisites are verified.

Acceptance: each supported device has a hardware ID, driver/backend, test evidence and rollback path. Never advertise generic Windows .sys support.

### Phase 8 — Release hardening
Fuzz PE/INF/package parsers; test integer overflow, malformed imports, dependency loops, sandbox boundaries, crash injection, install rollback, update/recovery and release artifact provenance.

Acceptance: required release gates are green on the exact release candidate and unresolved gaps are published.

## 9. Required tracking artifacts

Create or update existing documents rather than duplicating them:
- Compatibility architecture and security boundaries.
- Compatibility matrix for APIs, applications, runtimes, drivers and devices with evidence.
- Roadmap with phases, dependencies, blockers and acceptance gates.
- Driver compatibility policy and limitations.
- Testing/release gate commands, CI links, artifacts and result criteria.

Every tracked item must include status, source paths, owning interface, prerequisites/dependents, tests, exact evidence commit, known limitations and security implications.

## 10. Verification policy

- A CI result applies only to the exact commit tested.
- Cancelled, queued, unavailable or runner-blocked jobs are not passes.
- QEMU proves only the behaviors observed in that run, not physical hardware compatibility.
- Host tests prove only the host-side code they execute.
- Static checks and format detection are not execution tests.
- If a runner is unavailable, record the blocker; do not invent results.
- Keep the known-good baseline intact; isolate changes in small branches/PRs. Do not start dependent phases before prerequisites are accepted.

## 11. First actionable milestone

The first milestone is not “run all Windows applications and drivers.” It is:
1. Audit/reconcile existing application-handler and shared-desktop work against this model.
2. Prove one minimal x86-64 Windows console PE executable launches and exits through the chosen backend.
3. Prove the process remains constrained by native security policy.
4. Record exact test evidence and remaining gaps.
5. Only then expand API/runtime coverage and desktop integration.

This small vertical slice tests the central architectural assumption before large amounts of code are written.

## 12. Architectural decisions

- Independent native kernel: required.
- Shared compatibility layer: required, primarily user space.
- Direct execution for same-ISA machine code: preferred.
- Windows API/runtime/environment compatibility: required for Windows applications.
- Unified native desktop integration: required.
- Existing open-source compatibility technology: evaluate and reuse where feasible and license-compliant.
- Arbitrary Windows kernel drivers loaded directly into the native kernel: prohibited.
- Native drivers and standard device-class support: primary hardware strategy.
- Selected isolated Windows-driver compatibility: research-only until explicit safety prerequisites pass.
- Universal compatibility: aspiration, never an unverified release claim.


## 13. Detailed seven-part AI execution plan

For agent ownership, copy-ready prompts, dependencies, per-part implementation scope, required tests, stop conditions and the final integration checklist, see [Seven-Part AI Implementation Plan](SEVEN-PART-AI-IMPLEMENTATION-PLAN.md).

The seven workstreams are:

1. Kernel, boot and core interfaces.
2. Native hardware discovery, drivers and I/O.
3. Native filesystem, VFS and storage.
4. Native executables, execution router and package transactions.
5. Windows user-mode compatibility.
6. Shared desktop, graphics and user experience — preserve existing work.
7. Cross-part integration, security and release.

All agents must use dedicated branches and PRs, inspect actual source before editing, coordinate interface changes, preserve existing desktop/filesystem work, and provide test evidence tied to the exact commit. The seven-part plan is an execution guide, not evidence that the listed features already exist or are complete.
