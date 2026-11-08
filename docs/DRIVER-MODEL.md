# ProsenjitOS Driver Model

Status: foundational driver architecture implemented on 2026-09-30.

## Purpose

The driver model separates hardware discovery from driver implementation. Kernel
subsystems consume stable device contracts; they do not contain vendor-specific
probe logic.

Hardware/bus discovery -> device identity -> driver registry -> hardware-ID
matching -> controlled bind -> driver-owned private state -> generic subsystem
contract -> unbind/remove.

## Contracts

A pros_driver_device is a discovered device record containing a bus-neutral
hardware identity, device class, lifecycle state, optional bus data, and
the currently bound driver.

A pros_driver declares a stable name/version, driver ABI version, native or
compatibility origin, policy flags, a bounded hardware-ID match table, and
probe/remove/suspend/resume callbacks.

Drivers are matched by bus plus vendor/device/class/subclass/programming-interface
identity. Zero or all-ones ID fields are wildcards.

## Lifecycle

DISCOVERED -> BINDING -> BOUND is the normal path.
A failed probe becomes FAILED.
BOUND -> DISCOVERED is the normal unbind path.
A removed device becomes REMOVED and cannot be rebound.

A driver cannot be unregistered while it owns a device.

## Device permission broker contract

The driver registry deliberately does not grant device authority to applications.
The next security layer is a small capability-oriented device permission broker
between a process and a discovered device.

The broker contract should enforce these invariants:

1. A device must exist and be in a usable state before access can be granted.
2. The requested device class must match the class authority being requested.
3. A process receives no implicit device authority from knowing a device ID/name.
4. Grants are explicit and bounded to a device, process, and permission mask.
5. Unsupported or malformed requests fail closed.
6. Revocation must invalidate future access without modifying driver state.
7. The broker must not bypass existing process capability/permission checks.
8. Audit/reason information should identify the device and requested authority
   without exposing unnecessary hardware metadata.

Initial permission classes are intentionally small: READ, WRITE, CONTROL, and
MAP. Sensitive classes such as network configuration or raw DMA should remain
separate policy decisions rather than being implied by generic device access.

The first implementation target is a deterministic kernel policy primitive and
self-test; user-facing permission prompts and actual keyboard/display/network
drivers remain separate milestones.

## Native vs compatibility

Native drivers are the primary kernel-driver path. Compatibility drivers have a
separate origin and are selected through the same controlled registry rather than
being loaded as arbitrary kernel binaries.

Windows .sys binaries are not accepted as native drivers. A future Windows
package importer/compatibility service must inspect package metadata and route
compatible devices through an adapter layer.

## Compatibility driver execution boundary

The native ProsenjitOS driver ABI remains the preferred hardware path. A compatibility-origin field or package match is not permission to execute a foreign kernel binary.

- Linux .ko and Windows .sys files are not directly loadable as native ProsenjitOS drivers.
- Linux kernel-driver compatibility and Windows NT/WDM/NDIS compatibility are separate, long-term ABI/framework projects; neither is implied by the current registry or package-inspection scaffolding.
- Start with native drivers and isolated user-mode device services using the device permission broker where the device class permits it.
- Any future kernel-mode compatibility adapter requires an exact OS/framework/architecture ABI target, threat model, package trust/signing policy, capability-mediated device ownership, interrupt/DMA/MMIO mediation, cancellation and teardown, rollback, and hardware-specific tests.
- A Windows IRP shim must implement the real supported WDM contract, including IO_STACK_LOCATION stack entries, pending/completion/cancellation, MDLs, IRQL constraints, PnP/power and driver-object lifetime. A shortened illustrative IRP structure or direct physical-address-to-UserBuffer mapping is not an implementation.
- Do not map IRQL to scheduler priority alone. Do not pass raw physical addresses as user pointers. DMA requires validated pinned/mapped pages, device-specific DMA authority and safe teardown.

See [COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md](COMPATIBILITY-FIRST-IMPLEMENTATION-MODEL.md) for the staged Linux/Windows driver roadmap and mandatory acceptance gates.

## Security boundary

The registry does not itself grant applications access to hardware. Applications
must reach devices through a future capability/device-broker policy. Driver code
remains kernel-trusted until user-mode driver isolation is available.

Signed-driver enforcement is represented as policy metadata (SIGNED_REQ), while
cryptographic package verification remains a separate package/security milestone.

The file-admission policy also distinguishes explicit local trust from
cryptographic signature verification: a locally trusted, valid package may be
admitted without a signature, but that policy result is not represented as
cryptographic signature proof. Compatibility matching still requires the
admission-backed VERIFIED state and active transaction boundary. Inconsistent
signature metadata (signature marked valid while no signature is present) is
treated as suspicious/quarantined rather than as verification evidence.

Compatibility package matching is additionally gated by an active verified
transaction state. Trust/evidence metadata in IDLE, INSPECTED, or ROLLED_BACK
state cannot make a package selectable; rollback therefore revokes driver
selection eligibility until a fresh admission and verification transaction is
completed.

## Bus integration

PCI enumeration can create device records using vendor, device, class, subclass
and programming-interface fields. Platform and virtual devices can use the same
registry without pretending to be PCI devices.

The current ATA PIO implementation remains the real block-device provider. It can
later be wrapped by this model without changing the pros_block_device filesystem
contract.

## Deliberate non-goals

This milestone does not claim that keyboard, display, network, AHCI, NVMe, USB, or
vendor Windows drivers are implemented. Those are consumers of this model. No
vendor driver is invented merely to make the architecture appear complete.

## Verification

pros_driver_selftest() covers manager initialization, ABI validation, hardware-ID
matching, device registration and stable ID allocation, controlled probe/bind,
driver private-state handoff, unbind/remove, and lifecycle cleanup.

## User-mode driver isolation and compatibility-host target

**Status: planned architecture; not implemented by the foundational registry described above.**

The current registry/lifecycle foundation must not be confused with isolated driver execution. Driver code remains kernel-trusted unless and until the driver is hosted in a separately protected user address space and all hardware authority is brokered.

### Isolation contract

- Each user-mode driver/device service runs in its own process and address space, with private stacks, heap and resource limits.
- No driver receives a kernel pointer, unrestricted physical-memory mapping, another driver's memory, or implicit device authority.
- A device permission broker issues revocable, per-process, per-device grants with narrow READ/WRITE/CONTROL/MAP rights. Interrupt, MMIO, port-I/O, reset, power and DMA rights require separately defined authority.
- DMA buffers are pinned and validated, mapped through device-specific DMA APIs and IOMMU where available; use reviewed bounce buffers or deny DMA where isolation cannot be enforced.
- Shared memory is explicit, bounded, permissioned and revocable. It is not a substitute for DMA mapping.
- A driver-host crash must not corrupt kernel memory, other driver hosts, or unrelated processes.

Windows .sys and Linux .ko files remain kernel-driver formats, not ordinary user-space plugins. A future compatibility host needs a documented, version-specific ABI adapter and cannot simply load either binary as a userspace module.

### Supervisor, watchdog and recovery

A minimal trusted supervisor owns start/probe/stop/restart policy. Required behavior:
1. Enforce bounded startup, probe and I/O request deadlines.
2. Detect process exit, heartbeat failure, stuck requests and repeated faults.
3. Revoke capabilities, interrupt registrations, DMA mappings and shared-page mappings before teardown/restart.
4. Drain/cancel outstanding I/O and establish device quiescence before freeing buffers.
5. Apply bounded restart count/back-off; quarantine crash-looping drivers.
6. Reinitialize a device only through an authorized reset path.
7. Select a validated fallback driver or expose a degraded device-unavailable state.
8. Preserve structured, bounded health/audit diagnostics.

If DMA or interrupt quiescence cannot be proven, do not restart against possibly stale device activity. Quarantine the device and require a safe reset/recovery sequence.

### Generic fallback policy

Fallback is conditional on hardware support and a validated backend. It must never bind an unrelated driver merely to keep a device appearing online.

| Class | Preferred | Fallback/degraded behavior |
|---|---|---|
| Display | Native GPU/display | Firmware VBE/GOP or validated framebuffer; serial/headless recovery if unavailable |
| Storage | Native NVMe/AHCI | Supported ATA PIO or VirtIO block; otherwise device unavailable and preserve recovery path |
| Network | Native NIC | Supported VirtIO/generic backend; otherwise network offline while local OS continues |
| Keyboard/input | Native controller/HID | Supported generic USB HID or serial/firmware recovery console |
| Mouse | Native HID | Supported generic USB HID; otherwise disable pointer only |
| Graphics acceleration | Native GPU | Software rendering over framebuffer; reduced performance only |

Boot and service startup must use bounded timeouts. An unsupported device or failed optional driver must not hang the whole system. If no safe fallback exists, report the device unavailable and continue in a documented degraded mode whenever the device is not required to boot.

### Driver isolation acceptance gates

- Cross-process memory isolation and denied raw physical/MMIO access.
- Per-device capability grant/revoke tests, including revocation after driver death.
- DMA/IOMMU or bounce-buffer validation, teardown and stale-mapping tests.
- Interrupt unregister/quiescence and outstanding-I/O cancellation tests.
- Injected driver crash, hang, malformed message and restart-loop tests.
- Proof that another driver, kernel service and unrelated process continue functioning.
- Primary-driver failure/fallback selection tests for each supported device class.
- No fallback claim without a real emulated or physical device test.

These are future acceptance gates. They do not change the current implementation status or imply that generic USB HID, framebuffer, VirtIO network/block, NVMe or AHCI drivers are already available.
