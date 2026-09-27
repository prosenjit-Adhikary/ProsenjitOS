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

### Native applications
A native ProsenjitOS SDK/API will be provided.

### Linux applications
The long-term plan is a Linux compatibility environment with ELF loading, POSIX-compatible interfaces, filesystem/process translation, and optional containerized execution.

### Windows applications
The Windows compatibility goal will use a Windows API compatibility layer, conceptually similar to Wine. Wine translates Windows API calls into POSIX operations rather than emulating an entire PC, avoiding the cost of full machine emulation for many applications.

ProsenjitOS should initially integrate or interoperate with an external compatibility layer rather than attempting to implement the entire Windows API itself. Compatibility will be introduced progressively, starting with simpler user-mode applications.

Kernel-mode Windows drivers are a separate and much harder compatibility problem and will not be treated as ordinary applications.

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

GUI / CUI / Applications
        |
Native SDK | Linux Compatibility | Windows API Layer
        |
App Sandbox / Permission Broker / AI Service
        |
System Services | Package Manager | Security Manager
        |
System Call Interface / IPC
        |
Kernel Executive: Object | Process | Memory | I/O | Network | Security
        |
HAL / Drivers / Storage / Graphics
        |
x86-64 Hardware

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

The project has progressed beyond the initial boot-only design. A bootable x86-64 ISO is built and exercised through QEMU, with CI checking kernel startup, long-mode activation, and the user-mode/syscall path.

The implemented memory/isolation foundation now includes a physical page allocator, 4 KiB paging, kernel heap, per-address-space page tables, CPL3 entry, a shared user/kernel ABI, and a minimal syscall dispatcher. The next implementation focus is the kernel object/handle model and capability enforcement that will turn the syscall boundary into a permission-aware security boundary.

## Initial target
The first target was a bootable x86-64 kernel that reaches a C kernel entry point and writes a diagnostic message. Security, application isolation, GUI/CUI, compatibility, and AI services will be built as explicit subsystems rather than bolted on at the end.
