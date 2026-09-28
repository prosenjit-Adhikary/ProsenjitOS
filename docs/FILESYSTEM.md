# ProsenjitOS Filesystem Architecture

## Goal

ProsenjitOS will treat filesystem support as a first-class kernel I/O subsystem while keeping individual filesystem implementations modular.

    Application
       |
    File / directory API
       |
    VFS
       |
    Filesystem driver
       |-- FAT32
       |-- exFAT
       |-- NTFS
       |-- ProsenjitFS
       |
    Block-device layer
       |-- USB mass storage
       |-- ATA/SATA
       |-- NVMe
       |
    Partition layer (MBR/GPT)
       |
    Physical storage

The VFS must not depend on a specific disk controller or filesystem format.

## Supported filesystem targets

### FAT32

FAT32 is required for broad interoperability with older removable media and boot/recovery media.

### exFAT

exFAT is required for modern flash drives and SD/removable media.

### NTFS

NTFS is a mandatory compatibility target, not an optional future filesystem. Windows uses NTFS as its default modern filesystem and NTFS supports richer metadata, ACLs, journaling and large files than FAT32/exFAT.

The ProsenjitOS NTFS implementation will be staged:

1. NTFS boot-sector validation
2. volume geometry and cluster calculations
3. MFT record parsing
4. resident/non-resident attribute parsing
5. directory index traversal
6. Unicode filename handling
7. read-only file access
8. security metadata exposure through the VFS
9. safe write support
10. journal/recovery handling
11. advanced NTFS features where justified

The first NTFS milestone will deliberately be read-only. This prevents an incomplete driver from corrupting a Windows data volume.

### Current NTFS directory metadata milestone

The NTFS parser now validates and exposes the resident `$FILE_NAME` attribute fields needed for directory entries: parent MFT reference, allocated/real file size, flags, reparse tag, namespace, and UTF-16LE filename bytes. It also validates resident `$INDEX_ROOT` headers and iterates aligned index entries, including terminal-entry and sub-node flag rules. Directory lookup and `$INDEX_ALLOCATION` traversal remain the next layer above these parsers.

### Native ProsenjitFS

ProsenjitFS is the native filesystem target. Its on-disk format will be designed around crash recovery, checksummed metadata, permissions/capabilities, efficient directory indexing, large files, security metadata and atomic metadata updates.

## VFS contract

Every filesystem driver will expose a common VFS-facing contract:
- filesystem probe
- mount/unmount
- root vnode/inode
- lookup
- create
- remove
- open
- close
- read
- write
- stat
- directory iteration
- sync

Read-only filesystem drivers may reject write operations explicitly.

## Removable-media flow

A USB pendrive must not be assumed to be FAT32.

    USB device
      -> mass-storage driver
      -> block device
      -> MBR/GPT partition discovery
      -> filesystem probe
          -> FAT32
          -> exFAT
          -> NTFS
          -> unknown
      -> VFS mount

The partition layer performs explicit LBA bounds checks and non-destructive metadata validation before filesystem probing.

## Security integration

    USB / download / file creation
              |
              v
            VFS
              |
     Security admission broker
              |
     SHA-256 + signature + provenance + policy
              |
       Allow / quarantine / block

The kernel enforces the resulting policy but does not make external reputation-network calls itself.

For NTFS, Windows ACL/security metadata will not automatically become unrestricted ProsenjitOS privileges. The VFS/security layer will translate foreign filesystem metadata into the ProsenjitOS permission model according to an explicit compatibility policy.

## Data-safety rules

- Unknown filesystem signatures are never mounted as a guessed filesystem.
- Out-of-range metadata is rejected.
- Integer overflow in sector/cluster calculations is rejected.
- NTFS is initially mounted read-only.
- Failed journal/recovery state must not trigger destructive repair automatically.
- Untrusted removable media is subject to the file-admission/security pipeline.
- Critical ProsenjitOS files are protected from automatic destructive deletion.
- Filesystem corruption handling prefers recovery/quarantine over silent modification.

## Implementation order

1. Block-device abstraction
2. Partition discovery
3. VFS core
4. In-memory/RAM-backed VFS regression backend
5. FAT32 read-only driver
6. exFAT read-only driver
7. NTFS read-only driver
8. ATA/USB/NVMe storage drivers
9. Writable filesystem support
10. Native ProsenjitFS
11. Journaling/recovery
12. Full security-policy integration

The ordering intentionally separates filesystem parsing from hardware drivers so each layer can be tested independently.
