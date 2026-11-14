# ProsenjitOS Compatibility-First Implementation Model

**Status:** Architecture and implementation guide; design only unless a section is explicitly backed by source and exact-head test evidence.  
**Scope:** Native ProsenjitOS foundations, Linux/POSIX application compatibility, Windows application compatibility, shared resource/IPC services, and later driver compatibility.  
**Authority:** Authoritative target architecture, subsystem boundaries, dependency order, milestone gates and AI/human handoff contract. This document owns the cross-platform compatibility sequence. If another planning document gives a conflicting order, this document takes precedence until an explicitly reviewed PR changes it.  
**Document ownership:** [ARCHITECTURE.md](ARCHITECTURE.md) owns the concise OS-wide overview; [V2-UNIFIED-APPLICATION-RUNTIME.md](V2-UNIFIED-APPLICATION-RUNTIME.md) owns Windows runtime detail; [WINDOWS-APP-INSTALLATION.md](WINDOWS-APP-INSTALLATION.md) owns installation transactions; [DRIVER-MODEL.md](DRIVER-MODEL.md) owns native/device/driver contracts; [FILESYSTEM.md](FILESYSTEM.md) owns filesystem design and status; [FOUNDATION-AUDIT.md](FOUNDATION-AUDIT.md) owns the source-backed current foundation status; [NATIVE-SPAWN-CONTRACT.md](NATIVE-SPAWN-CONTRACT.md) owns the native process-launch contract. These documents add detail; they must not define a competing roadmap.

## 1. Mission and non-negotiable architecture

ProsenjitOS is an independent native operating system. Its own kernel, syscall ABI, process and thread model, memory manager, scheduler, capability security, device model, VFS, and ProsenjitFS remain authoritative.

The compatibility objective is to run a progressively larger, explicitly tested set of Linux/POSIX and Windows applications on the same ProsenjitOS installation and desktop. Compatibility means implementing the required executable formats, ABIs, APIs, runtime behavior, filesystem semantics, process/thread behavior, and service contracts. Merely parsing an executable or providing function-name stubs is not application compatibility.

### Authoritative target architecture

The architecture has one OS authority: the ProsenjitOS native microkernel and its native system foundation. Native applications and guest-ABI applications use different runtime paths, but neither gets a second kernel or a parallel operating system.

```text
APPLICATIONS
  Native ELF / ProsenjitOS ABI       Linux ELF / POSIX ABI       Windows PE32/PE32+ ABI
             |                                  |                           |
NATIVE RUNTIME                    LINUX COMPATIBILITY RUNTIME    WINDOWS COMPATIBILITY RUNTIME
  Native ELF loader                 Linux ELF startup adapter      ProsenjitOS PE loader
  Native libraries                  Linux syscall/API translator   DLL + API-set resolver
             |                                  |                           |
GUEST / NATIVE SEMANTIC ADAPTERS — SEPARATE ABI CONTRACTS
  Native ABI                 Linux FD / errno / signals       Windows HANDLE / NTSTATUS /
                                                              Win32 LastError / SEH
             \___________________________|______________________________/
                                         |
SHARED NATIVE USER-SPACE SERVICES AND BROKERS
  Process/thread · typed objects/capabilities · VFS namespaces · IPC/shared memory
  Registry service · graphics/windowing · audio · networking · package services
                                         |
NATIVE MICROKERNEL — SINGLE OS AUTHORITY
  CPU/interrupt entry · scheduler/context switch · PMM/VMM/address spaces
  syscall/usercopy · process/thread primitives · capability/object enforcement
  validated IPC · device/interrupt/DMA authority
                                         |
NATIVE OS FOUNDATION / HAL / NATIVE DRIVERS
  ProsenjitFS + native VFS · native libraries · storage/network/graphics providers
                                         |
HARDWARE
  CPU · RAM · storage · GPU · NIC · USB · peripherals
```

Windows compatibility components are ProsenjitOS-owned user-space components wherever practical. Windows system DLL names may resolve to ProsenjitOS-owned compatibility modules; application-private and third-party DLLs remain separately managed dependencies. A virtual Windows path/registry view is an adapter over native services, not a Windows installation.

Native drivers remain the preferred hardware path. User-mode driver compatibility is isolated and brokered. A Windows `.sys` or Linux `.ko` image is never loaded as an ordinary native kernel module. The current kernel is not to be described as a completed microkernel until service isolation and lifecycle gates are actually implemented and tested.

Wine may be studied as an architectural/behavioral reference. ProsenjitOS must not depend on, embed or require the Wine runtime. Any third-party source reuse requires a separate license, provenance, security and architecture review.
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
6. This document — the sole authoritative architecture and dependency sequence
7. docs/FOUNDATION-AUDIT.md for the source-backed native foundation status and known gaps
8. docs/NATIVE-SPAWN-CONTRACT.md before any process, ELF, CPL3 launch, or wait/reap work
9. docs/V2-UNIFIED-APPLICATION-RUNTIME.md for Windows application work
10. docs/DRIVER-MODEL.md for device/driver work
11. docs/FILESYSTEM.md for any VFS, path, file, or storage work
12. Relevant syscall, process/thread, memory, IPC, object/capability, exception, VFS, loader, and test source files

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

## 3. Six compatibility subsystem layers

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
- Windows thread, synchronization, exception/unwind, file, registry, console, GUI and service behavior in staged subsets.
- Keep this path consistent with docs/V2-UNIFIED-APPLICATION-RUNTIME.md.
- **API translation contract:** classify every exported/API operation as (a) a direct native operation mapping only when semantics are demonstrably equivalent, (b) a native user-space service operation, (c) a composite compatibility implementation, or (d) deterministically unsupported. Document the ABI/calling convention, input/output structures, size/version validation, UTF-16 and namespace behavior, access/share flags, HANDLE type and lifetime, concurrency/async/cancellation behavior, security checks, NTSTATUS/Win32 error mapping and tests. Never map a Windows syscall number directly to a native syscall number, and never count a success-only stub as implemented behavior.
- **Registry Engine:** treat the registry as a distinct compatibility subsystem, not as ordinary files. Specify hive/key/value types, per-user and per-application views, WOW64 views where supported, transaction/flush/recovery behavior, access-control mapping, handle lifetime, enumeration order/limits and deterministic error mapping. Store persistent data through authorized native storage/VFS services; do not expose raw host paths.
- **System DLL policy:** supported Windows system DLL names resolve to ProsenjitOS-owned modules/API-set contracts. App-private and third-party DLLs are dependency inputs governed by provenance, architecture, version, sandboxed search paths and explicit support status.

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

## 4. Canonical implementation sequence and dependency gates

This is the **only authoritative cross-platform compatibility sequence**. Later documents may add technical detail, but must not introduce a competing order. Design/specification work may be prepared early; implementation of a dependent runtime must wait for its prerequisite exit gates. Work one dependent milestone/PR at a time.

### Phase 0 — Master specification and source baseline

1. Read the repository instructions and all canonical documents listed in Section 2.
2. Reconcile architecture, subsystem ownership, terminology, dependency order and links. Remove stale or duplicate plans rather than adding another master document.
3. Audit current default-branch source, headers, callers, tests, open PRs, workflows and recent commits.
4. Record an evidence-backed implementation matrix and known blockers.

**Exit gate:** one consistent master specification; no unresolved document-level sequence conflict; source-backed baseline; open-PR conflict review; exact baseline commit and available build/test/CI evidence recorded. Documentation approval is not implementation completion.

### Phase 1 — Native microkernel and process-execution foundation

This is the first implementation milestone after Phase 0 documentation consolidation.

1. Reuse and audit current PMM/VMM/address-space, process/thread, scheduler, ELF, usercopy, capability/object and IPC primitives. Do not rebuild working subsystems.
2. Complete the native spawn transaction: non-runnable process construction, executable admission, ELF mapping, user stack/arguments, kernel stack, initial CPL3 interrupt-return frame, initial thread registration, policy/capability setup, and final publication/enqueue.
3. Preserve the established interrupt-frame ABI and per-thread TSS RSP0 contract. A new user thread must enter through a validated CPL3 IRET frame; never call a user entry point as a kernel C function.
4. Harden exit/wait/reap with a complete read-only preflight or explicitly retryable staged teardown before irreversible resource release. Cover handle-table, address-space, stack, TSS RSP0 and current-RSP invariants.
5. Add deterministic fault injection for allocation, admission, mapping, stack, thread, queue, publication and teardown failures.
6. Preserve existing boot-time CPL3 tests. Keep CPL3 timer preemption disabled and do not claim SMP until their independent prerequisites and tests pass.

**Exit gate:** a scheduler-created native user process demonstrably enters CPL3, receives the documented initial stack/arguments, executes and exits; spawn rollback leaves no visible/runnable child or leaked resources; wait/reap failures are retry-safe; exact-head warnings-as-errors build, focused tests, QEMU boot/launch evidence and required security workflows pass. The native ELF feature limits must be documented (including unsupported dynamic ELF features).

### Phase 2 — Native object lifecycle and guest HANDLE/FD adapters

1. Stabilize native typed-object lifetime, reference ownership, generation/stale-reference handling, capability checks, rights attenuation and revocation.
2. Implement a per-process Windows HANDLE table with object types, granted access, inheritance, duplication, waitability, close and invalid-handle semantics.
3. Implement a separate per-process Linux FD table with descriptor flags, close-on-exec, duplication, shared open-file-description semantics where required and errno behavior.
4. Define process/thread exit, inheritance, cancellation, cross-process transfer and object teardown.

**Exit gate:** Windows HANDLE values and Linux FDs remain distinct guest namespaces and never expose native pointers/capabilities; type confusion, stale values, rights escalation, cross-process access, exhaustion, double close and teardown tests pass.

### Phase 3 — Native VFS namespaces, IPC and shared-memory adapters

1. Define Windows per-process drive/device namespace, UTF-16 path rules, case behavior, sharing/locking and reparse-point policy.
2. Define Linux POSIX path and mount-namespace behavior independently.
3. Resolve both guest namespaces through the one authorized native VFS/ProsenjitFS storage authority; do not globally rewrite guest paths.
4. Stabilize IPC endpoint ownership, sender identity, blocking/wakeup, timeout, cancellation, back-pressure, endpoint revocation and process-death cleanup.
5. Add capability-controlled shared-memory objects only after ownership, mapping permissions, synchronization, revocation and teardown are proven. Keep copy-based IPC as the correctness baseline; require benchmarks before claiming zero-copy benefits.

**Exit gate:** path traversal, Unicode/case collisions, symlink/reparse, permissions, sharing, rename, namespace isolation, IPC races, buffer bounds, process death and shared-page revocation tests pass. Denied operations must not mutate persistent storage.

### Phase 4 — Linux/POSIX application compatibility

1. Keep native ELF loading and native syscall ABI separate from Linux ELF startup and Linux syscall translation.
2. Define a supported Linux process-entry contract, register/stack/TLS/auxv requirements and a versioned syscall/API matrix.
3. Implement only the Linux libc-facing behavior needed by named target binaries; add errno, signals, threads, synchronization, files, pipes, sockets and process behavior incrementally.
4. Never dispatch guest syscall numbers directly into native syscall IDs.

**Exit gate:** named real Linux binaries, architecture, libc/runtime versions, supported calls, exact results and limitations are recorded. Unsupported calls fail deterministically. POSIX subset support is not described as a complete Linux kernel ABI.

### Phase 5 — Windows PE loader and DLL resolver

1. Keep metadata inspection/parser code distinct from executable image mapping and process execution.
2. Implement bounded PE32/PE32+ validation, architecture checks, sections, relocations, page protections, imports/exports/forwarded exports, API sets, dependency graph, TLS, entry-point setup, unwind metadata needed by supported targets and rollback.
3. Implement a per-process DLL resolver with deterministic safe search order, system-module redirection to ProsenjitOS-owned compatibility modules, app-private dependency policy, provenance/version/architecture validation and unload/reference lifetime.
4. Fuzz malformed headers, directories, integer arithmetic, sections, relocations, imports, TLS and unwind metadata.

**Exit gate:** real image mapping and loader lifecycle are proven with malformed-input rejection, dependency/search-order tests, permission checks and complete rollback. A parser or image mapper alone is not Windows application execution.

### Phase 6 — Windows Win32/NT runtime and system semantics

1. Implement a deliberately small, documented Win32/NT API subset required by controlled real PE test programs.
2. Implement guest process/thread, memory, typed HANDLE/object, file/share, synchronization, asynchronous I/O/IOCP/cancellation and error/status semantics as required.
3. Implement the separate Registry Engine and Windows security mapping (SID, tokens, ACLs, impersonation/access checks) for the supported subset.
4. Implement native fault capture plus a distinct Windows exception/SEH and x64 unwind adapter; do not conflate SEH with Linux signals.
5. Classify each API under the translation contract in Layer B. Unsupported behavior must fail deterministically.

**Exit gate:** at least one controlled real PE console application has correct imports, output, exit status and cleanup; negative, security, malformed-input and API semantic tests pass. Publish a versioned supported/unsupported API matrix.

### Phase 7 — Graphics, audio, networking and desktop integration

1. Integrate only the GUI/window/message/drawing subset required by named applications with the existing ProsenjitOS desktop and native graphics services.
2. Route audio and networking through native services and their existing permission/security policies.
3. Test window lifecycle, input, focus, clipboard, rendering, audio/network access, multi-application behavior and crash cleanup.

**Exit gate:** supported applications use the same ProsenjitOS desktop/session and native service authority; unsupported graphics/API requirements are reported, not hidden behind stubs.

### Phase 8 — Installer, package and application management

1. Implement package/installer detection, trust/provenance checks, dependency resolution, per-app profiles, permissions and quotas.
2. Use transactional install/update/uninstall manifests, journaling, interrupted recovery and rollback where safe.
3. Treat MSI custom actions and EXE installers as untrusted executable code; do not grant elevation by default.

**Exit gate:** repeatable install/launch/update/uninstall tests, permission enforcement, dependency integrity, interrupted transaction recovery and rollback tests pass. Follow [WINDOWS-APP-INSTALLATION.md](WINDOWS-APP-INSTALLATION.md).

### Phase 9 — Isolated user-mode driver/device framework

1. Complete native driver/device broker contracts and user-mode service process prerequisites.
2. Implement isolated user-mode device hosts with private address spaces, capability-granted device access, IPC lifecycle, watchdog, bounded restart and resource revocation.
3. Start with suitable device classes and explicit tested framework/version subsets (including UMDF only where the required contracts are genuinely implemented).
4. Prove DMA pin/map/unmap, IOMMU or safe bounce-buffer policy, interrupt cleanup and device quiescence.

**Exit gate:** injected driver/service crash, timeout, unload and restart cannot corrupt the kernel or unrelated processes; device capabilities, DMA mappings and interrupts are safely revoked. Physical hardware claims require physical tests.

### Phase 10 — Restricted WDM/KMDF and other kernel-driver compatibility research

1. Select one exact driver framework/version and device class only after a threat model and ABI inventory.
2. Specify exact structures, calling conventions, IRP/IO_STACK_LOCATION lifecycle, pending/completion/cancellation, MDLs, IRQL constraints, PnP/power, DMA/IOMMU, interrupts, signing/trust and rollback.
3. Prototype behind strict isolation/brokering; never load arbitrary Windows `.sys` or Linux `.ko` binaries as native ProsenjitOS kernel modules.

**Exit gate:** framework-specific conformance, privilege-boundary, failure/recovery and device tests pass on the declared environment. No generic or universal driver compatibility claim.

### Dependency and parallel-work rule

- The next dependent implementation milestone starts only after the previous milestone's source review, regression suite and exact-head required CI gates pass.
- Pure specification, threat-model and parser-design review may be prepared in parallel when it does not change runtime assumptions; it must not be represented as implementation readiness.
- If a prerequisite fails, stop at that gate, fix it in a focused PR and update the evidence record.
- Do not merge a PR merely because it is open, approved in chat, or has a green run on an older commit.
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

## 9. Native kernel evolution and microkernel-oriented service boundary

The target is a **microkernel-oriented architecture**, reached incrementally. Do not describe the current kernel as a completed microkernel: existing kernel-resident subsystems remain in place until each service has a stable ABI, ownership model, security boundary, migration path and regression coverage.

### 9.1 Kernel responsibilities

Keep the privileged core as small and auditable as practical. Its long-term responsibilities are CPU and interrupt/exception entry, privilege transitions and context switching; PMM, VMM, page tables and address-space primitives; scheduler and minimal process/thread primitives; syscall dispatch and validated user-memory access; capability/object primitives and security enforcement; bounded IPC and shared-memory mapping authority; and minimal device discovery, interrupt/DMA mediation and hardware authority enforcement required by the HAL.

A subsystem moves out of the kernel only after its required kernel primitives and user-space service ABI are verified. No big-bang kernel rewrite is authorized by this architecture.

### 9.2 PMM, VMM and address-space invariants

PMM owns physical-page allocation, reservation, reference/lifetime tracking and safe reclamation. VMM owns virtual mappings, address-space isolation, permissions, mapping replacement and teardown. They are separate contracts.

Required invariants:
- Kernel and user mappings are distinct; user mode cannot map privileged kernel memory.
- Page permissions enforce W^X/NX where supported and reject writable-executable mappings unless a reviewed exception is explicitly required.
- Mapping ranges, canonical addresses, physical bounds, alignment, integer overflow and ownership are validated.
- Unmap/teardown invalidates stale translations and does not free pages still referenced by another mapping or DMA pin.
- Shared pages are explicit, capability-controlled, permission-limited and revocable.
- Process exit, failed load and partial mapping roll back all acquired pages and mappings.
- Per-address-space resource limits prevent one process or service from exhausting global memory.

Do not claim PMM/VMM completeness from allocator or page-table code alone. Verify real user-mode entry/return, context-frame lifetime, TLB invalidation, concurrency and process teardown on the exact target.

### 9.3 Fast IPC contract

Keep a simple, validated copy-based IPC path as the correctness baseline. A fast path may use bounded queues, direct handoff or shared pages only after measurement and security review.

The IPC ABI must define endpoint ownership, message size/count limits, sender identity, capability transfer, blocking/wakeup, timeout/cancellation, back-pressure, ordering, fairness, process-death cleanup and endpoint revocation. Every user buffer is validated at the boundary. Shared-memory transport requires explicit mapping rights, synchronization, lifetime and revocation; it is not a raw-pointer or physical-address channel.

Acceptance requires concurrent producer/consumer tests, malformed and oversized message tests, rights-denial tests, endpoint-close races, sender/receiver death tests, queue exhaustion and measured latency/throughput baselines. Do not label IPC fast without repeatable measurements against the copy baseline.

## 10. User-space subsystem topology and executable loading

Target service graph:

```text
Native kernel core
  PMM / VMM / scheduler / process primitives / syscalls / capabilities
  IPC + shared-memory authority / interrupt + DMA mediation
          |
          +-- Native service manager and watchdog
          +-- Native VFS + ProsenjitFS service boundary
          +-- Device broker and isolated driver-host processes
          +-- Linux/POSIX compatibility service
          |     ELF loader adapter -> Linux syscall/ABI translator
          +-- Windows compatibility service
          |     PE loader -> Win32/NT API layer -> DLL/module resolver
          +-- Graphics/display service -> framebuffer fallback backend
          +-- Network and storage service providers
```

This is a target topology, not a statement that these services already run outside the kernel. Each service needs a versioned ABI, capability policy, resource budget, startup/shutdown contract, health reporting and bounded restart policy.

### 10.1 ELF/Linux path

Keep the existing native ELF loader distinct from a Linux compatibility loader. ELF parsing/loading alone is not Linux compatibility. The Linux path needs a defined Linux process-entry contract, guest register/stack/TLS setup, syscall-number and argument translation, errno behavior, signal delivery, thread/synchronization behavior and a tested libc/application matrix.

The syscall translator must validate every guest pointer and length, translate only explicitly supported calls, preserve Linux-visible return/error semantics, and return a deterministic unsupported error for unknown calls. Never route arbitrary guest syscall numbers directly into native kernel syscall numbers.

### 10.2 PE/Windows path

Keep the user-space PE parser distinct from a full PE image loader. A production loader requires validated image mapping, relocations, section permissions, import/export resolution, DLL dependency search, TLS, entry-point setup, unwind metadata, process startup and rollback.

Windows API calls must pass through a versioned Win32/NT compatibility layer. Each API implementation must document input/output structure validation, handle/object semantics, error mapping, security checks and test coverage. A stub that merely returns success is not an implementation.

### 10.3 DLL redirection and interception

Provide a per-process, policy-controlled module resolver rather than global host DLL replacement. It should resolve imports using a deterministic documented search order; map supported Windows system DLL names to ProsenjitOS-owned compatibility modules or explicit API-set contracts; support per-application private DLL directories only under the app sandbox; validate architecture, image identity, dependency graph, version and module provenance; prevent DLL search-order hijacking and path traversal; keep native ProsenjitOS libraries in a separate namespace; expose only implemented exports; and maintain bounded diagnostics.

Interception is an ABI translation boundary, not permission to patch arbitrary executable code or bypass access control. Import redirection, delay-load behavior, forwarded exports, API-set resolution and module unload/reference lifetime need separate tests.

## 11. Driver process isolation, recovery and fallback devices

### 11.1 Isolation boundary

A user-space driver/device service must run in its own restricted process/address space, with private stacks and memory. It must not receive kernel pointers, unrestricted physical memory, unrestricted port/MMIO access, or another driver's private mappings.

Hardware access is granted only through the device permission broker and a narrow, typed device interface. DMA must use pinned pages and validated device-specific mappings, with IOMMU enforcement where available; otherwise use a reviewed bounce-buffer path or deny the operation. Interrupt delivery, MMIO, port I/O, reset and power operations require explicit per-device authority.

A crash in one isolated driver must not corrupt the kernel, another driver, or unrelated applications. A kernel-resident driver remains trusted kernel code and cannot be made safe merely by calling it a shim.

### 11.2 Driver supervisor and health policy

A privileged, minimal supervisor owns driver lifecycle policy. It must support startup deadline and bounded probe timeout; heartbeat/health state and request deadlines; fault/exit detection and bounded restart count; revocation of device capabilities and mappings before restart; cleanup of outstanding requests, DMA mappings, interrupts and shared pages; safe device reset/reinitialization; restart back-off and crash-loop quarantine; fallback binding or explicit device-unavailable state; and bounded audit diagnostics.

Do not restart a driver while stale DMA or interrupt activity can still target freed memory. If safe quiescence cannot be proven, quarantine the device and require a controlled reset/reboot path.

### 11.3 Generic fallback matrix

Fallbacks are capability-based and device-class-specific. They are not a promise that arbitrary unsupported hardware will work.

| Device class | Preferred path | Safe fallback | If no fallback is available |
|---|---|---|---|
| Display | Native GPU/display driver | Firmware VBE/GOP or validated linear framebuffer | Headless/serial recovery; do not hang boot |
| Storage | Native NVMe/AHCI driver | Supported ATA PIO or VirtIO block backend | Mark unavailable; preserve boot/recovery medium |
| Network | Native NIC driver | Supported VirtIO network or generic backend | Network offline; local OS continues |
| Keyboard/input | Native HID/controller driver | Firmware/serial console or supported generic USB HID | Keep recovery console if available |
| Mouse | Native HID driver | Supported generic USB HID | Disable pointer without stopping session |
| Graphics acceleration | Native GPU backend | Software rendering over framebuffer | Reduced performance, not kernel failure |

A fallback is selected only after identity/class validation and a bounded failed-bind or health transition. Do not bind a fallback that cannot safely operate the hardware. Keep primary and fallback drivers mutually exclusive for device ownership.

## 12. Unified exception normalization

Implement a kernel-owned exception capture boundary and a common internal fault-event representation, then separate guest adapters for Windows SEH and Linux signals.

The common event may carry fault class, originating process/thread identity, user instruction pointer, fault address where valid, access type, sanitized CPU context, source ABI, nesting/fatality state and an allowed recovery classification. It must not expose privileged kernel state or raw kernel pointers to user space.

The Windows adapter must implement only the documented SEH/NT exception subset, including exception records/context and correct x64 unwind metadata handling where required. The Linux adapter must separately implement signal dispositions, masks, pending signals, alternate stack/context and restart semantics for the supported subset. The two guest models must not be flattened into identical semantics.

Recovery policy must distinguish resumable, deliver-to-guest-handler, terminate-thread, terminate-process and fatal-kernel outcomes. User handlers cannot resume from an invalid or privileged context. Nested faults, corrupted stack/unwind data, handler failure and repeated fault loops must fail closed and be bounded.

Acceptance requires fault-injection tests for invalid memory access, execute-protection faults, illegal instruction, arithmetic fault, stack exhaustion, nested faults, corrupted unwind metadata, signal-mask edge cases, handler return validation and process cleanup. A normalized internal event is not by itself proof of SEH or Linux signal compatibility.

## 13. Active project checkpoint and handoff

**Checkpoint recorded: 2026-11-14.** Phase 0 documentation consolidation is being completed in PR #641. After the documentation PR has been reviewed and its exact-head required checks pass, the next dependent implementation milestone is **Phase 1 — Native microkernel and process-execution foundation**, beginning with a fresh source audit of `process.c`, `thread.c`, `elf_loader.c`, address-space teardown, interrupt-frame construction and TSS RSP0 ownership.

The source-backed current status and known wait/reap hazards are recorded in [FOUNDATION-AUDIT.md](FOUNDATION-AUDIT.md). The required native spawn behavior and rollback contract are recorded in [NATIVE-SPAWN-CONTRACT.md](NATIVE-SPAWN-CONTRACT.md). These documents are evidence/contracts, not proof that the missing implementation is complete.

**Handoff rule for every future human or AI session:**
1. Start from the current default-branch commit, not this chat or a remembered commit.
2. Read this master document, `AGENTS.md`, `README.md`, `ARCHITECTURE.md`, `PROJECT-STRUCTURE.md`, `ROADMAP.md`, and the canonical document for the selected subsystem.
3. Fetch current source, headers, callers, tests, build rules, open PRs, issues and exact-head workflow evidence.
4. Identify the earliest unmet phase and its explicit exit gate. Do not skip ahead because a later document contains design details.
5. Reuse existing implementations; do not duplicate subsystems or blindly merge stale branches.
6. Work one coherent dependent PR at a time. Add source-level regression/fault-injection tests and update the relevant canonical documents plus `DEBUG_LOG.md`.
7. Report exact paths, symbols, commit SHA, commands, workflow run IDs, results and limitations. Mark unrun or unavailable checks as UNVERIFIED.
8. Do not merge until the full diff, review feedback and exact-head required checks have been verified.
