# ProsenjitFS Design and Implementation Plan

## Status
This document is the implementation contract for the native ProsenjitFS filesystem. It is intentionally separate from the compatibility FAT32, exFAT and NTFS drivers.

Current kernel-side implementation covers format/mount validation, persistent allocation, inode/extents, journal replay, hash-indexed directories, create/mkdir, read/write/truncate, unlink/rename, VFS integration, and corruption/crash-point self-tests. The in-kernel self-test also exercises multi-extent file persistence across remounts, shrink/truncate reclamation, empty/non-empty directory unlink semantics, rename collision rejection, and cross-parent file rename persistence. The host formatter is available as tools/mkprosenjitfs.c and is smoke-tested in CI. The isolated QEMU integration branch now includes a polled ATA PIO block device and mounts a real QEMU IDE disk image through ProsenjitFS, including write/read and unmount/remount persistence verification.

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
Required ordering: construct metadata images; write journal records; flush; write commit; flush; write home locations; flush; retire/reuse the journal transaction.
Recovery replays only CRC-valid committed transactions. Replays are idempotent because records are complete replacement images.
The block-device layer gets an optional flush primitive. Existing initializers remain source-compatible; devices without a flush callback are treated as lacking a durability barrier, so ProsenjitFS will not claim power-loss durability on such a device.
Metadata is journaled. File data is written before the metadata transaction that publishes newly allocated blocks, with a flush barrier before publishing metadata.

## Integrity
CRC32C (Castagnoli) is used for error detection, not authentication or encryption. It protects superblocks, journal records, inode records, directory-entry blocks and allocation bitmap blocks. The checksum field is zero while calculating the checksum.

## Security
ProsenjitFS does not create a second authorization system. VFS flags, process capabilities, syscall authorization and the existing security admission boundary remain authoritative.
On-disk uid/gid/mode are metadata and cannot bypass kernel capability checks. Reserved fields allow future security metadata without changing the core inode ABI.

## Formatter
The native formatter is implemented as prosenjitfs_format() in the filesystem layer. A host-side tools/mkprosenjitfs utility uses the same on-disk format definitions and does not depend on Linux mkfs implementations. The host formatter is built and smoke-tested in CI.

## Mount validation
Mount validates magic/version, feature flags, block size and volume bounds, all metadata regions, bitmap sizes, inode-table bounds, root inode, persistent inode allocation, inode/extent references, extent accounting/overlap, directory indexes and entries, checksums and journal structure. Malformed metadata is rejected; journal recovery occurs before metadata validation and writable mount exposure.

## Implementation sequence
1. Define versioned structures and CRC32C.
2. Add optional block-device flush support without breaking existing callers.
3. Implement formatter and superblock/region validation.
4. Implement persistent bitmap allocation.
5. Implement inode and extent persistence.
6. Implement journal transaction/recovery.
7. Implement hash-indexed directories and strict UTF-8 names.
8. Implement VFS root/lookup/readdir/create/mkdir.
9. Implement file read/write/truncate and safe growth.
10. Implement unlink/rename and transaction coverage.
11. Add corruption and crash-point tests.
12. Add host-side format tests.
13. Add ProsenjitFS filesystem probing/storage discovery.
14. Add a minimal polled ATA PIO block-device path for the QEMU IDE disk.
15. Add QEMU regression using a real ProsenjitFS disk image, including write/read and unmount/remount persistence.
16. Run complete existing CI regression and preserve FAT32/exFAT/NTFS behavior.

## Current dependency boundary
The isolated QEMU integration branch exposes a real ATA PIO block device for the primary IDE channel and uses it to mount a host-formatted ProsenjitFS image under QEMU. The driver is intentionally polled and 28-bit LBA for this first hardware path; AHCI/NVMe remain future controller backends. The memory-backed self-tests remain required because they cover deterministic corruption and crash-point cases that are not practical to reproduce on every boot.

## Completion rule
ProsenjitFS is not complete until format/mount/validation, persistent allocation, directories, Unicode names, file I/O, rename/delete, journal recovery, corruption handling, VFS/security integration, a real QEMU disk path, complete CI regression and accurate documentation are verified. The ATA PIO QEMU gate is now implemented on the isolated branch; the remaining controller work is portability/coverage (AHCI/NVMe), not a blocker for the first real-disk filesystem integration.