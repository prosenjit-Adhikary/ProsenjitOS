# Drivers

This directory is the home for hardware-specific ProsenjitOS drivers.

## Rule

Drivers must expose small subsystem contracts and must not leak device-specific
registers or assumptions into VFS, networking, input, display, or other kernel
core code.

The kernel driver model is defined by:
- kernel/include/driver.h
- kernel/driver.c
- docs/DRIVER-MODEL.md

## Driver layering

Bus discovery
-> pros_driver_device
-> hardware-ID matching
-> controlled driver bind
-> driver-owned private state
-> generic subsystem contract

Native drivers are the primary path. Compatibility drivers must use an explicit
adapter boundary; vendor Windows .sys binaries are never treated as native
ProsenjitOS kernel drivers.

The current ATA PIO implementation remains a block-device provider. AHCI, NVMe,
USB, keyboard, display and network drivers can be added later without redesigning
the block/VFS contracts.

Do not add a vendor-specific driver just to satisfy a roadmap checkbox. A driver
must have a defined hardware match, lifecycle, security boundary and regression
test.
