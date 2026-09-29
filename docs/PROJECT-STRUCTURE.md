# ProsenjitOS — Full Engineering Structure

Status: architecture/foundation structure defined on 2026-09-29.

This document is the execution map for completing ProsenjitOS. It separates
boot, kernel, security, storage, drivers, user mode, GUI, compatibility, AI,
and recovery so later work cannot accidentally bypass an architectural
boundary.

## 1. Layered dependency order

Firmware
  -> Native BIOS/UEFI loader or GRUB adapter
  -> Stable ProsenjitOS boot protocol
  -> Kernel entry
  -> HAL / interrupt / memory / scheduler
  -> Object + capability + syscall + IPC security boundary
  -> Device/driver broker
  -> Block-device layer
  -> Partition layer (MBR/GPT)
  -> VFS
  -> FAT32 / exFAT / NTFS / ProsenjitFS
  -> File-security admission/quarantine
  -> System services
  -> User processes / ELF loader / init
  -> CUI + GUI
  -> SDK / POSIX / Windows compatibility
  -> Package manager / updates / AI services

A higher layer must consume a lower-layer contract; it must not reach around
that contract to access hardware or private implementation details.

## 2. Repository ownership

### boot/
Boot-only code. It loads the kernel and establishes the boot contract.
It must not mount filesystems or implement VFS/storage policy.

### kernel/
Freestanding kernel implementation:
- entry, interrupts, CPU state
- memory/paging/heap
- processes/threads/scheduler
- syscall/IPC
- object/capability security
- device and block interfaces
- filesystem providers
- VFS and partition discovery
- security admission/quarantine

### include/
Public/shared interfaces and user/kernel ABI definitions.
Private kernel implementation headers stay under kernel/include.

### scripts/
Reproducible build and image construction only. Image layout must be
documented and deterministic.

### docs/
Architecture, security contracts, storage formats, roadmap, recovery design,
and CI/regression contracts.

### .github/workflows/
Build/boot and security gates. Every new hardware/storage/security milestone
must have a deterministic regression marker before it is called verified.

## 3. Kernel subsystem tree

kernel/
├── CPU/boot entry
├── interrupts/PIC/IDT
├── memory/paging/heap/address spaces
├── process/thread/scheduler
├── syscall/usercopy
├── object/capability/intent/security
├── IPC
├── driver/device layer
├── PCI
├── ATA PIO (current real block provider)
├── block device
├── partition discovery
├── VFS
├── FAT32
├── exFAT + Up-case
├── NTFS
├── ProsenjitFS
├── storage discovery
└── security admission/quarantine

## 4. Storage path

Hardware
  -> ATA/AHCI/NVMe/USB provider
  -> pros_block_device
  -> MBR/GPT partition device
  -> filesystem probe
  -> VFS mount
  -> vnode/file/directory API
  -> filesystem-specific implementation
  -> security admission boundary

Current real path:
ATA PIO -> MBR -> ProsenjitFS -> mount/unmount -> rediscovery.

Required future providers:
AHCI, NVMe, USB mass storage.

FAT32, exFAT and NTFS must remain available; ProsenjitFS must not replace
them or become a requirement for ordinary removable-media access.

## 5. Security path

Process/file/device/network request
  -> identity
  -> capability/rights
  -> intent
  -> policy
  -> Reason Ledger
  -> allow / restricted / suspend / quarantine / deny
  -> auditable result

AI/security services are advisory only. Kernel policy remains authoritative.

## 6. User-mode path

ELF loader
  -> process creation
  -> address-space setup
  -> initial capabilities
  -> init/service manager
  -> shell/basic utilities
  -> package manager
  -> desktop/session

No GUI or compatibility layer should be allowed to become a privileged
kernel shortcut.

## 7. Compatibility path

Native API/SDK
  -> POSIX compatibility
  -> Linux application environment
  -> Windows API compatibility

Compatibility is user-space. Native kernel drivers remain ProsenjitOS
drivers.

## 8. Recovery path

Detect
  -> Verify
  -> Repair
  -> Verify
  -> Rollback if verification fails

Recovery must be non-destructive by default. Integrity checking and journal
replay must be independently testable before automatic repair is enabled.

## 9. Completion gates

### Gate A — Build
ISO and native BIOS image build with warnings-as-errors.

### Gate B — Boot
GRUB ISO boot and native BIOS boot with deterministic serial markers.

### Gate C — Kernel
Memory, interrupts, scheduler, process, syscall, IPC and usercopy regressions.

### Gate D — Security
Capability, sandbox, admission, quarantine and Reason Ledger regressions.

### Gate E — Storage
Block device -> partition -> filesystem -> VFS regressions.

### Gate F — User mode
ELF -> process startup -> init -> CUI utilities.

### Gate G — Desktop
Display/input -> existing desktop integration -> security UI.

### Gate H — Compatibility
POSIX/Linux/Windows application layers with isolated profiles.

### Gate I — Release security
Signing, verified boot, updates, rollback, fuzzing, recovery and release
artifact verification.

A gate is only marked complete after its automated regression exists and the
corresponding CI run is green.

## 10. Current execution rule

The project is now in a structure-first stabilization stage.

1. Finish missing architectural interfaces and deterministic test hooks.
2. Do not hide incomplete features by marking roadmap items complete.
3. Run the full CI matrix.
4. Fix build errors first.
5. Fix boot/runtime regressions second.
6. Fix storage/filesystem regressions third.
7. Fix security regressions fourth.
8. Only after the current baseline is green, start the next dependent feature.
