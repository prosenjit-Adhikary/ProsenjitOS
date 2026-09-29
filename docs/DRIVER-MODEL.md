# ProsenjitOS Driver Model

Status: foundation implemented on 2026-09-29.

## Layering

Hardware
-> PCI/USB/platform enumeration
-> device registry
-> Driver Manager
-> driver match/probe/bind
-> device-class interface
-> storage/input/display/network services
-> VFS/security/user services

A driver must not bypass the Driver Manager to claim a device.

## Foundation implemented

- Common device descriptor with bus, vendor/device, subsystem and class identity.
- Generic driver descriptor with match IDs, probe callback and optional remove callback.
- Bounded Driver Manager registries for drivers and devices.
- Deterministic registry-order probing.
- Single active driver claim per device.
- Failed probe leaves the device unclaimed.
- Successful removal clears the claim only after the remove callback succeeds.
- Boot-time Driver Manager self-test and serial regression marker.
- Existing PCI enumeration remains read-only and can feed this model later.
- Existing pros_block_device remains the storage contract; ATA PIO remains the current real provider.

## Security rules

1. Device ownership is kernel-controlled.
2. Driver loading must be policy-gated.
3. Untrusted driver packages must not silently become kernel drivers.
4. Windows .sys binaries are not native ProsenjitOS kernel drivers.
5. Vendor installers are packages to extract and inspect, not blindly execute.
6. Imported Windows packages require a compatibility or adapter execution path.
7. Device capabilities exposed to user mode must be explicitly attenuated.

## Planned compatibility path

Windows driver package:
.exe/.zip/.cab/.inf
-> extraction/inspection
-> INF and hardware-ID parsing
-> signature/trust verification
-> compatibility manifest
-> native-driver match OR Windows-driver adapter

Future .pdrv packages will carry a manifest, hardware IDs, native driver or
adapter, optional firmware, signature metadata and version/dependency data.

## Planned subsystem tree

kernel/
  device.c
  driver.c
  driver_manager.c
  pci.c
  ata_pio.c
  drivers/
    pci/
    storage/
    usb/
    input/
    display/
    network/
    platform/

The drivers/ directories are ownership boundaries. Common matching, lifecycle,
security and registry logic stays in the Driver Manager.

## Next implementation order

1. Connect PCI enumeration to device registration without granting ownership.
2. Add generic MMIO, I/O-port, IRQ and DMA resource descriptors.
3. Add storage driver adapter interface and migrate ATA PIO through it.
4. Add keyboard, display and network class interfaces.
5. Add signed package verification and offline driver package format.
6. Add Windows INF/package inspection and compatibility adapter contracts.
7. Add native AHCI, NVMe, USB and network drivers.
8. Add rollback, dependency and version management.
