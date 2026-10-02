# ProsenjitOS Regression Matrix

## Purpose

This matrix records the automated validation layers that should remain green while storage, boot, VFS, security, and kernel work proceeds in parallel.

## Boot paths

| Area | Required validation | Status |
|---|---|---|
| GRUB/Multiboot2 | ISO build + QEMU boot marker | Required CI gate |
| Native BIOS | Native disk-image build + QEMU boot marker | Required CI gate |
| Native BIOS handoff | Boot protocol, memory-map, and kernel handoff markers | Required CI gate |
| Native UEFI | Loader build + QEMU/firmware validation | Pending |

## Storage layers

| Area | Validation | Status |
|---|---|---|
| Block device | Read/write contract and bounds checks | Foundation |
| Partition | MBR/GPT discovery self-test | Required CI gate |
| External filesystem providers | Provider ABI/registration compatibility when introduced | Future CI gate |\n| ProsenjitFS | Format/mount/allocation/journal/recovery memory-backed regression | Required CI gate |
| Real disk | ATA PIO -> MBR -> ProsenjitFS persistent QEMU test | Required CI gate |

## ProsenjitFS crash/recovery coverage

The deterministic regression suite should continue to cover:

1. valid committed journal transaction replay;
2. incomplete/torn transaction discard;
3. invalid journal checksum rejection;
4. metadata checksum/range validation;
5. allocator state remaining consistent after recovery;
6. writable mount exposure only after recovery succeeds.

These tests validate filesystem logic but do not replace physical power-loss testing.

## Native BIOS safety checks

The native BIOS path should continue to validate:

- BIOS EDD capability before INT 13h extensions;
- boot-drive preservation across stage transitions;
- bounded stage-2 and kernel sizes;
- disk-read failure handling;
- A20 availability before protected-mode memory copies;
- valid memory-map construction;
- bounded kernel handoff addresses.

## CI change rule

A change that affects one subsystem should preserve all unrelated regression markers. If a required marker disappears, the change is considered incomplete even when the modified subsystem itself builds.

## Known dependency boundaries

The following work remains intentionally dependent on lower layers:

- Native ProsenjitFS QEMU disk testing uses the kernel-visible ATA PIO controller path and is a required CI gate.
- Native filesystem-aware boot requires a stable boot protocol plus storage/partition discovery.
- Future compatibility filesystem providers must not couple their parsers to kernel-core; provider I/O must use the common block/device contract.
- UEFI boot validation requires the native UEFI loader before Secure Boot/verified-boot work can be meaningfully tested.

### ProsenjitFS mutation regression

The deterministic native-filesystem self-test now covers persistent inode/data-block allocation and reclamation, file create/delete, directory creation/removal, nested directories, cross-directory file and directory moves, rename collision rejection, directory-parent cycle rejection, and non-empty-directory removal rejection. The test remains subject to the repository-wide GitHub Actions verification gate.

## Completion principle

A milestone is considered complete only when its implementation, regression coverage, CI gate, and documentation agree. Deterministic tests must not be described as physical hardware durability validation.


## Native ProsenjitFS final-hardening additions — 2026-10-01

- Metadata-region ordering/non-overlap and redundant-superblock consistency.
- Multi-mutation bitmap staging in one transaction.
- Binary data coverage: 00 01 7F 80 A5 D6 FF.
- Journal-image failure and post-commit target-write failure recovery.
- Existing journal replay, torn-transaction discard, and checksum-failure
  fail-closed tests.
- Physical-LBA/extent/VFS 0xD6 regression and remount persistence.
- Complete project build remains a gate because ProsenjitFS is linked into the
  kernel and native BIOS/QEMU image.
