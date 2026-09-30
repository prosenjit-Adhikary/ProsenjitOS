# Universal Driver Coverage Strategy

Status: architecture decision — 2026-09-30.

## Goal

The practical goal is not to hand-write a vendor driver for every piece of hardware. That does not scale for a new operating system.

The target is a high-coverage, low-friction hardware path:
1. Discover hardware without requiring a vendor installer.
2. Identify the device using stable bus/class/hardware IDs.
3. Prefer a native ProsenjitOS driver when one exists.
4. Fall back to a generic class driver when the device can safely operate through a standard protocol.
5. If no native driver exists, inspect an offline compatibility package and route it through a controlled adapter layer when technically possible.
6. Keep the machine bootable and usable even when a non-critical device has no driver.
7. Never execute a vendor installer merely to obtain a driver and never load a Windows `.sys` image directly as a ProsenjitOS kernel driver.

## Four-layer coverage model

### Layer 1 — Native drivers
Preferred, audited kernel drivers for hardware families with well-defined interfaces.
High-value families: legacy ATA/IDE, AHCI, NVMe, USB host controllers, USB HID keyboard/mouse, common framebuffer/display paths, common Ethernet controllers, common storage controllers.

### Layer 2 — Protocol/class drivers
This is the main scalability mechanism: implement one class driver for a standard protocol instead of one driver per vendor.
Examples: USB HID, USB Mass Storage, NVMe, AHCI, UEFI/framebuffer-standard display, and standard Ethernet infrastructure.

### Layer 3 — Compatibility adapters
Windows driver packages are input packages, not trusted native kernel modules. The compatibility service should accept local `.exe`, `.zip`, `.cab`, and `.inf` packages; inspect metadata without running vendor installers; parse hardware IDs; verify integrity/signatures when available; reject unsupported kernel-driver models; and use an adapter runtime only for deliberately supported interfaces.

A major long-term reference point is the **ReactOS Windows/NT driver-compatibility architecture**. ReactOS demonstrates that a Windows-compatible driver environment can be approached by implementing compatible NT/kernel, I/O, PnP, bus/class/driver interfaces rather than trying to make arbitrary Windows `.sys` binaries native. ProsenjitOS may study ReactOS's public architecture, interfaces, behavior, and testing approach, but will implement its own compatibility subsystem around the ProsenjitOS kernel/driver ABI.

ReactOS source is GPL-licensed. Therefore this project must **not copy ReactOS source code into ProsenjitOS unless the project's licensing and source-disclosure consequences are deliberately accepted**. The default ProsenjitOS plan is an independent, clean implementation based on documented interfaces, observed behavior, and independently authored code. Any future direct code reuse must receive an explicit license review first.

The compatibility engine is expected to grow in stages:
1. package/INF inspection and hardware-ID extraction;
2. Windows driver-model classification;
3. minimal NT/WDM compatibility ABI;
4. controlled driver loading and I/O dispatch;
5. PnP, interrupt, DMA, power, and bus abstractions as required;
6. subsystem-specific adapters such as storage, network, USB, and display;
7. health-check, isolation, rollback, and trust enforcement.

A Windows `.sys` file must never be copied into the native driver registry and executed as a ProsenjitOS driver.

### Layer 4 — Graceful generic fallback
Devices are classified as required, important, or optional. Missing optional drivers must never block installation. Unsupported important devices should allow a safe degraded install with a clear diagnostic record.

## Installer-time hardware bootstrap

The installer uses a small, high-coverage bootstrap environment:
Firmware -> bootloader -> bootstrap drivers -> hardware inventory -> storage/input/display/network bootstrap -> install -> first-boot driver manager

The installer must not depend on the final desktop driver stack.

### Offline-first rule
The OS must be installable from local media without Internet access. Installation media should contain native core drivers, generic class drivers, a hardware-ID database, signed package metadata, compatibility adapter components, and a driver bundle index.

## Driver selection policy

Selection is deterministic:
1. exact native hardware match;
2. native class/protocol match;
3. exact compatibility package match;
4. compatibility class/protocol adapter;
5. generic safe fallback;
6. leave device unbound with a diagnostic record.

Specificity is considered before origin preference so a generic driver cannot steal a device from a more appropriate exact match.

## Device capability reporting

For every device the inventory should expose bus/location, hardware IDs, selected driver, origin, version, trust state, capabilities, missing capabilities, and the reason for fallback/failure.

## Recovery and rollback

Driver installation is transactional: inspect -> verify -> stage -> bind -> health-check -> commit.
If health-check fails: unbind -> rollback -> restore previous driver -> record reason.
A bad optional driver must not make the machine unbootable. Boot-critical storage/input paths get a known-good fallback.

## What 100% solution means

Literal support for 100% of hardware cannot be guaranteed by a new OS without implementing or licensing every proprietary hardware interface.
The practical target is 100% installation continuity for unsupported non-critical hardware, plus maximum hardware coverage through standard protocols, native drivers, and a controlled compatibility layer.
The OS should never require a user to abandon installation simply because an optional vendor-specific device has no driver.

## Implementation status

The compatibility foundation now includes a staged NT/WDM boundary ABI, bounded IRP-major dispatch, and explicit PnP/power event entry points. Inspect/stage/health-check/quarantine/rollback remain explicit lifecycle states. The stage operation deliberately returns unsupported until the executable NT/WDM runtime exists; this prevents accidental native execution of Windows kernel binaries.

The first compatibility inspection slice is now implemented in the kernel: bounded INF-text inspection extracts PCI `VEN_xxxx&DEV_yyyy` hardware IDs, records the detected package model, and exposes the extracted IDs to the driver-manager layer. Package metadata now also carries trust state and bounded dependency IDs, with deterministic compatibility matching that rejects explicitly untrusted packages. The implementation remains metadata-only: it does not execute installers, parse arbitrary INF directives, perform cryptographic signature verification, or execute Windows `.sys` binaries.

## Implementation order

1. Keep the generic driver registry stable.
2. Add a device inventory database and deterministic match scoring.
3. Add driver-manager package metadata, trust and dependency model.
4. Add installer/bootstrap driver environment.
5. Add native class/protocol drivers: AHCI, NVMe, USB HID, USB storage, framebuffer, common Ethernet.
6. Add offline driver import.
7. Add INF/package inspection.
8. Add the **ReactOS-inspired, independently implemented NT/WDM compatibility foundation** for explicitly supported Windows driver interfaces. The initial ABI boundary is now present; executable PE/NT runtime work remains gated.
9. Add subsystem-specific Windows-driver adapters and compatibility tests.
10. Add transactional install/rollback, health checks, isolation, and trust enforcement.
11. Add desktop driver-management UI.

This strategy deliberately avoids making ProsenjitFS or the VFS depend on vendor driver details.


### Transactional compatibility package lifecycle

Compatibility packages now have an explicit bounded transaction state: **idle → inspected → verified → staged → healthy → committed**, with rollback returning the package to an inspected state. Verification requires explicit VERIFIED trust and a valid dependency graph. Health-check currently remains metadata-only and returns ENOTSUP until an executable compatibility runtime exists; therefore packages cannot be committed prematurely. Rejected/quarantined paths remain blocked.


### PE-to-NT compatibility metadata boundary

The NT compatibility layer now accepts a bounded PE byte buffer through a metadata-only inspection entry point. The PE inspector validates the DOS/PE headers, machine, optional-header form, section bounds, image size, entry RVA, and import metadata, then copies only bounded metadata into the compatibility image record. No PE entry point is invoked, no section is executed, and quarantined images are rejected. This keeps PE inspection separate from the future executable NT/WDM runtime.


### Bounded NT/WDM I/O boundary

The compatibility ABI now defines a bounded I/O request structure and submission path. Requests validate structure size, IRP major, operation type, and a 4 KiB compatibility buffer ceiling before dispatch. Quarantined images cannot receive I/O, and rollback removes registered dispatch handlers. This remains an ABI/simulation boundary: it does not execute Windows driver code or provide arbitrary kernel-memory access.


### Bounded device/resource handoff

The NT compatibility boundary now has a bounded device-object/resource descriptor. It carries bus/location, PCI identity/class fields, and up to eight abstract resources (I/O port, MMIO, IRQ, or DMA). The compatibility layer copies descriptors into private state and exposes them through validated get/set APIs; quarantined packages cannot access the descriptor. This is metadata/resource description only and does not grant raw MMIO, port-I/O, DMA, or interrupt ownership.


### PnP lifecycle and resource-conflict guard

The compatibility boundary now validates abstract resource descriptors before accepting them and rejects overlapping resources of the same type. PnP START is gated to staged/healthy images, while STOP/REMOVE cannot be issued from an un-staged inspected state; quarantined images remain blocked. This is lifecycle/resource policy only and does not yet claim real interrupt, DMA, MMIO, or port-I/O ownership.


### Device lifecycle/resource ownership guard

Compatibility devices now carry an explicit PRESENT/STARTED/QUERY_REMOVE/STOPPED/REMOVED lifecycle. PnP transitions are validated before the compatibility callback is accepted, and abstract I/O-port/MMIO/IRQ/DMA resources are checked for invalid ranges and conflicts both within a device and against other active compatibility devices. This remains a policy/ABI layer: it does not yet claim real interrupt-controller, DMA-engine, MMIO, or I/O-port programming.


## Current implementation boundary — 2026-09-30

The universal-driver foundation is implemented and fail-closed: hardware-ID matching, deterministic native/compatibility selection, bounded INF inspection, NT/WDM ABI/resource/PnP guards, package transaction rollback, SHA-256 content identity, and a dedicated signature-authority boundary are present. Generic admission no longer accepts caller-supplied signature-valid metadata as cryptographic proof. Ed25519 verification with RFC 8032 known-answer coverage is now implemented behind the signature-authority boundary; RSA-PSS, executable Windows-driver compatibility, and complete native device-class coverage remain explicit implementation milestones rather than being represented as complete.


### Offline bundle selection boundary — 2026-09-30

The offline driver bundle index now records package source, semantic version, and
SHA-256 content identity. Compatibility selection is delegated back to the
compatibility registry, so a bundle entry by itself never grants trust or
execution eligibility. A package must still satisfy the compatibility registry's
verified/admitted dependency and active-transaction rules before matching a
hardware ID. This preserves the fail-closed trust boundary while allowing ISO,
removable-media, and local driver sources to share one deterministic index.


### INF manufacturer/model mapping — 2026-09-30

Bounded INF inspection now separates the [Manufacturer] section from the referenced model sections. Manufacturer entries are recorded as model-section metadata, then the referenced sections are scanned for PCI hardware IDs without executing INF directives. Per-model PCI-ID counts are retained for later deterministic package selection. This is intentionally metadata-only; full INF directive semantics, archive extraction, and Windows driver execution remain outside the current boundary.

### INF model hardware metadata — 2026-09-30

The bounded INF inspector now follows manufacturer-to-model section references before counting PCI hardware-ID entries in the referenced model sections. It also records bounded PCI SUBSYS and REV metadata when present. These values are inspection metadata only; they are not treated as executable INF directives or as a cryptographic trust decision.


### Offline bundle content-identity guard — 2026-09-30

Offline bundle selection now requires the bundle entry's SHA-256 identity to match the current compatibility-package content identity before selection. A stale bundle index entry therefore fails closed rather than selecting metadata for different package bytes. The bundle remains a locator/index; it does not establish trust, signature validity, dependency trust, or executable eligibility.


### Deterministic offline candidate precedence — 2026-09-30

When multiple offline bundle entries are eligible for the same hardware ID, selection is deterministic: the highest hardware-match score is preferred first; ties are resolved by higher semantic package version, then bundle source precedence (ISO before removable media before local source), then the lowest package ID. A candidate with a stale SHA-256 bundle identity or without the compatibility registry's verified/admitted active transaction remains ineligible regardless of version or source. This ordering is a selection policy only; it does not establish package trust or executable compatibility.


### Admission-backed offline package import — 2026-09-30

The offline bundle layer now has an explicit import/admission boundary. A package import registers bounded metadata, computes SHA-256 from the supplied package bytes, binds an admission record only when its digest matches, starts a compatibility transaction, verifies the package and dependencies, and only then adds the package to the offline bundle index using the computed content identity. Failure at any transaction step rolls the compatibility package back; the bundle index is never used as a trust authority. Staging/health-check/commit remain separate because executable Windows-driver compatibility is not yet implemented.
