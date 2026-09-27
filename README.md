# ProsenjitOS

**ProsenjitOS** is an independent 64-bit x86-64 operating-system project developed with the assistance of ChatGPT.

The project is being designed from first principles. It may study ideas used by existing operating systems, but it will **not copy proprietary source code, proprietary binaries, branding, protected assets, or implementation details from Windows, Linux, or another operating system**.

## Vision

ProsenjitOS aims to become a practical general-purpose OS with:

- a native x86-64 kernel and native application platform
- both GUI and CUI/CLI environments
- strong application isolation and least-privilege security
- explicit permissions for files, devices, processes, IPC, and network access
- package integrity and signed software support
- Windows application compatibility through an independent compatibility layer
- Linux/POSIX application compatibility through an independent user-space compatibility layer
- a native ProsenjitOS SDK/API for applications that want the best integration
- an API-based AI service so AI can run remotely, locally, or on another machine without requiring a large model inside the OS
- modular drivers, services, and user-space components so the base system remains maintainable and hardware-efficient

Compatibility is a **user-space feature**, not a promise that arbitrary Windows or Linux kernel drivers will run unchanged. Kernel drivers will use the ProsenjitOS driver model.

## Security principles

Security is a core architectural requirement, not a later add-on.

The design will use multiple independent layers:

1. CPU privilege separation and protected memory
2. per-process identity
3. capability/handle-based resource access
4. application sandboxing
5. explicit permission brokering
6. executable/package integrity verification
7. controlled driver loading
8. syscall and IPC policy enforcement
9. filesystem and network access controls
10. security event logging and audit trails
11. update, rollback, and recovery mechanisms
12. secure/verified boot as the platform matures

No operating system can honestly promise perfect protection against every future attack. The engineering goal is to make compromise harder, limit blast radius, detect abuse, and provide reliable recovery.

## Compatibility strategy

ProsenjitOS will have three application paths:

- **Native:** ProsenjitOS ABI/API and SDK
- **Linux/POSIX:** ELF loader + POSIX/Linux user-space compatibility runtime
- **Windows:** Windows API compatibility runtime, conceptually similar to the role a compatibility layer such as Wine plays, but integrated with ProsenjitOS security and packaging

Existing applications will not receive unrestricted access to the host OS simply because they are compatible applications. They will run through the same security and permission model.

## AI strategy

AI is intended to be an **OS-level control and assistance layer**, not merely a chatbot application.

The architecture is:

`User request -> AI service/API -> intent and action planning -> Policy/Permission Broker -> approved system action`

AI will be exposed through an OS-level API/broker rather than requiring a large model to be bundled with the base OS:

`Application -> ProsenjitOS AI API -> Policy/Permission Broker -> Local/LAN/Remote AI Provider`

The broker will eventually handle authentication, permissions, quotas, privacy policy, provider selection, and optional local inference.

### Natural-language system administration

A core goal is to let users describe an administrative task in natural language. For example:

> "Install Chrome."

The AI should be able to:

1. understand the user's requested outcome
2. determine the required package/software and dependencies
3. explain the planned action when appropriate
4. check the user's identity and effective permissions
5. request elevation or confirmation when the action requires additional privileges
6. execute only the actions allowed by the policy engine
7. report the result and relevant errors to the user

AI should **not** receive unrestricted administrator/root access merely because it is an AI service. Every privileged operation must pass through the ProsenjitOS permission and policy model.

### User privilege model

The planned user model distinguishes at least:

- **Limited User:** normal day-to-day account with restricted access to protected system resources
- **Super User:** authorized account/session capable of performing protected administrative operations

The AI broker will preserve this distinction. A Limited User request such as software installation may require an explicit authorization/elevation flow rather than silently granting administrative access.

The final design may use capabilities, scoped elevation, approval prompts, policy rules, audit logging, or combinations of these mechanisms.

## Development status

### Completed — x86-64 boot foundation

- repository initialized
- architecture and roadmap documented
- 64-bit/security/compatibility/AI requirements defined
- kernel C entry scaffolded
- VGA diagnostic output scaffolded
- build-system skeleton created
- bootable x86-64 ISO produced
- x86-64 long-mode boot path implemented
- early CPU security/memory-protection initialization implemented
- early protected exception/IDT foundation implemented
- ISO boot-tested with QEMU
- CI build-and-boot test configured to verify kernel startup and x86-64 long-mode activation

### In progress / next engineering milestones

- physical memory manager (first allocator implemented)\n- virtual memory and paging
- syscall and capability model
- security policy and permission broker
- user/session and Limited User / Super User model
- storage and filesystem
- drivers and device broker
- CUI/CLI
- GUI
- Linux/POSIX compatibility runtime
- Windows API compatibility runtime
- package manager and signed packages
- AI API and system-action broker
- natural-language administrative workflows
- hardening, testing, recovery, and release engineering

> **Status note:** The project has reached the point where a bootable x86-64 image can be built and boot-tested in QEMU. This is an early kernel foundation, not a complete operating system.

## CI verification

The repository includes GitHub Actions automation that:

1. checks out the source
2. installs the required build and QEMU toolchain
3. builds the bootable ISO
4. boots the ISO in QEMU
5. verifies expected kernel serial-output markers
6. uploads the ISO and QEMU boot log as CI artifacts

The boot test currently verifies, among other startup markers:

- `ProsenjitOS: kernel online`
- `ProsenjitOS: x86-64 long mode active`

This provides a repeatable minimum regression check for the boot foundation.

## Repository layout

```text
ProsenjitOS/
├── boot/               # Boot protocol, early CPU setup and boot assets
├── kernel/             # Kernel source and headers
├── drivers/            # Hardware drivers
├── user/               # User-mode programs and services
├── docs/               # Architecture, security and development documentation
├── scripts/            # Build/development helpers
├── .github/            # CI and repository automation
└── Makefile            # Build entry point
```

## Engineering rule

**Original implementation first. Compatibility second.**

Where an existing open-source compatibility project is useful, it may be integrated only under its applicable license and with a clear boundary between that component and ProsenjitOS-original code. ProsenjitOS itself will not reproduce another OS's proprietary implementation.

## License

MIT
