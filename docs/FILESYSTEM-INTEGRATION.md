# ProsenjitOS filesystem integration boundary

## Status — 2026-09-29

The native BIOS bootloader is integrated on main and CI-verified. The next storage milestone is integration of ProsenjitFS without coupling the bootloader to partitions or filesystems.

## Frozen dependency direction

Firmware -> native BIOS/UEFI loader or GRUB compatibility adapter -> ProsenjitOS boot protocol / kernel entry -> Kernel -> Block-device layer -> Partition layer (MBR/GPT) -> VFS -> FAT32 / exFAT / NTFS / ProsenjitFS

### Bootloader boundary

The native BIOS loader must not parse MBR/GPT, probe FAT32/exFAT/NTFS/ProsenjitFS, mount a filesystem, depend on VFS symbols, or depend on a physical storage driver.

Its current fixed-LBA layout is only a bootstrap mechanism for loading the kernel. MBR/GPT and filesystem discovery remain below the bootloader boundary.

### Block-device boundary

Filesystem providers consume struct pros_block_device and its read/write operations. ProsenjitFS additionally uses the optional flush primitive introduced on its feature branch: block_device_set_flush() and block_device_flush(). Existing block-device initializers remain source-compatible; a device without a flush callback does not claim a durability barrier.

### VFS boundary

The existing VFS owns mount lifetime, vnode/file objects and reference counting, path lookup and directory iteration contracts, generic read/write/seek/close semantics, and filesystem-neutral security admission.

A filesystem provider owns on-disk format parsing, metadata validation, allocation, filesystem-specific journaling/recovery, and filesystem-specific vnode/file operations.

ProsenjitFS therefore attaches to the existing VFS rather than replacing it.

## ProsenjitFS branch integration notes

The current feature/prosenjitfs branch adds the expected provider-side pieces: versioned on-disk format definitions; ProsenjitFS implementation and self-test; optional block-device flush support; VFS probing for PROS_FS_PROSENJIT; storage discovery recognition for PROS_FS_PROSENJIT; and one CI marker for the ProsenjitFS self-test.

The branch changes to block.c, block.h, vfs.c, storage.c, kernel.c, and scripts/build.sh are integration points, not replacements for the architecture. Before merging, the branch must be rebased/updated onto the current main so the native bootloader commit is retained.

## Regression contract

Every filesystem integration change must preserve:
1. GRUB/Multiboot2 ISO build and QEMU boot.
2. Native BIOS disk-image build.
3. Native BIOS QEMU boot and native boot-handoff marker.
4. Partition discovery self-test.
5. FAT32 self-test.
6. exFAT self-test, including Up-case/Unicode coverage.
7. NTFS read/path/write regression markers already required by CI.
8. ProsenjitFS deterministic memory-backed self-test when its provider is compiled in.

A real QEMU disk-device test for ProsenjitFS is a separate milestone. It must not be claimed until the kernel has a real ATA/AHCI/NVMe/USB-visible block-device path.

## Non-overlap rule

Do not move ProsenjitFS logic into boot/native_stage1.asm, boot/native_stage2.asm, the native boot image builder's fixed-LBA protocol, or kernel entry/boot handoff code.

Do not duplicate VFS path/file semantics inside ProsenjitFS when an existing VFS operation can provide the contract.

Do not remove or bypass FAT32/exFAT/NTFS providers to make ProsenjitFS work.

## Merge gate

The ProsenjitFS implementation is not considered integrated merely because the branch compiles. Merge requires: branch based on current main; clean CI; native BIOS boot regression still green; existing FAT32/exFAT/NTFS regression green; ProsenjitFS self-test green; no bootloader/filesystem coupling; and documentation that distinguishes memory-backed filesystem validation from real physical/QEMU disk integration.