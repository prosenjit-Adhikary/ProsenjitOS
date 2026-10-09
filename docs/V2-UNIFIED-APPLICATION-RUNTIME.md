# ProsenjitOS Version 2 — Unified Application Runtime Architecture

**Status:** Proposed Version 2 architecture  
**Scope:** Design and roadmap only; this document does not claim implementation is complete.

## 1. Vision

ProsenjitOS remains the primary operating system and owns the desktop, kernel, native applications, security policy, and hardware interfaces. Version 2 aims to let supported Windows applications install and run from the same ProsenjitOS desktop without booting a complete Windows installation or requiring a full Windows desktop session.

The intended user experience is one desktop, one application launcher, one taskbar, and one application-management workflow for both native ProsenjitOS applications and supported Windows applications.

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

## 6. Version 2 implementation phases

### V2.0 — Prerequisites and architecture
- Stabilize and verify current kernel, process/thread lifecycle, address spaces, syscall ABI, VFS/ProsenjitFS, security policy, and display/userspace boundaries.
- Record stable interfaces and invariants before adding runtime code.
- Add design and test documents for package trust, runtime boundaries, and supported application profiles.

### V2.1 — Executable and console foundation
- Implement a bounded PE parser/validator with malformed-input tests.
- Define loader and runtime interfaces without weakening ELF/native execution.
- Build a minimal Windows console-program proof of concept.
- Validate process lifecycle, imports, error paths, resource cleanup, and sandbox enforcement.

### V2.2 — API and dependency runtime
- Implement only the APIs required by selected test programs.
- Add DLL loading, dependency/version handling, file semantics, synchronization, and memory API tests.
- Maintain an explicit supported/unsupported API matrix.
- Add repeatable positive, negative, fuzz, and regression tests.

### V2.3 — Desktop integration
- Integrate a selected GUI/API subset with the existing ProsenjitOS display stack.
- Add window lifecycle, input, clipboard, audio, and frame-presentation tests.
- Verify multiple applications, crashes, focus changes, and resource cleanup.

### V2.4 — Installer and application management
- Add safe installer/package inspection and a controlled installation workflow.
- Treat arbitrary installer executables as untrusted code; do not execute them with elevated rights by default.
- Support selected installer formats only after their execution model and rollback behavior are defined.
- Add uninstall, update, dependency, file-association, and integrity tests.

### V2.5 — Broader application coverage
- Expand the API set based on real application requirements and test evidence.
- Track each tested application, version, architecture, known limitation, and result.
- Treat framework-specific, Store/UWP, anti-cheat, DRM, and driver-dependent software as separate compatibility cases.

### Later research — Windows driver support
- Keep driver compatibility separate from the initial Version 2 application milestone.
- Define threat model, isolation, device ownership, signing/trust, failure recovery, and hardware test requirements before implementation.

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
