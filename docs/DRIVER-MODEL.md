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
hardware identity, device class, lifecycle state, optional bus data, and the
currently bound driver.

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

## Native vs compatibility

Native drivers are the primary kernel-driver path. Compatibility drivers have a
separate origin and are selected through the same controlled registry rather than
being loaded as arbitrary kernel binaries.

Windows .sys binaries are not accepted as native drivers. A future Windows
package importer/compatibility service must inspect package metadata and route
compatible devices through an adapter layer.

## Security boundary

The registry does not itself grant applications access to hardware. Applications
must reach devices through a future capability/device-broker policy. Driver code
remains kernel-trusted until user-mode driver isolation is available.

Signed-driver enforcement is represented as policy metadata (SIGNED_REQ), while
cryptographic package verification remains a separate package/security milestone.

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
