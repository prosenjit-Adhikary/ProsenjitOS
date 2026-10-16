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
| Storage/partition | Invalid discovery requests clear a valid output count; MBR/GPT LBA bounds, non-overlapping partition extents, GPT header and entry-array CRC validation against the standard IEEE CRC-32 known vector, checksum-corruption rejection, GPT entry-array read-I/O failure with zeroed output count, nonzero reserved-header-field rejection, GPT entry-array separation from headers/usable payload, protective-MBR rejection when the GPT header is invalid, partition-probe output reset on invalid arguments, malformed media, initial-device I/O failure, and protective-MBR GPT-header I/O failure, 8-byte entry-size alignment, cross-sector GPT entry reads, zeroed error counts, and no silent capacity truncation | Required CI gate |
| External filesystem providers | Provider ABI/registration compatibility when introduced | Future CI gate |
| ProsenjitFS | Format/mount/allocation/journal/recovery memory-backed regression | Required CI gate |
| ATA PIO path | Native BIOS QEMU block-path and LBA-boundary diagnostics | Required CI gate |
| Persistent ProsenjitFS disk | Isolated preformatted image: mount/write/flush/unmount/remount/read-back across separate QEMU boots | Pending implementation; never autoformat unknown disks |

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

## Persistent ATA/ProsenjitFS fixture requirements

The persistent-disk gate must be opt-in and operate only on a dedicated disposable raw image created by the test job. It must never select a host block device, reuse the runner's boot disk, or infer that an unknown partition is safe to format.

Required sequence:

1. Create a fresh, fixed-size raw image in the build directory and construct the expected MBR partition entry for the reserved ProsenjitFS partition. Verify the image path, exact byte length, partition start, and partition length before attaching it to QEMU.
2. Run a dedicated test-only format phase against that image's partition view. Formatting must be gated by an explicit test-mode build/configuration flag; it must not be reachable from ordinary native startup.
3. In the write phase, mount writable only after `prosenjitfs_probe()` succeeds, create/write a deterministic test file, close it, call the block-device flush operation, and unmount successfully.
4. Stop QEMU completely. Reboot with the same image attached, but switch to a read-only verification phase that must not format or mutate the volume.
5. Probe and mount the existing filesystem, verify the exact file bytes and length, unmount, and require explicit serial success markers from both phases.
6. Preserve the raw image between the two QEMU runs and fail closed on any unexpected image geometry, partition layout, mount/probe error, write/flush error, or missing marker.

The gate must not be marked complete until the implementation is reviewed and both QEMU phases pass on the configured Debian runner. This checks persistence across VM process restarts, not physical power-loss durability or real hardware behavior.

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

- Native BIOS QEMU testing currently covers ATA PIO block access and LBA-boundary diagnostics. Persistent ProsenjitFS mount/remount through ATA remains pending until a dedicated preformatted disposable-image fixture is added; never autoformat unknown/blank disks.
- Native filesystem-aware boot requires a stable boot protocol plus storage/partition discovery.
- Future compatibility filesystem providers must not couple their parsers to kernel-core; provider I/O must use the common block/device contract.
- UEFI boot validation requires the native UEFI loader before Secure Boot/verified-boot work can be meaningfully tested.

### ProsenjitFS mutation regression

The deterministic native-filesystem self-test now covers persistent inode/data-block allocation and reclamation, file create/delete, directory creation/removal, nested directories, cross-directory file and directory moves, rename collision rejection, directory-parent cycle rejection, and non-empty-directory removal rejection. The test remains subject to the repository-wide GitHub Actions verification gate.

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


## Partition-probe GPT header read failure — 2026-10-16

`partition_probe()` now resets its output type to `PROS_PARTITION_NONE` when a protective MBR is present but reading the GPT header at LBA 1 fails. The regression selectively fails only LBA 1 after a valid MBR read; this catches the case where the probe previously returned `PROS_PARTITION_EIO` while leaving the output incorrectly set to `PROS_PARTITION_MBR`. The test uses only the in-memory partition fixture. CI must pass before this change is considered verified.


## Partition-probe invalid-argument output — 2026-10-16

When the caller supplies a valid output pointer but the device pointer is null or the device sector size is unsupported, `partition_probe()` now clears the output type to `PROS_PARTITION_NONE` before returning `PROS_PARTITION_EINVAL`. The self-test covers both cases with a deliberately stale `PROS_PARTITION_GPT` output value. A null output pointer remains an invalid request and cannot be cleared. CI is required before considering the change verified.


## GPT CRC32 known-vector regression — 2026-10-16

The partition self-test checks the IEEE CRC-32 known vector `123456789` against `0xCBF43926` independently of the synthetic GPT fixture. This prevents the fixture's CRC-refresh helper and the parser from silently agreeing on a non-standard CRC implementation. The GPT header and entry-array corruption tests remain separate. CI is required before considering this addition verified.


## GPT entry-array read failure regression — 2026-10-16

The GPT parser's checksum-validation path now has explicit I/O-failure coverage. The in-memory block callback fails only when LBA 2 (the synthetic primary entry-array sector) is read, after the GPT header itself has been read successfully. The test requires `PROS_PARTITION_EIO` and a zero output count, ensuring no stale partition count escapes on media I/O failure. It does not write to a host disk. CI must pass before considering this regression verified.
