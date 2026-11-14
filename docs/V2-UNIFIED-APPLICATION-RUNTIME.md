# ProsenjitOS Version 2 — Unified Application Runtime Architecture

**Status:** Proposed Version 2 architecture  
**Scope:** Design and roadmap only; this document does not claim implementation is complete.

## 1. Vision

ProsenjitOS remains the primary operating system and owns the desktop, kernel, native applications, security policy, and hardware interfaces. Version 2 aims to let supported Windows applications install and run from the same ProsenjitOS desktop without booting a complete Windows installation or requiring a full Windows desktop session.

The intended user experience is one desktop, one application launcher, one taskbar, and one application-management workflow for both native ProsenjitOS applications and supported Windows applications.

**Compatibility implementation entry point:** [docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md) is the sole master specification for architecture boundaries, dependency order, milestones and acceptance gates. Read it before starting any compatibility task. This document is subordinate and owns Windows runtime detail only; it must not define a competing implementation sequence.

## 2. Non-goals and constraints

- Do not require a complete Windows OS installation or a Windows VM as the normal application execution path.
- Do not use Wine as a runtime dependency.
- Do not create a second desktop or expose a separate Windows shell to the user.
- Do not load Windows `.sys` kernel drivers directly as ProsenjitOS kernel drivers.
- Do not claim universal Windows application or driver compatibility. Support is incremental and must be demonstrated by tests.
- Do not weaken kernel isolation or grant applications unrestricted access to host resources.
- Do not copy proprietary Windows implementation code. Any third-party code reuse must follow its license and undergo review.

**Technical reality:** running Windows binaries without a Windows kernel still requires an independently implemented set of Windows executable-loading, ABI, API, runtime, and behavior contracts. Avoiding Wine does not remove this engineering requirement; it means ProsenjitOS owns or explicitly licenses the implementation rather than depending on Wine.

## 3. High-level architecture

```text
                  ProsenjitOS Desktop
          launcher / taskbar / window manager
                           |
              Unified Application Manager
                  /                    \
        Native Application        Windows Application
        Native ABI/runtime        Windows App Runtime
                  \                    /
                Shared user-space services
       VFS / IPC / graphics / audio / networking
                           |
                 ProsenjitOS kernel
      process / virtual memory / syscalls / security
                           |
                       Hardware
```

The Windows App Runtime is a user-space subsystem wherever feasible. Kernel changes must be limited to stable primitives genuinely required by the runtime. It must not become an unreviewed path around capabilities, process isolation, memory protections, or device permissions.

## 4. Main components

### 4.1 Unified Application Manager

- Detect package and executable types using bounded parsing; never infer trust from a filename extension.
- Maintain package metadata, install locations, dependencies, launch commands, and uninstall records.
- Register supported applications with the common launcher and file-association system.
- Keep native and Windows application metadata distinguishable while presenting a consistent user experience.
- Verify package provenance/signatures where available and apply explicit trust policy.
- Provide transactional installation and rollback where feasible.

### 4.2 Windows App Runtime

Build as a modular user-space runtime, not as one monolithic kernel feature. Candidate modules include:

1. PE32/PE32+ parser and loader, with strict bounds, overflow, architecture, section, and permission validation.
2. Import resolution and DLL loading.
3. Process startup, thread and synchronization behavior required by supported programs.
4. A prioritized subset of Win32/NT user-mode API contracts, implemented and tested incrementally.
5. Windows-style file/path, metadata, sharing, and error semantics where applications require them.
6. Runtime/dependency discovery for supported frameworks.
7. Compatibility reporting that clearly identifies unsupported APIs and application requirements.

The initial target should be a small, controlled Windows console program. GUI applications follow only after loader, process, memory, API, and security tests are stable.

### 4.3 Shared desktop and graphics bridge

Windows applications should appear as normal ProsenjitOS application windows. The runtime must connect to the selected userspace display/window system instead of introducing a separate desktop.

The GUI bridge may require a Windows-compatible window/message model, drawing API subset, input translation, clipboard support, audio routing, and frame presentation. Choose the rendering strategy after the current display stack is established; off-screen rendering is a possible technique, not an assumed universal solution. Some applications may remain unsupported, especially those requiring unavailable graphics APIs, anti-cheat components, protected media paths, or unusual window-manager behavior.

### 4.4 Shared OS services

Where semantics can be safely shared, integrate with ProsenjitOS services through explicit interfaces:

- VFS and controlled filesystem namespace mapping
- IPC and process lifecycle
- graphics, input, clipboard, and audio
- networking through the OS network/security policy
- package metadata and dependency storage
- security event logging and user-visible permission prompts

Windows applications must not gain unrestricted access to native files, credentials, devices, or privileged syscalls merely because they run through the compatibility runtime.

### Shared compatibility resource contracts

Linux and Windows runtimes must use shared native services through separate semantic adapters. The master compatibility plan defines the complete interfaces, dependency order and acceptance gates. The following requirements are mandatory:

- **Object/handle model:** retain the native typed object/capability manager as authority. Windows HANDLE tables and Linux FD tables are separate per-process adapters; do not equate guest numeric values with native handles or file descriptors. Preserve object type, access rights, duplication, inheritance, close, wait and teardown semantics.
- **VFS namespaces:** use ProsenjitOS VFS/ProsenjitFS as storage authority. Windows drive-letter/UTF-16 path namespaces and Linux POSIX mount/path namespaces remain distinct per-process views. Enforce native path authorization and sandbox policy after resolution; string conversion alone is not security.
- **IPC/shared memory:** use validated IPC for control and small messages. Use capability-controlled shared-page mappings for measured large-buffer cases, with bounds, ownership, synchronization, revocation and process-death cleanup. Never expose physical or kernel addresses to applications.
- **Process/thread adapters:** explicitly map guest process, thread, TLS, synchronization, priority and lifecycle semantics without bypassing the native scheduler or security policy.
- **Exceptions:** the kernel captures CPU traps and safely delivers user context. Linux signals and Windows SEH/unwind behavior require separate compatibility dispatchers; they are not simple one-to-one translations.
- **Unsupported behavior:** return documented failures and report missing API/ABI coverage. Do not use success-returning placeholder stubs as compatibility evidence.

### 4.5 Security and isolation

- Run each application with a defined identity, resource limits, and least-privilege capabilities.
- Keep untrusted PE parsing and complex API behavior out of privileged kernel code where possible.
- Validate all user/kernel and process-boundary buffers and messages.
- Apply filesystem and network policy to Windows applications as well as native applications.
- Make installation and execution admission fail closed when required integrity checks fail.
- Treat runtime crashes as user-space failures; do not let a malformed executable compromise kernel integrity.
- Keep audit logs bounded and avoid logging secrets.

## 5. Driver strategy

Windows user-mode applications and Windows kernel-mode drivers are different compatibility problems. Version 2 should prioritize application execution and native ProsenjitOS drivers.

Windows kernel-mode driver compatibility is a separate, later research track requiring appropriate NT/WDM or other driver-framework contracts, interrupt and DMA handling, device ownership, security isolation, and extensive hardware testing. Imported Windows driver packages must never be treated as native drivers or loaded directly into the ProsenjitOS kernel. If a future compatible driver execution model is attempted, it must have an explicit isolation and rollback design.

## 6. Mapping to the authoritative implementation sequence

This document does not define a separate roadmap. The phase numbers and dependency gates in [COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md) are authoritative. Windows-specific work maps to that sequence as follows:

| Master phase | Windows-runtime scope in this document | Dependency rule |
|---|---|---|
| 0 | Architecture, threat model, source baseline and API inventory | Documentation/design only; no implementation claim |
| 1 | Native process, address-space, scheduler, user-entry and teardown prerequisites | Must pass before any Windows executable launch |
| 2 | Native objects plus the separate Windows HANDLE adapter | Must preserve typed objects, rights, lifetime and process isolation |
| 3 | Windows drive/path namespace, IPC and shared-memory contracts | Must use native VFS/IPC authority; do not bypass security |
| 4 | No Windows runtime implementation; Linux/POSIX milestone remains independently sequenced | Do not skip this milestone or reorder the master plan |
| 5 | PE32/PE32+ image loader, relocations, imports/exports, TLS, DLL/API-set resolver and rollback | Parser-only work is not image loading or application execution |
| 6 | Win32/NT API subset, Registry Engine, file/object/synchronization/async I/O, security and SEH | Implement only tested contracts; deterministic unsupported errors |
| 7 | Graphics, window/message bridge, audio, networking and shared desktop | Requires stable Phase 6 runtime/service contracts |
| 8 | EXE/MSI/package inspection, dependencies, install transactions and rollback | Installer execution requires a supported runtime and sandbox |
| 9–10 | Isolated user-mode device framework, then restricted WDM/KMDF research | Separate driver threat model, framework/version scope and hardware gates |

Pure parser design, API inventory, threat modeling and test-fixture preparation may be done earlier if they do not assume unavailable runtime contracts. Runtime implementation and compatibility claims must obey the master sequence.
## 7. Verification gates

No phase is complete merely because it compiles. Each phase must pass:

- clean repository build and existing CI gates
- existing QEMU boot and kernel self-tests
- targeted unit/synthetic tests for malformed and valid inputs
- resource cleanup and repeated launch/exit tests
- isolation and permission-denial tests
- regression tests proving native ELF applications and existing VFS/ProsenjitFS paths still work
- documented results and limitations

Do not mark Windows compatibility as complete based only on one application launching successfully.

## 8. Architecture decisions to preserve

1. ProsenjitOS remains the native OS and owns the primary desktop.
2. Native and Windows applications share the desktop experience, not necessarily the same ABI.
3. Windows application execution uses a modular user-space runtime; the full Windows OS is not a required runtime dependency.
4. Wine is not a required runtime dependency; any external code reuse must be license-reviewed and documented.
5. Windows API behavior must be independently implemented or supplied by a properly licensed component; it cannot be skipped if real Windows binaries are expected to run.
6. Native drivers remain the preferred hardware path. Windows kernel drivers are not directly loadable by the ProsenjitOS kernel.
7. Compatibility is a tested matrix of specific applications and features, not a universal promise.
8. Stabilize existing kernel, filesystem, security, and userspace foundations before broad compatibility work.

## 9. Current status

Everything in this document is a Version 2 design target unless the implementation and verification evidence explicitly says otherwise. This document adds no runtime code and does not change the current bootable image or claim that Windows applications already run on ProsenjitOS.

## 10. Separate ELF/Linux and PE/Windows execution adapters

**Status: target architecture only.** The existing ELF64 loader and PE metadata parser are not, by themselves, complete Linux or Windows application runtimes.

### Linux/POSIX execution path

- Preserve the native ELF loader and native ABI.
- Add a distinct Linux/POSIX launch profile with guest register/stack/TLS conventions and an explicit process-entry contract.
- Translate supported Linux syscall numbers and argument structures through a dedicated adapter; never forward guest syscall numbers directly to native syscall dispatch.
- Validate every pointer, length, structure version and output buffer before access.
- Preserve Linux errno, signal, process/thread and synchronization semantics for the explicitly supported subset.
- Maintain a versioned Linux syscall/API matrix with real test binaries and deterministic unsupported-call behavior.

### Windows PE execution path

A complete PE loader must progress beyond metadata inspection and implement validated PE32/PE32+ image mapping and per-section page permissions; relocation processing; imports/exports and forwarded exports; dependency graph checks; TLS; process entry setup; architecture/calling convention; teardown; DLL/module resolver with deterministic search order, safe application-private directories and dependency/version checks; selected Win32/NT process, thread, memory, synchronization, console and file APIs; Windows error/status mapping; and real behavior for every claimed export.

A metadata parser or import-name reader does not map an image, resolve symbols, load DLLs or start a process.

### DLL redirection/interception policy

The Windows runtime may redirect supported system DLL imports to ProsenjitOS-owned compatibility modules or API-set implementations. Resolution must be per-process and sandbox-aware, never a global replacement of native OS libraries.

The resolver must validate module architecture, path, provenance, dependency graph and export availability; prevent writable-directory search hijacking; distinguish system modules from app-private dependencies; and maintain correct reference/unload lifetime. Missing modules or exports must return deterministic errors and diagnostics. Interception must not patch arbitrary code or bypass capability checks.

## 11. Common fault events with guest-specific exception semantics

The kernel captures CPU faults and validates the return context. It may publish a sanitized internal fault-event structure containing fault class, process/thread identity, user instruction pointer, fault address/access type, selected user context and recovery classification.

The Windows adapter translates supported events into Windows exception records/context and implements the required SEH dispatch and x64 unwind behavior. The Linux adapter independently implements supported signal dispositions, masks, pending state, alternate stack/context and restart/error behavior.

Do not equate Windows SEH with Linux signals or make them one-to-one aliases. The common event is an internal transport format only. Untrusted handlers cannot receive privileged kernel state or resume from an invalid context. Nested faults, corrupted unwind metadata and repeated fault loops require bounded termination behavior.

## 12. User-space subsystem and driver-host isolation

The long-term microkernel-oriented design moves complex compatibility runtimes, VFS/network/graphics services and supported device services into separately protected user-space processes where practical. Each service requires a versioned ABI, capability policy, resource budget, health reporting, startup/shutdown contract and bounded restart policy.

Windows .sys and Linux .ko remain kernel-driver binaries and are not directly loaded as native modules. Any future compatibility host must implement a real, version-specific driver framework adapter and broker every hardware operation. Per-driver process isolation, DMA/interrupt revocation, watchdog, crash recovery and device fallback are prerequisites, not optional hardening after compatibility.

## 13. Fallback and degraded-mode behavior

Optional device failures must not hang boot or terminate unrelated services. Use bounded discovery/probe timeouts and a class-specific fallback only when a validated backend exists:
- Display: firmware VBE/GOP or validated framebuffer, then serial/headless recovery.
- Storage: supported ATA PIO or VirtIO block backend, otherwise report unavailable and preserve the recovery path.
- Network: supported VirtIO/generic backend, otherwise keep the OS operational offline.
- Input: supported generic USB HID or serial/firmware recovery console.
- Graphics acceleration: software rendering over framebuffer.

A device without a safe fallback remains unavailable; never claim arbitrary unsupported hardware will work.

## 14. Additional Windows-specific acceptance requirements

The following are supplementary Windows-specific tests, not a second phase order. The master document controls when they may be implemented. Before a compatibility runtime is considered usable, verify the native process/user-mode boundary, address-space teardown, syscall/usercopy, capabilities, IPC, VFS authorization and security policy on the exact candidate commit.

New gates:
1. PMM/VMM mapping, W^X/NX, address-space isolation, rollback and teardown tests.
2. Fast IPC measured against a copy-based baseline, with bounds, revocation, process-death and concurrency tests.
3. ELF/Linux syscall adapter tests independent of the native ELF loader.
4. PE loader tests for mapping, relocations, imports, DLL search order, TLS, entry/exit and malformed input.
5. Win32/NT API behavior tests and a versioned supported-export matrix.
6. Separate Windows SEH and Linux signal fault-injection suites.
7. Driver-host isolation, watchdog, DMA/interrupt cleanup and fallback tests before any driver compatibility claim.

No design addition changes the current implementation status or authorizes marking a milestone complete without exact-head evidence.
