# ProsenjitOS Universal Storage Interpretation Engine (USIE)

**Target release:** ProsenjitOS 0.2  
**Status:** Proposed architecture / documentation only  
**Implementation status:** Not implemented by this document  
**Scope:** Read, interpret, and eventually write data on existing storage formats through a common user-facing file interface  
**Design rule:** Keep physical storage access, filesystem interpretation, recovery, and file-format processing as separate layers.

---

## 1. The idea in simple words

A disk does not store a ready-made folder tree. It stores physical states that the storage device controller exposes to the operating system as logical sectors. Filesystems such as NTFS, ext4, FAT32, exFAT, APFS, and others define different ways to organize those sectors into partitions, directories, file metadata, and file contents.

ProsenjitOS 0.2 proposes a **Universal Storage Interpretation Engine (USIE)**. It will read logical blocks through the storage-device interface, identify the structures present on the disk, interpret those structures using the appropriate format knowledge, and present the resulting files through one common interface.

The application should not need to know whether a file is stored on NTFS, ext4, FAT32, or ProsenjitFS. It should request a file through the normal ProsenjitOS file API. USIE and the relevant provider will resolve how to read that file from the underlying format.

This takes inspiration from data-recovery software, which can run on one operating system or filesystem and inspect a different filesystem using raw disk access and format-specific parsers. However, ordinary filesystem access and data recovery are different jobs: normal access must preserve consistency and enforce permissions; recovery may inspect damaged metadata and make cautious guesses.

## 2. Important technical clarification

**RLL is not a filesystem.** Run-Length Limited (RLL) is a family of recording codes used in magnetic storage. On modern HDDs, the drive's internal electronics handle magnetic read/write channels, encoding, error correction, and physical-sector details. The operating system normally sees logical block addresses (LBAs), not raw magnetic transitions or RLL bitstreams.

For this reason, USIE starts at the **logical block-device interface**, not at raw magnetic media. Truly reading raw physical signals would require special hardware/controller support and is not a portable operating-system interface.

The intended flow is:

```text
Physical media (magnetic / flash / optical)
        |
        v
Device controller and its error-correction layer
        |
        v
Logical block device (LBA sectors)
        |
        v
Partition and volume discovery
        |
        v
USIE format identification and interpretation
        |
        v
Common file/directory representation
        |
        v
ProsenjitOS applications and services
```

## 3. Architecture overview

```text
Native applications       Windows-compatibility services       System tools
          |                           |                            |
          +---------------------------+----------------------------+
                                      |
                              Common File API
                     open / read / write / seek / stat
                         list / create / rename / delete
                                      |
                           VFS (existing boundary)
                                      |
                         USIE provider / mount broker
                 +--------------------+---------------------+
                 |                    |                     |
          Normal filesystem      Read-only recovery     Native ProsenjitFS
             providers              scanner                provider
          +------+------+       +------+------+                 |
          |      |      |       |      |      |                 |
         NTFS   ext4  FAT/exFAT  signature  metadata     native on-disk parser
          |      |      |       carving    recovery              |
          +------+------+----------+-----------------------------+
                                      |
                           Partition / volume layer
                                      |
                             Block-device interface
                                      |
                        ATA/SATA, NVMe, USB, virtual disks
```

This diagram describes a target architecture, not currently completed support. The current repository's native filesystem is ProsenjitFS, with a VFS/provider boundary intended for future compatibility providers. USIE should fit into that boundary rather than replace ProsenjitFS or move filesystem parsing into the bootloader.

## 4. Responsibilities of each layer

### 4.1 Block-device layer

Provides bounded logical-sector reads and writes. It knows the device's sector size, capacity, supported commands, and whether a flush/durability operation is available. It does **not** understand files, directories, NTFS, ext4, or filenames.

Required safeguards:
- Check every LBA range against device capacity.
- Detect integer overflow in sector and byte calculations.
- Return explicit I/O errors; never treat a failed read as valid zero-filled data.
- Serialize or coordinate access where the device requires it.
- Expose read-only devices as read-only.
- Make cache flush and durability guarantees explicit.

### 4.2 Partition and volume discovery

Finds bounded partitions and volume regions (initially MBR and GPT). It produces a volume view whose reads and writes cannot escape the partition's range. Partition discovery must not assume that every partition contains a supported filesystem.

### 4.3 Format identification

Uses bounded, non-destructive probes to identify likely filesystem formats from validated signatures and structural invariants. A signature alone is not enough to trust a format. The probe must validate lengths, offsets, version/feature flags, and relevant checksums where defined.

If a volume is unknown, encrypted, unsupported, or ambiguous, USIE must report that state rather than guessing and mounting it as a known filesystem.

### 4.4 Filesystem interpretation providers

Each provider understands the on-disk structures for one format family. Examples include:
- NTFS: boot sector, MFT records, attributes, runlists, directory indexes, and supported metadata features.
- ext family: superblock, group descriptors, inode tables, extents or block maps, directory entries, and feature flags.
- FAT family: boot parameters, allocation tables, cluster chains, and directory entries.
- exFAT: allocation bitmap, FAT, entry sets, and cluster chains.
- ISO 9660/UDF: optical-media volume descriptors and directory structures.
- ProsenjitFS: the native ProsenjitOS format and its own journal, allocation, checksums, and metadata rules.

These are examples, not a promise that every listed provider already exists. Each provider needs its own implementation, test suite, and documented supported-feature matrix.

### 4.5 Common file representation

The VFS exposes common concepts: file handles, byte offsets, file length, directory enumeration, metadata, and errors. Provider-specific details must not be silently discarded. The common model should preserve or expose, where applicable:
- file type and size;
- timestamps and ownership/permission information;
- read-only state and write capability;
- symbolic links, hard links, reparse points, and special files where supported;
- alternate/named streams and extended attributes where supported;
- case-sensitivity and filename rules;
- encryption/compression/sparse-file status;
- unsupported-feature warnings.

A common API does not mean every filesystem has identical semantics. Where the target filesystem lacks a feature, the API must report that limitation explicitly.

### 4.6 Recovery subsystem

Recovery is a separate, optional, primarily read-only facility. It may use:
1. **Metadata-based recovery:** use filesystem records that remain available, such as NTFS MFT entries or ext inode/directory metadata.
2. **File carving:** scan blocks for known file signatures and format structure when filesystem metadata is missing.
3. **Consistency analysis:** compare metadata references, allocation maps, checksums, and known structural constraints.

Recovery results must be labelled as confirmed, partial, or uncertain. File carving may not recover original names, directory structure, timestamps, permissions, or fragmented file contents. Recovery must never silently become a normal writable mount or perform automatic destructive repair.

### 4.7 File-format processing and application runtimes

USIE provides file bytes; it does not automatically understand every file's content. PDF, DOCX, PNG, ZIP, executable files, and databases have their own formats and need separate parsers, codecs, applications, or runtimes. Running a Windows executable also requires Windows ABI/API compatibility; filesystem access alone does not make the program runnable.

## 5. Read and write flows

### Reading a file

1. The application calls the common file API with a path or file handle.
2. VFS resolves the mounted volume and provider.
3. The provider maps the file's logical byte range to filesystem blocks/clusters/extents.
4. The block-device layer reads the required LBAs with bounds checking.
5. The provider validates and interprets the on-disk structures, then returns file bytes.
6. VFS applies access-control and security policy before exposing the requested operation.

### Writing a file

1. The application requests a write through VFS.
2. VFS checks credentials/capabilities, mount policy, and whether the provider supports writes.
3. The provider validates allocation and metadata constraints, performs format-specific updates, and uses the format's supported transaction/journal rules.
4. Required flushes and ordering are applied where the device supports them.
5. The provider reports success only when its documented completion conditions are met.

**Read/write support must be feature-specific.** A provider that can read a format is not automatically safe to write it. Initial compatibility milestones should prefer read-only access until mutation, allocation, journal recovery, and crash consistency are tested.

## 6. Why recovery tools can inspect a different filesystem

A recovery application may run from an NTFS volume while inspecting an ext filesystem because the running program's executable format and the target disk's format are independent.

The program can request raw logical-block reads (subject to operating-system permissions), then run its own ext parser or signature scanner. It writes recovered files to a separate destination, ideally not the damaged source disk. It does not need the source filesystem to be the filesystem from which the application was launched.

USIE generalizes this principle: a provider can interpret a target volume independently of the filesystem hosting the application. The important dependencies are block-device access, correct format interpretation, and appropriate privileges—not the host filesystem format.

## 7. Compatibility limits and honest claims

A single universal interface is achievable; universal perfect support is not a realistic initial promise.

Potential limitations include:
- proprietary or incompletely documented formats;
- encrypted volumes without the required key;
- compression, deduplication, snapshots, or copy-on-write semantics;
- unsupported filesystem feature flags or incompatible versions;
- corruption or overwritten file content;
- hardware RAID, storage controllers, unusual sector sizes, or vendor-specific layers;
- access-control differences between operating systems;
- filesystem-specific journal and crash-recovery rules;
- special files, alternate streams, links, and Unicode/case-folding differences.

USIE must maintain a compatibility matrix per provider and per operation. For example, a provider may support mount, lookup, and read but not directory mutation or safe writes. The UI/API should expose that capability instead of claiming complete support.

## 8. Security requirements

Raw block access is powerful and must be restricted. USIE must:
- require explicit capabilities for raw-device access;
- isolate untrusted parsers from kernel-critical code where feasible;
- apply strict bounds checking, checked arithmetic, resource limits, and cycle/recursion limits;
- reject malformed or contradictory metadata;
- avoid auto-mounting unknown volumes with write access;
- default recovery scans to read-only;
- never write recovered data back to the source volume by default;
- avoid executing recovered files automatically;
- integrate file reads and execution with ProsenjitOS security admission/quarantine policy;
- record diagnostics without leaking secrets or unbounded disk contents;
- keep destructive repair disabled by default and require a verified backup or explicit operator action for repair workflows.

A parser bug in a kernel filesystem driver can compromise the whole OS. This is one reason to keep compatibility providers outside kernel-core and consider a constrained userspace service for complex third-party formats, once the process/IPC/security foundation supports it.

## 9. ProsenjitOS 0.2 implementation plan

This is a staged plan. Do not begin broad provider work until the current native ProsenjitFS and VFS contracts are verified.

### Phase 0 — Baseline and contracts
- Inspect the current block-device, partition, VFS/provider, ProsenjitFS, security, and test code.
- Record exact existing capabilities; do not infer support from documentation alone.
- Freeze provider lifecycle and error contracts, capability reporting, mount policy, and read-only/write policy.
- Keep bootloader and filesystem responsibilities separate.

**Exit gate:** design review and existing build/boot/filesystem regressions pass.

### Phase 1 — Safe discovery and read-only foundation
- Validate MBR/GPT volume boundaries.
- Add a bounded probe registry and explicit unknown/unsupported results.
- Add synthetic block devices and malformed-image tests.
- Add read-only provider mounting through VFS without changing ProsenjitFS on-disk format.

**Exit gate:** invalid offsets, overflow, malformed signatures, truncated metadata, and out-of-range reads are rejected deterministically.

### Phase 2 — First compatibility providers
- Choose a small, tractable first target such as FAT32 or ISO 9660 for read-only support.
- Add a documented feature matrix and golden disk-image tests.
- Add directory traversal, Unicode/path tests where applicable, large-file bounds tests, and error propagation.
- Consider NTFS/ext read-only providers only after the first provider and common contracts are stable.

**Exit gate:** reproducible tests prove the advertised read-only features on valid and malformed images.

### Phase 3 — Recovery tools
- Build a read-only volume inspector and recovery scanner.
- Separate metadata-based recovery from file carving.
- Add test images with deleted entries, damaged metadata, fragmented files, and overwritten regions.
- Export recovered content only to a different destination volume by default.

**Exit gate:** source images remain byte-for-byte unchanged during read-only recovery tests; uncertainty and partial recovery are reported accurately.

### Phase 4 — Carefully scoped write support
- Add write support only for a provider with a complete, reviewed mutation design.
- Test allocation/freeing, directory changes, metadata updates, journaling, flush ordering, interrupted writes, and remount recovery.
- Refuse writes when unsupported features could be damaged.
- Do not advertise safe NTFS/ext write support merely because simple overwrites work.

**Exit gate:** provider-specific mutation and crash/recovery tests pass; limitations are documented.

### Phase 5 — Broader coverage
- Add further filesystem providers based on documented demand and available specifications.
- Add read-only support before write support for complex formats.
- Track feature support per format/version and test against independent reference tools and real-world images where safe.
- Keep native ProsenjitFS as the native writable format unless a separate architecture decision changes that goal.

## 10. Testing and verification checklist

Every provider should include:
- valid minimal and representative images;
- malformed/truncated superblocks and directory entries;
- integer overflow and out-of-range block tests;
- corrupted checksums and conflicting metadata;
- unsupported feature flags;
- empty files, large files, fragmented files, sparse files, and Unicode names where applicable;
- directory iteration and path-resolution tests;
- permissions and read-only-device tests;
- short reads, I/O failures, flush failures, and device removal;
- repeat mount/unmount and concurrent-access tests;
- write interruption and remount recovery tests if write support exists;
- regression tests proving ProsenjitFS and boot flows are unchanged.

For every test run, record the commit SHA, exact command/workflow, result, logs, and whether the test used a synthetic device, QEMU, or physical hardware. A documentation change alone is not implementation or runtime verification.

## 11. Out of scope

USIE does not:
- decode raw magnetic transitions or RLL signals through ordinary SATA/NVMe interfaces;
- make every file format executable or viewable without a suitable application;
- bypass encryption or access-control requirements;
- guarantee recovery of overwritten or fragmented files from damaged metadata;
- replace the VFS, native ProsenjitFS, Windows API compatibility layer, or hardware drivers;
- promise full read/write support for all filesystem formats in release 0.2.

## 12. Definition of done for the architecture

The architecture can be considered ready for implementation planning when:
1. The block-device, partition, provider, and VFS boundaries are documented and consistent with the repository.
2. Every provider declares supported operations and unsupported on-disk features.
3. Unknown and malformed volumes fail safely.
4. Read-only interpretation is implemented and tested before broad write support.
5. Recovery is isolated from normal mounts and defaults to read-only.
6. Security and diagnostics requirements are covered by tests.
7. The roadmap distinguishes **planned**, **implemented**, and **verified** work.

**Release 0.2 target:** establish the extensible architecture and demonstrate a small, tested compatibility milestone—not claim universal filesystem compatibility before evidence exists.
