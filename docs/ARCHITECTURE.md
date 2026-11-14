# ProsenjitOS Architecture

ProsenjitOS is an independent 64-bit x86-64 operating system. Its architecture may take conceptual inspiration from Windows/NT and Unix-like systems, but all ProsenjitOS kernel and system interfaces will be independently implemented.

## Core goals
- 64-bit x86-64 first.
- GUI and CUI/CLI as first-class interfaces.
- Strong application isolation.
- Explicit permission-based access to files, devices, processes, IPC, and network resources.
- Windows application compatibility without copying Windows source code.
- Linux application compatibility through native Linux-compatible interfaces where practical.
- AI integration through APIs and optional remote/local services rather than requiring a large AI model inside the base OS.

## Security architecture

Security is a kernel-level design requirement, not an application added later.

### Application isolation
Every application gets an independent security identity and isolated execution environment. By default, one application must not be able to read or modify another application's private files, inspect another application's memory, control another application's processes, access private IPC endpoints, or access privileged hardware without authorization.

Access must be mediated by the kernel security manager and explicit capabilities/permissions.

### Capability and permission model
Files, processes, devices, sockets, IPC endpoints, and privileged operations will be represented by protected kernel objects/handles. Applications receive only the handles or capabilities they are authorized to use.

A future permission broker can ask the user for sensitive operations such as reading Documents, microphone access, camera access, or network access. The default policy follows least privilege.

### Sandbox
Normal desktop applications should run inside a sandbox with separate filesystem views where appropriate, restricted system calls, restricted device access, restricted process visibility, restricted IPC, policy-controlled network access, and resource limits.

Flatpak demonstrates a useful real-world model: applications can be isolated from host files, processes, devices, network, and selected IPC/syscalls by default. ProsenjitOS will implement its own security architecture rather than copying Flatpak internals.

### Malware resistance
ProsenjitOS will use multiple defensive layers:
1. Secure/verified boot support.
2. Signed application/package metadata.
3. Package integrity verification.
4. Per-application identities.
5. Sandboxing and least privilege.
6. Kernel-enforced memory/process isolation.
7. Controlled driver loading.
8. Security event logging.
9. Optional reputation/anti-malware services.
10. Automatic security updates.
11. Recovery mode.

No operating system can guarantee that all malware will be stopped. The design goal is to prevent unauthorized actions and contain compromise when prevention fails.

## GUI and CUI
Both interfaces are first-class OS components.

### CUI
- Interactive shell
- Scripting
- Pipes and redirection
- Process management
- Filesystem administration
- Networking tools
- Package management
- Security and permission administration
- Diagnostics and recovery commands

### GUI
- Window manager/compositor
- Desktop shell
- File manager
- Terminal emulator
- Settings/security center
- Application launcher
- Notification system
- Permission prompts
- Task/process manager

The GUI must never bypass kernel security. GUI and CUI use the same protected services.

## Application compatibility

ProsenjitOS is the only operating-system authority. Native applications and supported guest-ABI applications use separate user-space runtime paths and share native services only through explicit, security-checked adapters.

### Native applications
Native applications use the ProsenjitOS ABI, native ELF execution path, native libraries and SDK.

### Linux applications
Linux ELF applications use a separate Linux/POSIX startup and syscall/API compatibility runtime. Linux guest syscall numbers are never dispatched as native syscall IDs. Linux FD, errno, signal, namespace and process semantics remain guest-specific adapters.

### Windows applications
Windows PE32/PE32+ applications use a ProsenjitOS-owned user-space PE loader, DLL/API-set resolver and Win32/NT compatibility runtime. Each Windows API is either mapped to a proven-equivalent native operation, implemented by a native user-space service, implemented as a composite compatibility operation, or reported as deterministically unsupported. Windows HANDLE, NTSTATUS, Win32 LastError, UTF-16 paths, registry, SEH, asynchronous I/O and security semantics must not be silently collapsed into POSIX/native equivalents.

Wine may be studied as an architectural and behavioral reference. **ProsenjitOS must not depend on or embed the Wine runtime.** Any third-party source reuse requires a separate license, provenance, security and architecture review. Windows kernel drivers are a separate, later compatibility problem; a Windows `.sys` file is never loaded as an ordinary native kernel module.

### Single-OS execution topology

The complete layered diagram, native/Linux/Windows execution flows, device-only driver branch, security boundaries, and subsystem ownership are documented in [UNIFIED-OS-ARCHITECTURE.md](UNIFIED-OS-ARCHITECTURE.md). The key rule is that all three application paths use separate ABI/runtime adapters and converge on ProsenjitOS-owned native services and one kernel authority; ordinary application requests do not pass through foreign-driver compatibility.

This is the target architecture, not a claim that all runtimes or user-space services are implemented. The authoritative dependency sequence and acceptance gates are in [COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md).
## Package and application system
Planned concepts:
- Native `.proapp` packages.
- Linux package/container integration.
- Windows `.exe`/`.msi` compatibility installation through managed compatibility prefixes.
- Signed package metadata.
- Dependency management.
- Per-application sandbox profiles.
- Rollback/uninstall.
- User-visible application permissions.

The installer must never automatically grant broad privileges merely because an installer requests them.

## AI architecture

AI should be available without forcing every ProsenjitOS computer to contain a large local model.

### AI API layer
Application -> ProsenjitOS AI API -> Policy/Permission Broker -> Local AI or Remote AI Provider

Possible providers include a remote cloud API, a local lightweight model, a user-managed AI server on the LAN, or future hardware-accelerated local models.

Applications should not receive unrestricted API keys or credentials. The OS AI service should mediate authentication, permissions, quotas, privacy policy, and network access.

#
## Natural-language system administration

A core planned workflow is:

> User: "Install Chrome."

The AI service should translate the request into a structured action plan rather than executing arbitrary generated commands directly. The plan can include package lookup, dependency resolution, download verification, installation, configuration, and reporting.

Before execution:
1. Identify the requesting user/session.
2. Check the policy for each requested action.
3. Separate actions allowed at the current privilege level from protected actions.
4. If elevation is required, show the requested privilege and intended operation and require authorization.
5. Execute only approved, policy-compliant actions.
6. Record relevant actions and elevation events for audit/troubleshooting.

The AI service must never silently convert a Limited User session into a Super User session.

### User privilege model

The planned model includes at least:

- **Limited User:** ordinary account with restricted access to protected system resources.
- **Super User:** explicitly authorized account/session with permission to perform protected administrative operations.

The distinction is enforced by the OS security architecture, not by the AI model. AI may request an authorization/elevation flow, but it cannot grant itself privileges.

### AI safety and privacy boundaries

- AI-generated actions are data, not authority.
- Privilege is granted only by the OS policy/security layer.
- Sensitive data must not be sent to remote AI providers without an applicable policy/authorization decision.
- Provider credentials are isolated from applications.
- Actions and elevation events should be auditable.
- The system should support dry-run/planning mode before sensitive operations.

## Privacy
AI requests should clearly indicate whether data is processed locally, sent to a remote provider, stored, or shared with an application. Sensitive OS data must never be silently uploaded because an application invokes the AI API.

## Hardware/resource strategy
The base OS should remain lightweight. GUI, compatibility layers, containers, and AI services will be modular so a low-resource machine can run a minimal CUI installation while stronger machines can enable the full desktop stack.

## High-level architecture

```text
APPLICATIONS
  Native ELF / Native ABI | Linux ELF / POSIX ABI | Windows PE / Win32-NT ABI
             |                       |                       |
SEPARATE RUNTIMES
  Native ELF loader       Linux compatibility       ProsenjitOS PE loader
  Native libraries        ELF + syscall adapter    DLL resolver + Win32/NT runtime
             \______________________|_______________________/
                                    |
GUEST-SPECIFIC ADAPTERS
  Native objects | Linux FD/errno/signals | Windows HANDLE/NTSTATUS/SEH
                                    |
SHARED NATIVE USER-SPACE SERVICES
  VFS namespaces | IPC/shared memory | Registry | graphics/audio/network
  process/thread adapters | package services | device broker
                                    |
PROSENJITOS MICROKERNEL — SINGLE OS AUTHORITY
  scheduler/context switch | PMM/VMM | process/thread primitives
  syscall/usercopy | capabilities/objects | validated IPC | device authority
                                    |
NATIVE OS FOUNDATION / HAL / NATIVE DRIVERS
  ProsenjitFS + VFS | native libraries | storage/network/graphics providers
                                    |
                                 HARDWARE
```

Complex compatibility behavior and non-critical services belong in isolated user space wherever practical. The microkernel retains authority over privilege transitions, address spaces, capabilities, validated IPC, interrupts and device/DMA permissions.
## Design principles
1. Security boundaries must be enforced by the kernel, not trusted to applications.
2. Default permissions should be minimal.
3. GUI and CUI must use the same security model.
4. Compatibility layers must be isolated from the kernel.
5. Hardware-specific code stays behind HAL/driver interfaces.
6. Security features should fail closed where practical.
7. Performance-sensitive paths should avoid unnecessary full-system emulation.
8. Never copy proprietary Microsoft implementation code.
9. AI integration must be API-based and permission-aware.
10. Keep optional components modular so the base OS remains lightweight.

## Shared user/kernel ABI

User-space and kernel-space interfaces are separated from private kernel headers.

- `include/abi/` contains ABI-visible definitions that may be consumed by both sides.
- Syscall numbers and ABI-visible constants are defined centrally rather than duplicated in assembly and C.
- Private kernel implementation headers remain under `kernel/include/`.
- Once a published ABI value is used by user programs, changing it requires an explicit compatibility/versioning decision.

The first syscall ABI currently uses `int 0x80` as a deliberately simple bootstrap mechanism. The syscall entry saves general-purpose registers, dispatches through C, restores the register state, and returns with `iretq`. This is an initial kernel boundary, not yet the final high-performance syscall mechanism.

## Current implementation status

This file describes the target architecture; it is not the authoritative source-status ledger. For current source-backed implementation facts, missing functionality, lifecycle risks and verification limits, read [FOUNDATION-AUDIT.md](FOUNDATION-AUDIT.md) and [NATIVE-SPAWN-CONTRACT.md](NATIVE-SPAWN-CONTRACT.md). The compatibility implementation state is owned by the matrix and exact evidence required in [COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md).

The 2026-11-14 source audit records that the repository has substantial native kernel primitives, but general transactional native spawn/exec and scheduler-created initial CPL3 process launch remain incomplete. Linux/POSIX runtime, Windows PE execution and Win32/NT user-space runtime, guest HANDLE/FD adapters, guest VFS namespaces, shared-memory objects, guest signals/SEH and isolated user-space driver hosts are not implemented. The kernel must not be described as a completed microkernel. A bootable image, parser, scaffolding, documentation or isolated test marker does not establish application compatibility or production readiness.

Always re-fetch current source and exact-head CI before updating these statements.
## Initial target
The first target was a bootable x86-64 kernel that reaches a C kernel entry point and writes a diagnostic message. Security, application isolation, GUI/CUI, compatibility, and AI services will be built as explicit subsystems rather than bolted on at the end.


## Object and handle foundation

Kernel resources are represented as typed objects. Processes own a handle table containing opaque numeric handles, object references, and an explicit rights mask. Kernel code can validate both the expected object type and required rights before exposing a resource through a syscall.

The current implementation is deliberately small: it establishes ownership, reference counting, handle allocation/close, and the Limited/Super security-level fields needed by the process model. It does not yet claim complete capability security; syscall integration and policy enforcement remain future work.


## Intent-aware kernel authorization

ProsenjitOS now defines an experimental kernel authorization pipeline:

    Process
       |
    Intent
       |
    Capability check
       |
    Policy evaluation
       |
    Kernel decision
       |
    Reason Ledger
       |
    Execute / Deny / Quarantine

An intent is a structured description of an operation: operation class, target object type, handle, requested rights, and creation tick. Intent is not authority; the capability/handle remains the authority boundary.

The kernel authorization path validates the process and intent, then checks the requested rights against the process capability table and capability policy. A successful decision is ALLOW. Missing ordinary capability authority produces DENY. A sensitive unauthorized administrative request can produce QUARANTINE; quarantine is an attenuation/containment state and does not grant privilege.

Every decision is recorded in a bounded 128-entry Reason Ledger containing process/thread identity, intent ID, operation, object type, handle, requested rights, decision, reason code, and timer tick. The ledger is a diagnostic/security provenance mechanism, not a source of authority.

The quarantine state blocks a process from remaining READY/RUNNING and can be explicitly cleared by a trusted kernel policy path. No intent can elevate a Limited process into a Super process.

The current implementation is intentionally deterministic and small. It is a security foundation rather than a complete sandbox, filesystem ACL system, device broker, or user-facing permission prompt.


## Interrupt-frame scheduler ABI

The timer IRQ now has an explicit kernel interrupt-frame contract. IRQ0 saves all general-purpose registers, records the synthetic vector/error fields used by the common layout, and passes a stable frame pointer to the C scheduler. The scheduler records the interrupted frame against the current thread and can synthesize a first-run CPL0 interrupt-return frame for a thread that has not previously been interrupted.

The assembly return path can select the scheduler-returned frame, send the PIC EOI, restore the saved registers, and finish through iretq. The scheduler now also filters runnable threads whose owning process is already zombie/dead. The kernel-thread preemption path has now been exercised under QEMU with IRQ0 enabled for a bounded test window; both dedicated kernel test threads must make forward progress before boot continues. User-mode frame switching, address-space switching during preemption, nested-interrupt policy, scheduler locking, and SMP-safe scheduling remain future steps.


## Syscall intent authorization

Handle duplication is now routed through the same intent-aware security pipeline used by kernel self-tests. The syscall validates the source handle and resolves its object type before constructing a DUPLICATE intent. The security manager then checks the requested rights against the handle capability and process policy, records the decision in the Reason Ledger, and only then permits the existing duplication primitive to execute. This keeps the syscall as a policy enforcement boundary rather than allowing the syscall dispatcher to bypass the security model.


## IPC

The kernel now has a bounded IPC channel object (`PROS_OBJECT_IPC`) with eight 64-byte message slots. Existing capability handles authorize READ, WRITE, DUPLICATE, and TRANSFER access to the channel. CPL3 capability-mediated send/receive syscalls, complete user-range validation, blocking receive with scheduler handoff/wakeup, endpoint transfer, queue/waiter locking, and fault-recoverable user-copy are implemented and covered by kernel/QEMU regression markers. User-space channel creation and lifecycle ownership remain future work.
