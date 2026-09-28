# ProsenjitOS Roadmap

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
- [ ] IPC
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
- [ ] PCI enumeration
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
  - [ ] Read-only FAT32 directory/file reader
  - [ ] FAT32 write support
- [ ] exFAT filesystem support
  - [ ] Read-only exFAT mount/parser
  - [ ] exFAT write support
- [ ] NTFS filesystem support
  - [x] NTFS compatibility architecture documented
  - [ ] Read-only NTFS mount/parser
  - [x] NTFS safe overwrite of existing unnamed $DATA (no allocation/growth)
  - [ ] NTFS full write support (allocation, growth, metadata updates)
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
- [ ] Window manager/compositor
- [ ] Desktop shell
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
- [ ] Secure/verified boot
- [ ] Kernel self-protection
- [ ] Driver signing policy
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

## Long-term
- [ ] USB
- [ ] SMP/multi-core scheduling
- [ ] Power management
- [ ] UEFI
- [ ] Developer SDK
- [ ] Software repository
- [ ] Full desktop environment

ProsenjitOS is an independent project and does not contain Microsoft proprietary source code.
