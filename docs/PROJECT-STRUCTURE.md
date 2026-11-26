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
  -> ProsenjitFS
  -> future external filesystem providers
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
├── ProsenjitFS
├── future external filesystem providers
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

ProsenjitFS is the only filesystem currently linked into the kernel. FAT32, exFAT and NTFS compatibility support is intentionally externalized and must not be reintroduced into kernel-core.

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
- This is the monitoring foundation plus bounded persistent rotation/retention, crash-record persistence infrastructure with safe-context flushing, fatal-exception/recovery capture, and watchdog/subsystem event hooks; full runtime service-manager integration, fatal-path disk persistence, and network-stack instrumentation remain separate completion gates.

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
- Persistent retention/rotation is implemented; a bounded raw-block crash record store and safe-context flush API are implemented when a validated non-overlapping crash slot is available. Fatal exception capture currently records a pending crash in memory; reboot-surviving persistence from the fatal path still requires an explicitly safe recovery/reset mechanism.


- The health monitor now exposes a bounded deterministic text export of retained structured events and expanded VFS mutation/directory counters.
- Security authorization decisions and driver bind/unbind/failure transitions now emit bounded health events. Network-class PCI device lifecycle transitions now emit bounded network telemetry events for discovery/removal, bind/unbind, and probe failure; unsupported link/packet metrics remain explicit rather than fabricated.
- Export is caller-buffer bounded and does not allocate dynamically.
- The export is diagnostic transport only; persistent logs and reboot-surviving crash records are separate bounded transports and are not boot/corruption dependencies.

### Required logging architecture
- A central structured event/log collector interface.
- Persistent local logs suitable for post-crash diagnostics, with bounded generation rotation and retention.
- Severity/category/source/timestamp metadata.
- Boot, kernel panic and crash logs must survive reboot where technically
  possible.
- Log rotation, retention and bounded storage consumption, including safe rotation of pre-existing oversized active logs.
- Explicit privacy/security rules for sensitive log data.
- Deterministic diagnostic export for support and CI/QEMU regression.
- Monitoring must degrade safely if the logging backend is unavailable; persistence failures are
  recorded in the authoritative in-memory health ring, and logging must never become a new
  kernel corruption or boot dependency.

### Completion rule
Monitoring/logging is complete only after the interfaces, persistent storage,
rotation/retention, crash/panic capture, service/process health, network/security
events and automated regression coverage are implemented and verified. The current
crash record store is active only when its raw slots are proven outside discovered
partitions; service-manager and real network-stack instrumentation remain pending
until those subsystems exist. Filesystem error hooks, security-admission events, boot recovery checkpoints, and watchdog
checkpoint integration are part of the implementation. Network instrumentation remains limited
to the existing subsystem hook boundary until a real network stack exists. Do not mark this
requirement complete merely because debug prints exist.

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


## 11. Mandatory pull-request progress records

Every pull request must maintain a current, evidence-based implementation status record. This is a repository-wide requirement, not a PR #270-specific convention. The record must distinguish implemented code, partial work, design-only material, unimplemented requirements, blockers, and tests actually executed on the exact HEAD. It must include the current/base branches, exact HEAD SHA, source mapping, remaining work, regression risks, test/QEMU/CI evidence, completion gates, and next safe action.

Follow [PULL-REQUEST-IMPLEMENTATION-STATUS-STANDARD.md](PULL-REQUEST-IMPLEMENTATION-STATUS-STANDARD.md). Prefer updating the existing subsystem document; create a dedicated PR progress record only when no suitable document exists. Update it at each logical milestone and before handing work to another human or AI agent. A changed HEAD invalidates the claim that earlier exact-HEAD CI verified the new revision. Never report a PR complete while any applicable requirement or verification gate remains partial, blocked, failed, pending, or not run.
