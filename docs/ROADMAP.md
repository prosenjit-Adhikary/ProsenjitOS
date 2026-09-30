# ProsenjitOS Roadmap

## Current status — 2026-09-29

The repository has moved beyond the original boot/kernel-only milestone. Recent work has completed the PCI enumeration foundation, expanded FAT32 and exFAT geometry regression coverage, added an exFAT FAT-chain/file-stream read foundation, completed exFAT read-only directory entry-set parsing/lookup (including UTF-16 ASCII-subset decoding, NameLength handling, checksum validation, and case-insensitive lookup), integrated exFAT read-only root lookup/file read/directory enumeration into the VFS with vnode lifetime handling, and documented the firewall-inspired unknown-threat handling model. The repository is private.

### Current engineering focus
1. **Storage integration:** complete real partition/filesystem discovery and connect the existing FAT32/VFS path to realistic block-device flows.
2. **exFAT:** harden the read-only VFS integration; Up-case table discovery, checksum validation, compressed/uncompressed parsing, UTF-16 filename decoding, UTF-8 lookup, and Unicode case-folded VFS lookup are now integrated, with broader real-volume regression coverage remaining.
3. **NTFS:** continue from the current safe file-data write path toward directory-index mutation, cluster allocation, and journal/recovery; keep destructive metadata writes out until each stage is regression-tested.
4. **Security integration:** connect real filesystem-backed file reads/execute paths to the existing File admission/security broker and quarantine boundary, then add replaceable external reputation-provider adapters without making cloud access mandatory.
5. **I/O foundation:** the driver model foundation is now implemented around the PCI enumeration layer; next work is the device permission broker and concrete keyboard/display/network device abstractions.
6. **User mode:** stabilize the process startup path, ELF loading, init/service management, and basic CUI utilities before compatibility layers.
7. **Regression/CI:** keep every storage/security milestone tied to QEMU boot markers and focused synthetic filesystem tests; investigate any failed CI run before starting the next dependent milestone.
8. **Desktop/compatibility strategy:** use mature existing desktop/display components first (for example XFCE with existing X11/Xorg and/or Wayland components); do not build a new windowing system at this stage.
9. **Windows/OneDrive compatibility:** track Win32/Office application compatibility and reliable OneDrive-style background synchronization as long-term application-level compatibility targets.
10. **Self-healing/recovery architecture:** add layered health detection and safe recovery without allowing the recovery system to become a new source of corruption. The design must follow **Detect -> Verify -> Repair -> Verify -> Rollback** and keep destructive repair disabled by default.
11. **Driver compatibility:** build an independently implemented, ReactOS-inspired NT/WDM compatibility track after the native driver and package-management foundations are stable. This is a long-term compatibility subsystem, not a replacement for native drivers.

### Latest verified repository work
- PCI configuration/enumeration foundation implemented and marked complete.
- FAT32 geometry regression coverage expanded.
- exFAT geometry regression coverage expanded.
- exFAT FAT-entry validation and bounded file-stream reads implemented, including NoFatChain and ValidDataLength handling.
- exFAT read-only VFS integration implemented: root vnode, case-insensitive path lookup, file read, directory enumeration, vnode lifetime handling, and directory entry sets crossing cluster boundaries are covered by the boot regression.
- Firewall-inspired unknown-threat handling documented as a standing security requirement.
- NTFS safe overwrite, allocated-space growth, $Bitmap allocation, and non-contiguous runlist growth are implemented; full directory metadata/journal writes remain incomplete.
- VFS vnode lookup/reference semantics now explicitly transfer an owned lookup reference; cached FAT32/NTFS nodes retain their cache references, while dynamically allocated exFAT child vnodes release cleanly at final put.
- Both repository GitHub Actions workflows were green on the last verified storage/VFS commit (`3ed84998`). The Up-case parser work is intentionally isolated on a feature branch until CI verifies it.

### CI note
The repository contains build/boot and security-architecture GitHub Actions workflows. The latest verified storage/security implementation passed both repository GitHub Actions workflows: build/boot and security-architecture CI. Future storage changes must preserve both gates.

## Phase 0 — Repository bootstrap
- [x] Repository created
- [x] Initial README
- [x] License and ignore rules
- [x] Architecture/security requirements documented
- [x] First bootable image

## Phase 1 — 64-bit boot and kernel foundation
- [x] BIOS boot path foundation
- [ ] Native ProsenjitOS bootloader
  - [ ] Stable ProsenjitOS Boot Protocol
  - [ ] Native UEFI loader
  - [x] Native legacy BIOS loader hardening
  - [ ] Boot-device discovery
  - [ ] Kernel/module validation
  - [x] Native loader QEMU/CI regression
  - [ ] Optional traditional-bootloader adapter (GRUB/other)
- [x] 32-bit protected mode
- [x] Long-mode transition
- [x] x86-64 kernel entry
- [x] VGA and serial console
- [x] Linker script and reproducible build
- [x] Early boot security checks

## Phase 2 — CPU and memory isolation
- [x] GDT/IDT
- [x] Interrupt and exception handlers
- [x] Timer (PIT channel 0, IRQ0 tick source)
- [x] Physical page allocator
- [x] Virtual memory and paging
- [x] Kernel heap
- [x] User/kernel address-space separation foundation
- [x] NX/W^X memory policy where supported

## Phase 3 — Kernel architecture
- [x] Kernel object model
- [x] Protected handles
- [x] Process manager
  - [x] Process lifecycle states
  - [x] Parent-process identity
  - [x] Basic child-process creation
  - [x] PID registry and process lookup
  - [x] Process exit/zombie state
  - [x] Parent wait/reap semantics
  - [x] Process resource cleanup on reap
  - [x] Safe unregister/double-reap protection
- [x] Thread scheduler
  - [x] Thread object and lifecycle state
  - [x] FIFO run queue and scheduler selection
  - [x] Timer-driven preemption
- [x] Context switching foundation (kernel-stack switch + callee-saved state)
- [x] Preemptive interrupt-frame context switching
  - [x] Interrupt-frame ABI and scheduler hand-off foundation
  - [x] Kernel-thread timer preemption validated under QEMU
- [x] System-call boundary
- [x] IPC
  - [x] Kernel message/channel objects
  - [x] IPC object represented by capability handles
  - [x] Capability-mediated send/receive syscalls
  - [x] CPL3 user-buffer validation for IPC I/O
  - [x] Capability-mediated endpoint transfer
  - [x] Kernel-mediated cross-process transfer syscall
  - [x] Kernel receiver wait queue and scheduler wakeup primitive
  - [x] Blocking receive syscall scheduler handoff
  - [x] IPC channel queue/waiter concurrency lock
  - [x] Validated kernel user-copy primitives for IPC
  - [x] Fault-recoverable user-copy with page-fault fixup
  - [x] Per-CPU usercopy fault state for SMP
  - [x] SMP-safe scheduler/run-queue locking

## Phase 4 — Security foundation
- [x] Per-process security identity
- [x] Capability/permission model
  - [x] Kernel capability-handle foundation
  - [x] Per-process capability policy
  - [x] Capability transfer rules
  - [x] Kernel-mediated cross-process transfer syscall
  - [x] Intent-aware authorization pipeline
  - [x] Policy decision and bounded Reason Ledger
  - [x] Process quarantine containment state
- [x] Protected object manager (capability-backed kernel object/handle lifecycle)
- [x] Application sandbox foundation (per-process syscall allowlist enforced at syscall boundary)
- [ ] Filesystem access control
- [ ] Process isolation
- [ ] Device permission broker
- [ ] Network permission policy
- [x] Security event log (Reason Ledger foundation)
- [ ] Signed package verification
- [ ] Controlled driver loading
- [x] File-security policy contract and admission decision engine
- [x] Freestanding SHA-256 implementation with known-answer self-test
- [x] Security trust-state model (Trusted/Verified/Unknown/Suspicious/Malicious)
- [x] Local file verification contract and SHA-256 reputation interface
- [x] Replaceable reputation provider adapter contract (hash-only, user-space compatible)
- [x] Filesystem-independent file admission broker (hash + policy + reputation)
- [x] Protected quarantine metadata store and execution admission contract
- [x] Filesystem-neutral VFS admission boundary with startup regression test
- [ ] Filesystem-backed file admission/security broker
- [ ] External reputation provider adapters

## Phase 5 — I/O and drivers
- [x] Driver model
  - [x] Stable device/driver registry, hardware-ID matching, lifecycle bind/unbind, native/compatibility origin metadata, and boot self-test
  - [x] Legacy ATA PIO block-device provider (polling, LBA28/LBA48, write and flush)
  - [x] Native QEMU persistent-disk handoff into the partition/VFS stack
  - [ ] Universal hardware coverage strategy (native + class/protocol + compatibility + graceful fallback)
  - [x] Driver Manager metadata/trust/dependency/matching foundation
  - [ ] Full Driver Manager / package-management architecture
  - [ ] Hardware-ID database and device/driver matching
  - [ ] Cryptographic signed driver-package verification and trust policy
  - [x] Compatibility capability/resource validation and bounded PnP lifecycle guard
  - [ ] Windows driver package importer (extract/inspect `.exe`, `.zip`, `.cab`, `.inf` packages without executing vendor installers)
  - [x] Bounded INF hardware-ID inspection foundation (PCI VEN/DEV extraction)
  - [ ] Full Windows INF parsing and compatible package metadata extraction
  - [x] Staged NT/WDM compatibility boundary ABI with quarantine/rollback states
  - [ ] **ReactOS-inspired NT/WDM executable compatibility architecture (independent implementation; no direct ReactOS source copy by default)**
  - [ ] Windows-driver compatibility layer / adapter API
  - [ ] Native-vs-compatibility driver selection and fallback flow
  - [ ] Installer/bootstrap driver environment and offline-first hardware bundle
  - [ ] Offline driver installation from removable/local media
  - [ ] Driver rollback, removal, versioning, and dependency handling
  - [ ] Driver isolation/sandbox and compatibility health checks
  - [ ] Never load Windows `.sys` binaries directly as native ProsenjitOS kernel drivers; imported Windows packages require a compatible execution/adapter path
- [ ] Keyboard
- [x] Storage abstraction (block-device contract)
- [x] PCI enumeration
- [ ] Display abstraction
- [ ] Network device abstraction

## Phase 6 — Storage and networking
- [ ] VFS
  - [x] VFS mount object foundation
  - [x] Filesystem type contract for FAT32/exFAT/NTFS/ProsenjitFS
  - [x] Path, vnode/inode, file and directory APIs
  - [x] Filesystem probe and partition integration
  - [x] Native ATA -> MBR -> ProsenjitFS persistent mount regression
- [ ] FAT32 filesystem support
  - [x] Read-only FAT32 BPB mount/parser foundation
  - [x] Read-only FAT32 directory/file reader
  - [ ] FAT32 write support
- [ ] exFAT filesystem support
  - [x] Read-only exFAT mount/parser and FAT-chain/file-stream read foundation
  - [x] exFAT read-only directory/file VFS integration (root vnode, lookup, read, readdir, vnode lifetime)
  - [x] isolated exFAT Up-case table parser + compressed/uncompressed regression coverage
  - [x] exFAT Up-case table discovery/checksum validation during mount
  - [x] exFAT UTF-16 filename decoding and UTF-8 VFS lookup using the parsed Up-case table
  - [ ] exFAT write support
- [ ] NTFS filesystem support
  - [x] NTFS compatibility architecture documented
  - [ ] Read-only NTFS mount/parser
  - [x] NTFS safe overwrite of existing unnamed $DATA (no allocation)
  - [x] NTFS file growth inside already allocated non-sparse $DATA
  - [x] NTFS `$Bitmap` contiguous free-cluster allocation primitive
  - [x] NTFS file growth with exact contiguous `$Bitmap` allocation and runlist extension
  - [x] NTFS non-contiguous runlist growth with mapping-pairs append
  - [ ] NTFS full write support (directory metadata updates, journal/recovery)
  - [ ] NTFS journal/recovery handling
- [ ] Native filesystem design
  - [x] ProsenjitFS persistent file create/read/write/truncate
  - [x] ProsenjitFS transactional unlink with inode/block reclamation
  - [x] ProsenjitFS same-directory rename, including dot-prefixed hidden names
  - [x] ProsenjitFS cross-directory move/rename with directory-parent cycle protection
  - [x] ProsenjitFS crash/corruption recovery coverage (deterministic journal replay, torn-transaction discard, checksum-corruption fail-closed, and mount metadata corruption rejection)
- [ ] Ethernet
- [ ] ARP
- [ ] IPv4
- [ ] UDP/TCP
- [ ] Network sandbox policy

## Phase 7 — User mode and CUI
- [x] User address spaces
- [ ] ELF loader
- [ ] Process startup
- [ ] Init/service manager
- [ ] Secure shell
- [ ] Basic utilities
- [ ] Package manager

## Phase 8 — GUI
- [ ] Graphics abstraction
- [ ] Framebuffer
- [ ] Input subsystem
- [ ] Existing window-system integration (X11/Xorg and/or Wayland)
- [ ] Existing desktop environment integration (initial target: XFCE)
- [ ] ProsenjitOS-specific window manager/compositor (future, optional)
- [ ] Desktop shell integration
- [ ] File manager
- [ ] Terminal emulator
- [ ] Settings/security center
- [ ] Permission prompts

## Phase 9 — Application compatibility
- [ ] Native ProsenjitOS SDK
- [ ] POSIX-compatible user API
- [ ] Linux ELF compatibility environment
- [ ] Linux application packaging/container integration
- [ ] Windows API compatibility layer integration
- [ ] Windows application installer
- [ ] Microsoft Office compatibility validation (Word/Excel/PowerPoint/Outlook)
- [ ] OneDrive-compatible authentication and background-sync validation
- [ ] Office + cloud-sync file-locking/conflict/rename/move validation
- [ ] Compatibility sandbox profiles
- [ ] Application permissions UI

## Phase 10 — AI API
- [ ] OS AI service
- [ ] Stable AI API for applications
- [ ] Permission broker for AI requests
- [ ] Remote AI provider interface
- [ ] Local model provider interface
- [ ] LAN AI provider interface
- [ ] Credential isolation
- [ ] AI data/privacy policy

## Phase 11 — Hardening
- [ ] Production/release build separation from debug artifacts
- [ ] Release symbol/debug-metadata minimization
- [ ] Compiler/linker and memory-safety hardening
- [ ] Kernel control-flow and integrity protections
- [ ] Secure/verified boot
- [ ] Boot-component signing and integrity verification
- [ ] Measured boot
- [ ] Boot rollback/recovery protection
- [ ] Kernel self-protection
- [ ] Driver signing policy
- [ ] Signed kernel/system-component update policy
- [ ] Kernel tamper/integrity detection
- [ ] Reproducible/independently verifiable release artifacts where practical
- [ ] Update/rollback system
- [ ] Recovery environment
- [ ] Security auditing
- [ ] Fuzzing and regression tests
- [ ] Self-healing and recovery architecture
  - [ ] Boot health/failure detection and known-good boot fallback
  - [ ] ProsenjitFS integrity checker and safe repair engine
  - [ ] Journal-first crash recovery and recovery verification
  - [ ] Orphan inode/block detection and conservative reclamation
  - [ ] Filesystem metadata redundancy/degraded-mode handling
  - [ ] Service/process watchdog with bounded restart policy
  - [ ] Configuration transaction + health-check rollback
  - [ ] System health manager and recovery-state logging
  - [ ] Recovery environment and manual recovery override
  - [ ] Non-destructive recovery by default; destructive repair requires explicit recovery mode
  - [ ] Self-healing fault-injection, corruption, and rollback regression tests
- [ ] Malware containment tests
  - [ ] File admission and quarantine regression tests
  - [ ] Critical-file protection and recovery tests
  - [ ] Sandbox/behavior containment tests
  - [ ] Offline-safe security policy tests
  - [ ] External reputation-provider integration tests

## Standing architectural decisions

- **Security:** do not depend on source-code secrecy as the primary defense. Assume an attacker may reverse-engineer release binaries; security must remain based on isolation, least privilege, capabilities, integrity, authentication and layered hardening.
- **Release hardening:** reduce unnecessary production information exposure (debug symbols, internal metadata and unnecessary exports) without sacrificing auditability or maintainability.
- **Desktop:** use mature existing desktop/window-system technology first; a new ProsenjitOS windowing stack is not an immediate development target.
- **Boot:** prefer an original ProsenjitOS bootloader and stable boot protocol; MBR/GPT remain partition-layer concerns, and GRUB/other traditional loaders are optional adapters rather than kernel dependencies.
- **Filesystem extensibility:** future filesystem providers should attach through the VFS/provider boundary without requiring kernel-core redesign; existing FAT32/exFAT/NTFS/ProsenjitFS implementations remain protected from unnecessary architectural churn.
- **Compatibility:** Windows Office and OneDrive support are engineering targets to be validated incrementally, not current guarantees.
- **Driver compatibility:** maximize practical hardware coverage without attempting to author every vendor driver natively. Native ProsenjitOS drivers remain the primary path; a controlled Windows-driver import/compatibility subsystem is a critical long-term feature. ReactOS is an architectural/reference point for understanding NT/WDM compatibility, but ProsenjitOS will use an independently implemented compatibility layer by default. ReactOS source is GPL-licensed, so direct source reuse requires an explicit license review before any reuse. Vendor `.exe` installers should be treated as packages to extract and inspect (when technically possible), not executed blindly. The importer should recognize common package forms such as `.exe`, `.zip`, `.cab`, and `.inf`, inspect hardware IDs and metadata, verify package trust/signatures where available, and install only through the ProsenjitOS driver model/compatibility layer. Windows `.sys` binaries must not be assumed to be directly loadable by the ProsenjitOS kernel.

## Long-term
- [ ] USB
- [ ] SMP/multi-core scheduling
- [ ] Power management
- [ ] UEFI
- [ ] Developer SDK
- [ ] Software repository
- [ ] Full desktop environment

ProsenjitOS is an independent project and does not contain Microsoft proprietary source code.
