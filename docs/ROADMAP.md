# ProsenjitOS Roadmap

## Current status — 2026-09-30

The repository has moved beyond the original boot/kernel-only milestone. The current kernel filesystem scope is intentionally narrowed to native ProsenjitFS; legacy FAT32, exFAT and NTFS implementations were removed from the kernel image so compatibility support can later be supplied through external filesystem providers.

### Current engineering focus
1. **Native filesystem production verification:** finish ProsenjitFS storage, transaction, crash, recovery and LBA verification.
2. **Filesystem-provider boundary:** define a stable external provider ABI without linking compatibility parsers into kernel-core.
3. **Compatibility providers:** later add NTFS/exFAT/FAT32 providers outside the kernel image.
4. **Security integration:** connect real filesystem-backed file reads/execute paths to the existing File admission/security broker and quarantine boundary, then add replaceable external reputation-provider adapters without making cloud access mandatory.
5. **I/O foundation:** the driver model foundation is now implemented around the PCI enumeration layer; next work is the device permission broker and concrete keyboard/display/network device abstractions.
6. **User mode:** stabilize the process startup path, ELF loading, init/service management, and basic CUI utilities before compatibility layers.
7. **Regression/CI:** keep every storage/security milestone tied to QEMU boot markers and focused synthetic filesystem tests; investigate any failed CI run before starting the next dependent milestone. The latest GitHub Actions runner failure occurs before workflow steps are created, so post-change CI verification remains pending.
8. **Desktop/compatibility strategy:** use mature existing desktop/display components first (for example XFCE with existing X11/Xorg and/or Wayland components); do not build a new windowing system at this stage.
   - **Completion status:** desktop remains incomplete. Source inspection found a kernel ELF loader and kernel-side service manager, but the boot path uses an embedded CPL3 test ELF; a userspace PID 1, filesystem-backed normal process launch, real display backend, keyboard/pointer event path, and Xorg/Wayland + XFCE session integration are not yet verified. See `docs/DESKTOP-IMPLEMENTATION-PLAN.md`.
9. **Windows/OneDrive compatibility:** track Win32/Office application compatibility and reliable OneDrive-style background synchronization as long-term application-level compatibility targets.
10. **Self-healing/recovery architecture:** add layered health detection and safe recovery without allowing the recovery system to become a new source of corruption. The design must follow **Detect -> Verify -> Repair -> Verify -> Rollback** and keep destructive repair disabled by default.
11. **Display binding:** kernel owns only device discovery/binding, framebuffer/backend validation, permissions and lifecycle; window systems and desktop components remain userspace.

- **Driver compatibility:** build an independently implemented, ReactOS-inspired NT/WDM compatibility track after the native driver and package-management foundations are stable. This is a long-term compatibility subsystem, not a replacement for native drivers.

### Latest verified repository work
- PCI configuration/enumeration foundation implemented and marked complete.
- Firewall-inspired unknown-threat handling documented as a standing security requirement.
- Signature-verification trust boundary contains a freestanding Ed25519 verification implementation with strict point/scalar validation, SHA-512 challenge hashing, subgroup-small-order rejection, and an RFC 8032 known-answer self-test; source-level implementation is present, but current GitHub Actions runner failures prevent claiming current crypto CI verification. RSA-PSS remains fail-closed as unsupported.
- Ed25519 field arithmetic foundation now uses a 5×51-bit representation with canonical encoding/reduction, explicit carry-safe multiplication, subtraction, and constant-schedule inversion; the extended-coordinate point layer now includes identity, complete add/double formulas, curve-equation checking, canonical point decoding with sign-bit recovery, affine encoding, and base-point round-trip self-tests. Ed25519 scalar reduction/multiplication and signature verification are now implemented and startup-gated by self-tests. GitHub Actions currently starts push runs but the runner job is failing before any workflow step is created, so this new crypto implementation remains unverified by CI.
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
- [x] Virtual memory and paging (4-level mappings, canonical/physical bounds, W^X/NX checks, duplicate-map protection, translation/query invariants and deep self-test)
- [x] Kernel heap (bounded growth, overflow-safe allocation, ownership-checked free, coalescing and deep self-test)
- [x] User/kernel address-space separation foundation (isolated user mappings, ownership tracking, alias protection, range validation, teardown and deep self-test)
- [x] Process/thread/syscall runtime verification (process lifecycle/reap, address-space switching, scheduler/run-queue, process-thread teardown, IPC block/wake, interrupt-frame save/restore, syscall ABI/allowlist/unknown-call checks)
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
  - [ ] Per-CPU usercopy fault state for SMP
  - [ ] SMP-safe scheduler/run-queue locking

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
- [x] Device permission broker (bound-device grants, checks, and teardown revocation)
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
  - [x] Explicit device-class capability contract with fail-closed unsupported-class handling
  - [x] PCI base-class mapping for storage/network/display/audio/serial/input/USB
  - [ ] Full Driver Manager / package-management architecture
  - [ ] Hardware-ID database and device/driver matching
  - [ ] Cryptographic signed driver-package verification and trust policy
    - [x] Fail-closed signature authority boundary and caller-metadata rejection
    - [~] Real Ed25519/RSA-PSS verifier with known-answer tests
      - [x] Freestanding Ed25519 field/point/scalar arithmetic primitive layer
      - [x] Ed25519 point/scalar verification implementation
      - [x] Ed25519 RFC 8032 known-answer verification test
      - [ ] RSA-PSS SHA-256 verification implementation
  - [x] Compatibility capability/resource validation and bounded PnP lifecycle guard
  - [~] Windows driver package importer (bounded `.inf` inspection exists; archive/executable extraction remains pending)
  - [x] Bounded INF hardware-ID inspection foundation (PCI VEN/DEV extraction)
  - [ ] Full Windows INF parsing and compatible package metadata extraction
  - [x] Staged NT/WDM compatibility boundary ABI with quarantine/rollback states
  - [ ] **ReactOS-inspired NT/WDM executable compatibility architecture (independent implementation; no direct ReactOS source copy by default)**
  - [ ] Windows-driver compatibility layer / adapter API
  - [x] Native-vs-compatibility driver selection and fallback flow (deterministic bounded matching)
  - [~] Installer/bootstrap driver environment and offline-first hardware bundle (bounded bundle index, content-identity guard, and admission-backed package import implemented; installer/bootstrap integration remains)
  - [~] Offline driver installation from removable/local media (admission-backed package import/indexing foundation implemented; physical media discovery/extraction and installer integration remain)
  - [~] Driver rollback, removal, versioning, and dependency handling (transactional rollback + bounded dependencies + deterministic bundle version/source/package-ID selection + active-package immutability + safe staged update sequence + guarded package retirement/stale-locator cleanup implemented; persistent package/update lifecycle remains pending)
  - [ ] Driver isolation/sandbox and compatibility health checks
  - [ ] Never load Windows `.sys` binaries directly as native ProsenjitOS kernel drivers; imported Windows packages require a compatible execution/adapter path
- [ ] Keyboard
- [x] Storage abstraction (block-device contract)
- [x] PCI enumeration
- [x] Display abstraction (kernel framebuffer/backend contract, validation, present path, lifecycle boundary)
- [ ] Network device abstraction

## Phase 6 — Storage and networking
- [ ] VFS
  - [x] VFS mount object foundation
  - [x] Native ProsenjitFS filesystem type contract
  - [x] Path, vnode/inode, file and directory APIs
  - [x] Filesystem probe and partition integration
  - [x] Native ATA -> MBR -> ProsenjitFS persistent mount regression
- [ ] External compatibility filesystem providers
  - [ ] Provider ABI/registration boundary
  - [ ] NTFS provider (external to kernel)
  - [ ] exFAT provider (external to kernel)
  - [ ] FAT32 provider (external to kernel)
- [ ] Native filesystem design
  - [x] ProsenjitFS persistent file create/read/write/truncate
  - [x] ProsenjitFS bitmap-backed inode/data-block allocation and reclamation
  - [x] ProsenjitFS hash-indexed directory lookup/readdir/create
  - [x] ProsenjitFS transactional mkdir/rmdir with directory-index allocation/reclamation
  - [x] ProsenjitFS transactional unlink with inode/block reclamation
  - [x] ProsenjitFS same-directory rename, including dot-prefixed hidden names
  - [x] ProsenjitFS cross-directory file/directory move with directory-parent cycle protection
  - [x] ProsenjitFS directory mutation regression: nested directories, move, non-empty rmdir rejection, empty rmdir reclamation
  - [x] ProsenjitFS crash/corruption recovery coverage (deterministic journal replay, torn-transaction discard, checksum-corruption fail-closed, and mount metadata corruption rejection)
- [ ] Ethernet
- [ ] ARP
- [ ] IPv4
- [ ] UDP/TCP
- [ ] Network sandbox policy

## Phase 7 — User mode and CUI
- [x] User address spaces
- [x] ELF64 loader (ET_EXEC validation, PT_LOAD mapping, NX/W^X enforcement, argv/env stack construction, rollback; dynamic relocation deferred)
- [x] ELF64 loader runtime verification (boot-time real address-space mapping/permission checks, loaded-code byte check, argv/env stack/argc validation, then CPL3 entry; dynamic relocation deferred)
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
  - [~] System health manager and recovery-state logging (bounded structured health-event ring + overflow regression + heap/timer/scheduler sampling + boot self-test + deterministic diagnostic export + central log facade + bounded VFS persistence with rotation/retention + fatal exception/recovery capture + bounded watchdog/subsystem event hooks integrated; raw-block crash records with validation/consumption plus safe-context deferred flush infrastructure, and storage/driver/filesystem/security health events integrated; fatal-path reboot-surviving persistence, full runtime service-manager/network-stack instrumentation, recovery orchestration and final CI verification remain)
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
- **Filesystem extensibility:** future filesystem providers should attach through the VFS/provider boundary without requiring kernel-core redesign; the native ProsenjitFS implementation remains in kernel-core while future FAT32/exFAT/NTFS providers stay outside the kernel image.
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
