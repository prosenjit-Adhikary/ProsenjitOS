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

## Unknown-threat handling and firewall-inspired security model

ProsenjitOS will treat **unknown activity as a security state**, not as an automatic malware verdict.

The design is intentionally informed by publicly documented techniques used by major security platforms such as Microsoft Defender, Palo Alto Networks WildFire, and Fortinet FortiSandbox. These platforms combine reputation/signatures with static analysis, machine learning, sandbox/dynamic behavior analysis, network indicators, quarantine/blocking, and feedback from centralized threat intelligence rather than relying on one signal alone. Microsoft documents local behavioral analysis plus cloud protection for cases where the endpoint cannot make a clear determination; WildFire forwards unknown samples for static/ML and dynamic analysis; FortiSandbox combines static scanning, cloud intelligence, and contained VM execution with behavior and network inspection. citeturn0search1turn0search6turn1search9

### ProsenjitOS target decision pipeline

```text
Process / file / script / network activity
                 |
                 v
        Kernel Security Monitor
                 |
        +--------+---------+
        |                  |
   Known/trusted       Unknown
        |                  |
      Policy             Freeze /
      checks            Restricted
        |                  |
        v                  v
      Allow       +--------+---------+
                  | Local analysis  |
                  | reputation      |
                  | static checks   |
                  | behavior        |
                  | network         |
                  | sandbox         |
                  +--------+---------+
                           |
                    Security Manager
                           |
                  +--------+---------+
                  |                  |
             AI assessment      No AI / timeout
                  |                  |
                  +--------+---------+
                           |
                    Kernel Policy
                      is final
                           |
          +----------------+----------------+
          |                |                |
        Allow          Quarantine        Terminate
      (scoped)         + isolate       + revoke
```

### Unknown-process policy

When a process is not known to ProsenjitOS:

1. **Do not immediately trust it.**
2. Start it with the minimum possible capabilities, or keep it suspended while a decision is pending.
3. Collect a structured security context: executable hash, signature/publisher, parent process, launch source, requested capabilities, file operations, process creation, IPC, device access, persistence attempts, DNS/network destinations, and other relevant behavior.
4. Check local reputation, allow/deny policy, package/signature state, and known indicators first.
5. If still unresolved, perform controlled behavior analysis in a sandbox/restricted execution environment.
6. Ask the ProsenjitOS Security Manager/AI analysis layer for an **advisory assessment** when available.
7. The AI response must never directly grant kernel privileges. The kernel policy engine remains authoritative.
8. If the analysis is inconclusive, the safe default is **restricted execution, quarantine, or deny**, depending on the requested capability and risk.
9. If malicious behavior or a hard policy violation is confirmed, immediately suspend/terminate the process, revoke its capabilities, isolate/quarantine affected objects, and record the reason.
10. Every decision and important evidence must be written to the Security Event/Reason Ledger.

### Multi-signal analysis

No single signal should decide trust in isolation. ProsenjitOS should correlate:

- cryptographic hash and local reputation
- code signature and trusted publisher
- package provenance and integrity
- executable format validation
- parent/child process relationships
- privilege and capability requests
- filesystem behavior
- process injection or cross-process manipulation attempts
- persistence/autostart changes
- device access
- IPC behavior
- DNS and network destinations
- connection timing and unusual traffic patterns
- sandbox behavior
- memory/runtime indicators where available
- historical reputation and previous local observations
- AI/security-service assessment

This follows the general defense-in-depth pattern documented by commercial security platforms: unknown samples are subjected to additional analysis rather than being treated as either automatically safe or automatically malicious. citeturn0search0turn0search4turn1search0

### AI safety boundary

The AI/security-analysis layer is an **advisor, not the final authority**.

It may return:

- `ALLOW_RESTRICTED`
- `ALLOW`
- `SUSPEND_FOR_ANALYSIS`
- `QUARANTINE`
- `TERMINATE`
- `INCONCLUSIVE`

The kernel/policy engine may reject an AI recommendation whenever a hard security rule is violated. AI failure, timeout, unavailable network service, malformed responses, or suspected AI compromise must never silently become an `ALLOW` decision.

### Firewall-inspired improvements planned for ProsenjitOS

The security roadmap will progressively add:

- local reputation cache
- threat-intelligence/query interface
- static inspection
- lightweight machine-learning classification hooks
- restricted dynamic execution/sandboxing
- behavior tree/event correlation
- network-aware threat scoring
- quarantine and recovery
- capability revocation on containment
- tamper-resistant Reason Ledger entries
- reusable indicators of compromise
- verdict caching with expiration
- offline-first protection when external services are unavailable
- safe sample/report submission with privacy controls
- protection against analysis abuse, request flooding, and security-service denial of service

**Important:** ProsenjitOS will not copy proprietary implementation or source code from these vendors. The goal is to adopt the documented **security principles and layered decision flow** and implement an original architecture appropriate for this OS.

This model extends the existing kernel-enforced security + user-space PSS architecture and is now a standing security requirement for future implementation work.

## Native bootloader and boot protocol strategy

ProsenjitOS will use an **original ProsenjitOS bootloader/boot protocol** as the preferred boot path rather than making GRUB or another traditional bootloader a mandatory dependency.

This does not remove MBR/GPT support. MBR and GPT remain partition/storage metadata formats and will be handled at the appropriate partition/storage layer when needed. The kernel will not depend directly on either partition format or on one specific bootloader.

The preferred architecture is:

```text
Firmware
  |
  +-- BIOS --> ProsenjitOS legacy boot path
  |
  +-- UEFI --> ProsenjitOS UEFI loader
                    |
                    +--> boot-device/partition discovery
                    +--> kernel/component integrity validation
                    +--> boot information preparation
                    +--> ProsenjitOS Boot Protocol
                              |
                              v
                       ProsenjitOS Kernel
```

The kernel and bootloader will communicate through a stable **ProsenjitOS Boot Protocol**. A future GRUB or other traditional-loader path may be supported through an adapter that translates its boot information into this protocol. This keeps the kernel independent from any specific bootloader.

The boot path will be implemented incrementally: protocol definition and validation first, native loader next, MBR/GPT-aware storage discovery where required, then signed components, Secure/verified boot, measured boot and rollback/recovery protection.

**Important:** the native bootloader is an engineering target; it is not being claimed as fully implemented merely because the architecture is documented.

See [docs/BOOT-ARCHITECTURE.md](docs/BOOT-ARCHITECTURE.md) for the complete boot architecture, security requirements and implementation order.

## Native boot implementation status

The native BIOS boot path is now implemented and CI-verified in QEMU. The current implementation is intentionally a fixed-LBA bootstrap path:

`BIOS -> native stage-1 -> native stage-2 -> common kernel handoff`

The existing GRUB/Multiboot2 path remains available as a compatibility/development path. MBR/GPT and filesystem discovery remain storage-layer responsibilities rather than prerequisites for the kernel boot contract.

See [docs/BOOT.md](docs/BOOT.md) for the current native loader layout, handoff boundary, limitations, and future storage/UEFI integration direction.

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

## Kernel security, reverse-engineering resistance and release hardening

ProsenjitOS will **not rely on source-code secrecy or obfuscation as its primary security mechanism**. The kernel and security architecture should remain maintainable, auditable, and understandable to trusted developers. The security objective is that an attacker should face strong, independent security boundaries even if they obtain or reverse-engineer the released kernel binary.

### Design decision

Use **defense in depth + minimal disclosure + release hardening**, rather than security through obscurity.

The project will track these principles:

- Keep security-critical source code reviewable and maintainable.
- Keep unnecessary internal symbols, debug metadata and implementation details out of production/release images.
- Separate debug symbols from production kernel artifacts.
- Minimize exported kernel interfaces and internal attack surface.
- Do not embed private signing keys, long-lived credentials, AI provider secrets, encryption master keys, or other high-value secrets in the kernel or release image.
- Use compiler/linker hardening and memory-safety protections appropriate to the architecture and toolchain.
- Add control-flow, stack, memory, module/driver and kernel-integrity protections as the platform matures.
- Require authenticated/signed kernel, driver and system-component updates.
- Progress toward Secure Boot, measured/verified boot and rollback protection.
- Treat kernel integrity verification and tamper detection as separate controls from code secrecy.
- Keep privilege boundaries, capabilities and policy enforcement effective even when implementation details are known.
- Avoid unnecessary information disclosure through diagnostics, interfaces, error paths and device-facing APIs.
- Make production builds deterministic/reproducible where practical so release artifacts can be independently verified.

### Threat-model principle

A ProsenjitOS release should remain secure even under the assumption that an attacker can inspect the binary and understand substantial parts of its implementation.

**Reverse-engineering resistance is therefore a secondary hardening layer, not the root of trust.** Where legally and technically appropriate, additional measures such as symbol minimization, stripping, controlled binary metadata and limited implementation disclosure may raise the cost of analysis, but they must never replace authentication, isolation, least privilege, memory protection, secure boot, integrity verification, or sound security policy.

### Planned release-hardening pipeline

`Source -> review/tests -> hardened release build -> integrity/signing -> verified boot/update -> runtime security policy`

Debug/development artifacts will remain separate from production artifacts so developers can diagnose failures without unnecessarily exposing that information in normal releases.

### Security boundary rule

If a future optimization intended to make reverse engineering harder would reduce auditability, reliability, recoverability, or the strength of a fundamental security boundary, the fundamental security property takes priority.

This decision is a standing architectural requirement for future kernel, driver, security-service and release-engineering work.

## Desktop, GUI and Windows-application compatibility tracking

ProsenjitOS will **not develop a new X Window System, Wayland compositor, or desktop environment at the current stage**. The project will use mature, existing desktop and display technologies first so development effort stays focused on the kernel, storage, security, drivers, compatibility layers, and system services.

### Current desktop strategy

- **Desktop environment:** use an existing mature environment such as **XFCE** during the early desktop phase.
- **Display/window stack:** use existing **X11/Xorg and/or Wayland** components where appropriate instead of creating a new windowing system.
- **Application stack:** prefer existing applications and open-source components where their licenses permit.
- **ProsenjitOS-specific work:** focus on integration, security policy, packaging, system services, and compatibility rather than replacing the entire desktop stack.
- A future ProsenjitOS desktop shell or compositor may be considered only after the underlying OS and compatibility foundations are mature.

### Windows application compatibility target

The long-term compatibility target includes practical use of common Windows applications through an independent user-space compatibility layer. In particular, the project should track compatibility for:

- Microsoft Office applications such as Word, Excel, PowerPoint and Outlook
- Windows application installation, process execution, file dialogs and common Win32 APIs
- Windows-style filesystem semantics required by applications
- Unicode filenames, timestamps, attributes, locking/sharing and rename/move behavior
- NTFS compatibility for Windows-originated data and removable storage

### OneDrive compatibility target

OneDrive synchronization is a **tracked compatibility requirement**, not a claim that it already works.

The compatibility layer and filesystem/user-space services should eventually support the behaviors required for reliable OneDrive use, including:

- Microsoft account authentication/sign-in flows where technically and legally feasible
- background synchronization
- file creation, modification, rename and move propagation
- file locking/sharing behavior used by Office applications
- timestamps, metadata and Unicode filenames
- online/offline state handling
- conflict detection and resolution
- reliable synchronization of Office documents stored in a OneDrive-backed directory

The combined target is:

`Windows application -> compatibility layer -> VFS/filesystem -> storage`

and, for cloud-backed files:

`Office application -> Windows compatibility layer -> OneDrive-compatible sync service -> VFS/filesystem -> storage`

**Compatibility status:** these are engineering targets and test requirements, not current guarantees. Compatibility will be validated incrementally with application-level tests as the relevant runtime, networking, authentication, filesystem semantics and background-service infrastructure are implemented.

### Future filesystem extensibility

Existing filesystem implementations should remain independent of this desktop strategy. FAT32, exFAT, NTFS and ProsenjitFS will continue through the existing VFS/storage architecture.

Future filesystems should be addable through a filesystem-provider/registration boundary rather than requiring changes to the kernel's core file API. This reserved extension path is intended to make future support for filesystems such as APFS, Btrfs or other formats possible without redesigning the kernel core.

The desired architecture is:

`Applications -> File API -> VFS -> Filesystem Registry/Providers -> Block/Partition Layer -> Hardware`

with existing providers kept intact and future providers added behind the same interface. Unsupported filesystems should fail cleanly rather than destabilizing the kernel.


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

### Current engineering status — 2026-09-28

The kernel/security foundation is substantially beyond the initial boot milestone. The current priority has shifted to storage integration, filesystem-backed security admission, device/driver foundations, and user-mode startup.

Completed or substantially implemented:
- process lifecycle, scheduler, interrupt-frame context switching and syscall boundary
- capability handles, rights validation, transfer/attenuation, and per-process policy
- syscall sandbox enforcement and user/kernel address-space isolation foundation
- IPC channels, blocking receive/wakeup, validated user-copy and SMP-safe scheduler/run-queue locking
- security intent/policy decision pipeline, Reason Ledger and quarantine metadata foundation
- SHA-256 verification and filesystem-neutral security admission boundary
- VFS contracts, partition/block-device foundations and filesystem probing
- FAT32 read-only mount/directory/file support with expanded geometry regression coverage
- PCI configuration/enumeration foundation
- NTFS directory traversal, case folding, file-data read, safe overwrite and allocated/non-contiguous growth
- exFAT geometry/mount regression foundation

Next, in order:
1. real partition/filesystem integration and FAT32 end-to-end storage flow
2. read-only exFAT directory/file support
3. NTFS directory-index mutation, broader allocation, and journal/recovery
4. filesystem-backed security admission plus replaceable reputation adapters
5. driver model/device broker and keyboard/display/network device abstractions
6. ELF loader, process startup, init/service manager and basic CUI
7. package/signature infrastructure, compatibility runtimes, GUI and AI service layers
8. hardening, fuzzing, recovery and release engineering

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


## Native ProsenjitFS

ProsenjitFS is now integrated as the native ProsenjitOS filesystem through the existing block-device, partition and VFS layers. FAT32, exFAT and NTFS remain compatibility filesystems.

The current implementation provides a versioned, CRC32C-protected on-disk format with redundant superblocks, persistent allocation bitmaps, inode/extent metadata and a redo journal. Its deterministic memory-backed format/mount/allocation/journal regression is now part of the boot-test workflow. Full directory mutation, complete file I/O, corruption/crash-point coverage and real QEMU storage-device integration remain subsequent milestones.

See docs/PROSENJITFS.md and docs/FILESYSTEM-INTEGRATION.md for the format contract and integration boundary.
