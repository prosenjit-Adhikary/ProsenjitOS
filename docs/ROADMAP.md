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
- [ ] Timer
- [x] Physical page allocator
- [x] Virtual memory and paging
- [x] Kernel heap
- [ ] User/kernel address-space separation
- [ ] NX/W^X memory policy where supported

## Phase 3 — Kernel architecture
- [ ] Kernel object model
- [ ] Protected handles
- [ ] Process manager
- [ ] Thread scheduler
- [ ] Context switching
- [ ] System-call boundary
- [ ] IPC

## Phase 4 — Security foundation
- [ ] Per-process security identity
- [ ] Capability/permission model
- [ ] Protected object manager
- [ ] Application sandbox
- [ ] Filesystem access control
- [ ] Process isolation
- [ ] Device permission broker
- [ ] Network permission policy
- [ ] Security event log
- [ ] Signed package verification
- [ ] Controlled driver loading

## Phase 5 — I/O and drivers
- [ ] Driver model
- [ ] Keyboard
- [ ] Storage
- [ ] PCI enumeration
- [ ] Display abstraction
- [ ] Network device abstraction

## Phase 6 — Storage and networking
- [ ] VFS
- [ ] FAT-compatible filesystem support
- [ ] Native filesystem design
- [ ] Ethernet
- [ ] ARP
- [ ] IPv4
- [ ] UDP/TCP
- [ ] Network sandbox policy

## Phase 7 — User mode and CUI
- [ ] User address spaces
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

## Long-term
- [ ] USB
- [ ] SMP/multi-core scheduling
- [ ] Power management
- [ ] UEFI
- [ ] Developer SDK
- [ ] Software repository
- [ ] Full desktop environment

ProsenjitOS is an independent project and does not contain Microsoft proprietary source code.