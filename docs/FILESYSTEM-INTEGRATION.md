# ProsenjitOS filesystem integration boundary

## Status — 2026-09-30

The native BIOS bootloader and ProsenjitFS provider are integrated on main. The latest filesystem work completes the VFS/provider mutation scope for ProsenjitFS: persistent allocation/reclamation, file create/delete, mkdir/rmdir, same-directory rename, cross-directory move, and directory-parent cycle protection. The native bootloader remains independent of partitions/filesystems.

Final CI verification is currently pending because recent GitHub Actions jobs fail during job startup before workflow steps are created; no successful post-change QEMU regression result is claimed.

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

## Real block-device progress

The native BIOS QEMU path now exercises the real storage chain rather than only
the synthetic filesystem tests:

- the native image contains a deterministic MBR entry for a reserved
  ProsenjitFS partition at LBA 2048 (512 sectors);
- the kernel probes the IDE primary channel through the polling ATA PIO driver,
  including IDENTIFY, LBA28/LBA48 reads/writes, and cache flush;
- the partition layer creates a bounded block-device view of that partition;
- the first boot formats the reserved partition when no ProsenjitFS superblock
  exists;
- the kernel then probes, mounts, journal-recovers, unmounts, and rediscoveries
  the persistent ProsenjitFS volume through the normal storage layer.

This is intentionally separate from the fixed-LBA native boot layout: the
bootloader still loads stage 2 and the kernel from its reserved LBAs, while
filesystem storage begins at LBA 2048. The native QEMU CI gate requires the
persistent ATA -> MBR -> ProsenjitFS milestone.


## ProsenjitFS branch integration notes

The feature/prosenjitfs branch supplied the initial provider implementation. Its work was integrated through a separate integration branch so the running branch itself was not modified. The provider now lives on main behind the frozen block/VFS boundary.

## Regression contract

Every filesystem integration change must preserve:
1. GRUB/Multiboot2 ISO build and QEMU boot.
2. Native BIOS disk-image build.
3. Native BIOS QEMU boot and native boot-handoff marker.
4. Partition discovery self-test.
5. ProsenjitFS native format/mount/allocation/journal/recovery regression.
8. ProsenjitFS deterministic memory-backed self-test when its provider is compiled in, including allocation/reclamation and directory mutation regression.
9. Native ATA -> partition -> ProsenjitFS physical-LBA boundary diagnostic, including write/read LBA agreement and payload readback.

A native QEMU disk-device test is now a mandatory regression gate. The native image uses the kernel-visible ATA PIO path and the reserved MBR ProsenjitFS partition described above.

## Non-overlap rule

Do not move ProsenjitFS logic into boot/native_stage1.asm, boot/native_stage2.asm, the native boot image builder's fixed-LBA protocol, or kernel entry/boot handoff code.

Do not duplicate VFS path/file semantics inside ProsenjitFS when an existing VFS operation can provide the contract.

Do not reintroduce compatibility filesystem parsers into the kernel merely to make ProsenjitFS work. Compatibility providers belong outside the kernel image.

## Merge gate

The ProsenjitFS implementation scope is now covered by deterministic memory-backed mutation tests and the native physical-LBA diagnostic gate. The implementation is not labeled CI-verified until a post-change GitHub Actions run successfully executes the workflow steps. Broader corruption/crash-point coverage and real power-loss validation remain separate reliability milestones.