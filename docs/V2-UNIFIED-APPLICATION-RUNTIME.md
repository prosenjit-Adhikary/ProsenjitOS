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
| 4 | Complete the scoped Linux/POSIX milestone first; keep Linux and Windows guest ABIs/runtime paths separate | Do not skip this prerequisite or begin the Windows PE runtime early |
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


## 15. Reference-derived Windows symbol-resolution and import-audit plan

**Status: planned engineering guidance only.** This section records techniques worth studying from the open-source AnyPS5 project. It does not add a master phase, change phase order or dates, authorize early Windows-runtime implementation, or claim any Windows compatibility feature is implemented.

**Reference:** [AnyPS5 source repository](https://github.com/boykopovar/AnyPS5). Study its relinker pipeline, NID-based import/export matching, module construction, patching boundaries, and import-audit approach as architectural/behavioral references. Before relying on any implementation detail, record the exact upstream commit inspected and verify its current source and license. Do not copy source, generated databases, proprietary material, or binaries. Any proposed code reuse requires separate provenance, license, security, and architecture review under AGENTS.md.

### 15.1 Transfer the technique, not the PS5 identifier format

AnyPS5's NID mechanism is specific to the PS5 module ecosystem. Windows PE imports must not be treated as NIDs or matched by function name alone. ProsenjitOS will use a Windows-aware symbol identity and resolution contract:

- importing module identity (DLL/API-set contract, architecture, and applicable version/context);
- imported symbol kind: name or ordinal, and whether the import is a function or data export;
- export forwarding/API-set redirection chain, when present;
- requesting process identity, package/app-private dependency scope, and safe search policy;
- selected compatibility module and export implementation;
- ABI/signature contract and supported semantic/version range.

Resolution must be deterministic, per-process, architecture-aware, sandbox-aware, and resistant to DLL search-order hijacking. Name matching is case-insensitive where Windows loader rules require it, but module identity and API-set contract resolution must not be collapsed into a global name-only lookup.

### 15.2 Planned components

1. **PE Import/Export Inventory and Audit Tool (offline):** inspect normal and delay-load imports, import-by-name and import-by-ordinal entries, export tables, forwarded exports, API-set references where discoverable, architecture, and dependency graph. Bound all parsing and report malformed or ambiguous structures without executing the image.
2. **Compatibility Symbol Registry (versioned data):** record module/contract identity, architecture, export name/ordinal, symbol kind, API contract/signature reference, ABI/calling convention, implementation target, dependencies, supported version range, status, and test evidence. Keep guest-visible Windows identities separate from native ProsenjitOS symbols.
3. **Runtime Symbol Resolver:** resolve imports and dynamic lookups through the same policy; support forwarded exports, API-set contracts, delay-load resolution, app-private dependencies, reference-counted module lifetime, and deterministic missing/ambiguous-symbol errors.
4. **Compatibility Module/Export Adapters:** expose ProsenjitOS-owned Windows-compatible modules and route each export to a verified implementation. Keep API semantics in user space wherever practical; kernel entry points remain narrow native primitives.
5. **Import Audit and Coverage Reporter:** compare a target application's discovered requirements with the registry and implementation/test evidence. Report implemented-and-tested, partial, stub, missing, unsupported-by-policy, or unverified; never count a stub or success-only placeholder as implemented.
6. **Regression Corpus:** maintain small redistributable or original test PE files covering name/ordinal imports, forwarded exports, delay imports, missing DLL/export, duplicate names in different DLLs, architecture mismatch, malformed tables, cycles, unsafe search paths, and unload/reload lifetime.

### 15.3 Registry entry contract

Each registry record must identify, at minimum:

| Field | Required meaning |
|---|---|
| Guest module identity | DLL name or API-set contract, plus architecture/context |
| Export identity | Name or ordinal; function versus data export |
| Guest ABI contract | Calling convention, parameter/return layout, structure versions and ownership rules |
| Semantic contract | Observable Windows behavior, errors/status, access checks and concurrency rules |
| Native target | ProsenjitOS-owned adapter/service implementation, if any |
| Dependencies | Required modules, API contracts and version constraints |
| Support state | Planned, implemented, tested, partial, stub, missing or policy-rejected |
| Evidence | Test IDs, supported versions and exact source/CI evidence |

The registry is not an assertion that every Windows export is known. Unknown exports remain unknown and must fail deterministically with diagnostics.

### 15.4 Resolution and loading rules

- Validate PE headers, architecture, image bounds, import/export directory bounds, string termination, ordinal ranges, relocation state, and dependency graph before binding.
- Resolve name and ordinal imports against the correct guest module identity; do not substitute a same-named export from an unrelated DLL.
- Resolve forwarded exports and API-set contracts with cycle/depth limits and explicit version/context policy.
- Keep system compatibility modules, application-private DLLs, and third-party dependencies distinct. Validate provenance, architecture, version, safe search directories, and dependency ownership.
- Support delay-load and runtime LoadLibrary/GetProcAddress behavior only to the extent specified and tested by the applicable phase contract.
- Preserve module reference counts, initialization order, TLS/module callbacks, unload behavior, and rollback on partial load failure.
- Never patch arbitrary application code or bypass native capabilities, filesystem authorization, process isolation, or security policy.
- Map errors/status values according to the Windows contract. Missing, malformed, ambiguous, unsupported, or denied operations must not return false success.

### 15.5 Phase mapping — no phase-plan change

This work is subordinate to the authoritative Phase 0–10 sequence in docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md:

| Existing master phase | Permitted work from this plan |
|---|---|
| Phase 0 | Read-only AnyPS5/source research; record upstream commit/license; inventory existing ProsenjitOS PE/DLL code; define registry schema, threat model, test corpus and gap report. No runtime implementation ahead of dependencies. |
| Phases 1–4 | Do not start Windows runtime implementation. Preserve and complete the existing native foundation, object/handle, VFS/IPC, and Linux/POSIX dependency gates in their authoritative order. |
| Phase 5 | Implement and test PE import/export parsing, the Windows-aware registry, static import audit, DLL/API-set resolution, forwarders, delay-load and loader integration only as allowed by the Phase 5 detailed specification and its prerequisites. |
| Phase 6 | Add real Win32/NT export implementations and ABI/semantic adapters incrementally; update registry status only when behavior and tests pass. Include error/status, security, object/handle, synchronization, async I/O and SEH contracts as applicable. |
| Phase 7 | Extend the same evidence-backed registry and audit process to graphics, window/message, audio and networking APIs after their dependencies are stable. |
| Phase 8 | Use dependency/import reports to inform installer and runtime dependency admission; installer execution remains subject to the existing Phase 8 sandbox and transaction gates. |
| Phases 9–10 | No change. Driver compatibility remains a separate, later, isolated device-only track. |

Read-only research, source inventory, documentation, and test-fixture design may be prepared without moving a runtime milestone earlier. Any implementation must begin only at the earliest unmet authoritative dependency gate.

### 15.6 Acceptance criteria

The symbol-resolution work is accepted incrementally only when tests demonstrate:

- correct name and ordinal binding with DLL identity preserved;
- correct forwarded-export and API-set resolution with bounded cycle handling;
- correct delay-load and dynamic lookup behavior for the supported subset;
- architecture/version/context mismatch rejection;
- deterministic missing/ambiguous import diagnostics;
- safe DLL search and app-private dependency isolation;
- reference counting, initialization, TLS, unload and partial-load rollback correctness;
- malformed-input, overflow, permission-denial, resource-exhaustion and teardown coverage;
- no regression to native ELF launch, native services, VFS/ProsenjitFS, security boundaries or existing merged functionality;
- exact-head required review and CI evidence before merge or dependent work.

**Completion is evidence-based:** an export is supported only when its actual ABI and observable behavior are implemented and tested. Registry entries, parser output, stubs, or a successful launch of one application do not establish broad Windows compatibility.
