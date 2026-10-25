# ProsenjitFS Design and Implementation Plan

## Part 2 follow-up — VFS read output-count contract (2026-10-24)

- [x] Audited `vfs_file_read()`: unlike the write fix in this branch, it cleared `bytes_read` only after argument and permission checks. A rejected read could therefore leave a stale caller-provided count.
- [x] Changed `kernel/vfs.c` to clear a non-null output count before validation/dispatch; added regressions for permission-denied and filesystem-`EIO` reads using a nonzero sentinel.
- [x] Verified on exact PR #235 head `89a35608bcd278b01977f5abded40f2208208d9e`: [Build/boot-test #40119299299](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119299299) and [Security architecture #40119299311](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119299311) passed; merged as `d82d1010f3c58696c15dfb09c326ba2b6d612e43`.
- Scope: VFS output contract only; no on-disk format change, no crash-atomicity or hardware-durability claim.


## Status
This document is the implementation contract for the native ProsenjitFS filesystem. It is the only filesystem implementation currently linked into the kernel. FAT32, exFAT and NTFS compatibility support is intentionally externalized.

## Current implementation scope — 2026-09-30

The provider now covers persistent file-data I/O, bitmap-backed inode/block allocation and reclamation, hash-indexed directory lookup/readdir/create, transactional mkdir/rmdir, transactional unlink/rename, cross-directory moves with directory-parent cycle protection, redo-journal replay, torn-transaction discard, journal checksum corruption rejection, and mount-time metadata corruption checks in the deterministic memory-backed self-test.


### Crash/power-loss verification — 2026-10-06

The native deterministic self-test now models a real device durability boundary with a durable medium plus a volatile write-cache layer. A real VFS file overwrite is interrupted at transaction stages 1 through 9, including journal header/image persistence, commit persistence, home-block persistence, and journal retirement. The crash reset discards unflushed cache blocks, drops the in-memory filesystem state without a clean unmount, remounts the same durable image, and verifies the expected pre-commit or post-commit data. This provides deterministic power-loss/interrupted-transaction coverage for the real ProsenjitFS transaction path; it is not a claim of physical hardware power-loss testing.

The same self-test also interrupts recovery both before and after the recovery flush and then remounts the same image, verifying that a committed replacement-image transaction can be replayed again without double-application corruption.

The directory mutation regression now exercises directory allocation, nested directories, cross-directory file moves, directory moves, cycle rejection, non-empty-directory removal rejection, and empty-directory reclamation. This is an implementation/self-test milestone. The native ATA PIO -> MBR -> ProsenjitFS QEMU path is implemented and is a mandatory CI gate; the repository must not call the filesystem CI-verified until a post-change workflow run executes all build and boot steps successfully.

This is still not a claim of physical power-loss validation. The native QEMU disk path exercises ATA PIO and a persistent ProsenjitFS partition, but QEMU is not a substitute for a real power-loss hardware test. Devices without a durable flush barrier cannot provide a power-loss durability guarantee.

## Architecture
Storage controller / test block device -> pros_block_device -> MBR/GPT partition view -> VFS -> ProsenjitFS.
ProsenjitFS is the kernel filesystem. Future FAT32, exFAT and NTFS compatibility providers must attach outside kernel-core through the filesystem-provider boundary; the bootloader does not parse ProsenjitFS.

## Version 1 on-disk layout
All filesystem blocks equal the underlying block-device sector size. Supported sizes are powers of two from 512 through 4096 bytes.
Block 0: primary superblock. Block 1: redundant superblock. Then redo journal, persistent block bitmap, persistent inode bitmap, inode table, and remaining file/directory/extent data.
The formatter records every region in the superblock; mount code does not assume fixed metadata offsets beyond the two superblock copies.

## Superblock
Contains magic, format version, feature flags, UUID, block size, total blocks, journal/bitmap/inode-table regions, root inode, transaction sequence, maximum inode/file limits and CRC32C.
The backup copy is validated independently. If both copies are invalid, mount rejects the volume. If only one is valid, mount may use it read-only and marks redundant metadata degraded rather than silently rewriting it. If both copies pass checksum and structural validation but disagree byte-for-byte, mount rejects the volume: choosing one of two conflicting layouts could direct metadata I/O or allocation to the wrong regions. Filesystem probing applies the same conflict rule: it validates both copies before reporting the volume as supported, rejects two valid-but-conflicting copies as corruption, and still discovers a volume when exactly one copy is valid so mount can expose it read-only. The native self-test covers this case by changing the backup UUID and recomputing its checksum, ensuring the rejection is due to disagreement rather than a bad checksum. It also invalidates each copy's checksum in turn and verifies that either single-copy fallback is marked degraded and mounted read-only.

## Inodes and extents
Inodes are persistent 64-bit identifiers and contain type, mode, uid/gid, link count, generation, size, allocated bytes, timestamps, parent inode, extent-list head, directory-index root, feature flags and inode CRC32C.
File data uses a persistent linked extent list. Each extent records logical start, physical start and block count, avoiding a fixed-size file table.
Reserved inode fields are retained for future ACL/capability references, extended attributes, encryption state and format extensions.

The v1 inode reader admits a record only after CRC32C and semantic fields validate. Non-free records must use a known v1 type, contain only defined v1 flag bits, have a nonzero generation and an in-range parent inode; logical size and block-aligned allocated-byte accounting must fit within the volume's data-area capacity. Extent chains and directory index/entry graphs are validated separately by their bounded readers and the read-only integrity scan. These checks do not change the v1 disk layout or make the low-level inode writer a validator; that writer remains usable by corruption-injection tests.

Each directory index's embedded inode ID is checked against the owning inode number supplied by its caller, in addition to validating the index checksum, format version, generation and bucket heads. A checksum-valid index pointing at a different in-range inode is corruption, not a second namespace view. This is a reader-side check within the unchanged v1 layout.

Namespace lookup validates cross-record relationships: an entry must reference an allocated inode, the entry type must match the inode type, and the child's recorded parent must be the containing directory. Direct directory self-links are rejected. The read-only integrity scan remains responsible for global duplicate ownership and longer parent cycles.

Both path lookup and directory enumeration enforce the same child-reference invariant: the directory entry must reference an allocated inode, its declared type must match the inode, and the child's parent must equal the containing directory; direct directory self-links are corruption. This prevents listing from exposing inconsistent metadata merely because it follows the enumeration path rather than normal lookup.

## Directories and Unicode
Directories use deterministic hash buckets pointing to chains of directory-entry blocks rather than a fixed-size entry array.
Each entry contains inode number, node type, UTF-8 byte length, UTF-8 name, name hash, entry checksum and next-entry link.
Version 1 filename policy: VFS input/output is UTF-8; names are case-sensitive; no Unicode normalization is performed; the exact UTF-8 byte sequence is the name identity; invalid UTF-8, NUL and path separators are rejected.

## Allocation
Allocation metadata is persistent bitmaps. The allocator uses bounded first-fit scanning and prefers contiguous runs. Inode allocation uses a separate bitmap.
Mount validates bitmap bounds and every inode/extent reference is range checked before use. No persistent allocation state depends only on RAM.

Keep-size contiguous preallocation is implemented through
`prosenjitfs_preallocate_blocks()`. A v1 request reserves and zeroes up to
eight logical data blocks without increasing inode size, publishes the data
bitmap, extent metadata and inode update in one redo-journal transaction, and
reuses an adjacent tail extent when physically contiguous. The reserved blocks
therefore become immediately writable without another allocation. Larger
dynamic fallocate policies and a VFS-visible fallocate operation remain future
extensions.

## Journal and crash consistency
Version 1 uses a redo journal. A transaction stores complete target-block images followed by a CRC-protected commit record.
Required ordering: construct metadata images; invalidate the reused commit slot and flush; write journal header/images; flush; write commit; flush; write home locations; flush; retire the journal header. Clearing the selected commit slot before publishing the next header prevents a stale valid commit record from a prior mount-local transaction sequence from being mistaken for the new transaction.
Recovery replays only CRC-valid committed transactions. A missing commit marker or stale transaction ID is treated as an uncommitted/torn transaction because a reused slot may retain a prior transaction's valid record; a matching transaction ID with a contradictory record count fails closed as corruption. Replays are idempotent because records are complete replacement images.
The block-device layer gets an optional flush primitive. Existing initializers remain source-compatible; devices without a flush callback are treated as lacking a durability barrier, so ProsenjitFS will not claim power-loss durability on such a device. If a transaction fails after the commit-record write is attempted, the current mount is forced read-only until remount recovery resolves the journal; it must not start another transaction over an uncertain committed record.
Metadata is journaled. File data is written before the metadata transaction that publishes newly allocated blocks, with a flush barrier before publishing metadata.

## Read-only integrity scan API

The kernel-only `prosenjitfs_check_integrity(fs)` API exposes the existing consistency checker for an already active mount. Mount recovery (including committed journal replay) must have completed before the scan is called. The caller must serialize the scan against filesystem mutations; this API does not acquire a filesystem-wide operation lock itself.

The scan is strictly read-only: it validates reachable directory/inode/extent ownership, parent-chain reachability, inode and block allocation bitmap agreement, reserved/tail bitmap bits, and checked metadata references. `prosenjitfs_check_integrity()` preserves the simple status-code API. Callers needing diagnostics may use `prosenjitfs_check_integrity_report(fs, report)`, which fills the normalized status, first consistency-failure source location (zero when no source location is available), number of inode records visited, and number of block-bitmap bits checked, report-only orphan candidates (allocated data blocks without a reachable metadata owner), including the first candidate's volume-relative block number, and report-only orphan inode candidates (allocated, structurally valid non-root inodes with no directory-entry reference and a valid parent chain), including the first inode number. When a report is requested, the scan continues through the bounded block bitmap after recording orphan candidates, returns `PROSENJITFS_ECORRUPT`, and does not modify the volume; the legacy status-only API retains fail-fast behavior. Counts describe work completed before return; they are not claims that every metadata category was fully scanned when the status is an error. The API returns `PROSENJITFS_OK` for a consistent image, `PROSENJITFS_EINVAL` for a null or inactive mount, `PROSENJITFS_EOVERFLOW` for bounded-size arithmetic failures, `PROSENJITFS_EIO` for allocation or device-read failures, and `PROSENJITFS_ECORRUPT` when consistency validation fails. It does not reclaim orphaned allocations, modify metadata, or attempt repair. Report-only orphan allocation and inode-candidate enumeration are implemented by the structured report API; neither path performs repair or reclamation.

## Integrity
CRC32C (Castagnoli) is used for error detection, not authentication or encryption. It protects superblocks, journal records, inode records, directory-entry blocks and allocation bitmap blocks. The checksum field is zero while calculating the checksum.

## Security
ProsenjitFS does not create a second authorization system. VFS flags, process capabilities, syscall authorization and the existing security admission boundary remain authoritative.
On-disk uid/gid/mode are metadata and cannot bypass kernel capability checks. Reserved fields allow future security metadata without changing the core inode ABI.

## Formatter
The native formatter will be implemented as prosenjitfs_format() in the filesystem layer. A host-side mkprosenjitfs utility will use the same on-disk format definitions and will not depend on Linux mkfs implementations.

## Mount validation
Mount validates magic/version, requires all defined v1 feature flags and rejects unknown feature bits, block size and volume bounds, all metadata regions, bitmap sizes, inode-table bounds, root inode, inode/extent references, checksums and journal structure. Malformed metadata is rejected; recovery occurs before writable mount exposure.

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
13. Add ProsenjitFS filesystem probing/storage discovery. **Completed in the native BIOS storage path.**
14. Run the mandatory QEMU regression using the native ProsenjitFS disk image and verify the ATA -> MBR -> ProsenjitFS path.
15. Run complete native ProsenjitFS CI regression and preserve the external filesystem-provider boundary.
16. Add ProsenjitFS self-healing hooks: integrity scan, journal-first recovery, conservative orphan reclamation, degraded-metadata handling, and post-repair verification.
17. Add fault-injection tests for torn writes, corrupted metadata, interrupted recovery, and rollback; automatic destructive repair must remain disabled outside explicit recovery mode.

## Current verification boundary
The repository exposes an ATA PIO block-device path to the native BIOS QEMU image and the mandatory workflow checks the resulting MBR -> ProsenjitFS persistence chain. Deterministic memory-backed tests supplement that path. Physical power-loss durability remains a hardware-validation limitation and is not claimed.

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

## Final Native ProsenjitFS verification/hardening — 2026-10-01

The native provider now validates metadata-region ordering/non-overlap before
mounting and requires both redundant superblock copies to agree when both
validate. Transaction staging preserves previously modified bitmap images when
multiple allocations or frees share one bitmap block.

The deterministic self-test covers the known 0xD6 physical-LBA regression,
explicit binary bytes 00 01 7F 80 A5 D6 FF, allocation-bit persistence,
remount persistence, journal replay/discard/checksum rejection,
fault-injected journal-image and post-commit target failures, and checksummed
metadata-layout corruption.

A requested read-only mount never mutates a dirty journal. Recovery of a dirty
journal therefore requires a writable mount.


## Native ProsenjitFS hardening contract — 2026-10-07

### Inode generation and stale handles

Every vnode captures the persistent inode generation at creation. File read,
write, and truncate operations re-read the authoritative inode and reject the
operation with `PROS_VFS_ESTALE` when the generation or inode type no longer
matches. Deletion advances the generation before freeing the inode record, and
new allocation advances the persisted generation again. This prevents an old
open handle from becoming an alias for a subsequently reused inode number.

Closing a stale handle remains safe because close releases the VFS vnode
reference without dereferencing the replaced inode.

### Append serialization

`O_APPEND` is serialized across the complete Native ProsenjitFS write
operation. The lock covers authoritative inode-EOF selection, allocation,
extent publication, data write, inode update, and journal commit. Append does
not rely on a cached vnode EOF.

### Truncate crash contract

Version 1 truncate shrink operations are bounded by
`PROSENJITFS_MAX_TX_BLOCKS == 16`. The complete shrink metadata update is
preflighted before any home-block mutation. If the conservative transaction
bound cannot represent the complete operation, truncate returns
`PROS_VFS_EOVERFLOW` without publishing a partial shrink. A successful
shrink publishes extent reclamation, bitmap changes, and the new inode size in
one redo-journal transaction.

### Symlinks

`PROSENJITFS_INODE_SYMLINK` remains a reserved v1 inode type. The current VFS
contract has no symlink creation/readlink/path-following operations, so Native
ProsenjitFS rejects symlink inodes at vnode materialization rather than
silently treating them as regular files. This is intentional; no partial
symlink implementation is claimed.


### Transaction failure hygiene

Transaction staging is treated as volatile state. `pfs_tx_add()`,
allocator staging, and inode-bitmap staging clear the in-memory transaction
when validation or capacity fails before commit, preventing a failed
operation from contaminating the next filesystem operation.

### Storage/LBA boundary

Native ProsenjitFS block numbers remain volume-relative filesystem blocks.
The validated LBA helper performs the device-sector conversion and the
partition device remains responsible for applying partition-relative offset.
Metadata, journal, and out-of-range targets are rejected before block I/O.


## Verified source audit baseline — 2026-10-24

This section records findings checked against `main` commit `cfe04a8520ec917a90b284bbc726a8bd0cc83a2d`. It complements the format contract above; it does not replace the exact-head verification rule in the completion matrix.

### Kernel, boot and storage path

- The kernel is a freestanding 64-bit x86-64 kernel primarily written in C, with NASM assembly entry/context-switch/low-level components. `scripts/build.sh` builds the GRUB/Multiboot2 ISO and a separate native BIOS disk image. GCC flags include `-ffreestanding`, `-fno-builtin`, `-fno-pie`, `-fno-stack-protector`, `-mno-red-zone`, `-Wall -Wextra -Werror` and `-O2`.
- The hardware path under test is ATA PIO -> block-device callback interface -> MBR/GPT partition parsing and a partition-relative block-device view -> VFS probe/mount -> ProsenjitFS. The bootloader does not parse ProsenjitFS.
- `struct pros_block_device` exposes sector size/count and read/write/optional-flush callbacks. LBA values are device-local sector indices; checked range/add helpers must be used before I/O. The filesystem's version-1 design requires a power-of-two block size from 512 through 4096 bytes, matching the underlying sector size.
- The public format is in `include/prosenjitfs_format.h`; kernel-private mount/transaction/diagnostic state is in `kernel/include/prosenjitfs.h`; implementation and in-kernel self-test are together in `kernel/prosenjitfs.c`.

### On-disk structures and safety boundaries

- Version 1 uses primary and redundant superblocks, a redo journal, block bitmap, inode bitmap, inode table and file/directory/extent data regions. Region locations are taken from validated metadata rather than assumed fixed offsets beyond the two superblock copies.
- The disk format defines checksummed superblocks, inodes, extents, directory indexes/entries and journal records. CRC32C detects accidental corruption; it is not cryptographic authentication.
- Persistent inode/block bitmaps back allocation. Files map logical to physical blocks through linked extents. Directories use a 32-bucket hash index and linked entries. Names are case-sensitive UTF-8 byte sequences with a maximum of 255 bytes; the documented policy performs no Unicode normalization.
- The VFS contract has explicit vnode/file/mount structures and callbacks for lookup, readdir, create, mkdir, rmdir, unlink, rename, read, write, truncate and close. Paths and names are bounded by `PROS_VFS_PATH_MAX` and `PROS_VFS_NAME_MAX`; traversal, malformed metadata and arithmetic boundary cases still require adversarial regression coverage.
- The transaction contract requires flush barriers around journal publication, commit and home-block writes. Without a block-device flush callback, the implementation cannot claim device-level power-loss durability. A failed transaction after attempting to publish a commit record forces the current mount read-only until recovery resolves the uncertain state.
- The structured integrity scan is read-only. It reports inconsistencies and orphan candidates; it does not repair/reclaim them. Its documented caller must exclude concurrent filesystem mutations.

### Verified implementation boundary and known gaps

- The main-branch workflow at the audit commit passed toolchain validation, ISO build, ISO QEMU boot and native BIOS QEMU boot, and emitted the required ProsenjitFS success marker. Security architecture checks also passed at that same commit.
- These results are deterministic test/emulator evidence, not physical drive power-loss evidence. No real disk was formatted or modified during this audit.
- The implementation is concentrated in one very large translation unit (`kernel/prosenjitfs.c`) with both filesystem logic and a sizable self-test. Changes to test injection and diagnostics must remain bounded and must not obscure which invariant failed.
- The active `kernel/block.c` includes explicitly labelled temporary cross-layer block-init tracing and an O0/noinline diagnostic path. Audit it for necessity and remove or isolate it only after its remaining diagnostic purpose is verified; do not remove it blindly as part of this documentation-only phase.
- `docs/PROSENJITFS-COMPLETION-MATRIX.md` is the source of truth for current CI evidence; `docs/PROSENJITFS-IMPLEMENTATION-PLAN.md` and `docs/PROSENJITFS-TEST-MATRIX.md` define the next acceptance gates.


## FS-28 — Truncate-shrink tail isolation

When a truncate shrink ends inside an allocated data block, Native ProsenjitFS zeros
from the new EOF to the end of that block in the same redo-journal transaction as
the changed inode size and any extent/bitmap updates. This prevents a later write
past EOF from re-exposing bytes left beyond the prior EOF. The v1 disk layout is
unchanged. Runtime status must be taken from exact-head Build/boot-test and Security
architecture CI; emulator/mock-device results do not certify physical power-loss
durability.


## FS-29 — Inode allocator output contract

`prosenjitfs_alloc_inode()` only publishes its output inode number after the inode
bitmap reservation transaction succeeds. An I/O failure must return an error and
leave the caller's output unchanged. The regression injects a pre-commit journal
write failure and checks output/media invariants before accepting the result.


## FS-28 / FS-29 — exact-head verification record (2026-10-25)

Code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f` passed
[Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331)
and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332).
The build logs contain the ProsenjitFS self-test, native filesystem LBA-boundary,
and ATA PIO -> MBR -> ProsenjitFS persistent-mount success markers in ISO and
native BIOS QEMU runs. This evidence is for the mock-device/emulator pipeline,
not physical-device power-loss certification or general concurrent-mutation safety.


## FS-30 — Journal-retirement error contract

`pfs_journal_clear()` returns named filesystem errors for invalid internal state and
propagates errors from LBA validation, block writes and flushes without replacing
them with undocumented numeric values. A failed recovery-time journal retirement
must report I/O error and be retryable after the underlying device fault clears.
The self-test covers write and flush failures after an interrupted pre-commit
transaction; the v1 disk format and journal protocol are unchanged.


## FS-31 — Committed journal replay write failure

A failed home-block write during recovery must return EIO without clearing the durable
commit record. After a simulated power loss discards the incomplete volatile replay,
the next mount must reapply all journal images idempotently, complete the committed
unlink, and pass integrity checks. This is deterministic mock-device evidence only.


## FS-31 — exact-head verification (2026-10-25)

Code/test head `73bff7fe8aad278e04f08ee7594b901bb11da6f9` passed [Build/boot-test #8593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866752) and [Security architecture #8129](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866715). The logs contain
ProsenjitFS self-test, LBA-boundary and ATA/MBR persistent-mount success markers
in both ISO and native BIOS QEMU paths. The replay-write failure, simulated
power-loss retry, committed unlink and integrity assertions passed. This remains
mock-device/QEMU evidence, not physical-device power-loss certification.


## FS-32 — Preallocation free-state error contract

The metadata-block selection loop in `prosenjitfs_preallocate_blocks()` propagates
negative errors from `pfs_data_block_is_free()`. Only 0 (allocated) and 1 (free)
are valid normal outcomes. The regression injects a bitmap read EIO at that scan
and verifies that allocation aborts without modifying durable, volatile or dirty
media state; the v1 format and journal protocol are unchanged.
