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
- malware containment and recovery as part of the security architecture
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
11. malware detection, containment, and recovery
12. update, rollback, and recovery mechanisms
13. secure/verified boot as the platform matures

### Malware and file-security architecture

ProsenjitOS will not make the kernel itself depend on external malware-analysis websites. File security is planned as a **user-space security service** enforced by kernel policy.

The planned admission flow is:

`New file / download / USB / package
        ↓
Local verification
        ↓
Signature + format + permissions + provenance + SHA-256
        ↓
Trusted ───────────────→ Allow
        │
        └→ Unknown → hash reputation → local sandbox/behavior analysis
                                      ↓
                              Suspicious → Quarantine
                                      ↓
                              Malicious → Block + quarantine
`

Important security rules:

- A hash is evidence, not proof of safety by itself.
- An unknown hash is **not automatically malicious**.
- Digital signatures and trusted publishers will be considered in addition to hashes.
- Hash-only reputation lookups should be preferred before any file upload to an external service.
- External reputation providers such as VirusTotal- or Hybrid Analysis-style services are evidence sources, not the sole authority.
- Suspicious files should be quarantined rather than immediately deleted.
- Critical system files should be protected and recoverable; automatic destructive deletion is not the default.
- Process isolation and sandboxing will limit the impact of a malicious file that reaches execution.
- Security decisions and containment actions will be recorded in the security event/Reason Ledger.
- The system should continue to provide a safe offline path when cloud reputation services are unavailable.

See [docs/SECURITY-MALWARE.md](docs/SECURITY-MALWARE.md) for the detailed design and staged implementation plan.

No operating system can honestly promise perfect protection against every future attack. The engineering goal is to make compromise harder, limit blast radius, detect abuse, and provide reliable recovery.

## Storage and filesystem strategy

ProsenjitOS is being designed to access common removable and internal storage without depending on a single filesystem. The storage stack separates hardware, block I/O, partitions, VFS, and filesystem drivers.

Target filesystem set:

- **FAT32** — broad legacy/removable-media compatibility
- **exFAT** — modern flash/removable-media compatibility
- **NTFS** — Windows data-volume and removable-storage compatibility
- **ProsenjitFS** — native filesystem for ProsenjitOS

NTFS is a mandatory compatibility target. The first NTFS implementation will be read-only so an incomplete parser cannot accidentally modify a Windows volume. Later stages will add safe write support and journal/recovery handling.

The planned removable-media path is:

`USB mass storage -> block device -> MBR/GPT -> filesystem probe -> FAT32/exFAT/NTFS -> VFS`

Filesystem implementations are isolated behind the VFS and block-device contracts. This allows the same filesystem code to work with USB, SATA and NVMe storage drivers. Filesystem metadata is also subject to the ProsenjitOS security/admission model.

### NTFS implementation progress

The read-only NTFS driver now includes:

- validated NTFS boot geometry
- FILE-record multi-sector USA/fixup validation
- resident attribute discovery and data access
- nonresident attribute runlist extraction
- multi-run and sparse-run decoding with bounded validation
- MFT record 0 bootstrap from its $DATA runlist
- fragmented MFT record mapping for subsequent record reads
- bounded MFT run storage to keep the freestanding kernel deterministic
- resident $FILE_NAME metadata parsing with UTF-16LE filename exposure
- resident $INDEX_ROOT header and entry validation, including terminal/sub-node flags
- exact UTF-16LE resident directory-name lookup returning the target MFT reference
- read-only `$INDEX_ALLOCATION` child-node traversal with INDX USA/VCN validation and bounded cycle protection
- NTFS `$Root` (MFT record 5) directory lookup and component-by-component path traversal

The MFT and directory-index paths remain metadata-read-only. `$INDEX_ROOT` and `$INDEX_ALLOCATION` traversal now reach child INDX nodes, while NTFS `$UpCase` case-folding, alternate `$FILE_NAME` namespaces, and general file-data streaming are supported. Writable NTFS mounts now support safe overwrite of existing unnamed `$DATA` and growth inside already allocated non-sparse space. Growth zero-fills gaps and updates the nonresident `$DATA` real/initialized sizes plus MFT `$FILE_NAME` data-size copies. Cluster allocation, directory-index metadata updates and journal/recovery writes remain later milestones.

See [docs/FILESYSTEM.md](docs/FILESYSTEM.md) for the detailed filesystem architecture and implementation order.

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

### Completed — initial memory-management foundation

- physical memory manager with Multiboot2 memory-map discovery
- 4 KiB physical page allocation/free support
- initial x86-64 virtual-memory/page-table manager
- first 1 GiB identity mapping retained for early kernel operation
- 4 KiB high-half mapping and translation smoke test
- initial page-backed kernel heap with allocation/free and memory smoke tests
- initial shared user/kernel ABI header under `include/abi/`
- register-safe `int 0x80` syscall entry with a minimal dispatcher
- CPL3 syscall round-trip test with ABI version validation and controlled return to the kernel
- generation-checked kernel handles with explicit capability rights
- object type and rights validation plus stale-handle rejection
- process-level handle validation/close interfaces
- per-process Limited/Super capability policy enforcement
- capability duplication restricted to existing rights and process policy
- kernel-mediated cross-process capability copy/move transfer with authority attenuation
- per-process syscall sandbox allowlist enforced at the syscall boundary

### In progress / next engineering milestones

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
- malware containment/security service
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
6. validates the security architecture documentation contract
7. uploads the ISO and QEMU boot log as CI artifacts

The security documentation check verifies that the planned malware-security architecture remains represented in the repository without pretending that the future scanner/service is already implemented.

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
