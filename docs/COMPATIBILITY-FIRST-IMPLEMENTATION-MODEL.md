# ProsenjitOS Compatibility-First Implementation Model

**Status:** Architecture and implementation guide; design only unless a section is explicitly backed by source and exact-head test evidence.  
**Scope:** Native ProsenjitOS foundations, Linux/POSIX application compatibility, Windows application compatibility, shared resource/IPC services, and later driver compatibility.  
**Canonical role:** This is the master entry point for all compatibility work. Detailed Windows application/runtime decisions remain in [V2-UNIFIED-APPLICATION-RUNTIME.md](V2-UNIFIED-APPLICATION-RUNTIME.md); driver contracts remain in [DRIVER-MODEL.md](DRIVER-MODEL.md); filesystem design/status remains exclusively in [FILESYSTEM.md](FILESYSTEM.md).

## 1. Mission and non-negotiable architecture

ProsenjitOS is an independent native operating system. Its own kernel, syscall ABI, process and thread model, memory manager, scheduler, capability security, device model, VFS, and ProsenjitFS remain authoritative.

The compatibility objective is to run a progressively larger, explicitly tested set of Linux/POSIX and Windows applications on the same ProsenjitOS installation and desktop. Compatibility means implementing the required executable formats, ABIs, APIs, runtime behavior, filesystem semantics, process/thread behavior, and service contracts. Merely parsing an executable or providing function-name stubs is not application compatibility.

### Architecture

- Native ProsenjitOS applications use the native ABI and SDK.
- Linux ELF applications use a Linux/POSIX user-space compatibility runtime.
- Windows PE applications use a Windows Win32/NT user-space compatibility runtime.
- These runtimes consume shared user-space services for object/resource mapping, VFS namespaces, IPC/shared memory, process/thread adapters and exception delivery.
- All paths cross the native syscall, user-memory validation, capability and security boundaries.
- The ProsenjitOS kernel, VFS, ProsenjitFS, device model, HAL and native drivers remain authoritative.

Compatibility code belongs in user space wherever practical. Kernel work is limited to stable, generally useful primitives justified by native and compatibility consumers. No compatibility runtime may bypass syscall validation, process isolation, capability checks, user-memory validation, device permission brokering, or filesystem authorization.

### Hard rules

1. Do not replace or fork the ProsenjitOS kernel into a Windows or Linux kernel.
2. Do not require a complete Windows installation or a Windows VM as the normal Windows application path.
3. Do not make Wine a runtime dependency. It may be studied as a behavioral/reference implementation; any code reuse requires a separate license and architecture review.
4. Do not copy proprietary OS source, binaries, branding, or protected implementation details.
5. Do not load Windows .sys or Linux .ko images directly as ordinary ProsenjitOS kernel modules.
6. Do not claim universal Windows/Linux application or driver compatibility. Publish a tested application/API/driver matrix.
7. Do not call an API implemented merely because an export stub returns success. Unsupported behavior must fail deterministically and be reported.
8. Preserve native application behavior and all existing kernel/filesystem/security invariants.
9. Keep implementation, design, build, test, and verified status distinct.
10. Follow AGENTS.md: inspect current code and open PRs, work on a dedicated branch, use one coherent PR at a time, and merge only after exact-head checks pass.

## 2. Required repository reading and audit before implementation

Before starting a compatibility task, every human or coding agent must read, in this order:

1. AGENTS.md
2. README.md
3. docs/ARCHITECTURE.md
4. docs/PROJECT-STRUCTURE.md
5. docs/ROADMAP.md, including the Version 2 and driver phases
6. This document
7. docs/V2-UNIFIED-APPLICATION-RUNTIME.md for Windows application work
8. docs/DRIVER-MODEL.md for device/driver work
9. docs/FILESYSTEM.md for any VFS, path, file, or storage work
10. Relevant syscall, process/thread, memory, IPC, object/capability, exception, VFS, loader, and test source files

Then inspect the current default-branch source, public headers, callers, tests, build rules, workflows, recent commits, open PRs, and issues. Do not rely on an old chat, a roadmap checkbox, an open PR, or an earlier test run as proof of current implementation.

Create a source-backed evidence table before coding:

| Area | Source/API inspected | Existing behavior | Missing contract | Tests/evidence |
|---|---|---|---|---|
| Process/thread/address space | exact paths and symbols | verified facts only | specific gap | exact test/run |
| Object/handle/capabilities | exact paths and symbols | verified facts only | specific gap | exact test/run |
| Syscall/usercopy/IPC | exact paths and symbols | verified facts only | specific gap | exact test/run |
| Exceptions/context switching | exact paths and symbols | verified facts only | specific gap | exact test/run |
| VFS/ProsenjitFS | canonical filesystem document + source | verified facts only | specific gap | exact test/run |
| Driver/device broker | driver model + source | verified facts only | specific gap | exact test/run |
| Build/CI | workflow and exact commit | actual result | blocker | run ID/log |

If a prerequisite is missing or unstable, work on that prerequisite or document the blocker. Do not build a compatibility shim over an assumed interface.

## 3. Six architectural layers

### Layer A — Application and executable formats

- Native ProsenjitOS programs use the native ABI and SDK.
- Linux programs use ELF and a Linux/POSIX compatibility environment.
- Windows programs use PE32/PE32+ and a Windows user-mode compatibility environment.
- Installer/package formats are separate from executable formats. An MSI parser or installer frontend does not imply that arbitrary MSI custom actions can run.
- Architecture, ABI, calling convention, executable permissions, relocation model, TLS, stack setup, and process startup must be explicit.

### Layer B — Compatibility runtimes

**Linux/POSIX path**
- ELF loader integration and Linux process startup semantics required by selected programs.
- POSIX libc-facing behavior and a deliberately scoped Linux syscall translation layer.
- Linux-specific APIs only when required by a named compatibility target.
- Signal, thread, futex/synchronization, socket, proc-style and filesystem semantics as supported and tested.
- Never claim that a POSIX subset is a complete Linux kernel ABI.

**Windows path**
- Bounded PE32/PE32+ loader and Windows process startup.
- Win32 and required NT user-mode API contracts.
- DLL dependency resolution and API implementation.
- Windows thread, synchronization, exception/unwind, file, registry-like, console, GUI and service behavior in staged subsets.
- Keep this path consistent with docs/V2-UNIFIED-APPLICATION-RUNTIME.md.

### Layer C — Shared resource and namespace services

These are common native services with separate compatibility adapters. Do not erase semantic differences between Windows, Linux and ProsenjitOS.

#### C1. Object manager, Windows handles and Linux descriptors

Use one native object/resource lifecycle model where appropriate, but keep **per-process Windows HANDLE tables and Linux file-descriptor tables separate**.

Windows handles are not simply file descriptors. Windows HANDLE values can refer to files, processes, threads, events, mutexes, sections, pipes and other typed objects, with access masks, inheritance, duplication, waitability and object-specific semantics. Linux descriptors are small per-process integers with descriptor flags, close-on-exec behavior, duplication and descriptor-specific semantics. A Windows handle must never be guessed to equal a native FD.

Required design:
- Typed native object references with generation/stale-reference protection and reference-counted lifetime.
- Per-process Windows handle table: opaque handle values, object type, granted access mask, inheritability, duplicate/close semantics, and invalid-handle behavior.
- Per-process Linux FD table: descriptor allocation, close, dup/dup2/dup3 behavior as supported, descriptor flags, close-on-exec, and correct per-process lifetime.
- Adapter functions map each guest-visible value to a validated native object/capability reference.
- Access checks are performed on every operation; possession of a numeric handle/FD is not authority to bypass capability policy.
- Define process exit, thread exit, inheritance, duplication, cancellation, object deletion and teardown behavior.
- Add type-confusion, stale-handle, cross-process, rights-escalation, exhaustion, double-close and lifetime tests.

A placeholder integer such as 0x4C is only an example, not a fixed handle ABI. Do not expose native kernel pointers or raw capabilities as guest handles.

#### C2. Unified VFS and path namespace

Use the native VFS and ProsenjitFS as the storage authority. Provide separate path parsers/resolvers for Windows and Linux semantics, then resolve both into controlled VFS namespace objects.

Windows adapter requirements include drive-letter/device namespace policy, UTF-16 paths, separator normalization, per-application prefixes, case behavior, sharing/locking semantics, file attributes, timestamps, reparse points and Windows error mapping as needed by supported apps.

Linux adapter requirements include POSIX absolute/relative paths, mount namespaces, permissions, links, file descriptor-relative operations and Linux errno behavior as needed by supported apps.

Do not globally rewrite a Windows drive path into a Linux path. A per-process namespace maps a sandboxed Windows C drive to a configured VFS root. Linux root remains a distinct guest namespace. Case sensitivity and Unicode normalization must be explicit; do not silently conflate different filenames.

All operations must pass native VFS authorization, per-process sandbox policy, path traversal checks and mount policy. Path conversion is not a security boundary by itself.

#### C3. Shared IPC and memory transport

Use ordinary validated IPC for control messages and small payloads. Add shared-memory/page-mapping transport for large buffers only when measurement shows a benefit.

Required contracts:
- Region creation, mapping, unmapping, ownership, lifetime and revocation.
- Capability-controlled access to each region and direction-specific read/write permissions.
- Bounds, alignment, overflow, aliasing and page-permission validation.
- Explicit synchronization and producer/consumer ownership protocol.
- Cancellation, process death, partial transfer and teardown handling.
- Cache-coherency rules where relevant; DMA buffers remain subject to the device/DMA API and must not be conflated with ordinary shared memory.
- Copy-based fallback for small transfers, unsupported mappings and security-sensitive paths.

Never pass a physical address as a user pointer. Never expose a kernel pointer. Zero-copy is a measured optimization, not a requirement for every IPC operation.

### Layer D — Process, thread and exception semantic adapters

- Define guest process/thread identity, startup, exit, wait, signal/termination, TLS, thread-local runtime state, stack layout, context switching and resource cleanup.
- Preserve native scheduler and security authority. Guest priority classes or scheduling hints must be bounded mappings, not arbitrary privilege elevation.
- Native CPU exceptions are captured and validated by the kernel. A user-space compatibility runtime may translate a delivered exception into guest ABI semantics.
- Linux signals and Windows exceptions are different models; do not implement them as a one-to-one name substitution.
- Windows x64 SEH requires correct exception records/context, dispatcher behavior, language-specific handler metadata and stack unwinding, including the PE exception directory and unwind information. __try/__except is not supported merely because a page fault can be detected.
- Linux signal delivery requires signal masks, dispositions, pending state, alternate stack/context behavior and restart/error semantics for the supported subset.
- Define fatal/unhandled exception behavior, nested faults, corrupted unwind metadata, cancellation and thread exit.
- Add controlled fault-injection tests for access violations, illegal instructions, arithmetic faults, stack faults, nested exceptions and invalid unwind metadata.

Do not forward arbitrary privileged exception state directly to an untrusted daemon. The kernel owns trap capture, privilege transitions and safe user-context delivery.

### Layer E — Driver and device compatibility

Native ProsenjitOS drivers remain the preferred and trusted hardware path. All hardware access is controlled by the device registry, permission broker, capabilities and HAL.

Keep these tracks separate:

1. **Native drivers:** ProsenjitOS driver ABI and native device model.
2. **User-mode device services/drivers:** isolated processes using brokered device interfaces where the hardware and device class support this safely.
3. **Linux driver compatibility:** a distinct, long-term Linux kernel ABI/driver-framework effort; Linux .ko is not a portable user-space plugin and cannot be assumed loadable.
4. **Windows driver compatibility:** a distinct, long-term NT/WDM/NDIS/framework compatibility effort; Windows .sys is not a native ProsenjitOS module.

The Windows IRP/WDM shim is a future adapter specification, not a drop-in C structure or proof of compatibility. The illustrative IRP in a proposal must not be treated as the actual Windows ABI: real WDM uses versioned/architecture-specific structures and an IO_STACK_LOCATION array, IRP lifecycle and pending/completion rules, object references, cancellation, MDLs, IRQL rules, PnP/power and framework contracts. Never dispatch a driver against an invented shortened structure.

Any future Windows driver adapter must specify, per supported framework/version:
- exact structure layout, calling convention, exported/imported kernel routines and ABI;
- driver load/unload, DriverEntry, device objects, symbolic links and dispatch registration;
- IRP allocation, stack locations, completion, pending, cancellation, synchronous/asynchronous I/O and lifetime;
- pool allocation, object/reference semantics, synchronization and IRQL constraints;
- MDL pin/map/unmap, DMA adapter/IOMMU, buffer direction and lifetime;
- interrupt registration/acknowledgement/deferred work;
- MMIO/port-I/O mediation, device ownership, capability checks and teardown;
- PnP, power, suspend/resume and failure recovery;
- signing/trust, package admission, isolation, rollback and hardware test coverage.

Do not emulate IRQL merely by mapping a number to scheduler priority. It is a constraint on legal operations and interrupt context; the native interrupt/synchronization model must enforce equivalent safety. DMA requires pinned pages, validated mappings, device-specific DMA/IOMMU authority and teardown, not a raw physical address copied into an IPC message.

Start with native drivers and user-mode driver/device services. Kernel-mode compatibility is later research gated by a threat model and real hardware/QEMU tests. No universal driver-support promise.

### Layer F — Desktop, packaging and application management

Supported applications should appear in the existing ProsenjitOS desktop/session and launcher. Do not create a separate Windows desktop or Linux desktop as the default experience.

Installer detection, package trust, dependencies, per-app prefix, permissions, transaction, rollback, update and uninstall are separate services. A valid PE file is not necessarily an installer; MSI custom actions are arbitrary code and must be sandboxed. Follow docs/WINDOWS-APP-INSTALLATION.md.

## 4. Implementation order and dependency gates

This order is the default dependency sequence. The exact scope of each milestone must be confirmed against the current source audit; it is not a claim that any item below is already implemented.

### Phase 0 — Baseline and prerequisite audit

Verify exact current status of:
- process/thread creation, exit, wait/reap, address-space lifecycle and user-mode entry;
- syscall ABI, usercopy, exception entry/return and context switching;
- native object manager, per-process handles, capabilities and revocation;
- IPC, blocking/wakeup, synchronization and process-death cleanup;
- VFS file/directory operations, authorization and ProsenjitFS persistence;
- device registry, permission broker, DMA/interrupt primitives and user/kernel boundary;
- build, QEMU, native boot and security workflows.

**Exit gate:** a source-backed interface map, open-PR conflict check, exact baseline build/test evidence, and documented blockers. Do not start application compatibility implementation while critical process, memory, IPC, VFS or security contracts are unstable.

### Phase 1 — Shared object/resource foundations

1. Audit and stabilize native typed object lifecycle and capability-backed references.
2. Specify and implement the Windows handle-table adapter without changing Linux FD semantics.
3. Specify and implement the Linux FD-table adapter.
4. Add object-type, rights, duplication, inheritance, close, stale-ID, process-exit and resource-exhaustion tests.

**Exit gate:** independent tests prove Windows handle semantics and Linux FD semantics remain distinct while both resolve safely to native objects.

### Phase 2 — VFS namespace adapters

1. Audit native VFS and ProsenjitFS contracts.
2. Define Windows per-process drive/prefix namespace and UTF-16 path handling.
3. Define Linux mount namespace and POSIX path behavior.
4. Add path traversal, case collision, Unicode, symlink/reparse, permissions, sharing, rename, mount isolation and denied-operation tests.

**Exit gate:** both guest namespaces can safely access only authorized VFS objects; denied operations do not mutate persistent storage.

### Phase 3 — IPC, shared memory and process/thread adapters

1. Stabilize validated IPC and cancellation/teardown.
2. Define shared-memory capability and mapping contracts.
3. Implement copy-based baseline and benchmark it.
4. Add shared-page transport only for proven large-buffer use cases.
5. Define guest process/thread lifecycle, TLS, synchronization and scheduling mappings.

**Exit gate:** cross-process isolation, revocation, crash/exit cleanup, buffer bounds, concurrency and regression tests pass. Performance claims require measured comparison.

### Phase 4 — Linux/POSIX application compatibility

1. Audit ELF loader and native process startup; preserve native ELF behavior.
2. Define supported POSIX API/syscall matrix.
3. Implement libc-facing contracts required by selected test programs.
4. Add signal, thread, synchronization, file, pipe, socket and process behavior incrementally.
5. Run real Linux ELF test programs and negative/security tests.

**Exit gate:** named binaries, architecture, libc/runtime version, supported API set and exact test results are recorded. Do not equate POSIX support with a complete Linux kernel ABI.

### Phase 5 — Windows user-mode application compatibility

1. Audit PE inspection/parser code; distinguish metadata inspection from executable mapping/loading.
2. Implement bounded PE32/PE32+ validation, mapping, relocations, section protections, imports/exports, TLS, entry point and teardown.
3. Implement DLL search/dependency policy and a small real API set (for example, console/process/file primitives) required by controlled test binaries.
4. Implement required NT/Win32 process, thread, memory, synchronization and file semantics.
5. Implement Windows exception/unwind semantics before claiming support for applications that depend on SEH.
6. Add registry-like configuration, console, GUI, graphics, audio, networking and services only as separately tested milestones.
7. Integrate installation and application management only after runtime preflight and isolation work.

**Exit gate:** at least one controlled real PE console application launches, imports resolve to real tested behavior, output and exit status are correct, cleanup works, malformed binaries fail safely, and native/Linux regressions remain green. Broader compatibility requires a versioned matrix.

### Phase 6 — Driver compatibility research and staged execution

1. Complete native driver model, device permission broker, native DMA/interrupt and lifecycle contracts.
2. Define a user-mode device service model and brokered access for suitable device classes.
3. Evaluate one narrowly scoped Linux or Windows driver framework target with exact ABI/version documentation.
4. Build a threat model and isolated prototype; never start with arbitrary third-party kernel drivers.
5. Add hardware-specific test devices, fault injection, cancellation, DMA/interrupt and recovery tests.

**Exit gate:** no driver framework is declared supported without real ABI conformance, device-specific tests, privilege-boundary tests, unload/recovery tests and explicit supported hardware/version lists.

## 5. Required acceptance and regression gates

Every milestone must provide:
- clean build on the exact candidate commit;
- existing boot and security workflows on the exact candidate commit;
- focused unit/synthetic tests for valid, malformed, boundary and failure inputs;
- QEMU tests where the environment can exercise the behavior;
- native ProsenjitOS application regression;
- filesystem/VFS/ProsenjitFS regression for file-related changes;
- process/thread/IPC cleanup and resource-leak checks;
- capability, permission-denial and cross-process isolation tests;
- fuzzing or property-based tests for complex parsers where practical;
- a clear list of tests not run and why;
- exact commit SHA, workflow run IDs and log references;
- documented supported/unsupported API and application cases.

A compile, parser success, stub export, synthetic-only test, open PR, skipped job, offline runner or old green workflow is not proof of end-to-end compatibility.

## 6. Required compatibility matrix

Maintain a version-controlled matrix with at least these columns:

| Target | Binary/version/architecture | Required APIs/ABIs | Implemented behavior | Test method | Exact result/commit | Limitations |
|---|---|---|---|---|---|---|

Track Linux and Windows independently. Record application build, runtime/library version, ProsenjitOS commit, test environment and outcome. Never use a single successful app to imply broad compatibility.

## 7. Coding-agent handoff protocol

When a human or AI agent is asked to continue this work:

1. Read all documents listed in Section 2.
2. Inspect the current repository source and default-branch head; do not assume this document describes current code.
3. Search open PRs/issues and recent commits for duplicate or conflicting work.
4. State the exact milestone and prerequisite gate being addressed.
5. Produce a source-backed gap analysis and a small implementation plan.
6. Work on a dedicated branch and keep one coherent PR active at a time.
7. Change only the necessary source, tests, build integration and canonical documentation.
8. Run targeted tests first, then exact-head required workflows; inspect logs rather than relying only on status labels.
9. Update the relevant canonical document and append a factual dated entry to docs/DEBUG_LOG.md.
10. Open a PR with scope, design, security implications, tests, exact limitations and follow-up work.
11. Do not merge until required exact-head checks pass and the full diff is reviewed.
12. Report what changed, what ran, what passed/failed, exact commit/PR/workflow evidence, and what remains.

### Task prompt template

> Continue ProsenjitOS compatibility work using docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md as the master plan. First read AGENTS.md, README.md, docs/ARCHITECTURE.md, docs/PROJECT-STRUCTURE.md, docs/ROADMAP.md, and the relevant canonical subsystem documents. Inspect the current default-branch source, headers, callers, tests, build rules, recent commits, open PRs and exact workflow evidence. Do not assume planned work is implemented. Identify the earliest unmet dependency gate, produce a source-backed gap analysis, and implement only that milestone on one dedicated branch/PR. Preserve native kernel, capability, VFS, ProsenjitFS and user-space boundaries. Add tests for success, malformed input, permission denial, resource exhaustion and teardown. Run and inspect exact-head required workflows. Update canonical documentation and docs/DEBUG_LOG.md. Do not claim completion or merge without verified evidence.

## 8. Current status

This document records an approved target architecture and execution method only. It does not assert that the handle/FD adapters, complete unified VFS namespaces, shared-memory transport, POSIX runtime, PE loader, Win32/NT runtime, SEH dispatcher, Linux driver compatibility, or Windows WDM compatibility are implemented.

Use source inspection and exact-head test evidence to update status when implementation begins. The native ProsenjitOS kernel and ProsenjitFS remain the priority foundation; compatibility work must not be used to bypass unfinished native prerequisites.
