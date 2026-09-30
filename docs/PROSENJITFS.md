# ProsenjitFS Design and Implementation Plan

## Status
This document is the implementation contract for the native ProsenjitFS filesystem. It is intentionally separate from the compatibility FAT32, exFAT and NTFS drivers.

## Current implementation scope — 2026-09-30

The provider now covers persistent file-data I/O, bitmap-backed inode/block allocation and reclamation, hash-indexed directory lookup/readdir/create, transactional mkdir/rmdir, transactional unlink/rename, cross-directory moves with directory-parent cycle protection, redo-journal replay, torn-transaction discard, journal checksum corruption rejection, and mount-time metadata corruption checks in the deterministic memory-backed self-test.

The directory mutation regression now exercises directory allocation, nested directories, cross-directory file moves, directory moves, cycle rejection, non-empty-directory removal rejection, and empty-directory reclamation. This is an implementation/self-test milestone; current GitHub Actions runner failures occur before workflow steps are created, so this commit is not yet CI-verified.

This is still not a claim of physical power-loss validation: a real QEMU disk-device test remains pending until an ATA/AHCI/NVMe/USB-visible block-device path exists. Devices without a durable flush barrier cannot provide a power-loss durability guarantee.

## Architecture
Storage controller / test block device -> pros_block_device -> MBR/GPT partition view -> VFS -> ProsenjitFS.
FAT32, exFAT and NTFS remain compatibility filesystem providers. Filesystem parsing stays in the kernel filesystem layer; the bootloader does not parse ProsenjitFS.

## Version 1 on-disk layout
All filesystem blocks equal the underlying block-device sector size. Supported sizes are powers of two from 512 through 4096 bytes.
Block 0: primary superblock. Block 1: redundant superblock. Then redo journal, persistent block bitmap, persistent inode bitmap, inode table, and remaining file/directory/extent data.
The formatter records every region in the superblock; mount code does not assume fixed metadata offsets beyond the two superblock copies.

## Superblock
Contains magic, format version, feature flags, UUID, block size, total blocks, journal/bitmap/inode-table regions, root inode, transaction sequence, maximum inode/file limits and CRC32C.
The backup copy is validated independently. If both copies are invalid, mount rejects the volume. If only one is valid, mount may use it read-only and marks redundant metadata degraded rather than silently rewriting it.

## Inodes and extents
Inodes are persistent 64-bit identifiers and contain type, mode, uid/gid, link count, generation, size, allocated bytes, timestamps, parent inode, extent-list head, directory-index root, feature flags and inode CRC32C.
File data uses a persistent linked extent list. Each extent records logical start, physical start and block count, avoiding a fixed-size file table.
Reserved inode fields are retained for future ACL/capability references, extended attributes, encryption state and format extensions.

## Directories and Unicode
Directories use deterministic hash buckets pointing to chains of directory-entry blocks rather than a fixed-size entry array.
Each entry contains inode number, node type, UTF-8 byte length, UTF-8 name, name hash, entry checksum and next-entry link.
Version 1 filename policy: VFS input/output is UTF-8; names are case-sensitive; no Unicode normalization is performed; the exact UTF-8 byte sequence is the name identity; invalid UTF-8, NUL and path separators are rejected.

## Allocation
Allocation metadata is persistent bitmaps. The allocator uses bounded first-fit scanning and prefers contiguous runs. Inode allocation uses a separate bitmap.
Mount validates bitmap bounds and every inode/extent reference is range checked before use. No persistent allocation state depends only on RAM.

## Journal and crash consistency
Version 1 uses a redo journal. A transaction stores complete target-block images followed by a CRC-protected commit record.
Required ordering: construct metadata images; write journal records; flush; write commit; flush; write home locations; flush; retire/reuse the journal transaction. Mount recovery now explicitly tests committed replay, torn transactions, and corrupt commit checksums.
Recovery replays only CRC-valid committed transactions. Replays are idempotent because records are complete replacement images.
The block-device layer gets an optional flush primitive. Existing initializers remain source-compatible; devices without a flush callback are treated as lacking a durability barrier, so ProsenjitFS will not claim power-loss durability on such a device.
Metadata is journaled. File data is written before the metadata transaction that publishes newly allocated blocks, with a flush barrier before publishing metadata.

## Integrity
CRC32C (Castagnoli) is used for error detection, not authentication or encryption. It protects superblocks, journal records, inode records, directory-entry blocks and allocation bitmap blocks. The checksum field is zero while calculating the checksum.

## Security
ProsenjitFS does not create a second authorization system. VFS flags, process capabilities, syscall authorization and the existing security admission boundary remain authoritative.
On-disk uid/gid/mode are metadata and cannot bypass kernel capability checks. Reserved fields allow future security metadata without changing the core inode ABI.

## Formatter
The native formatter will be implemented as prosenjitfs_format() in the filesystem layer. A host-side mkprosenjitfs utility will use the same on-disk format definitions and will not depend on Linux mkfs implementations.

## Mount validation
Mount validates magic/version, feature flags, block size and volume bounds, all metadata regions, bitmap sizes, inode-table bounds, root inode, inode/extent references, checksums and journal structure. Malformed metadata is rejected; recovery occurs before writable mount exposure.

## Implementation sequence
1. Define versioned structures and CRC32C.
2. Add optional block-device flush support without breaking existing callers.
3. Implement formatter and superblock/region validation.
4. Implement persistent bitmap allocation.
5. Implement inode and extent persistence.
6. Implement journal transaction/recovery.
7. Implement hash-indexed directories and strict UTF-8 names.
8. Implement VFS root/lookup/readdir/create/mkdir/rmdir.
9. Implement file read/write/truncate and safe growth.
10. Implement unlink/rename and transaction coverage.
11. Add corruption and crash-point tests.
12. Add host-side format tests.
13. Add ProsenjitFS filesystem probing/storage discovery.
14. Add QEMU regression using a real ProsenjitFS disk image once a QEMU-visible block-device path exists.
15. Run complete existing CI regression and preserve FAT32/exFAT/NTFS behavior.
16. Add ProsenjitFS self-healing hooks: integrity scan, journal-first recovery, conservative orphan reclamation, degraded-metadata handling, and post-repair verification.
17. Add fault-injection tests for torn writes, corrupted metadata, interrupted recovery, and rollback; automatic destructive repair must remain disabled outside explicit recovery mode.

## Current dependency boundary
The repository does not yet expose a real ATA/AHCI/NVMe block device to the kernel/QEMU boot test. Therefore physical QEMU disk detection cannot honestly be claimed until that dependency exists. Deterministic memory-backed block-device self-tests will exercise the real filesystem code now; they supplement rather than replace the eventual QEMU disk test.

## Completion rule
ProsenjitFS is not complete until format/mount/validation, persistent allocation, directories, Unicode names, file I/O, rename/delete, journal recovery, corruption handling, VFS/security integration, QEMU disk integration, complete CI regression and accurate documentation are verified.

## Self-healing design requirements
Self-healing is a planned reliability layer, not an excuse to hide corruption. Recovery must be conservative and observable:

- **Detect:** identify checksum, range, allocation, journal, inode, extent, and directory inconsistencies.
- **Verify:** distinguish recoverable corruption from ambiguous damage before modifying persistent state.
- **Repair:** prefer journal replay, redundant metadata, and orphan reclamation; never silently overwrite uncertain user data.
- **Verify again:** re-run integrity checks after every repair transaction.
- **Rollback:** if post-repair validation fails, stop writable exposure and enter degraded/recovery mode.
- Destructive repair is disabled by default and requires an explicit recovery-mode decision.
- Every recovery action must be recorded in the system recovery/security log.
- A future system health manager may consume these filesystem health signals, but filesystem recovery must remain safe when the rest of the OS is unavailable.