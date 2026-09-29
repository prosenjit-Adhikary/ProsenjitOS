# ProsenjitOS Roadmap

## Current status — 2026-09-28

The repository has moved beyond the original boot/kernel-only milestone. Recent work has completed the PCI enumeration foundation, expanded FAT32 and exFAT geometry regression coverage, added an exFAT FAT-chain/file-stream read foundation, completed exFAT read-only directory entry-set parsing/lookup (including UTF-16 ASCII-subset decoding, NameLength handling, checksum validation, and case-insensitive lookup), and documented the firewall-inspired unknown-threat handling model. The repository is private.

### Current engineering focus
1. **Storage integration:** complete real partition/filesystem discovery and connect the existing FAT32/VFS path to realistic block-device flows.
2. **exFAT:** continue the new read-only FAT-chain/file-stream foundation with directory-entry parsing and VFS vnode integration.
3. **NTFS:** continue from the current safe file-data write path toward directory-index mutation, cluster allocation, and journal/recovery; keep destructive metadata writes out until each stage is regression-tested.
4. **Security integration:** connect real filesystem-backed file reads/execute paths to the existing admission/quarantine boundary, then add replaceable external reputation-provider adapters without making cloud access mandatory.
5. **I/O foundation:** build the driver model and block-device/device broker around the completed PCI enumeration layer, then add keyboard/display/network device abstractions.
6. **User mode:** stabilize the process startup path, ELF loading, init/service management, and basic CUI utilities before compatibility layers.
7. **Regression/CI:** keep every storage/security milestone tied to QEMU boot markers and focused synthetic filesystem tests; investigate any failed CI run before starting the next dependent milestone.
8. **Desktop/compatibility strategy:** use mature existing desktop/display components first (for example XFCE with existing X11/Xorg and/or Wayland components); do not build a new windowing system at this stage.
9. **Windows/OneDrive compatibility:** track Win32/Office application compatibility and reliable OneDrive-style background synchronization as long-term application-level compatibility targets.

### Latest verified repository work
- PCI configuration/enumeration foundation implemented and marked complete.
- FAT32 geometry regression coverage expanded.
- exFAT geometry regression coverage expanded.
- exFAT FAT-entry validation and bounded file-stream reads implemented, including NoFatChain and ValidDataLength handling.
- Firewall-inspired unknown-threat handling documented as a standing security requirement.
- NTFS safe overwrite, allocated-space growth, $Bitmap allocation, and non-contiguous runlist growth are implemented; full directory metadata/journal writes remain incomplete.
- No open pull requests are currently present.

### CI note
The repository contains build/boot and security-architecture GitHub Actions workflows. The latest verified storage/security implementation passed both repository GitHub Actions workflows: build/boot and security-architecture CI. Future storage changes must preserve both gates.

## Phase 0 — Repository bootstrap
- [x] Repository created
- [x] Initial README
- [x] License and ignore rules
- [x] Architecture/security requirements documented
- [x] First bootable image

## Phase 1 — 64-bit boot and kernel foundation
- [x] BIOS boot path
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
- [ ] Driver model
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
  - [ ] Filesystem probe and partition integration
- [ ] FAT32 filesystem support
  - [x] Read-only FAT32 BPB mount/parser foundation
  - [x] Read-only FAT32 directory/file reader
  - [ ] FAT32 write support
- [ ] exFAT filesystem support
  - [x] Read-only exFAT mount/parser and FAT-chain/file-stream read foundation
  - [ ] exFAT directory/file VFS integration (next implementation target)
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
- [ ] Kernel self-protection
- [ ] Driver signing policy
- [ ] Signed kernel/system-component update policy
- [ ] Kernel tamper/integrity detection
- [ ] Reproducible/independently verifiable release artifacts where practical
- [ ] Update/rollback system
- [ ] Recovery environment
- [ ] Security auditing
- [ ] Fuzzing and regression tests
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
- **Filesystem extensibility:** future filesystem providers should attach through the VFS/provider boundary without requiring kernel-core redesign; existing FAT32/exFAT/NTFS/ProsenjitFS implementations remain protected from unnecessary architectural churn.
- **Compatibility:** Windows Office and OneDrive support are engineering targets to be validated incrementally, not current guarantees.

## Long-term
- [ ] USB
- [ ] SMP/multi-core scheduling
- [ ] Power management
- [ ] UEFI
- [ ] Developer SDK
- [ ] Software repository
- [ ] Full desktop environment

ProsenjitOS is an independent project and does not contain Microsoft proprietary source code.
