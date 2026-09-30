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

The first compatibility inspection slice is now implemented in the kernel: bounded INF-text inspection extracts PCI `VEN_xxxx&DEV_yyyy` hardware IDs, records the detected package model, and exposes the extracted IDs to the driver-manager layer. Package metadata now also carries trust state and bounded dependency IDs, with deterministic compatibility matching that rejects explicitly untrusted packages. The implementation remains metadata-only: it does not execute installers, parse arbitrary INF directives, perform cryptographic signature verification, or execute Windows `.sys` binaries.

## Implementation order

1. Keep the generic driver registry stable.
2. Add a device inventory database and deterministic match scoring.
3. Add driver-manager package metadata, trust and dependency model.
4. Add installer/bootstrap driver environment.
5. Add native class/protocol drivers: AHCI, NVMe, USB HID, USB storage, framebuffer, common Ethernet.
6. Add offline driver import.
7. Add INF/package inspection.
8. Add the **ReactOS-inspired, independently implemented NT/WDM compatibility foundation** for explicitly supported Windows driver interfaces.
9. Add subsystem-specific Windows-driver adapters and compatibility tests.
10. Add transactional install/rollback, health checks, isolation, and trust enforcement.
11. Add desktop driver-management UI.

This strategy deliberately avoids making ProsenjitFS or the VFS depend on vendor driver details.
