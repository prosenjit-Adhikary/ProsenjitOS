# ProsenjitOS boot architecture

ProsenjitOS uses a native boot path as the primary design target while keeping the existing Multiboot2/GRUB path as a compatibility and development fallback.

## Native BIOS path

The current native path is deliberately independent of MBR/GPT partition parsing and filesystem discovery:

`BIOS -> native stage-1 -> native stage-2 -> flat kernel image -> kernel`

- stage-1 is exactly one 512-byte BIOS boot sector
- stage-1 loads a fixed, reserved stage-2 area
- stage-2 collects the firmware E820 memory map
- stage-2 converts that map into a Multiboot2-compatible in-memory structure
- stage-2 loads the kernel from a fixed LBA layout and copies it to the kernel's linked address
- the native boot magic tells the kernel that the handoff came from the ProsenjitOS bootloader
- the kernel keeps its existing Multiboot2 memory-map parser, avoiding a second memory-manager ABI

The native disk layout is currently:

`LBA 0 -> stage-1`
`LBA 1..32 -> stage-2 reserved area`
`LBA 33.. -> flat ProsenjitOS kernel`

This is an early bootloader milestone, not the final installation format.

## Why partition tables are not required at this stage

A BIOS firmware can execute a boot sector without ProsenjitOS first implementing MBR/GPT partition discovery. The native prototype therefore keeps partitioning below the bootloader boundary.

Later storage support can add:

`BIOS/UEFI -> bootloader -> partition discovery -> filesystem -> VFS`

without changing the kernel/bootloader contract.

For removable media and installed disks, the storage layer retains MBR and GPT support for broad disk-layout compatibility. They are not prerequisites for the native boot protocol itself.

## GRUB compatibility

GRUB is not part of the native boot dependency. The repository continues to build a Multiboot2/GRUB ISO for development and compatibility.

The intended relationship is:

- **Primary:** ProsenjitOS native bootloader
- **Compatibility:** Multiboot2/GRUB or another external bootloader
- **Kernel:** one kernel image with a small boot-handoff boundary

## Filesystem and partition boundary

The native loader currently uses a fixed LBA layout rather than parsing a filesystem. MBR/GPT parsing and FAT/exFAT/NTFS/ProsenjitFS support remain in the kernel storage/VFS layers. This separation keeps early boot simple while preserving removable-media compatibility.

## Future UEFI path

UEFI support can be added as another boot protocol implementation:

`UEFI firmware -> ProsenjitOS EFI loader -> common boot handoff -> kernel`

GPT/ESP handling belongs to UEFI installation/storage tooling. The kernel should receive normalized boot information instead of depending on a particular firmware or partition format.

## Security and hardening roadmap

The fixed-layout native prototype proves the native boot handoff without making Secure Boot claims. Future hardening milestones include signed kernel verification, measured boot, rollback protection, stronger loader validation, and eventually a filesystem/partition-aware installation format.

