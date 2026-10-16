# ProsenjitOS filesystem integration boundary

## Status — 2026-10-16

The native BIOS bootloader remains independent of partitions/filesystems, and the ProsenjitFS provider attaches through the block-device, partition, and VFS boundaries. The deterministic memory-backed ProsenjitFS self-test covers format/mount, allocation/reclamation, file and directory mutations, journal recovery, corruption rejection, and boundary regressions.

The current CI workflow passed ISO build/QEMU boot and native BIOS/QEMU boot on commit `e43a98a891e5354d806c383b2e0f3ece8e7e2b0d`. The native path confirms bootloader handoff, ATA PIO block-path access, partition discovery, and LBA-boundary diagnostics. It does **not** currently prove that a preformatted ProsenjitFS filesystem is mounted and remounted through the physical ATA -> MBR -> ProsenjitFS chain. That end-to-end persistent-volume gate remains pending; do not describe it as verified.

## Frozen dependency direction

Firmware -> native BIOS/UEFI loader or GRUB compatibility adapter -> ProsenjitOS boot protocol / kernel entry -> Kernel -> Block-device layer -> Partition layer (MBR/GPT) -> VFS/provider boundary -> ProsenjitFS

### Bootloader boundary

The native BIOS loader must not parse MBR/GPT, probe filesystems, mount a filesystem, depend on VFS symbols, or depend on a physical storage driver.

Its current fixed-LBA layout is only a bootstrap mechanism for loading the kernel. MBR/GPT and filesystem discovery remain below the bootloader boundary.

### Block-device boundary

Filesystem providers consume struct pros_block_device and its read/write operations. ProsenjitFS additionally uses the optional flush primitive introduced on its feature branch: block_device_set_flush() and block_device_flush(). Existing block-device initializers remain source-compatible; a device without a flush callback does not claim a durability barrier.

### VFS boundary

The existing VFS owns mount lifetime, vnode/file objects and reference counting, path lookup and directory iteration contracts, generic read/write/seek/close semantics, and filesystem-neutral security admission.

A filesystem provider owns on-disk format parsing, metadata validation, allocation, filesystem-specific journaling/recovery, and filesystem-specific vnode/file operations.

ProsenjitFS therefore attaches to the existing VFS rather than replacing it.

## Real block-device progress and current limit

The native BIOS QEMU path currently verifies the real ATA PIO block-device path, bootloader handoff, and filesystem LBA-boundary diagnostics. A separate in-memory storage-discovery self-test verifies that an MBR partition without a valid ProsenjitFS superblock is not advertised as a supported volume.

The native boot test is intentionally read-only with respect to filesystem formatting: startup must never automatically format an unknown or blank partition. The workflow no longer requires a persistent ATA -> MBR -> ProsenjitFS mount marker because that could make an unformatted test partition look like permission to format it. The current CI pass therefore must not be interpreted as proof of persistent ProsenjitFS mount/remount on the ATA partition.

Next storage integration gate: provide an explicitly created, preformatted, disposable ProsenjitFS disk image as a separate test fixture; attach it only to the dedicated QEMU test; mount, write, flush, unmount, remount, and read back known data; and verify partition-relative LBA bounds. The fixture must be generated deterministically and never point at a user/host disk. Keep blank/unknown-disk probing non-destructive.


## ProsenjitFS branch integration notes

The feature/prosenjitfs branch supplied the initial provider implementation. Its work was integrated through a separate integration branch so the running branch itself was not modified. The provider now lives on main behind the frozen block/VFS boundary.

## Regression contract

Every filesystem integration change must preserve:
1. GRUB/Multiboot2 ISO build and QEMU boot.
2. Native BIOS disk-image build.
3. Native BIOS QEMU boot and native boot-handoff marker.
4. Partition discovery self-test.
5. ProsenjitFS deterministic memory-backed format/mount/allocation/journal/recovery regression.
6. Native ATA PIO block-path and LBA-boundary diagnostics.
7. Non-destructive partition discovery: blank or unsupported partitions must not be autoformatted.
8. A dedicated preformatted-image ATA -> MBR -> ProsenjitFS write/flush/unmount/remount/read-back test remains a pending gate; it must use an isolated disposable image, not an unknown or host disk.

## Non-overlap rule

Do not move ProsenjitFS logic into boot/native_stage1.asm, boot/native_stage2.asm, the native boot image builder's fixed-LBA protocol, or kernel entry/boot handoff code.

Do not duplicate VFS path/file semantics inside ProsenjitFS when an existing VFS operation can provide the contract.

Do not reintroduce compatibility filesystem parsers into the kernel merely to make ProsenjitFS work. Compatibility providers belong outside the kernel image.

## Merge gate

Commit `e43a98a891e5354d806c383b2e0f3ece8e7e2b0d` passed the ISO build/QEMU boot and native BIOS/QEMU boot jobs; artifact upload was skipped because upload is opt-in while storage is at quota. These results verify the listed boot milestones, not a persistent ProsenjitFS mount on the ATA partition. Keep that end-to-end fixture/test as an explicit pending gate. Physical power-loss validation remains a separate hardware reliability milestone.