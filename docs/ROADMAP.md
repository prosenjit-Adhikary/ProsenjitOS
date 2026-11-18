# ProsenjitOS Roadmap

> **Planning authority and phase-numbering rule (reconciled 2026-11-18):** `docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md` owns the only authoritative cross-platform Phase 0–10 sequence and exit gates. The older numbered sections below are retained as a subsystem/platform inventory for historical implementation tracking; their old phase numbers are **not** the master phase numbers and must not be used to select the next milestone. In particular, the old “64-bit boot and kernel foundation” section is already-established platform groundwork, not Master Phase 1. Boot Protocol v1/UEFI work is a separate boot-platform workstream and is not a prerequisite that may displace Master Phase 1. The current dependent implementation milestone is **Master Phase 1 — Native microkernel and process-execution foundation**: audit/reuse PMM/VMM, process/thread, scheduler, ELF, usercopy, capability/object and IPC primitives; complete transactional native spawn into CPL3; and harden retry-safe wait/reap with fault-injection and exact-head build, focused tests, QEMU and security gates. Do not begin Linux/Windows runtimes or later master phases before its exit gate passes.


## Filesystem scope gate — 2026-10-25

The owner-confirmed release architecture is native ProsenjitFS as the only filesystem implementation in the kernel core. FAT32, exFAT, EXT/EXT4, NTFS, and future formats are separate providers behind the common VFS interface. **ProsenjitOS 1.0 requires FAT32 and exFAT support**, including end-to-end USB removable-media paths: USB mass storage, block-device registration, partition discovery, filesystem provider mount, VFS file I/O, safe unmount, and media-removal tests. **EXT/EXT4 and NTFS are deferred to later releases**, not 1.0 blockers. None of these compatibility formats is part of native ProsenjitFS.

The provider ABI/registration boundary and FAT32/exFAT provider adapters are implemented and integrated with the VFS for deterministic synthetic-media and QEMU tests. The current main-branch filesystem status records exact post-merge Build/QEMU and Security verification for the FAT32/exFAT provider. This does not mean physical USB removable-media support is complete: live host-controller start/DMA, USB enumeration, BOT transport binding, physical LUN block-device registration, partition discovery from USB media, safe unmount, and removal/reinsert handling remain separate 1.0 gates. Userspace filesystem isolation is still a future design target, not a completed runtime.

All current filesystem decisions, exact-head evidence, implementation status, security gates, and preserved historical plans are centralized in [docs/FILESYSTEM.md](FILESYSTEM.md). Legacy filesystem documents are navigation stubs only.

## Native ProsenjitFS release gates — owner decision (2026-10-25)

- **1.0:** native filesystem format/mount, allocation and inode/block bounds checks, directory namespace operations, file read/write/seek/truncate within documented limits, VFS integration, metadata validation/checksums where defined, journal/recovery tests, corruption/error-path tests, and **enforced permissions** (UID/GID/mode plus trusted VFS/kernel authorization; allowed/denied operation tests; denied operations must not mutate persistent media). These are acceptance targets, not a claim that all are implemented.
- **1.1:** **mandatory native ProsenjitFS encryption**. Requires reviewed threat model/format, vetted authenticated encryption, key lifecycle, authenticated metadata/data I/O, nonce uniqueness, tamper/wrong-key handling, journal/recovery integration, and exact-candidate tests. No custom cryptography; fail closed on authentication failure.
- **1.2 and later:** remaining native features staged after the above gates, such as extended ACLs, quotas, snapshots, compression, deduplication, online checking/repair, performance improvements, and stronger concurrency/durability. These are backlog candidates, not fixed promises.
- The detailed feature contract and caveats live in [docs/FILESYSTEM.md](FILESYSTEM.md).

## Current status — 2026-09-30

The repository has moved beyond the original boot/kernel-only milestone. The kernel core contains only native ProsenjitFS. FAT32, exFAT, NTFS, and future compatibility filesystems must be implemented as modular external providers behind the VFS/provider ABI, so new formats can be added without adding their parsers to kernel core. All filesystem architecture, implementation status, compatibility-provider work, release gates, and test evidence are tracked in the single source of truth: [docs/FILESYSTEM.md](FILESYSTEM.md). Do not create separate filesystem planning/status/test documents; update FILESYSTEM.md.

### Current engineering focus
1. **Filesystem workstream (single source of truth: [docs/FILESYSTEM.md](FILESYSTEM.md)):** finish native ProsenjitFS storage, transaction, crash, recovery and LBA verification; define the stable external provider ABI; then implement FAT32/exFAT/NTFS providers outside kernel core.
4. **Security integration:** connect real filesystem-backed file reads/execute paths to the existing File admission/security broker and quarantine boundary, then add replaceable external reputation-provider adapters without making cloud access mandatory.
5. **I/O foundation:** the driver model foundation is now implemented around the PCI enumeration layer; next work is the device permission broker and concrete keyboard/display/network device abstractions.
6. **User mode:** stabilize the process startup path, ELF loading, init/service management, and basic CUI utilities before compatibility layers.
7. **Regression/CI:** keep every storage/security milestone tied to QEMU boot markers and focused synthetic filesystem tests; investigate any failed CI run before starting the next dependent milestone. The latest GitHub Actions runner failure occurs before workflow steps are created, so post-change CI verification remains pending.
8. **Desktop/compatibility strategy:** use mature existing desktop/display components first (for example XFCE with existing X11/Xorg and/or Wayland components); do not build a new windowing system at this stage.
9. **Windows/OneDrive compatibility:** track Win32/Office application compatibility and reliable OneDrive-style background synchronization as long-term application-level compatibility targets.
10. **Self-healing/recovery architecture:** add layered health detection and safe recovery without allowing the recovery system to become a new source of corruption. The design must follow **Detect -> Verify -> Repair -> Verify -> Rollback** and keep destructive repair disabled by default.
11. **Display binding:** kernel owns only device discovery/binding, framebuffer/backend validation, permissions and lifecycle; window systems and desktop components remain userspace.\n\n- **Driver compatibility:** build an independently implemented, ReactOS-inspired NT/WDM compatibility track after the native driver and package-management foundations are stable. This is a long-term compatibility subsystem, not a replacement for native drivers.

### Latest verified repository work
- PCI configuration/enumeration foundation implemented and marked complete.
- Firewall-inspired unknown-threat handling documented as a standing security requirement.
- Signature-verification trust boundary contains a freestanding Ed25519 verification implementation with strict point/scalar validation, SHA-512 challenge hashing, subgroup-small-order rejection, and an RFC 8032 known-answer self-test; source-level implementation is present, but current GitHub Actions runner failures prevent claiming current crypto CI verification. RSA-PSS remains fail-closed as unsupported.
- Ed25519 field arithmetic foundation now uses a 5×51-bit representation with canonical encoding/reduction, explicit carry-safe multiplication, subtraction, and constant-schedule inversion; the extended-coordinate point layer now includes identity, complete add/double formulas, curve-equation checking, canonical point decoding with sign-bit recovery, affine encoding, and base-point round-trip self-tests. Ed25519 scalar reduction/multiplication and signature verification are now implemented and startup-gated by self-tests. GitHub Actions currently starts push runs but the runner job is failing before any workflow step is created, so this new crypto implementation remains unverified by CI.
- Both repository GitHub Actions workflows were green on the last verified storage/VFS commit (`3ed84998`). The Up-case parser work is intentionally isolated on a feature branch until CI verifies it.

### CI note
The repository contains build/boot and security-architecture GitHub Actions workflows. The latest verified storage/security implementation passed both repository GitHub Actions workflows: build/boot and security-architecture CI. Future storage changes must preserve both gates.

## Version 2 — Unified Application Runtime (design approved; implementation pending)

Version 2 targets a single ProsenjitOS desktop for native applications and supported Windows applications, without requiring a complete Windows OS installation, a Windows VM as the normal app path, or Wine as a runtime dependency. Windows binaries still require an independently implemented or properly licensed Windows ABI/API runtime; this is a substantial compatibility-engineering effort, not something that can be omitted.

The cross-platform compatibility master plan, **sole authoritative dependency order**, phase exit gates, shared HANDLE/FD and VFS/IPC boundaries, driver isolation rules, and AI/human handoff instructions are documented in [docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md). The detailed Windows runtime is subordinate in [docs/V2-UNIFIED-APPLICATION-RUNTIME.md](V2-UNIFIED-APPLICATION-RUNTIME.md); it maps its work to the master phases and must not create a competing sequence.

- [ ] Stabilize and verify kernel/process/syscall/VFS/security/userspace prerequisites.
- [ ] Implement a bounded PE32/PE32+ parser and loader with malformed-input tests.
- [ ] Run a minimal controlled Windows console application through a user-space runtime.
- [ ] Add only the Windows API/DLL behavior required by selected, documented test applications.
- [ ] Integrate supported GUI applications into the existing desktop/window system.
- [ ] Implement safe application installation, dependency management, update, and uninstall flows.
- [ ] Maintain a tested application/API compatibility matrix; do not claim universal support.
- [ ] Keep Windows kernel-driver execution as a separate later research track; never load Windows `.sys` files directly as native ProsenjitOS kernel drivers.

**Sequencing rule:** follow the exact Phase 0–10 order and exit gates in the compatibility master specification. The checkboxes above summarize Version 2 outcomes; they are not an independent implementation order. Version 2 remains a target, not a claim that Windows applications currently run.

## Historical platform inventory — Repository bootstrap (legacy numbering removed)
- [x] Repository created
- [x] Initial README
- [x] License and ignore rules
- [x] Architecture/security requirements documented
- [x] First bootable image

## Platform foundation — 64-bit boot and kernel foundation (not Master Phase 1)
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

## CPU and memory isolation foundation (legacy subsystem inventory)
- [x] GDT/IDT
- [x] Interrupt and exception handlers
- [x] Timer (PIT channel 0, IRQ0 tick source)
- [x] Physical page allocator
- [x] Virtual memory and paging (4-level mappings, canonical/physical bounds, W^X/NX checks, duplicate-map protection, translation/query invariants and deep self-test)
- [x] Kernel heap (bounded growth, overflow-safe allocation, ownership-checked free, coalescing and deep self-test)
- [x] User/kernel address-space separation foundation (isolated user mappings, ownership tracking, alias protection, range validation, teardown and deep self-test)
- [~] Process/thread/syscall runtime verification (process lifecycle/reap, address-space switching, scheduler/run-queue, process-thread teardown, IPC block/wake, synthetic interrupt-frame save/restore, syscall ABI/allowlist/unknown-call checks; real CPL3 context-frame lifetime and full privilege-return switching remain unverified)
- [x] NX/W^X memory policy where supported

## Kernel architecture foundation (legacy subsystem inventory)
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
  - [~] Timer-driven preemption (kernel IRQ0 path exists; user-mode IF is deliberately cleared, so CPL3 timer preemption is not enabled)
- [x] Context switching foundation (kernel-stack switch + callee-saved state)
- [~] Preemptive interrupt-frame context switching (kernel-thread/QEMU foundation exists; complete CPL3 frame lifetime and privilege-return handoff are not yet verified)
  - [~] Interrupt-frame ABI and scheduler hand-off foundation (synthetic-frame tests exist; stable per-thread CPL3 entry-frame ABI remains pending)
  - [x] Kernel-thread timer preemption validated under QEMU
  - [ ] Stable per-thread syscall/interrupt frame storage and complete CPL3 IRET frame (RSP/SS) preservation
  - [~] TSS RSP0/IST policy: per-thread RSP0 plus dedicated IST1 NMI, IST2 double-fault, and IST3 page-fault stacks; per-CPU TSS/IST ownership and stack-guard validation remain pending
  - [ ] Real assembly-entry/return regression tests for CPL0 and CPL3
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

## Security foundation (legacy subsystem inventory)
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

## I/O and drivers foundation (legacy subsystem inventory)
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

## Storage and networking foundation (legacy subsystem inventory)
- [ ] VFS
  - [x] VFS mount object foundation
  - [x] Native ProsenjitFS filesystem type contract
  - [x] Path, vnode/inode, file and directory APIs
  - [x] Filesystem probe and partition integration
  - [x] Native ATA -> MBR -> ProsenjitFS persistent mount regression
- [ ] External compatibility filesystem providers
  - [x] Provider ABI/registration boundary
  - [ ] NTFS provider (external to kernel; deferred beyond 1.0)
  - [x] exFAT provider (external to kernel; deterministic fixture/QEMU coverage)
  - [x] FAT32 provider (external to kernel; deterministic fixture/QEMU coverage)
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

## User mode and CUI foundation (legacy subsystem inventory)
- [x] User address spaces
- [x] ELF64 loader (ET_EXEC validation, PT_LOAD mapping, NX/W^X enforcement, argv/env stack construction, rollback; dynamic relocation deferred)\n- [x] ELF64 loader runtime verification (boot-time real address-space mapping/permission checks, loaded-code byte check, argv/env stack/argc validation, then CPL3 entry; dynamic relocation deferred)
- [ ] Process startup
- [ ] Init/service manager
- [ ] Secure shell
- [ ] Basic utilities
- [ ] Package manager

## GUI foundation (legacy subsystem inventory)

- [~] Initial userspace DML registry/preflight source (versioned size-tagged manifests, relative-path validation, required-feature checks, bounded registry, dependency/cycle checks, default selection); host unit tests added but not yet run.

**Architecture and release gates:** follow [Desktop Implementation Plan](DESKTOP-IMPLEMENTATION-PLAN.md). The DML and all desktop environments remain userspace components; stable versioned ABI, capability negotiation, service ownership/isolation, validated desktop manifests, transactional package updates/rollback, and measured performance/security gates are prerequisites—not claims of current implementation.

- [ ] Versioned kernel-userspace and shared-service contracts with compatibility tests
- [ ] Shared-service ownership, per-user session isolation, bounded failure/restart policies
- [ ] Validated desktop manifests and deterministic dependency/capability resolution
- [ ] Trusted package install/update with integrity checks and safe rollback
- [ ] Desktop lifecycle, crash recovery, and malformed-input/security tests
- [ ] Repeatable boot/session, idle CPU/RSS, input latency, frame pacing, IPC, and recovery baselines
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

## Application compatibility backlog (legacy subsystem inventory)
- [ ] Native ProsenjitOS SDK
- [ ] POSIX-compatible user API
- [ ] Linux ELF compatibility environment
- [ ] Linux application packaging/container integration
- [ ] Independent Windows App Runtime (PE/DLL loader and selected Windows API contracts; no Wine runtime dependency)
- [ ] Windows application installer
- [ ] Microsoft Office compatibility validation (Word/Excel/PowerPoint/Outlook)
- [ ] OneDrive-compatible authentication and background-sync validation
- [ ] Office + cloud-sync file-locking/conflict/rename/move validation
- [ ] Compatibility sandbox profiles
- [ ] Application permissions UI

## AI API backlog (legacy subsystem inventory)
- [ ] OS AI service
- [ ] Stable AI API for applications
- [ ] Permission broker for AI requests
- [ ] Remote AI provider interface
- [ ] Local model provider interface
- [ ] LAN AI provider interface
- [ ] Credential isolation
- [ ] AI data/privacy policy

## Hardening backlog (legacy subsystem inventory)
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

## Additional architecture requirements — compatibility, isolation and recovery

**Status:** approved design direction / planned work. The entries below are not implementation claims. The master dependency order and technical contracts are in [docs/COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md).

### Microkernel-oriented kernel evolution

- [ ] Keep the current kernel architecture description factual; do not call the current kernel a completed microkernel.
- [ ] Audit and harden PMM physical-page ownership, reference lifetime and reclamation.
- [ ] Verify VMM address-space isolation, W^X/NX, bounds/overflow checks, mapping rollback, TLB invalidation and teardown.
- [ ] Verify real CPL3 entry/return, per-thread interrupt/syscall frame lifetime and privilege-return context.
- [ ] Define a versioned user/kernel service ABI and service ownership model.
- [ ] Move non-critical complex services to isolated user-space processes incrementally, only after stable ABI and recovery tests.
- [ ] Keep kernel authority over capabilities, validated IPC, memory mappings, interrupts and DMA.

### IPC and shared-memory performance

- [ ] Preserve bounded copy-based IPC as the correctness baseline.
- [ ] Add capability-controlled shared-memory/page transport only after benchmark evidence.
- [ ] Define message limits, sender identity, queue back-pressure, timeout/cancel, endpoint revocation and process-death cleanup.
- [ ] Add concurrency, denial, malformed-message, teardown-race and performance regression suites.

### Linux ELF and syscall compatibility

- [ ] Keep native ELF execution separate from the Linux/POSIX compatibility profile.
- [ ] Implement a Linux process-entry adapter and explicit guest register/stack/TLS contract.
- [ ] Implement a dedicated Linux syscall-number/argument translator; never dispatch guest syscall numbers as native syscall IDs.
- [ ] Add supported syscall/API matrix, errno mapping, signals, threads and synchronization incrementally.
- [ ] Validate with named real ELF binaries and deterministic unsupported-call tests.

### Windows PE, DLL and API compatibility

- [ ] Extend PE metadata parsing into a bounded user-space image loader: mapping, relocations, section permissions, imports/exports, TLS, entry point and teardown.
- [ ] Implement a per-process DLL resolver with safe search order, module provenance, dependency/version checks and API-set redirection.
- [ ] Implement selected Win32/NT API behavior behind a versioned ABI; do not count success-only stubs as implemented APIs.
- [ ] Add Windows file/path semantics, handle semantics, console and later GUI behavior as independently tested subsets.
- [ ] Keep native libraries and Windows compatibility modules in separate namespaces.

### Unified exception model

- [ ] Define a sanitized kernel-owned fault-event structure and validated user-context delivery.
- [ ] Implement Windows SEH/NT exception adapter separately from Linux signal adapter.
- [ ] Add x64 unwind metadata and stack-unwind validation before claiming relevant SEH support.
- [ ] Add fault-injection coverage for access violation, illegal instruction, arithmetic/stack faults, nested faults, invalid unwind data and signal-mask behavior.
- [ ] Bound handler failure, repeated fault and recovery loops; reject invalid user resume contexts.

### Isolated drivers, watchdog and fallback

- [ ] Run user-mode device services/drivers in separate protected processes with private address spaces and resource budgets.
- [ ] Broker device access through revocable per-device capabilities; no raw kernel pointers or unrestricted physical-memory mapping.
- [ ] Add DMA pin/map/unmap and IOMMU policy; use safe bounce buffers or deny DMA if isolation cannot be guaranteed.
- [ ] Add supervisor/watchdog with bounded probe/request timeouts, crash detection, capability revocation, safe quiescence, restart back-off and crash-loop quarantine.
- [ ] Add crash recovery proving one driver failure cannot corrupt kernel or unrelated processes.
- [ ] Add validated display fallback (VBE/GOP/framebuffer), storage fallback (supported ATA PIO/VirtIO block), network fallback (supported VirtIO/generic backend), and generic HID/serial recovery input.
- [ ] If no safe fallback exists, mark the device unavailable and continue in degraded mode where possible; optional device failure must not hang boot.
- [ ] Add primary-failure/fallback and no-fallback regression tests for each actually supported backend.

**Roadmap sequencing rule:** [COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md) owns the one authoritative Phase 0–10 dependency order. This section records additional subsystem requirements only. Do not parallelize dependent milestones as if their prerequisites were complete.
