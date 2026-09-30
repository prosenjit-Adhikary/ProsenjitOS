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
- kernel logging, panic/crash diagnostics, and monitoring hooks

### include/
Public/shared interfaces and user/kernel ABI definitions.
Private kernel implementation headers stay under kernel/include.

### scripts/
Reproducible build and image construction only. Image layout must be
documented and deterministic.

### docs/
Architecture, security contracts, storage formats, roadmap, recovery design,
monitoring/logging requirements, and CI/regression contracts.

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
├── security admission/quarantine
└── logging/monitoring/crash diagnostics

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

## 9. Monitoring and logging contract

Monitoring and logging are mandatory platform infrastructure, not optional
debugging features.

### Required monitoring domains
- System health: CPU, memory, storage, filesystem, boot and kernel health.
- Process/service health: lifecycle, failures, hangs and bounded watchdog events.
- Network: device/link state and security-relevant network events.
- Security: authentication/authorization, capability, policy, quarantine and
  other security events.
- Filesystem/storage: mount, I/O, journal/recovery, corruption and integrity
  events.
- Driver/device: discovery, bind/unbind, faults and permission changes.
- Kernel: warnings, exceptions, faults, panic and crash diagnostics.

### System health monitoring foundation
- A bounded structured health-event ring is now available to kernel subsystems.
- Health events carry sequence, timer timestamp, severity, category, source, code and value metadata.
- Health snapshots expose heap usage and event/drop counters without requiring dynamic allocation.
- The initial monitor self-test and boot-time sample run after kernel heap initialization.
- This is the monitoring foundation plus bounded persistent rotation/retention, fatal-exception/recovery capture and watchdog/subsystem event hooks; reboot-surviving panic persistence and full runtime service/network instrumentation remain separate completion gates.

### Central diagnostic export
### Central logging facade
### Persistent log sink boundary
- An optional VFS-backed persistent sink is now defined with bounded file size, rotation threshold, and file-count policy.
- The sink attaches only to an active writable VFS mount and uses the existing VFS file contract.
- Persistence is non-authoritative: sink absence/failure does not fail the central health event path.
- Rotation/retention execution is now implemented with bounded generation shifting and oldest-generation removal; filesystem failures fail closed. Fatal exception events are captured in the health ring and bounded watchdog/subsystem hooks are available.


- Kernel logging now has a single structured emission boundary in `kernel/log.c`.
- The first sink is the bounded health-event ring; future serial/persistent sinks can be attached without changing callers.
- Sink availability is explicit, and logging remains non-authoritative for boot/storage correctness.
- Persistent retention/rotation is implemented; crash-survival across reboot remains a separate milestone.


- The health monitor now exposes a bounded deterministic text export of retained structured events and expanded VFS mutation/directory counters.
- Security authorization decisions and driver bind/unbind/failure transitions now emit bounded health events; no unsupported network metrics are synthesized.
- Export is caller-buffer bounded and does not allocate dynamically.
- The export is diagnostic transport only; it does not yet provide persistent-on-disk retention or crash-survival across reboot.

### Required logging architecture
- A central structured event/log collector interface.
- Persistent local logs suitable for post-crash diagnostics.
- Severity/category/source/timestamp metadata.
- Boot, kernel panic and crash logs must survive reboot where technically
  possible.
- Log rotation, retention and bounded storage consumption.
- Explicit privacy/security rules for sensitive log data.
- Deterministic diagnostic export for support and CI/QEMU regression.
- Monitoring must degrade safely if the logging backend is unavailable; logging
  must never become a new kernel corruption or boot dependency.

### Completion rule
Monitoring/logging is complete only after the interfaces, persistent storage,
rotation/retention, crash/panic capture, service/process health, network/security
events and automated regression coverage are implemented and verified. Do not
mark this requirement complete merely because debug prints exist.

## 10. Completion gates

### Gate A — Build
ISO and native BIOS image build with warnings-as-errors.

### Gate B — Boot
GRUB ISO boot and native BIOS boot with deterministic serial markers.

### Gate C — Kernel
Memory, interrupts, scheduler, process, syscall, IPC, usercopy, kernel
logging, panic/crash diagnostics and monitoring-hook regressions.

### Gate D — Security
Capability, sandbox, admission, quarantine, Reason Ledger and security-event
logging regressions.

### Gate E — Storage
Block device -> partition -> filesystem -> VFS regressions, including
filesystem/storage monitoring events where applicable.

### Gate F — User mode
ELF -> process startup -> init -> CUI utilities.

### Gate G — Desktop
Display/input -> existing desktop integration -> security UI.

### Gate H — Compatibility
POSIX/Linux/Windows application layers with isolated profiles.

### Gate I — Release security
Signing, verified boot, updates, rollback, fuzzing, recovery and release
artifact verification.

### Gate J — Observability
Structured logging, persistent logs, rotation/retention, boot/crash/panic
diagnostics, system/process/service/network/security/filesystem monitoring,
diagnostic export and regression coverage.

A gate is only marked complete after its automated regression exists and the
corresponding CI run is green.

## 11. Current execution rule

The project is now in a structure-first stabilization stage.

1. Finish missing architectural interfaces and deterministic test hooks.
2. Do not hide incomplete features by marking roadmap items complete.
3. Treat monitoring/logging and diagnostic infrastructure as mandatory platform
   requirements, not optional polish.
4. Run the full CI matrix.
5. Fix build errors first.
6. Fix boot/runtime regressions second.
7. Fix storage/filesystem regressions third.
8. Fix security regressions fourth.
9. Verify observability/diagnostic regressions alongside the affected subsystem.
10. Only after the current baseline is green, start the next dependent feature.
