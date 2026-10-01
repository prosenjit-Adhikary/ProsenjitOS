# NTFS Write Implementation Status

## Current supported write path

ProsenjitOS now has a real NTFS file-data write path for validated, unnamed `$DATA` streams.

Supported cases:
- resident unnamed `$DATA` overwrite
- nonresident unnamed `$DATA` overwrite
- partial-sector read-modify-write
- full-sector writes
- growth inside an already allocated non-sparse extent
- zero-filling the gap between the old EOF and a forward write
- updating nonresident `$DATA` real/initialized sizes
- updating resident `$FILE_NAME` data-size copies when applicable
- `$Bitmap` free-cluster scanning and allocation
- exact allocation immediately after the final extent when available
- non-contiguous extent allocation when contiguous growth is unavailable
- mapping-pairs append for a newly allocated extent
- bitmap rollback when runlist metadata expansion cannot be committed
- VFS write integration and vnode-size synchronization

The implementation deliberately rejects unsupported metadata-changing cases instead of silently producing incomplete NTFS metadata.

## Current regression coverage

The synthetic NTFS disk exercises:
1. resident write and USA re-sealing
2. nonresident overwrite across a sector boundary
3. preallocated EOF growth with gap zero-fill
4. contiguous cluster allocation
5. fragmented cluster allocation
6. mapping-pairs encoding for an appended extent
7. `$Bitmap` state after allocation
8. VFS open/read/write/reopen verification
9. `$INDEX_ALLOCATION` full-leaf split, new INDX-node creation, `$INDEX_ROOT` separator promotion, allocation growth, and lookup routing

The build workflow requires the NTFS file-data write marker during the QEMU regression boot.

## Durability boundary

The NTFS metadata/data write path now issues a block-device flush barrier before reporting a successful MFT/data write. This closes the device-cache durability boundary when the underlying device provides a real flush primitive. A device without a flush callback still provides no physical durability guarantee.

## Important transaction boundary

The current metadata/data write path is **not journaled**. Allocation is rolled back when a pre-commit metadata operation fails, but a failure after physical data sectors have already been written can leave stale data in clusters that are subsequently returned to the free bitmap.

That is acceptable for the current controlled milestone only because the driver does not claim crash-safe filesystem transactions.

Full crash-safe write support still requires NTFS `$LogFile` transaction handling, restart-page/log-page validation, redo/undo transaction replay, and a recovery model.

## Next implementation milestones

The current creation path covers:
1. allocate and initialize a free MFT record
2. create a resident `$FILE_NAME`
3. create an initial unnamed `$DATA` attribute
4. insert the new `$FILE_NAME` key into the parent directory index
5. expose the new file through VFS `O_CREATE`

The next NTFS write stages are:
1. root→`$INDEX_ALLOCATION` conversion when a directory has no existing index-allocation stream
2. recursive splitting of a non-root internal index node when its parent is full
3. synchronize `$MFTMirr` where required
4. add deletion and reclamation
5. add journal-aware commit/recovery

A future implementation must also handle MFT record growth through additional records/attribute lists rather than assuming every attribute fits in one 1 KiB record.

## Deliberate exclusions

The current path does not yet write:
- named data streams
- sparse mappings
- compressed data
- encrypted data
- arbitrary attribute-list expansion
- root→`$INDEX_ALLOCATION` conversion and recursive internal-node splitting
- security descriptors
- `$LogFile` journal transactions and crash-recovery replay

These should be added only with corresponding metadata validation and regression tests.

### Latest safety hardening

- MFT record reuse now advances the record sequence number so stale file references are not silently revived.
- Directory creation checks for an existing case-insensitive name before allocating a new MFT record.
- Directory index insertion preserves NTFS $UpCase ordering and rejects case-insensitive duplicates.
- Resident `$INDEX_ROOT` entries can now be removed by child file reference; `$INDEX_ALLOCATION` delete/rebalance remains intentionally rejected.
- VFS directory lookup clears temporary $UpCase comparison state on all early failures.

- File creation now rejects inactive/read-only mounts at the public NTFS create API boundary.
- VFS create and lookup enforce the same UTF-8 filename control/reserved-character rules.


### Journal compatibility boundary

The repository does not yet contain an NTFS-compatible `$LogFile` transaction engine. NTFS identifies `$LogFile` as the transactional logging file, and its recovery state is represented by restart pages, LSNs and NTFS client log records. This project must not describe a generic write-ahead log as NTFS `$LogFile` compatibility. The current write path therefore remains a durability-ordered metadata/data path, not crash-recovery-complete.
