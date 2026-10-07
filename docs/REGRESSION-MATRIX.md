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


### Deterministic power-loss transaction matrix — 2026-10-06

The Native ProsenjitFS self-test now executes a real VFS overwrite with simulated volatile device-cache loss and interruption at transaction stages 1–9. Stages before durable journal commit must preserve the old file image; stages after durable commit must recover the new image through redo replay. The matrix covers transaction begin, journal header write, journal image write, journal flush, commit flush, home-write interruption, home flush, post-home recovery boundary, and journal-retirement boundary. The test remounts the same durable medium after each simulated crash and verifies file contents.

Interrupted recovery is also exercised at both pre-flush and post-flush boundaries. The failed recovery mount is followed by simulated power loss and a second mount, verifying idempotent replay of the same committed replacement-image transaction.

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

The unlink transaction-capacity regression now verifies that the in-memory target set enforces the fixed on-disk maximum, deduplicates repeated bitmap targets, and returns `ENOSPC` without extending the transaction when the final target would exceed capacity. This is a deterministic contract test; oversized real-file unlink and crash/recovery behavior still require local QEMU execution.
The unlink preflight also rejects extent metadata/data overlap and physical overlap between distinct extents before transaction staging, preventing ambiguous ownership/reclamation on corrupted extent chains.

The deterministic native-filesystem self-test now covers persistent inode/data-block allocation and reclamation, file create/delete, directory creation/removal, nested directories, cross-directory file and directory moves, rename collision rejection, directory-parent cycle rejection, and non-empty-directory removal rejection. The test remains subject to the repository-wide GitHub Actions verification gate.

## Native ProsenjitFS failed-staging retry regression — 2026-10-07

The deterministic self-test now verifies that allocator and inode-bitmap update wrappers clear their transaction state when staging rejects an operation, and that a valid allocation/free operation can immediately retry on the same mounted filesystem. The retry is followed by the consistency checker so a failed pre-commit mutation cannot contaminate a later transaction or persistence state.

This remains a deterministic software-model regression; local Debian compilation and QEMU execution are still required before runtime PASS is claimed.

## Native ProsenjitFS rmdir transaction-capacity preflight — 2026-10-07

The native rmdir path now performs a read-only transaction-capacity preflight before constructing persistent mutation images. It accounts for the namespace block (or previous directory entry), child inode block, inode bitmap block, removed directory-entry allocation bitmap, and child directory-index allocation bitmap. Capacity failure therefore occurs before transaction staging, matching the atomic preflight contract already used by unlink.

Runtime compile/QEMU execution remains required before this source-level hardening is considered a runtime PASS.

## Completion principle

A milestone is considered complete only when its implementation, regression coverage, CI gate, and documentation agree. Deterministic tests must not be described as physical hardware durability validation.


## Native ProsenjitFS persistence/corruption regression — 2026-10-06

The native self-test now includes a persistent-image regression matrix covering:
- write/unmount/remount/read at 0, 1, 511, 512, 513, 1023, 1024 and 1025-byte boundaries;
- overwrite/truncate/append-style writes across remounts;
- nested directory-tree creation, rename, unlink and removal persistence;
- inode and block allocation bitmap persistence and released-block reuse;
- a reusable inode/extent/bitmap/device-boundary consistency checker;
- invalid inode/block and extent references;
- allocation exhaustion and post-exhaustion reuse;
- oversized truncate rejection and integer/LBA overflow checks;
- filesystem-end, device-end and partition-view boundary rejection.

The corruption fixture deliberately stores an inode with a valid checksum but an impossible extent head. Mount remains safe, while data access and the consistency checker fail closed with corruption errors.

The minimum-size image also exhausts data blocks and inode reservations, verifies ENOSPC without abandoning consistency, then remounts the image. A partition view shortened by one sector is rejected before Native ProsenjitFS can cross the partition boundary.

These are deterministic software-model regressions. They do not replace physical power-loss testing or local QEMU/ATA persistence validation.

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


## Native ProsenjitFS final I/O + VFS hardening — 2026-10-06

The final native file-I/O contract now explicitly covers VFS append semantics, seeking beyond EOF for sparse logical offsets, filesystem-size checked writes, and persistence of appended and sparse data across remount. The VFS rejects APPEND without WRITE, reports write argument/state errors consistently, and limits seek positions to the representable file-I/O offset domain. Native ProsenjitFS self-tests also execute a 100-cycle create/write/unmount/remount/read/unlink/remount persistence and allocation-reuse regression with the reusable consistency checker.

Runtime execution must still be distinguished from source verification: these additions are not considered QEMU/runtime PASS until the local Debian/QEMU build actually executes them.


## Native ProsenjitFS deep hardening — 2026-10-06

The native VFS/filesystem boundary now has additional fail-closed checks for:
- busy-unmount rejection while any descendant vnode/file/directory handle is live;
- zero-length read semantics without requiring a data buffer;
- inode-authoritative EOF refresh for reads and append writes;
- superblock bitmap-capacity validation so corrupted geometry cannot redirect allocation into another metadata region;
- consistency checking of metadata ownership, directory-entry ownership, extent/data-block ownership, inode parent relationships, allocation bitmap agreement, duplicate physical ownership, and file allocated-byte accounting;
- fail-closed handling for corrupted directory entries during rmdir/unlink;
- checked file extent ordering and logical-range validation.

The consistency checker is diagnostic-only and must never be used as a silent repair mechanism. Local Debian compilation and QEMU execution remain required before any runtime PASS is claimed.

## Native ProsenjitFS unlink transaction-capacity audit — 2026-10-07

The native unlink path now performs a read-only transaction preflight before staging any persistent mutation. It walks and validates the complete extent chain and accounts for every unique journal target required for namespace removal, inode metadata, inode/block allocation bitmaps, the directory-entry allocation, and all bitmap blocks covering extent metadata/data allocations. If the operation cannot fit the filesystem's existing journal transaction capacity, unlink fails closed with ENOSPC before namespace, inode, or allocation state is staged, so an oversized unlink cannot leave a namespace-only deletion or leak blocks.

The on-disk v1 journal layout and PROSENJITFS_MAX_TX_BLOCKS remain unchanged; increasing the fixed journal-header target array would require an on-disk format migration. Large/multi-extent files therefore remain supported up to the current journal capacity, while operations beyond that capacity are rejected atomically rather than partially reclaimed.

Runtime compile/QEMU/Debian/hardware execution remains a separate verification gate and is not claimed by this source-level audit.
