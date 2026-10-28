# ProsenjitOS Filesystem — Single Source of Truth

**Canonical filesystem document:** this is the only active planning, architecture, implementation-status, test-matrix, release-scope, and verification document for every ProsenjitOS filesystem.
**Policy:** do not create separate filesystem roadmap, completion-matrix, test-matrix, implementation-plan, or release-spec files. Update this file instead.
**Last consolidated:** 2026-10-28.
**Important status rule:** code present, planned, tested on an older commit, and verified on the exact candidate commit are different states. Never claim a capability is complete without source and test evidence. Historical appendices below preserve prior audit notes; where they conflict with the current summary at the top, the current summary and exact-head CI evidence take precedence.

## Release feature contract — owner decision (2026-10-25)

This section is the release-scope authority for native ProsenjitFS. Features below are targets and acceptance criteria, not claims that implementation is already complete. Mark a feature complete only after source inspection and tests on the exact release candidate.

### ProsenjitOS 1.0 — native ProsenjitFS

Required:
- Format and mount a ProsenjitFS volume with validated on-disk metadata and version/feature checks.
- Core block/inode allocation and release with bounds checks; detect invalid, duplicate, or out-of-range references.
- Create, lookup, enumerate, rename, and remove files/directories with correct namespace and parent-directory handling.
- File open/read/write/seek/truncate and supported metadata queries; return defined errors for unsupported operations.
- Space accounting and allocation metadata consistency checks.
- Transactional metadata updates using the existing journal/recovery design, with interruption/replay and malformed-journal tests. Do not claim physical power-loss durability without appropriate tests.
- Metadata checksums and corruption detection where defined by the current on-disk format.
- VFS integration for path resolution and consistent error propagation.
- **Mandatory permissions in 1.0:** persistent UID/GID/mode (or the format's documented equivalent), trusted VFS/kernel authorization for every relevant operation, owner/group/other read-write-execute checks, directory traversal/list/create/delete/rename rules, permission changes subject to authorization, and default-deny behavior for unknown credentials or unsupported authorization states. Test allowed and denied cases, including verifying denied operations do not mutate disk state. UID/GID/mode fields alone do not count as enforcement.
- Deterministic self-tests, malformed metadata/journal tests, allocation-boundary tests, and exact-candidate build/boot/persistence/security test evidence.

The exact supported limits (maximum file size, directory size, extent count, link semantics, timestamp precision, concurrency, and durability guarantees) must be documented from the implementation. Do not promise POSIX completeness, hard links, ACLs, quotas, snapshots, compression, or general concurrent safety unless implemented and verified.

### ProsenjitOS 1.1 — mandatory native filesystem encryption

Required for 1.1:
- Reviewed threat model and encryption format/specification before implementation is called release-ready.
- Confidentiality and integrity protection using a vetted authenticated-encryption construction; never invent a custom cryptographic algorithm.
- Defined key generation, provisioning, unlock, rotation/replacement, failure handling, and secure key lifetime/zeroization policy.
- Authenticated metadata and file-data I/O with nonce/sector uniqueness rules and corruption/tamper detection.
- Integration with allocation, journaling, crash recovery, permissions, formatting, and mount/unmount paths.
- Explicit behavior for wrong keys, damaged tags, partial writes, interrupted transactions, and recovery; fail closed without exposing unauthenticated plaintext as valid data.
- Tests for wrong keys, tampering, replay/rollback assumptions, crash/recovery, key-handling failures, and denied-access/no-mutation cases. Encryption is not complete until exact-candidate tests pass and the design is reviewed.

### ProsenjitOS 1.2 and later — staged remaining features

Add other native filesystem capabilities incrementally after 1.0 permissions and 1.1 encryption are stable. Each feature must have a separate design, compatibility/migration plan where relevant, implementation, negative tests, and exact-candidate evidence. Candidate later features include ACLs beyond basic owner/group/other permissions, quotas, snapshots, compression, deduplication, online checking/repair, performance optimizations, richer journaling/durability, and broader concurrency guarantees. This list is a future backlog, not a promise that every item will be delivered in a particular release.

### Release separation reminder

FAT32 and exFAT are separate compatibility providers targeted for 1.0; EXT/EXT4 and NTFS are deferred to later releases. These formats are not part of native ProsenjitFS. The native feature schedule above applies only to ProsenjitFS.

## FS-32 — Invalid journal target geometry regression (2026-10-28)

- Added on current-main-based candidate branch `fix/prosenjitfs-invalid-journal-geometry-current-main-20261028`.
- Builds checksum-valid committed journal records whose second target is reserved block 0, reserved block 1, `total_blocks` (out of range), or the journal region start.
- Requires mount recovery to return `PROSENJITFS_ECORRUPT` and preserve durable, volatile, and dirty mock-device CRCs.
- **Status: verified on exact PR #271 head** `88516324a1007753aa43b213bd17bb0c5ea2700b`. Build/boot-test [run #40674196387](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196387) and Security architecture [run #40674196316](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196316) both passed; merged into `main` as `965e70902c725caae280b6403f851c93ed8d7059`. The regression covers reserved/out-of-range journal target geometry on a deterministic mock device only; it does not establish physical-drive power-loss durability or general VFS concurrency safety.
- Scope is deterministic mock-device validation; this does not establish physical-device power-loss durability or general VFS concurrency safety.

## FS-34 — Trusted permission-enforcement source audit (2026-10-28)

- **Result: Version 1.0 release blocker confirmed; no enforcement fix is claimed.** Current `kernel/vfs.c` checks open-handle `PROS_VFS_O_READ`/`PROS_VFS_O_WRITE` flags and mount read-only state, but those are handle/mount policy checks, not owner/group/other permission enforcement.
- `kernel/prosenjitfs.c` initializes and persists inode `uid`, `gid`, and `mode` fields, but the current source audit found no use of those fields in authorization checks. `kernel/include/vfs.h` exposes no effective-credential context in its file-operation interfaces; `kernel/include/process.h` has process capability policy, which must be connected through a trusted authorization path rather than inferred from inode metadata.
- Missing acceptance evidence includes owner/group/other read-write-execute/search/list checks, create/delete/rename and permission-change authorization, default-deny for unknown credentials, and denied-operation tests proving durable/volatile/dirty media are unchanged.
- Next safe implementation gate: define the effective credential source and VFS authorization API first; enumerate every path-based and open-handle operation; then add allow/deny tests and exact-head CI. Do not bolt isolated mode-bit checks into ProsenjitFS callbacks or trust caller-supplied UID/GID.
- This is a source-level audit only. No runtime permission enforcement or release readiness is claimed.

## FS-35 — UTF-8 directory-name boundary regression (2026-10-28)

- Added regression coverage for empty/reserved names, overlong encodings (2/3/4-byte forms), isolated continuation, truncated sequence, invalid middle continuation, surrogate scalar, leading byte above U+10FFFF, valid 1–4-byte UTF-8, U+10FFFF, exact 255-byte acceptance and 256-byte rejection.
- Public VFS integration creates/removes valid UTF-8 names and a 255-byte component; malformed UTF-8 and 256-byte components must be rejected before metadata mutation, preserving durable/volatile/dirty mock-media CRCs and a clean integrity scan.
- **Status: exact-head CI pending.** Do not mark verified or merge until Build/boot-test and Security architecture checks pass on this exact PR head.
- Scope: deterministic mock-device validation only; this does not establish malformed on-disk directory-entry coverage, general namespace concurrency, or physical-drive power-loss durability.

## FS-33 — Preallocation bitmap-read error propagation (2026-10-28)

- Ported the allocator fix onto the current `main` baseline after the older candidate encountered merge conflicts.
- The extent-metadata candidate scan now propagates negative returns from `pfs_data_block_is_free()`; only 0 (allocated) and 1 (free) are ordinary results.
- Added a deterministic read-fault regression requiring EIO, consumed fault injection, unchanged durable/volatile/dirty mock-device CRCs, writable mount state, and a passing integrity check.
- **Status: merged and exact-head CI passed.** PR [#273](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/273), head `3f540676c38f897daa8d61c80740050edbbc96e7`, passed Build/boot-test [run 40674294185](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674294185) and Security architecture [run 40674293924](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674293924). Merged as `2069fd7a28517fbe311a20a5140ac43eefbdd2cd`.
- The v1 on-disk format is unchanged. Mock-device evidence does not certify physical-drive power-loss durability or general VFS concurrency.

## 0. Reconciliation audit — 2026-10-25

### Owner-confirmed architecture decision

**Confirmed 2026-10-25:** only native ProsenjitFS belongs in the kernel core. FAT32, exFAT, EXT/EXT4, NTFS, and future filesystem formats are separate drivers/providers connected through VFS. **ProsenjitOS 1.0 scope: native ProsenjitFS plus FAT32 and exFAT providers, including end-to-end USB removable-media support for both.** EXT/EXT4, NTFS, and other compatibility filesystems are planned for later releases, subject to implementation and verification. No compatibility filesystem parser is to be merged into ProsenjitFS or treated as native kernel-core filesystem code.

This section records cross-document/source checks performed during consolidation. It prevents old design notes from being mistaken for current implementation evidence.

### Release-scope decision — resolved by owner

The project owner confirmed: **ProsenjitFS is the only native filesystem implementation in the kernel core**. **Version 1.0 includes FAT32 and exFAT providers**, with end-to-end USB removable-media support for both: USB mass-storage, block-device registration, partition discovery, provider mount, VFS file operations, safe unmount, and media-removal tests. **EXT/EXT4, NTFS, and other compatibility filesystem formats belong to later releases**, with each format advertised only after its provider and tests are complete. These providers remain separate from the ProsenjitFS core. This release scope supersedes historical notes suggesting EXT4 as a 1.0 requirement.

### Provider implementation reality

The current `include/vfs_provider.h` and `kernel/vfs_provider.c` provide early registry/ABI scaffolding. The operations currently exposed are probe, mount, and unmount; the registry itself is compiled into the kernel, and `kernel/vfs.c` currently recognizes only `PROS_FS_PROSENJIT`. There is no complete userspace provider process, IPC request path, provider-side VFS operation table for read/write/lookup, or demonstrated sandbox/device broker for compatibility filesystems. Therefore the external-provider architecture is a **design target / partial foundation**, not a completed plug-in runtime. Phase B must complete and test that boundary before any compatibility provider is advertised.

### Compatibility filesystem source audit

The former `docs/NTFS-WRITE-STATUS.md` describes NTFS write behavior and QEMU markers, but the current branch's source/build tree does not contain an NTFS implementation source, an NTFS filesystem enum, or an NTFS object in the build. Treat those statements as historical notes, not evidence of current NTFS read/write support. FAT32 and exFAT are mandatory Version 1.0 provider targets, but remain unverified until source and exact-head tests prove the full implementation. No unsupported volume may be auto-mounted or written.

### Security and removable-media gates

- **Permissions:** mandatory ProsenjitOS 1.0 native ProsenjitFS release gate. Require trusted VFS/kernel authorization and denial-without-mutation tests; metadata fields alone are insufficient.
- **Encryption:** mandatory ProsenjitOS 1.1 native ProsenjitFS release gate. Current encryption documents are design material, not implementation evidence; require reviewed design, key lifecycle, authenticated I/O, journal/recovery integration, and exact-head tests.
- **USB/external storage:** the present storage evidence is ATA PIO + partition/VFS + ProsenjitFS QEMU persistence. It does not prove USB mass-storage discovery or physical pendrive/external HDD/SSD support. Those remain separate Phase C acceptance items.
- **Verification:** the documentation-only consolidation has no runtime effect. No filesystem runtime test is claimed by this audit. Exact-head build/boot and security workflow results must be checked for the candidate commit; an empty status response is not a pass.

## 1. Product decision and architecture

### 1.1 Native kernel filesystem vs external compatibility providers

The original idea was to put all filesystem implementations inside the kernel. The project later deliberately changed direction: **the kernel core must not contain every filesystem implementation**. Adding or changing formats inside the kernel would increase kernel size, attack surface, coupling, and maintenance cost. Future formats must be addable without rebuilding or redesigning the native filesystem core.

- **ProsenjitFS** is the native filesystem and the only filesystem implementation intended to be linked into the kernel core.
- **FAT32, exFAT, NTFS, and future compatibility formats** must be implemented as separate filesystem drivers/providers behind a stable provider ABI and VFS boundary, as other operating systems provide support for multiple filesystem formats. They must not be built into the native ProsenjitFS implementation. The exact provider execution model (loadable kernel module versus isolated userspace provider) remains an engineering decision and must be selected based on safety, portability, and the current kernel architecture; “separate provider” does not by itself imply userspace execution.
- The VFS exposes one common file/directory API to applications. Applications should not need to know which filesystem stores a file.
- The kernel must not link legacy FAT32/exFAT/NTFS parsers back into the kernel core.
- The native ProsenjitFS feature roadmap and the external-provider roadmap are separate workstreams but tracked in this one document.

### 1.2 Target storage flow

```text
Application / file manager / system service
                 |
          Common file API
                 |
                VFS
          /      |       \
         /       |        \
 ProsenjitFS  Provider ABI  Future provider
 (native)     / broker      (new format)
   |              |
   |        FAT32 / exFAT / NTFS providers
   |              |
   +--------------+-------------------+
                  |
        permissioned block-device API
                  |
     partition discovery (MBR / GPT)
                  |
    ATA/SATA / NVMe / USB mass storage
                  |
       internal disk / USB drive / external SSD
```

The VFS must not depend on a specific disk controller or filesystem format. Storage drivers and partition discovery are separate layers from filesystem parsing.

### 1.3 User-facing goals

Users should be able to connect a USB pendrive, external HDD, or external SSD and have ProsenjitOS detect its partition table and filesystem, then mount it through the same VFS interface. The intended compatibility targets are:

- **FAT32:** removable media and broad device compatibility.
- **exFAT:** modern large removable drives and SDXC-style media.
- **NTFS:** Windows-originated files and external disks.
- **ProsenjitFS:** native OS/system and user-data filesystem.

A filesystem being listed here is a **target**, not a claim that physical USB discovery, mount, or full read/write support is already complete. Until a provider and device path pass tests, unsupported filesystems must fail closed with a clear error and must never be guessed or modified.

### 1.4 Provider contract

The provider ABI and broker must specify at least:
- versioned ABI and capability negotiation;
- probe/identify without destructive writes;
- mount/unmount and root vnode/inode exposure;
- lookup, stat, directory iteration, open/close, read, write, create, remove, rename, truncate, and sync where supported;
- explicit read-only/read-write capability flags and per-operation unsupported errors;
- bounded request sizes, timeouts, cancellation, and error mapping;
- validated offsets, lengths, sector/cluster calculations, integer-overflow checks, and partition bounds;
- controlled access to block devices (providers must not bypass the device permission broker);
- provider identity, integrity/trust policy, sandboxing/isolation, restart/failure containment, and audit logging;
- safe unmount, outstanding-handle invalidation, and media-removal handling;
- testable mock block-device interface and fuzzable metadata parser entry points.

No provider may bypass VFS access-control, file-admission/security, or quarantine policy. Malformed media must not cause guessed mounts or destructive automatic repair.

## 2. Current filesystem status (source and evidence required)

### 2.1 Native ProsenjitFS

The native filesystem code and deterministic self-tests exist in the repository. The implementation work has included a versioned on-disk layout, format/mount, bitmap allocation, inode/block handling, directory namespace operations, file I/O, extent ownership, checksummed metadata, redo journaling/recovery, and corruption/error-path tests. These broad areas do **not** imply every filesystem feature is complete or that general concurrency and physical power-loss durability have been certified.

Current release gates:
- confirm exact candidate SHA and source tree;
- run required build/boot and security-architecture workflows on that exact SHA;
- require native ProsenjitFS self-test, LBA-boundary diagnostics, partition path, and persistent mount markers;
- exercise transaction interruption/replay and malformed metadata/journal cases;
- keep physical-device power-loss durability and general concurrent VFS safety marked unverified unless separately demonstrated.

### 2.2 Compatibility-provider status

- **FAT32 provider:** **mandatory ProsenjitOS 1.0 target**, but not yet verified as supported. It remains incomplete until a separate provider, VFS ABI integration, USB mass-storage path, partition discovery, and exact-candidate tests are evidenced.
- **EXT/EXT4 provider:** planned for a later release; no current 1.0 requirement. Do not imply it exists or is tested without source and exact-head test evidence.
- **exFAT provider:** **mandatory ProsenjitOS 1.0 target**, but not yet verified as supported. Acceptance requires a separate provider, ABI integration, Unicode names, allocation-bitmap/cluster validation, supported VFS file operations, malformed-media tests, and end-to-end removable-media tests.
- **NTFS provider:** planned for a later release. The current source/build tree audit did not establish a present NTFS implementation; do not claim support without source and exact-head test evidence.
- **Physical removable-media support:** a ProsenjitOS 1.0 release gate for FAT32 and exFAT USB media. It is not complete merely because ATA/QEMU persistent-disk or parser tests pass. USB controller/device discovery, USB Mass Storage (BOT/SCSI as supported), block-device registration, MBR/GPT partition enumeration, FAT32 provider mounting, read/write semantics, safe unmount, media removal, and physical-device regression tests must all be evidenced.

### 2.3 Required status vocabulary

- **Planned:** requirements documented; no complete implementation evidence.
- **In progress:** some code exists, acceptance incomplete.
- **Implemented:** code exists, but tests may still be incomplete.
- **Tested:** named tests passed on a named source revision.
- **Verified:** required tests/workflows passed on the exact candidate SHA, limitations documented.
- **Deferred:** explicitly excluded from the current release.

## 3. Unified roadmap and acceptance gates

### Phase A — Native ProsenjitFS correctness and first dependable release
- [ ] Re-audit format/mount and all file/directory operations against the current source.
- [ ] Finish transaction, journal, replay, malformed-target, allocator failure, and I/O error-path coverage.
- [ ] Verify inode/block allocation, extent ownership, directory-index integrity, truncate/append behavior, stale-data isolation, rename/unlink semantics, and persistent remount.
- [ ] Establish explicit locking/object-lifetime rules; do not claim general concurrency safety without race and stress tests.
- [ ] Define and test the trusted VFS permission model (UID/GID/mode or the explicitly chosen capability model); denied operations must leave persistent media unchanged.
- [ ] Run exact-head Build/boot-test and Security architecture CI, including ISO boot and native BIOS/ATA/MBR persistence paths.
- [ ] Publish limitations: QEMU/mock-device tests are not physical power-loss certification; no durability guarantee without a real durable flush barrier.
- [ ] Keep automatic destructive repair disabled by default; integrity scan/reporting is not repair.

### Phase B — Stable VFS/provider boundary
- [ ] Complete the provider ABI beyond current probe/mount/unmount registry scaffolding: generic lookup/readdir/open/read/write/create/remove/rename/truncate/sync operations, capability negotiation, and explicit error mapping.
- [ ] Decide and implement the provider execution model (userspace process + IPC is the preferred isolation target; do not treat an in-kernel registry flag as isolation).
- [ ] Define provider process model, sandboxing, device-access broker, lifecycle, cancellation, timeout, error mapping, and failure recovery.
- [ ] Add mock provider and mock block-device tests before adding more filesystem formats.
- [ ] Ensure all providers use the same VFS/security/file-admission boundary and cannot bypass permissions or quarantine.
- [ ] Add compatibility tests for provider mount/unmount, media removal, stale handles, and unsupported operations.

### Phase C — External compatibility providers and removable media
- [ ] **ProsenjitOS 1.0 release blocker: FAT32 and exFAT USB removable-media support.** Implement and verify USB controller/device discovery, supported USB Mass Storage transport, block-device registration, partition discovery, separate FAT32/exFAT provider probe/mount, VFS directory/file operations, safe write/flush behavior where supported, unmount, and media-removal handling. State the exact supported hardware/transport and filesystem subsets.
- [ ] FAT32 provider: identify/probe, mount, directory lookup/iteration, read, safe write support, create/delete/rename/truncate, sync, and malformed-media tests; document unsupported FAT variants/features.
- [ ] exFAT provider: identify/probe, mount, Unicode names, read/write operations, allocation bitmap/cluster-chain validation, sync, and malformed-media tests.
- [ ] NTFS provider: first establish robust read-only access; then separately gate safe write support, allocation, directory-index updates, metadata transactions, and recovery. Never describe partial write support as full NTFS support.
- [ ] Add automatic filesystem detection for supported partitions, with explicit user-visible failure for unknown/unsupported formats.
- [ ] Add USB mass-storage discovery and partition support for MBR/GPT; test pendrive and external HDD/SSD workflows.
- [ ] Verify that mounting an untrusted removable drive integrates with file-admission, quarantine, provenance, and audit policy.
- [ ] Ensure provider implementations remain outside kernel core and can be updated independently of ProsenjitFS.

### Phase D — Advanced native filesystem features (only after contracts/tests)
- [ ] Decide each feature individually: file-data checksums, hard links, symlinks, xattrs, sparse files, ACLs, enhanced diagnostics, benchmarks.
- [ ] Require on-disk compatibility review before any format-dependent feature.
- [ ] Keep CoW/snapshots, compression, encryption, full parity with mature filesystems, and risky automatic repair deferred unless separately approved and designed.

### Release acceptance checklist
- [ ] Scope/status table in this file updated from actual source and exact-head CI.
- [ ] Required workflows pass on the exact release candidate SHA.
- [ ] Native filesystem self-tests and persistence checks pass.
- [ ] Provider ABI tests pass independently of hardware.
- [ ] Each compatibility provider has its own conformance/fuzz/error-path results before being advertised as supported.
- [ ] **ProsenjitOS 1.0:** FAT32- and exFAT-formatted USB removable media pass end-to-end discovery, partitioning, mount, file I/O (according to each advertised provider contract), unmount, and media-removal tests on supported physical hardware; emulation-only evidence must be labelled and is not a claim of physical-device compatibility.
- [ ] No unsupported filesystem is guessed, mounted, or modified.
- [ ] Limitations and data-safety behavior are documented.


---

# Historical appendix: PROSENJITFS.md

> Preserved from the former separate filesystem document during consolidation. Historical status and CI references apply only to the revisions explicitly named there; use the current summary and exact-head verification for present release decisions.

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
- `docs/FILESYSTEM.md` is the source of truth for current CI evidence; `docs/FILESYSTEM.md` and `docs/FILESYSTEM.md` define the next acceptance gates.


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

## Release roadmap: ProsenjitFS 1.0 and 1.1 (2026-10-25)

**Version terminology:** ProsenjitFS product/release version (1.0, 1.1) is distinct from the existing **v1 on-disk format**. Do not silently change the v1 disk layout under a product-version label. Any incompatible layout change requires a new on-disk format version, feature negotiation, and an explicit upgrade/rollback strategy.

### ProsenjitFS 1.0 — reliable native filesystem baseline

The 1.0 goal is correctness and a verifiable release, not feature-count parity with NTFS/ext4/ZFS/Btrfs.

**Required release scope**
- Preserve the current v1 on-disk layout unless a separately reviewed format change is approved.
- Verify format/mount, superblock redundancy/degraded read-only behavior, metadata-region bounds, bitmap/inode/extent validation, directory operations, persistent file read/write/truncate, rename/unlink, and VFS/security integration.
- Finish systematic read/write/flush fault-injection coverage for reachable storage operations and journal phases; verify pre-commit recovery returns the old consistent state and committed recovery returns the complete new state.
- Finish adversarial boundary/corruption tests for superblocks, feature/version flags, region geometry, bitmap tail bits, inode semantics, extent chains/ownership, directory names/references and journal targets.
- Audit and test read/write/truncate/namespace concurrency. Do not claim general concurrency safety until locking, object lifetime, and race tests justify it.
- Require passing exact-head Build/boot-test and Security architecture CI for the release candidate, including ISO boot, native BIOS QEMU boot, ProsenjitFS self-test, and ATA PIO -> MBR -> persistent ProsenjitFS mount.
- Publish known limitations: QEMU/mock-device tests are not physical hardware power-loss certification; devices without a durable flush barrier cannot promise power-loss durability.
- Keep integrity scan read-only. Automatic destructive repair remains disabled by default.

**Not a 1.0 requirement:** snapshots, general Copy-on-Write, compression, encryption, self-healing writes, broad foreign-filesystem support, and full NTFS/ext4/ZFS/Btrfs feature parity.

### ProsenjitFS 1.1 — integrity and usability expansion

1.1 begins only after the 1.0 release gates pass. It must be backward-compatible with 1.0 volumes where feasible; any required on-disk changes must be explicitly versioned and tested.

**Planned 1.1 work**
1. **File-data integrity:** design and implement checksums for file-data blocks/extents, including checksum metadata layout, checksum update atomicity, read-time mismatch reporting, and tests for corruption, partial writes, truncate, overwrite, remount and recovery. CRC32C is error detection, not authentication. Do not describe metadata CRCs as file-data checksums.
2. **Recovery diagnostics:** structured, user-visible error/status reporting for corrupt data, degraded redundant metadata, journal replay and failed I/O. Recovery must not silently overwrite uncertain user data.
3. **Namespace and metadata usability:** assess and implement hard links, symbolic links and extended attributes only with explicit VFS semantics, on-disk representation, permissions, cycle/bounds checks, and test coverage.
4. **Sparse files:** define hole semantics, logical versus allocated size accounting, zero-fill behavior, truncate/grow behavior, and bitmap/extent invariants before implementation.
5. **Permissions/ACL design:** define mode/uid/gid enforcement and ACL semantics at the VFS/security boundary. On-disk metadata alone must never grant authority. Full ACL support is conditional on a stable authorization model.
6. **Host formatter and compatibility tests:** complete and validate mkprosenjitfs/formatter behavior, safe target-device checks, format/mount round trips and malformed-image tests.
7. **Performance baseline:** benchmark sequential and random I/O, directory lookup, allocation, and mount/recovery on reproducible test images; optimize only with correctness tests preserved.

**Deferred beyond 1.1 unless separately approved:** snapshots and general Copy-on-Write, compression, encryption/key management, deduplication, RAID/self-healing redundancy, and automatic destructive repair. These features materially increase format, security, recovery, and testing complexity.

### Feature provenance and honesty rules

- ext4-inspired: extents, bitmap allocation and journal/recovery concepts; this is not ext4 compatibility.
- NTFS-inspired: transactional recovery and a path toward ACLs; this is not NTFS compatibility.
- ZFS/Btrfs-inspired: integrity-first design and future snapshot/Copy-on-Write exploration; current CRC32C metadata checksums do not equal end-to-end file-data integrity, snapshots, or self-healing.
- Keep FAT32/exFAT/NTFS providers outside the kernel-core ProsenjitFS format, as specified by the filesystem-provider boundary.
- Track every feature as **Planned**, **Implemented**, **Tested**, or **Verified**. Source code alone is not verification; use exact-head CI evidence. Do not mark a feature Verified based on an older commit's CI.

## Detailed product release contract — 1.0 / 1.1

The detailed, feature-by-feature release specification, acceptance checklist, test matrix, deferred-feature list, and verification rules are maintained in [FILESYSTEM.md](FILESYSTEM.md). Treat that document as the product-release scope reference. Product version 1.1 is a planned backlog, **not evidence that its features are implemented**. File-data checksums, hard links, symlinks, xattrs, sparse files, ACLs, enhanced diagnostics and benchmarks require individual design/code/test evidence. CoW/snapshots, compression, encryption, full filesystem parity and risky automatic repair remain deferred unless separately approved.

Release status must be assessed against the exact candidate commit and passing required CI. Keep the existing limitations explicit: metadata CRC32C is not file-data integrity protection; integrity scan is read-only and requires caller serialization; mock-device/QEMU testing is not physical power-loss certification; general concurrent VFS safety is not proven.

## Mandatory permission and encryption release policy

Permission enforcement is mandatory for ProsenjitFS product release 1.0; storing UID/GID/mode fields without enforcing them in the trusted VFS/security layer is insufficient. See [PROSENJITFS-PERMISSIONS-AND-ENCRYPTION.md](PROSENJITFS-PERMISSIONS-AND-ENCRYPTION.md) for operation-by-operation permission semantics, negative tests, and the basic-encryption threat model and release gates. Encryption must remain planned/experimental until reviewed cryptography, key handling, authenticated I/O and recovery integration are tested. Do not invent cryptography or silently downgrade encrypted volumes.


---

# Historical appendix: PROSENJITFS-IMPLEMENTATION-PLAN.md

> Preserved from the former separate filesystem document during consolidation. Historical status and CI references apply only to the revisions explicitly named there; use the current summary and exact-head verification for present release decisions.

## FS-28 — truncate-shrink partial-block tail isolation (**PASS** on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- [x] Source audit found a privacy/correctness gap: shrink retained the final allocated data block without clearing bytes beyond a non-block-aligned new EOF; a later write past EOF could re-expose pre-truncate bytes.
- [x] Updated `pfs_file_truncate()` to locate the retained EOF block before changing the extent chain, zero the suffix, and add that image to the same redo transaction as the inode size and any extent/bitmap changes. Duplicate transaction targets fail closed; staging errors reset the volatile transaction.
- [x] Added a native self-test: truncate `/hello.bin` to 100 bytes, write one byte at offset 200, then require bytes 100–199 to read as zero and byte 200 to match the new payload.
- [x] Preserved the version-1 on-disk format; the change uses the existing redo journal.
- [x] Code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f` passed [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332). Logs include successful ProsenjitFS self-test, LBA-boundary diagnostic, and ATA/MBR persistent-mount markers in ISO and native BIOS QEMU runs.
- **Durability scope:** deterministic mock-device/QEMU evidence only; no physical-device power-loss certification or general concurrent-mutation guarantee.

## FS-29 — inode allocator output on transaction failure (**PASS** on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- [x] Source audit found `prosenjitfs_alloc_inode()` published the candidate inode number before the inode-bitmap redo transaction completed.
- [x] The output is now assigned only after `pfs_inode_bitmap_update()` succeeds; all error returns preserve the caller's value.
- [x] Added a mock-device regression that fails the first journal write during inode reservation and checks `EIO`, unchanged output sentinel, unchanged durable/volatile/dirty media, writable mount state, and a clean integrity scan.
- [x] The existing v1 metadata format and journal protocol are unchanged.
- [x] Code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f` passed [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332); the native self-test marker is present in both QEMU boot paths.

## FS-30 — journal-retirement I/O error propagation (**PASS** on code/test head `5aa0569a24093b11aee7685c7f3f136f02aaaded`)

- [x] Replaced undocumented `-101..-106` returns in `pfs_journal_clear()` with the filesystem's named validation/corruption errors and direct propagation of the original LBA, write and flush status.
- [x] Added a mount-recovery regression for both journal-retirement write failure and flush failure. Each starts from a stage-4 interrupted transaction with durable header/images and no matching commit, requires mount to return `PROSENJITFS_EIO`, then retries recovery and verifies the old payload and integrity.
- [x] No on-disk v1 format or journal protocol changes.
- [x] [Build/boot-test #8590](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503632) and [Security architecture #8126](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503601) passed; logs show the filesystem self-test marker in ISO and native BIOS QEMU runs.

## FS-31 — committed-journal replay write failure (**PASS** on code/test head `73bff7fe8aad278e04f08ee7594b901bb11da6f9`)

- [x] Added a regression that interrupts a file unlink at stage 5, confirms the durable journal header and matching commit, then fails the second home-block write during mount recovery.
- [x] The test requires recovery mount to return EIO, simulates power loss to discard the partial unflushed replay, retries mount, and verifies the committed unlink is fully applied and integrity is clean.
- [x] The v1 disk layout and redo-journal protocol are unchanged.
- [x] [Build/boot-test #8593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866752) and [Security architecture #8129](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866715) passed on this code/test head. Logs contain the filesystem self-test marker in ISO and native BIOS QEMU paths; replay-write fault, power-loss retry, unlink state and integrity assertions are inside that self-test.

## Part 2 verification checkpoint — 2026-10-25

- [x] Inode-record admission: `prosenjitfs_read_inode()` rejects checksum-valid records with unsupported type/flags, zero generation, invalid parent, oversized logical size, or invalid allocated-byte accounting. Seven malformed-record cases restore the root and end with a passing integrity scan.
- [x] Directory-index ownership: readers and callers supply the expected owner ID; a CRC-valid index naming another in-range inode is rejected, and the original index is restored through the journal path.
- [x] Namespace child references: path lookup and readdir reject dangling references, entry/inode type mismatch, wrong parent, and direct directory self-links. Corruption regressions restore original metadata and verify integrity.
- [x] On-disk v1 layout/version unchanged. No physical-device durability or general concurrency claim.
- [x] Exact source head `70ed44cd45987682ac50f9fbfd1b82a510e25033` passed [Build/boot-test #8548](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283756) and [Security architecture #8084](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283791). Build includes ISO and native BIOS QEMU boot, ProsenjitFS self-test, LBA-boundary diagnostic, and ATA-to-MBR persistent-mount checks.
- [ ] Broader P2/P3/P4 work remains open: systematic read/write/flush fault sweeps, additional journal recovery cut points, and remaining metadata/extent/namespace boundary cases.
## Part 2 continuation — 2026-10-25

## Integration CI failure and source correction — 2026-10-25

- [x] Build/boot-test run `#40159887034` exposed a compile failure at `kernel/prosenjitfs.c:10624`: an `rmdir-write-sweep` cleanup clause referenced `found_lba` from the wrong (unlink-read-sweep) scope after branch-conflict reconciliation.
- [x] Reverted that misplaced cleanup from the unlink read-failure sweep and anchored the optional post-recovery `vfs_rmdir()` cleanup to the actual rmdir write-failure sweep, where `found_lba` is declared and indicates whether the directory survives recovery.
- [x] Corrected integration branch was verified by PR #242 exact-head Build/boot-test #40160091435 and Security architecture #40160091518; ISO and native BIOS QEMU boot checks passed.


- Reconciled the existing native filesystem implementation rather than introducing a parallel design.
- Ported allocator block-geometry validation and boundary/exhaustion regressions onto the latest `main` source while preserving the already-merged FS-18 home-flush recovery test. Integrated source commit: `f533f1a3f8c2a6dd2c0f6891c8323c68562b4fd1`.
- Original allocator-hardening exact-head CI passed; this integration commit own Build/boot-test and Security checks remain required and are not pre-claimed.
# ProsenjitFS Implementation Plan — Audit-Driven

## Part 2 follow-up — VFS read output-count contract (2026-10-24)

- [x] Audited `vfs_file_read()`: unlike the write fix in this branch, it cleared `bytes_read` only after argument and permission checks. A rejected read could therefore leave a stale caller-provided count.
- [x] Changed `kernel/vfs.c` to clear a non-null output count before validation/dispatch; added regressions for permission-denied and filesystem-`EIO` reads using a nonzero sentinel.
- [x] Exact PR-head Build/boot-test #40119299299 and Security architecture #40119299311 passed on `89a35608bcd278b01977f5abded40f2208208d9e`; PR #235 merged as `d82d1010f3c58696c15dfb09c326ba2b6d612e43`.
- Scope: VFS output contract only; no on-disk format change, no crash-atomicity or hardware-durability claim.


## Part 2 execution update — 2026-10-24

**Execution branch:** `fix/prosenjitfs-vfs-write-error-count-20261024`  
**Starting main HEAD:** `e875810769cb022f1a450bce86027966a3982fcc`

- [x] Re-read the completion matrix, implementation plan, test matrix, architecture contract, `kernel/prosenjitfs.c`, `kernel/vfs.c`, and `kernel/include/vfs.h` before changing code.
- [x] Confirmed the existing v1 core already implements superblock/version validation, bitmap allocation, inode/extent metadata, hash-bucket directories, VFS namespace/file operations, redo journaling/recovery, and a read-only integrity scan. The immediate gap is not a missing replacement filesystem; continue hardening the existing provider.
- [x] Fixed a VFS output-contract defect in `vfs_file_write()`: the caller-provided `bytes_written` value was not cleared before permission checks or filesystem callback dispatch, so an error could leave a stale count that looked like successful partial I/O.
- [x] Added a focused VFS self-test callback that returns `PROS_VFS_EIO`; the regression seeds the output count with a nonzero sentinel and requires `EIO` plus a zero byte count.
- [ ] Verify the exact execution branch with Build/boot-test and Security architecture workflows. Source changes are not runtime-verified until those gates finish on this branch.
- [x] Reviewed PR #230 exact head `22da8502bce69a3c69c6ddf222eb643ffa89aab0`; Build #40088773369 and Security #40088773288 passed, and it merged as `3df77093edec9a6961a507fe05f4c32420d73391`.
- [x] Confirmed `main` already contains the equivalent rmdir write-failure sweep. PR #232 is duplicate coverage on an older/conflicting branch; do not merge the duplicate blindly.


**Audit date:** 2026-10-24  
**Baseline:** `main` at `cfe04a8520ec917a90b284bbc726a8bd0cc83a2d`  
**Status source:** [Completion matrix](FILESYSTEM.md)  
**Architecture source:** [ProsenjitFS design and verified audit](FILESYSTEM.md)  
**Test gates:** [Test matrix](FILESYSTEM.md)

This is the working checklist for completing existing ProsenjitFS safely. The order is dependency-driven; do not mark a step complete from code inspection alone when its acceptance criteria require runtime evidence.

## P0 — Diagnose and repair the currently failing fault-injection test

- [x] Inspect PR #233 head `74b8cedf5b96055823b9a9a5d4a963b8d2f1a3b0` and map QEMU stage `0x28E1` to the invariant assertion at `kernel/prosenjitfs.c:10465`.
- [x] Split the compound assertion into distinct diagnostic returns for operation status, durable-media CRC, volatile-media CRC, dirty-map CRC, filesystem read-only state and mount read-only state.
- [x] Fix the error namespace boundary: `pfs_make_vnode()` returns filesystem-native errors and VFS callback callsites map through `pfs_vfs_error()`; the earlier direct return could confuse ProsenjitFS `EIO (-2)` with VFS `ENOSYS (-2)`.
- [x] Verify by source inspection that a not-reached countdown is disarmed before readback/integrity/unmount.
- [x] Confirm on exact PR #233 head `84d2b84948065011cf0c94291e8bcaf5d9f7079c`: Build/boot-test #40116501666 and Security architecture #40116501659 pass; workflow logs include the full ProsenjitFS format/mount/allocation/journal/recovery success marker and native ATA PIO -> MBR -> ProsenjitFS persistent mount marker. This is mock-device/QEMU evidence, not physical-device certification.
- **Acceptance:** met on PR #233 exact head `84d2b84948065011cf0c94291e8bcaf5d9f7079c` (Build/boot-test #40116501666 and Security #40116501659 passed). The fix passed exact-head CI and merged into `main` as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`; post-merge main CI is pending.

## P1 — Complete and reconcile per-operation fault-injection coverage

- [x] PR #233 append/truncate read-failure sweeps passed exact-head Build/boot-test #40116501666 and Security #40116501659 on `84d2b84948065011cf0c94291e8bcaf5d9f7079c`; merged into `main` as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`.
- [x] PR #230 create/overwrite/rmdir write-failure sweep reviewed and exact-head CI passed (Build #40088773369; Security #40088773288).
- [ ] For every accepted test PR, capture exact head SHA and both Build/boot-test and Security architecture run links in `docs/FILESYSTEM.md`.
- [x] Deduplicated overlapping sweeps: closed PRs #232, #240 and #241 as redundant/superseded after their unique coverage was merged or ported through PRs #239 and #242.
- **Acceptance:** no open test branch is called verified until its exact head passes the required CI workflows; failure injection never leaks into post-operation verification.

## P2 — Make I/O-failure classification and cleanup systematic

### Allocator bitmap read-failure regressions — current integration

- [x] Added mock-device read-failure checks for `pfs_find_free_inode` and `prosenjitfs_alloc_inode`.
- [x] Added independent data-bitmap read-failure checks for `pfs_find_free_data_block`, `pfs_find_free_data_block_except`, `pfs_find_free_data_block_excluding`, and `pfs_data_block_is_free`.
- [x] Each reached fault must propagate `PROSENJITFS_EIO`, preserve candidate sentinels, consume the injected fault, leave durable/volatile/dirty media CRCs unchanged, keep the mount writable, and pass a subsequent integrity scan.
- [x] Exact PR #242 head `96418d7bf512ee49e42eb325d0864738781891ab` passed [Build/boot-test #40160091435](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091435) and [Security architecture #40160091518](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091518). Build includes ISO and native BIOS QEMU boot checks; no physical-device durability or general concurrency claim.

- [x] Targeted journal-image flush failure/retry (FS-17) and post-commit home-block flush failure/recovery (FS-18) now have passing exact-head Build/boot-test and Security evidence. Broader stage-by-stage fault injection remains open.

- [ ] Audit `kernel/prosenjitfs.c` read/write/flush propagation paths for superblocks, journal, bitmaps, inode table, extents, directory indexes and directory entries.
- [ ] Extend mock-device tests for read, write and flush errors at each reachable operation point, including short/partial transfers if the interface supports them.
- [ ] Ensure device `EIO` stays an I/O error and is not converted into metadata corruption unless bytes were successfully read and shown malformed.
- [ ] Check all failure exits for transaction-buffer reset, temporary heap/scratch cleanup, vnode/file closure, allocation rollback, mount state, and preservation of prior file data.
- **Acceptance:** every injected fault has a bounded, deterministic expected result; clean integrity checks pass after recovery; no in-memory staged transaction contaminates the next operation.

## P3 — Re-validate transactional durability and recovery invariants

- [x] Added a targeted home-block flush failure test at transaction flush #4: the commit record is already durable, the current mount freezes read-only, and remount recovery replays the complete new payload. Exact PR #239 head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf` passed Build/boot-test #40156447788 and Security #40156447759; merged as `1b87449daf9555d15723dea284d3725b87e354d0`.
- [ ] Sweep remaining failure points around journal-slot invalidation, journal header/images, flush barriers, commit record, home-block application, and journal retirement.
- [ ] For pre-commit interruption, recovery must expose the old consistent state; for a valid committed transaction, recovery may expose the complete new state. Reject mixed/partial logical results.
- [ ] Re-run recovery interruption tests both before and after flush, including repeat recovery after simulated power loss.
- [ ] Test missing/stale commit markers, matching transaction IDs with contradictory record counts, invalid checksums, duplicate/out-of-range transaction targets and overflowed region calculations.
- **Acceptance:** all committed/uncommitted outcomes are deterministic and idempotent; ambiguous commit state fails closed or leaves the mount read-only until recovery; no repair writes occur during a read-only scan.

## Allocator geometry hardening — ported onto current main (2026-10-25)

- [x] Centralized accepted filesystem block geometry in `pfs_block_size_valid()`: only 512, 1024, 2048 or 4096-byte power-of-two blocks pass. Applied before allocator bitmap/index arithmetic, inode location and public inode I/O, preallocation, integrity scanning, and vnode read/write/truncate arithmetic.
- [x] Added malformed-geometry coverage for 0, 511, 513, 4097 and 8192; output-sentinel/media-invariance assertions; bounded inode/data bitmap exhaustion checks; final-valid-bit checks; and strengthened the rmdir crash-recovery assertions/cleanup.
- [x] The original geometry PR exact head `3d76a1fdbc2a58518bc914392caf5c846be04ce2` passed Build/boot-test #40119726528 and Security architecture #40119726499. This current-main port is source-committed as `f533f1a3f8c2a6dd2c0f6891c8323c68562b4fd1`; the integrated head still requires its own CI before it is marked verified.
## P4 — Validate metadata, allocation and namespace invariants adversarially

- [ ] Expand remaining boundary tests for primary/backup superblocks, feature/version/block-size validation, region overlap/order, bitmap capacity/tail bits, inode-table bounds, further inode semantic combinations, extent loops/overlaps, extent ownership, and block-to-sector arithmetic.
- [x] Source/test coverage for checksum-valid inode type/flag/generation/parent/size/allocated-byte admission passed on exact source head `70ed44cd45987682ac50f9fbfd1b82a510e25033` (Build #8548 and Security #8084). P4 remains open for remaining boundary families.
- [ ] Expand directory tests for malformed UTF-8, zero/overlong names, checksum-valid bad hashes, wrong buckets, duplicate names/references, self-cycles, chain cycles, bad child types/parents and parent chains that do not reach root.
- [x] Added a bounded total-path-length guard in `vfs_path_lookup()` before vnode reference acquisition; an unterminated path reaching `PROS_VFS_PATH_MAX` is rejected with `PROS_VFS_EOVERFLOW` instead of scanning arbitrarily many separators.
- [x] Added VFS self-tests for empty and overlong paths, repeated separators, `.`, `..` and root escape clamping, missing intermediate directory, non-directory components, and preservation of the caller's result pointer on error.
- [ ] Exact PR #242 Build/boot-test and Security architecture CI must pass before these regressions are considered runtime-verified.
- [ ] Keep orphan detection report-only until a separate recovery design proves how to distinguish an orphan from valid but temporarily unreachable metadata.
- **Acceptance:** malformed media is rejected without out-of-range access or media mutation; valid media passes the read-only scan; all error values are stable and documented.

## P5 — Specify operation serialization before expanding concurrency

- [ ] Audit uses of `pfs_io_lock`, all VFS mutation entry points and integrity-scan callers in `kernel/prosenjitfs.c` and `kernel/vfs.c`.
- [ ] Define lock ownership, lock ordering, recursion rules and whether VFS calls can be re-entered. Do not infer filesystem-wide exclusion from the narrow append lock.
- [ ] Add controlled interleaving tests for append/write/truncate/rename/unlink versus scans and for vnode/file lifetime around unlink/rename.
- [ ] Reconcile the locking design with scheduler/preemption and interrupt-state contracts before enabling concurrent execution paths.
- **Acceptance:** documented locking proves which mutable state is protected, and the test setup exercises the same contract as production call sites. Otherwise keep concurrency explicitly unsupported.

## P6 — Complete operational/maintenance gaps

- [x] Removed the temporary cross-layer `block_device_init()` trace and forced `O0`/`noinline` attributes from `kernel/block.c`. The ProsenjitFS self-test retains its generic before/after checkpoints (`0xF004`/`0xF005`), so test-stage reporting remains without instrumenting the production block-init path. Exact-head CI is required for acceptance.
- [x] Host-side `mkprosenjitfs` uses the shared kernel formatter/format definitions on the disposable image generated by `scripts/build.sh`. It rejects non-regular targets via `lstat`/`O_NOFOLLOW`/`fstat`, validates the expected MBR/partition layout before formatting, and has SHA-256-backed refusal tests for special files and malformed regular images. Exact-head CI remains required for acceptance.
- [ ] Keep automated repair/reclamation disabled until a journal-first, conservative repair design includes pre/post-integrity verification, read-only fallback and recovery logging.
- [x] Defined the controlled, opt-in physical-device durability validation protocol below. **Execution remains NOT TESTED**; mock-device and QEMU tests are not physical power-loss certification.
- **Acceptance:** production diagnostics do not alter the filesystem control path unnecessarily; tools use explicit image paths and refuse unsafe targets; recovery is observable and non-destructive by default.

### Physical-device durability validation protocol — plan only (2026-10-28)

**Purpose and limits**

This is a separate, opt-in lab validation for real-hardware storage durability. It is not evidence that ProsenjitFS has passed physical power-loss testing. Mock-device fault injection and QEMU boot tests cannot prove that a drive, controller, bridge, or firmware honors flush barriers under sudden power loss.

**Safety preconditions**

- Use only a dedicated expendable test drive and separate test host or controlled power-cut fixture. Never use a production system, personal data disk, or device containing data that must be preserved.
- Record drive model, serial number, firmware, capacity, connection type, controller, and any USB/SATA bridge.
- Confirm the target device using two independent identifiers before destructive setup; disconnect or make inaccessible unrelated writable drives where practical.
- Use a disposable image or dedicated test partition. Never rely on a device name such as `/dev/sdX` remaining stable across boots.
- Keep automatic formatting, repair, orphan reclamation, and unattended destructive operations disabled.
- Preserve exact kernel commit, formatter version, test image hash, firmware settings, device identifiers, and logs for every run.

**Test sequence**

1. **Baseline:** create the image/partition using the reviewed formatter path; mount it; write deterministic sequence-numbered payloads and checksums; flush, cleanly unmount, read back, and verify.
2. **Flush capability:** record whether the block path exposes a flush callback and whether write-cache settings are reported. If no meaningful durable flush barrier exists, classify physical power-loss durability as unsupported for that configuration.
3. **Controlled interruption:** use a purpose-built fixture to cut power at randomized points while repeatedly performing small file replacements and metadata mutations. Never pull cables or interrupt power on a live production machine.
4. **Recovery:** restore power and mount without repair tools. Record whether recovery selects the complete pre-commit state or the complete committed state.
5. **Integrity:** run the read-only integrity scan, verify payload hashes and namespace invariants, and reject mixed or partial logical results.
6. **Repeatability:** repeat across many interruption points and cold boots; preserve every failure and never discard unexplained results.
7. **Read-only fallback:** for ambiguous/corrupt recovery, verify the filesystem fails closed or remains read-only without silently rewriting or reclaiming metadata.
8. **Control run:** repeat the workload without power interruption to distinguish fixture/media failures from recovery failures.

**Required workload coverage**

- File create, overwrite, append, truncate, unlink, mkdir, rmdir, and rename.
- Journal header/images flush, commit-record publication, home-block application, and journal retirement.
- Repeated recovery after an interrupted recovery attempt.
- Full and nearly-full allocation state, directory metadata, and extent updates.
- Devices with and without a supported flush path, recorded as separate configurations.

**Acceptance criteria**

- Every recovered operation yields a complete allowed state: old state if commit was not durable, or new state if a valid commit was durable.
- No mixed payload, invalid namespace, leaked/duplicated allocation, or silent metadata repair is observed.
- Ambiguous commit state fails closed or exposes a read-only mount until safely resolved.
- The read-only integrity scan does not mutate durable media.
- Results are reproducible, tied to exact source and hardware identifiers, and reviewed before any durability claim is published.

**Reporting and current status**

This test is destructive by design and may damage the test filesystem or drive. A human operator must select and verify the expendable target separately for every session; the harness must never discover and choose a physical disk automatically. Report results only for the exact tested device/controller/firmware configuration. Passing one setup does not certify all storage devices.

**Status: plan defined; physical execution NOT TESTED.** ProsenjitFS must not be described as physically power-loss certified until this protocol has been executed on explicitly selected expendable hardware and its evidence reviewed.

## Change and verification discipline

1. Inspect current source, the exact PR head and the current `main` base before editing.
2. Make the smallest change that addresses the demonstrated failing invariant; retain public interfaces unless a reviewed design requires a change.
3. Add/adjust a focused regression before broader runs.
4. Build and run the test matrix; keep logs and run IDs linked in the status matrix.
5. If CI fails, map the exact serial stage/assertion to source, fix that cause, and rerun. Never infer green from an earlier commit.
6. Do not format, overwrite, delete or repartition a real disk during discovery or tests. Use the deterministic mock device and repository-generated disposable image only.


## FS-26 — Ambiguous pre-commit journal-images flush (2026-10-25)

- [x] Added a deterministic mock-device regression for flush #2: journal header/images persist but flush reports `EIO` before a commit record is published.
- [x] Require failed write with zero bytes, current mount remaining writable, simulated crash/remount preserving the complete old payload, and a passing read-only integrity scan.
- [x] Exact PR #250 head `32541b0d67e9a077b10bf8896a0881bf862c01cd` passed Build/boot-test [#40168090685](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40168090685) and Security architecture [#40168090633](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40168090633); merged into `main` as `4cb57fc7e80b4a9cca6ecfb0a5cdf7b668c60055`. This is not physical-device durability certification.


## FS-27 — CRC-valid journal header with duplicate targets (2026-10-25)

- [x] Add a deterministic mock-device regression that creates a committed transaction stopped before home-block replay, duplicates two journal target LBAs, recomputes both header and commit checksums, and requires mount recovery to return `PROSENJITFS_ECORRUPT` before replay.
- [x] Exact PR #271 head `88516324a1007753aa43b213bd17bb0c5ea2700b` passed Build/boot-test [#40674196387](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196387) and Security architecture [#40674196316](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196316); merged into `main` as `965e70902c725caae280b6403f851c93ed8d7059`. The duplicate-target regression was already present in the exact tested head; this is mock-device/QEMU evidence, not physical-drive durability certification.
- Scope is structural validation on an in-memory mock device; no physical-device durability or general concurrency claim.

## Release roadmap — ProsenjitFS 1.0 / 1.1 (2026-10-25)

This roadmap separates product release versions from the existing v1 on-disk format. Product 1.0 does not automatically mean the disk format is changed; incompatible format changes require explicit format-versioning and migration planning.

### Release 1.0 — correctness and release gates

- [ ] Complete systematic block-device read/write/flush fault sweeps, including journal invalidation, header/images, flush barriers, commit record, home-block replay and journal retirement.
- [ ] Complete deterministic recovery cut-point tests: uncommitted transactions expose the old consistent state; committed transactions expose the complete new state; retries are idempotent.
- [ ] Complete adversarial geometry and metadata tests for both superblocks, feature/version flags, overlapping regions, bitmap capacity/tail bits, inode-table bounds, inode semantics, extent loops/overlaps/ownership, directory references/names and journal target validation.
- [ ] Audit concurrency and object lifetime for read/write/truncate and namespace mutation. Add tests or document unsupported concurrent use; do not claim general safety without evidence.
- [ ] Confirm formatter target safety and format/mount round-trip tests.
- [ ] Run Build/boot-test and Security architecture workflows on the exact release-candidate head; retain links and commit SHA in the completion matrix.
- [ ] Publish limitations and a release checklist; QEMU/mock-device evidence must not be described as physical hardware power-loss certification.
- **Release gate:** no open P0/P1 correctness issue; all required tests pass on exact release-candidate SHA; documentation matches code and CI evidence.

### Release 1.1 — data integrity and filesystem usability

- [ ] Specify a backward-compatible or explicitly versioned file-data checksum design. Cover checksum granularity/storage, atomic updates, overwrite, append, truncate, sparse regions, recovery, and read-time corruption reporting.
- [ ] Add file-data corruption injection tests proving detection without silently returning corrupted bytes as valid data.
- [ ] Define hard-link and symbolic-link semantics, including inode link counts, parent/path handling, cycle and bounds protections, deletion behavior, and VFS integration; implement only after the design is reviewed.
- [ ] Define extended-attribute representation and limits, including safe unknown-attribute handling.
- [ ] Define sparse-file holes and zero-read semantics, logical size vs allocated bytes, and allocation/accounting invariants.
- [ ] Specify permissions/ACL enforcement with VFS and process capabilities as authority; on-disk uid/gid/mode must not bypass kernel authorization.
- [ ] Complete host formatter safety and format/mount round-trip validation.
- [ ] Establish repeatable performance benchmarks for file I/O, allocation, directory lookup and mount/recovery.
- **1.1 gate:** each shipped feature has design, implementation, failure-path tests, exact-head CI evidence and a documented on-disk compatibility story.

### Deferred roadmap (not promised for 1.1)

- [ ] Copy-on-Write and snapshots/rollback.
- [ ] Compression.
- [ ] Encryption and key management.
- [ ] Deduplication, redundant storage and self-healing data repair.
- [ ] Automatic destructive repair (must remain disabled by default until explicit recovery policy, audit logging and rollback are proven).

### Status vocabulary

Use four distinct statuses: **Planned** (documented, not implemented), **Implemented** (code exists), **Tested** (relevant tests ran), **Verified** (acceptance tests passed on an identified exact commit). A feature may have multiple subfeatures at different statuses; do not mark an entire category complete based on one test.

## Detailed 1.0 / 1.1 implementation and acceptance plan

Use [FILESYSTEM.md](FILESYSTEM.md) as the detailed acceptance contract. Version 1.0 work is gated on correctness, recovery/fault-injection coverage, formatter safety, VFS/security boundaries and exact-head CI. Version 1.1 is divided into P0 data-integrity/recovery work, P1 gated namespace/sparse/security/tooling work, and P2 performance/self-healing work. Do not implement format-dependent features before reviewing on-disk compatibility. A checkbox in the specification is a requirement, not a claim of completion; update it only with code references and test evidence.

## Security release blocker: permissions; conditional encryption work

Before Version 1.0 can be release-verified, audit and enforce permission checks for file/directory operations at the trusted VFS/security boundary, then pass the permission-denial/no-mutation/remount tests in [PROSENJITFS-PERMISSIONS-AND-ENCRYPTION.md](PROSENJITFS-PERMISSIONS-AND-ENCRYPTION.md). Treat this as a blocking release gate. Encryption is a separate design-first task: inspect block-device and boot/key-input architecture, specify a reviewed authenticated-encryption and key-management design, and only then implement. If the complete encryption gate cannot be met, defer encryption rather than ship an unsafe partial implementation.


---

# Historical appendix: PROSENJITFS-COMPLETION-MATRIX.md

> Preserved from the former separate filesystem document during consolidation. Historical status and CI references apply only to the revisions explicitly named there; use the current summary and exact-head verification for present release decisions.

## Part 2 metadata-hardening status — 2026-10-25

- **PASS — source head `70ed44cd45987682ac50f9fbfd1b82a510e25033`:** [Build/boot-test #8548](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283756) and [Security architecture #8084](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283791) both completed successfully.
- Inode admission: seven checksum-valid malformed-record cases rejected; original root restored; integrity scan passed.
- Directory-index owner: a checksum-valid wrong-owner index rejected; original index restored transactionally; integrity scan passed.
- Namespace child references: lookup and readdir enforce inode existence, type equality, parent equality, and direct-self-link rules. Type/parent corruption tests restore metadata and require a clean integrity scan.
- v1 on-disk format unchanged. Mock-device/QEMU evidence does not certify physical-device power-loss durability or general VFS concurrency.
## Current integration update — allocator bitmap read-error coverage (2026-10-25)

- Added test cases in `kernel/prosenjitfs.c` for read failures in the inode bitmap finder and public inode allocator, plus all four data bitmap lookup/query paths.
- Acceptance checks include filesystem `EIO` classification, unchanged result sentinels, consumed one-shot read-failure injection, writable mount state, unchanged durable/volatile/dirty mock-media CRCs, and successful integrity scanning after each sweep.
- Source commit `c7218d60a8b5b499f2baeec4170c8642c6c11f55`; exact PR #242 integration CI is still required. Do not treat the earlier PR #218/#219 branch states as verification of this port.

## Latest verified follow-ups — 2026-10-25

- [x] **FS-18 home-block flush failure:** inject transaction flush #4 after commit publication; require zero bytes on failure, read-only current mount, committed payload replay after simulated power loss, and clean integrity scan. Exact PR #239 head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf` passed [Build/boot-test #40156447788](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447788) and [Security #40156447759](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447759); merged as `1b87449daf9555d15723dea284d3725b87e354d0`. Mock-device/QEMU only.
- [x] **FS-19 allocator geometry hardening:** the original PR #237 exact head `3d76a1fdbc2a58518bc914392caf5c846be04ce2` passed [Build/boot-test #40119726528](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119726528) and [Security #40119726499](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119726499). Its changes are ported onto newer main with FS-18 and reconciled rmdir recovery checks in source commit `f533f1a3f8c2a6dd2c0f6891c8323c68562b4fd1`; **the integrated commit still requires fresh exact-head CI**.
- Geometry tests cover block sizes 0, 511, 513, 4097 and 8192, sentinel/media invariance, bounded bitmap exhaustion, and final valid allocator bits. Accepted on-disk v1 format is unchanged.
- [x] **FS-20 allocator bitmap read failures:** tests cover inode/data bitmap lookup and query read errors, output sentinels, unchanged durable/volatile/dirty CRCs, writable mount state and post-fault integrity. - [x] Exact PR #242 head `96418d7bf512ee49e42eb325d0864738781891ab` passed [Build/boot-test #40160091435](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091435) and [Security architecture #40160091518](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091518). Build includes ISO and native BIOS QEMU boot checks; no physical-device durability or general concurrency claim.
- [x] **FS-21 VFS path bounds and resolution:** complete path-length limit and tests for empty/overlong path, repeated separators, `.`/`..`, root escape clamp, missing intermediate directory, non-directory traversal and result-sentinel preservation. - [x] Exact PR #242 head `96418d7bf512ee49e42eb325d0864738781891ab` passed [Build/boot-test #40160091435](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091435) and [Security architecture #40160091518](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40160091518). Build includes ISO and native BIOS QEMU boot checks; no physical-device durability or general concurrency claim.
- Build/boot-test #40159887034 **failed at ISO compilation** with `kernel/prosenjitfs.c:10624:18: error: 'found_lba' undeclared`. The misplaced cleanup was removed from the unlink-read sweep and moved to the actual rmdir-write sweep. A fresh exact-head build/boot and security pass is required.

## FS-28 — Truncate-shrink tail isolation (**PASS** on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- The shrink path stages zeroes from the new EOF through the end of a retained partial data block in the same redo transaction as inode/extent/allocation metadata.
- Regression sequence: shrink `/hello.bin` to 100 bytes, write a marker at offset 200, and confirm that the gap is all zeroes after the file extends to 201 bytes.
- On-disk v1 format is unchanged. [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332) passed on the code/test head; no physical-device durability or general concurrent-mutation claim.

## FS-29 — Inode allocation output on transaction failure (**PASS** on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- `prosenjitfs_alloc_inode()` now publishes its output inode number only after the bitmap transaction returns success.
- A targeted pre-commit journal-write failure test requires `PROSENJITFS_EIO`, unchanged output sentinel and mock-media CRCs, writable state, and a clean integrity check.
- On-disk v1 format is unchanged. [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332) passed on the code/test head; no physical-device durability or general concurrent-mutation claim.

## FS-30 — Journal-retirement I/O error propagation (**PASS** on code/test head `5aa0569a24093b11aee7685c7f3f136f02aaaded`)

- `pfs_journal_clear()` now returns named filesystem errors for invalid internal state and propagates the exact result of `pfs_lba_add()`, `pfs_write_block()`, and `pfs_flush()`.
- New recovery self-test fails retirement write and flush independently after stage-4 interruption; mount must return EIO, retry must recover successfully, original payload must survive, and integrity must pass.
- On-disk v1 layout/protocol unchanged. [Build/boot-test #8590](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503632) and [Security architecture #8126](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503601) passed on this code/test head.

## FS-31 — Committed-journal replay write failure (**PASS** on code/test head `73bff7fe8aad278e04f08ee7594b901bb11da6f9`)

- New mock-device test fails the second home-block write during recovery of a durably committed unlink; a power-loss reset then requires repeat recovery to complete the unlink and pass integrity.
- v1 format/protocol unchanged. [Build/boot-test #8593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866752) and [Security architecture #8129](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866715) passed on the code/test head. QEMU/mock-device evidence only; no hardware power-loss certification.

# ProsenjitFS Completion Matrix

## Part 2 follow-up — VFS read output-count contract (2026-10-24)

- [x] Audited `vfs_file_read()`: unlike the write fix in this branch, it cleared `bytes_read` only after argument and permission checks. A rejected read could therefore leave a stale caller-provided count.
- [x] Changed `kernel/vfs.c` to clear a non-null output count before validation/dispatch; added regressions for permission-denied and filesystem-`EIO` reads using a nonzero sentinel.
- [x] Exact PR-head Build/boot-test #40119299299 and Security architecture #40119299311 passed on `89a35608bcd278b01977f5abded40f2208208d9e`; PR #235 merged as `d82d1010f3c58696c15dfb09c326ba2b6d612e43`.
- Scope: VFS output contract only; no on-disk format change, no crash-atomicity or hardware-durability claim.


## Part 2 execution update — 2026-10-24

- Starting main HEAD: `e875810769cb022f1a450bce86027966a3982fcc`.
- Change under verification: `kernel/vfs.c` now clears `bytes_written` before validating/dispatching a write, preventing stale caller state after any write error. A focused VFS self-test asserts that a callback returning `PROS_VFS_EIO` leaves the count at zero.
- Status: **verified** on exact PR head `89a35608bcd278b01977f5abded40f2208208d9e`: [Build/boot-test #40119299299](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119299299) and [Security architecture #40119299311](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119299311) passed; merged as `d82d1010f3c58696c15dfb09c326ba2b6d612e43`.
- This is VFS contract hardening in the existing ProsenjitFS integration, not a new filesystem format or claim of crash atomicity.


## Current source-backed audit — 2026-10-24 (authoritative snapshot)

**Repository:** `prosenjit-Adhikary/ProsenjitOS`  
**Audited baseline:** `main` at `cfe04a8520ec917a90b284bbc726a8bd0cc83a2d`  
**Audit branch:** `audit/prosenjitfs-part1-20261024`

### Verified baseline

- Main-branch Build/boot-test run [#8426](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40083299324) passed at the audited commit. Its completed steps include toolchain validation, desktop/DML prerequisite tests, ISO build, ISO QEMU boot and native BIOS QEMU boot.
- Main-branch Security architecture run [#7962](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40083299325) passed at the same commit.
- The workflow's required serial markers include partition discovery, VFS provider-boundary self-test, storage volume discovery, and the ProsenjitFS format/mount/allocation/journal/recovery self-test. These establish the baseline at this exact main commit; they do not prove unmerged test branches.
- Source inspection confirms a freestanding C filesystem in `kernel/prosenjitfs.c`, shared block/VFS contracts in `kernel/include/block.h` and `kernel/include/vfs.h`, and a version-1 disk format in `include/prosenjitfs_format.h`. The current provider includes formatting, probing/mounting, persistent allocation, inodes/extents, hash-indexed directories, VFS file/namespace operations, redo-journal recovery and a read-only integrity-report API.
- Existing design notes explicitly limit the integrity scan to callers that serialize it against mutations. The `io_lock` in filesystem state must not be treated as proof of global VFS/SMP safety.
- Existing design notes distinguish deterministic mock-device crash injection from physical-device power-loss validation. No automatic destructive repair or orphan reclamation is currently exposed by the integrity checker.

### Required status categories

| Category | Audited finding |
|---|---|
| **Verified complete (baseline only)** | The audited main commit builds and passes both ISO QEMU and native BIOS QEMU workflow gates; Security architecture check passes. The existing format/mount/allocation/journal/recovery self-test passes on that baseline. |
| **Partially implemented** | Systematic per-operation read/write/flush fault injection is being added incrementally. Current VFS locking is not a general concurrency guarantee. The structured integrity scan reports problems but does not repair them. |
| **Missing / deferred** | A host-side `mkprosenjitfs` utility and VFS-visible fallocate policy are documented as future work. Conservative repair/reclamation, physical-device power-loss testing, and concurrent mutation/scan guarantees remain deferred. |
| **Verified on PR branch (not yet merged)** | PR #233's earlier head `74b8cedf5b96055823b9a9a5d4a963b8d2f1a3b0` failed at stage `0x28E1` / line 10465 (`-27`), checking expected EIO, unchanged durable/volatile/dirty mock-media CRCs and writable mount state. Source audit found that `pfs_make_vnode()` leaked filesystem `EIO (-2)` into VFS, where `-2` is `PROS_VFS_ENOSYS`. Fixed by retaining filesystem status inside the helper and translating at VFS call boundaries; split assertion into predicate-specific diagnostics. Current exact head `84d2b84948065011cf0c94291e8bcaf5d9f7079c` passed [Build/boot-test #40116501666](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40116501666) and [Security architecture #40116501659](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40116501659). Logs confirm the ProsenjitFS self-test marker and ATA PIO -> MBR -> ProsenjitFS persistent mount marker; both QEMU runs terminated with expected timeout 124 after required markers. PR #233 has now merged as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`. Exact-head Build/boot-test #40116501666 and Security #40116501659 both passed; post-merge main Build/boot-test #40116980733 and Security #40116980685 are pending. |
| **Blocked by unavailable tooling** | This session has no attached local repository/interactive shell for independent local compilation. GitHub Actions is accessible and did run PR #233 on self-hosted runner `home`; the failure is a test failure, not evidence of runner unavailability. |
| **Not yet verified** | PR #233's append/truncate read-failure sweep passed on its exact head and has now merged into `main` as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`. Create/overwrite-write and rmdir-write PRs must pass their exact heads before being counted green. General concurrency and physical durability remain unverified. |

### Immediate next action

1. **Completed and verified on PR #233:** `pfs_make_vnode()` returns filesystem-native errors; VFS callbacks translate them via `pfs_vfs_error()`. The append sweep has distinct diagnostic returns for operation status, durable/volatile/dirty CRC and read-only state.
2. Exact head `84d2b84948065011cf0c94291e8bcaf5d9f7079c` passed Build/boot-test #40116501666 (ISO QEMU + native BIOS QEMU; required filesystem and ATA/partition serial markers present) and Security architecture #40116501659.
3. Next, review the still-open create/overwrite-write and rmdir-write sweeps. Keep general concurrency and physical-device durability explicitly separate from mock-device/QEMU acceptance.
4. Use `docs/FILESYSTEM.md` and `docs/FILESYSTEM.md` as the dependency-ordered next-work checklist.

## Historical verification notes — 2026-10-22 (superseded by the 2026-10-24 snapshot)

- PR #193 fixed a diagnostic contract bug in the report-only integrity scanner: allocated inode bitmap + FREE inode record previously returned `-20000 - inode_number`, which the public wrapper incorrectly decoded as a source line. It now returns corruption with the actual checker source line and has a targeted regression. PR #193 merged as `0e61bc728f1eb0fe5cf3a701de806d98a04bfeb0`.
- Exact PR head `7a3ff53146aa9685d3becc0d304b62f16e8d01d4` passed build/boot-test [#39783774495](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39783774495) and security architecture [#39783774327](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39783774327). Build steps including ISO build, filesystem diagnostics, ISO QEMU boot, and native BIOS QEMU boot all passed.
- Follow-up documentation commit `3580b1bb1b97cc1deffc8b30ecbad352356fe8ed` passed build/boot-test [#39784256277](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39784256277) and security architecture [#39784256637](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39784256637). On this exact current-main descendant, toolchain verification, desktop/DML tests, ISO build, ProsenjitFS diagnostics, ISO QEMU boot, and native BIOS QEMU boot all passed.
- PRs #170 and #179 are merged. PRs #172 and #173 were closed as superseded by #179, not independently merged. Issue #166 remains open for staged integrity/recovery follow-up.
- PRs #167 and #168 contain earlier, overlapping versions of the read-only/structured integrity API; their feature scope has since been consolidated and extended through #179 and #193. These stale branches conflict with current main and should not be used as merge candidates.
- PR #142 remains a diagnostic-only branch with a recorded native BIOS QEMU boot failure on its own head. Do not treat passing descendant-main boot tests as proof that this separate branch is fixed.

- Current `main` commit `141f63ae7c79ab85252fab84f8fc44bc05bfdf8d` (`docs(prosenjitfs): record green feature-mask verification`) passed build/boot-test [#39782771424](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39782771424) and security architecture [#39782771420](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39782771420).
- The build/boot job ran on self-hosted runner `office`. Verified steps: toolchain validation, desktop script checks, DML unit tests, ISO build, ProsenjitFS block-init diagnostics, ISO QEMU boot, and native BIOS QEMU boot all passed.
- The merged integrity/journal work is consolidated through PR #179 (`4bb5a241054b8e7b661672fc9ee64b0a30919756`); later merged hardening includes checksum-valid extent, bitmap-tail, directory-bucket and duplicate-name corruption regressions documented below.
- PRs #170 and #179 are merged. PRs #172 and #173 were closed as superseded by #179, not independently merged. Issue #166 remains open for staged integrity/recovery follow-up.
- PR #142 remains a diagnostic-only branch with a recorded native BIOS QEMU boot failure on its own head. Do not treat passing descendant-main boot tests as proof that this separate branch is fixed.

## Purpose and baseline

This is a source-grounded tracking document, not a declaration that ProsenjitFS is production-complete. The inspection baseline for the integrity-scan implementation is branch
`hardening/prosenjitfs-report-only-orphan-inodes` (source file SHA observed during inspection:
`ed78325c8539de09daca7968a9b18d73323a162c` for `kernel/prosenjitfs.c`).

Integration status as of 2026-10-22:
- PR #170 (orphan block reporting) was merged into `main`.
- The combined integrity/inode/extent validation and journal recovery hardening was merged through PR #179.
- Issue #166 remains open for follow-up on the repair roadmap; automatic orphan reclamation is deliberately deferred.

Do not reclaim orphan objects or describe unrun tests as passing.

## Completion matrix

Status meanings: **Implemented in inspected source** means code exists, not that the full acceptance gate has passed. **Needs verification** means this inspection did not establish a passing result on the exact final commit. **Deferred** means the feature is not part of the current safe scope.

| Subsystem | Source-grounded status | Remaining acceptance work |
|---|---|---|
| On-disk format / superblock geometry | Implemented in existing ProsenjitFS; integrity checker validates key region ordering, bounds, bitmap capacity, inode-table capacity, and root inode. | Corrupt each geometry/feature/version case and verify mount rejects safely; run the current full matrix. |
| Mount and journal recovery ordering | Merged PR #179 includes journal-slot invalidation, contradictory commit rejection, fail-closed handling, and recovery-restart regression coverage; consolidated build/boot and security workflows passed. | Continue adversarial recovery/failure-injection review and real-device durability validation; QEMU/mock-device tests cannot prove physical power-loss behavior. |
| Block bitmap and allocation ownership | Integrity checker marks metadata references, compares allocated bits to ownership, checks metadata bits and out-of-range tail bits, and reports unowned allocated data blocks in report mode. | PR #191 merged as `a843f4ac1d62ef27c6fe3a06d03359783a50b5c2`; its exact head passed build/boot-test #7824 and security architecture #7360. Checksum-valid block-bitmap and inode-bitmap tail-bit regressions verify out-of-range allocation bits are rejected without scan-side media mutation, then pass after restoration. Continue boundary and read-failure coverage. |
| Inode bitmap and inode metadata | Merged PR #179 validates allocation state, inode checksums/types, parent range, bitmap tail bits, and inode sizes against volume capacity; checksum-valid oversized-inode regression passes. | Continue adversarial generation/checksum cases and stable report-detail review as follow-up hardening. |
| Directories and names | Checker validates directory-index and entry bounds, name length/UTF-8/hash/checksum, duplicate names in a bucket, child type/parent, duplicate references, and parent chains to root. | PR #184 adds a checksum-valid directory-entry self-cycle regression, checking scan-side media immutability and clean rescan after restoration. PR #187 merged as `6e9e70676617d8f128ca03134cd1c8ebe0eca829`; its exact head passed build/boot-test #7802 and security architecture #7338. The checker now rejects entries whose hash does not map to their containing bucket, with a checksum-valid wrong-bucket regression and read-only media-state checks. PR #192 merged as `2abc3feffba5a5d2941b8580e7af06c3f5427c99`; its exact head passed build/boot-test #7831 and security architecture #7367. It creates two distinct filenames that intentionally collide in the same directory bucket, corrupts one entry into a duplicate name with a valid checksum, and verifies read-only rejection and clean rescan after restoration. Broader repeated-reference and corruption-at-read-boundary cases remain follow-up. Review symlink namespace semantics against the actual API. |
| File extents and block ownership | Merged PR #179 validates bounded extent chains, logical ordering/arithmetic, volume-capacity bounds, block ownership and allocated-byte totals. PR #181 adds pre-iteration logical/physical range validation and a checksum-valid oversized-block-count regression; build/boot and security workflows passed on exact PR head `a237ebd02e77609b47258ff18a4677857bbd8350`. | PR #182 adds checksum-valid zero-length and out-of-volume physical-start regressions; build/boot and security workflows passed on exact PR head `666a7e2f96f6c5dd0c7b11d1cf16602b08dafbaa`. PR #185 merged as `0e8e996f55df9da7840502d4dba4e5d0cb0934a8`; its exact head passed build/boot-test #7795 and security architecture #7331. The checksum-valid duplicate physical-block ownership regression checks read-only scan state and a clean rescan after restoration. PR #188 merged as `e5af0af4df3cf0d67eadbcedf799fb384f22d5de`; its security architecture check passed (run #7347). The exact PR build job was marked cancelled despite the listed ISO/native BIOS steps completing successfully, so do not count that run alone as a clean pass. The descendant `main` commit `569fb12b786a1bef4ba7ef8a13357b66af66707c` subsequently passed build/boot-test #7834, including ISO build, ISO QEMU boot, and native BIOS QEMU boot. The cross-inode regression is therefore covered by a passing descendant build, but exact-head PR verification remains distinct. PR #189 merged as `2029eb371c6865a839f282b0538c59030f3c8e2c`; its exact head passed build/boot-test #7814 and security architecture #7350. The checksum-valid overlapping-logical-range regression confirms corruption is rejected without scan-side media mutation. PR #190 merged as `0026779c6006a72f21c1a824d2fd8ec403a05a17`; its exact head passed build/boot-test #7818 and security architecture #7354. The checksum-valid extent-chain self-cycle regression confirms repeated extent metadata is rejected without scan-side media mutation. Continue broader adversarial overlap review. |
| Orphan data-block detection | Implemented as report-only candidate detection in the inspected branch. Report mode completes the bounded bitmap scan and returns corruption when candidates exist. | Preserve zero mutation of durable, volatile, and dirty state on all outcomes; no reclamation without a separately proven crash-safe design. |
| Orphan inode detection | Implemented as report-only candidate detection for allocated, structurally valid non-root inodes with no directory reference and a parent chain reaching root. | Test unusual namespace/link semantics and ensure candidates are never treated as safe-to-free solely from this report. |
| Structured integrity report | Existing report includes status, first error location, inode and bitmap scan counts, and first/count of orphan block and inode candidates. | Ensure error classification is stable and documented; verify bounded memory and report initialization for every early-return path. |
| Create/read/write/append/truncate/delete | Core mutation paths and deterministic self-tests exist in the current implementation. | Run normal, zero-length, block/extent-boundary, max-size, invalid-input, persistence and remount tests on the exact final commit. |
| Directory create/remove/rename/path traversal | Core directory and mutation paths exist. Rename and unlink now have read/write failure sweeps; rmdir has a cyclic-chain classification regression and read-failure sweep. Exact PR heads passed ISO QEMU, native BIOS QEMU, and Security architecture checks. | Continue mutation rollback/persistence tests for mkdir/create/truncate/append and remaining path/name edge cases; test allocation failures and all transaction boundaries. |
| Journal commit / replay / flush failures | Merged PR #179 invalidates reused commit slots, rejects contradictory matching-ID commit records, forces uncertain post-commit failures read-only, and tests recovery restart after interrupted replay. Combined build/boot and security workflows pass. | Maintain the physical power-loss caveat for simulated QEMU/mock-device tests; add real-device validation separately. |
| Failed I/O and resource cleanup | Rename and unlink read/write failure sweeps and rmdir read-failure sweep are merged; they check error propagation, mount/media invariants where applicable, remount recovery, payload preservation, and report-only integrity. A real EIO-to-corruption misclassification in unlink extent reclamation was fixed. | Extend systematic short/read/write/flush injection to create, mkdir, rmdir writes, truncate, append, and all journal boundaries; continue cleanup/resource-exhaustion tests. |
| Concurrency and serialization | Integrity API explicitly requires caller-side serialization against concurrent mutation. | Confirm all callers obey this precondition; do not claim the scan is internally concurrency-safe. |
| Read-only/degraded mount behavior | Degraded-superblock read-only behavior is a stated architecture constraint in Issue #166. | Test that uncertain metadata/recovery states remain read-only and cannot mutate media. |
| VFS/provider and boot integration | Code-bearing PRs #223–#226 passed exact-head Build/boot-test (including ISO QEMU and native BIOS QEMU) and Security architecture CI; documentation logs and matrix were refreshed afterward. | Re-run full CI on any subsequent code changes; continue provider-boundary, resource cleanup, and hardware-backed validation. |
| CoW, snapshots, reflink, ACLs/xattrs, links, quotas, compression, dedup, online resize, relocation | Listed as long-term targets in README, not established as implemented by this audit. | Explicitly defer until design, API/on-disk compatibility, recovery semantics and tests are implemented. |
| Repair / orphan reclamation | **Deferred for safety.** The inspected scan is read-only and report-only. | Require proven reachability/link semantics, journaled or otherwise crash-safe repair, resumable interrupted repair, explicit operator-controlled mode, dry run, and a clean follow-up scan before implementation. |

## Safe execution sequence

1. The report-only integrity and journal-hardening branches have been reconciled and merged through PR #179.
2. Continue fixing source-proven defects with a regression test for each behavior change.
3. Run narrow filesystem tests, then the warning-clean build, security workflow, ISO QEMU boot, native BIOS QEMU boot, remount/persistence and recovery/fault-injection suites.
4. Tie every verification result to an exact commit SHA and inspect actual workflow logs.
5. Do not enable destructive repair until the repair safety gates above are satisfied.

## Verification record

- Consolidated change PR #179 was merged to `main` as commit `4bb5a241054b8e7b661672fc9ee64b0a30919756`.
- Exact pre-merge head `3cb91f739a839bf05461b754b698b4f3c7341b18` passed [build and boot-test #7775](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39772556092) and [security architecture #7311](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39772555954).
- The build/boot workflow passed desktop/DML unit tests, ISO build, ProsenjitFS block-init diagnostic, ISO QEMU boot, and native BIOS QEMU boot.
- Follow-up PR #181 was merged as `0fb37a3abd8079e08e4ac9d7918b355fd57b45b4`; its exact head passed [build/boot-test #7780](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39774423290) and [security architecture #7317](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39774423437). The added oversized-block-count regression verifies corruption is rejected without scan-side media mutation.
- Follow-up PR #182 was merged as `4654c889b0b6c59b2f656c60a285e25700bcb6f2`; its exact head passed [build/boot-test #7787](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39775368199) and [security architecture #7323](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39775368156). Added zero-length and out-of-volume physical-start corruption cases, each checked for scan-side media immutability and a clean rescan after restoration.
- Follow-up PR #185 merged as `0e8e996f55df9da7840502d4dba4e5d0cb0934a8`; exact head passed [build/boot-test #7795](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39776334832) and [security architecture #7331](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39776334904). It verifies checksum-valid duplicate physical-block ownership is rejected without scan-side media mutation.
- Follow-up PR #187 merged as `6e9e70676617d8f128ca03134cd1c8ebe0eca829`; exact head passed [build/boot-test #7802](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39777353077) and [security architecture #7338](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39777353159). It validates directory hash-bucket membership and checks a checksum-valid misplaced-entry fixture.
- Follow-up PR #188 merged as `e5af0af4df3cf0d67eadbcedf799fb384f22d5de`; exact head passed [build/boot-test #7807](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39778043437) and [security architecture #7343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39778043379). It verifies cross-inode physical-block aliasing is rejected by the global ownership scan.
- Follow-up PR #189 merged as `2029eb371c6865a839f282b0538c59030f3c8e2c`; exact head passed [build/boot-test #7814](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39778530084) and [security architecture #7350](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39778530132). It verifies a checksum-valid logical overlap is rejected and the report-only scan does not mutate mock-media state.
- Follow-up PR #190 merged as `0026779c6006a72f21c1a824d2fd8ec403a05a17`; exact head passed [build/boot-test #7818](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779110024) and [security architecture #7354](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779110075). It verifies a checksum-valid self-cycle in an extent chain is rejected.
- Follow-up PR #191 merged as `a843f4ac1d62ef27c6fe3a06d03359783a50b5c2`; exact head passed [build/boot-test #7824](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779654470) and [security architecture #7360](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39779654485). It verifies invalid block- and inode-bitmap tail bits are rejected and that the scan does not mutate mock media.
- Follow-up PR #192 merged as `2abc3feffba5a5d2941b8580e7af06c3f5427c99`; exact head passed [build/boot-test #7831](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39780459233) and [security architecture #7367](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39780458900). It covers duplicate directory names in an intentionally colliding hash bucket and confirms the report-only scan does not mutate mock media.
- Integrity regressions cover report-only orphan blocks/inodes, checksum-valid out-of-capacity extents and oversized inode sizes, deterministic reports, and read-only media invariance. Journal regressions cover reused commit slots, contradictory commit metadata, write/flush failure boundaries, and recovery restart.
- Mock-device/QEMU tests do not prove physical hardware power-loss durability.
- Automatic repair and orphan reclamation remain deliberately deferred; a report-only candidate is not safe to free without a separately reviewed, crash-safe and resumable repair design.


## Verification update — 2026-10-22 13:07 UTC

- Main merge commit: `0e61bc728f1eb0fe5cf3a701de806d98a04bfeb0`.
- Exact PR head `7a3ff53146aa9685d3becc0d304b62f16e8d01d4` passed build/boot-test [#39783774495](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39783774495) and security architecture [#39783774327](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39783774327).
- Follow-up documentation commit `3580b1bb1b97cc1deffc8b30ecbad352356fe8ed` passed [build/boot-test #39784256277](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39784256277) and [security architecture #39784256637](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39784256637), including successful ISO and native BIOS QEMU boots.
- The regression now guarantees that allocated-bitmap/free-inode corruption is not mistaken for a synthetic source-line encoding and that the report has a bounded source-line location.
- This is a green exact-PR-head milestone, not a declaration of production-complete filesystem behavior. Remaining acceptance work includes injected read/write/flush failures, mutation rollback and persistence/remount coverage, report error-classification review, adversarial namespace/link semantics, caller serialization review, and physical-device durability validation.
- Automatic orphan repair/reclamation remains deferred. QEMU and mock-device tests do not prove physical power-loss durability. PR #142's own branch remains independently unverified/failing.

- Current main: `141f63ae7c79ab85252fab84f8fc44bc05bfdf8d`.
- Build and boot-test [#39782771424](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39782771424): **PASS** on self-hosted runner `office`; desktop script validation, DML tests, ISO build, ProsenjitFS block-init diagnostics, ISO QEMU boot, and native BIOS QEMU boot all succeeded.
- Security architecture checks [#39782771420](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39782771420): **PASS** on the same commit.
- The unknown-feature regression rejects unsupported v1 feature bits even when both redundant superblocks have valid CRCs, then restores the fixture.
- This is a green build/security/boot milestone, not a declaration of production-complete filesystem behavior. Remaining acceptance work includes broader injected read/write/flush failures, persistence/remount and rollback testing for each mutation path, report initialization/error-classification checks, adversarial namespace/link semantics, caller serialization review, and physical-device durability validation.
- Automatic orphan repair/reclamation remains deferred. QEMU and mock-device tests do not prove physical power-loss durability. PR #142's own branch remains unverified/failing independently even though descendant `main` boots successfully.


## Verification update — 2026-10-22 13:35 UTC

- PR #194 merged as `3d8a40d0a6ee5fa1189abe3e1b9bda774350d140`. Exact PR-head build/boot-test #39786642331 and security architecture #39786642131 passed. It added mock-device read fault injection and verified that an inode-bitmap read failure is reported as `PROSENJITFS_EIO`, with durable/volatile/dirty mock-media state unchanged.
- PR #195 merged as `02063bac9d92dad5781230c7898d27b73544c191`. Exact final PR head `8ed82f7bf1ee074cdae9e40be7362519671935e5` passed build/boot-test #39787466192 and security architecture #39787466220. Main merge commit build job #39787804429 and security #39787804339 also completed successfully; build included ISO QEMU and native BIOS QEMU boot.
- Integrity scan now preserves `PROSENJITFS_EIO` across inode bitmap, inode, parent-chain, directory-index/entry, extent and block-bitmap reads instead of collapsing device failures into corruption.
- Test-coverage limitation: the deterministic injected read-failure regression directly targets the inode-bitmap read path. The other propagated read paths still need focused injected fault cases. Broader write/flush fault coverage, mutation rollback/remount, adversarial namespace/link cases, caller serialization and physical-device durability remain acceptance work.
- Automatic orphan repair/reclamation remains deferred. Mock-device and QEMU tests do not prove physical power-loss durability. This is incremental hardening, not a claim of production-complete filesystem behavior.


## Verification update — 2026-10-23: integrity-scan read-failure sweep

- Commit `2ddc86ac087a90be018d8ab14a877d26be4bb060` expands the integrity scanner's mock-device read-failure regression from a single injected read to a sequential sweep over each read position reached by a clean scan.
- For each injected read failure, the regression requires `PROSENJITFS_EIO` in both the return value and report status, and verifies durable, volatile and dirty mock-media state remains unchanged. The sweep stops when the selected read position is beyond the scan's read sequence.
- Exact commit build/boot-test [#39927032201](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39927032201): **PASS**. Toolchain validation, desktop/DML tests, ISO build, block-init diagnostics, ISO QEMU boot and native BIOS QEMU boot all passed; both boot logs contained the ProsenjitFS format/mount/allocation/journal/recovery self-test success marker, and the native BIOS log also contained the ATA/MBR/ProsenjitFS persistent-mount marker.
- Exact commit security architecture checks [#39927032230](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39927032230): **PASS**.
- This closes the narrow acceptance gap for sequential read-failure classification during the report-only integrity scan. It does **not** close all ProsenjitFS acceptance work: broader mutation rollback/persistence-remount coverage, write/flush failure paths, adversarial namespace/link semantics, caller-serialization review and physical-device power-loss durability still require independent verification. QEMU/mock-device success is not proof of real-hardware durability.


## Verification update — 2026-10-23: degraded-mount mutation rejection

- Commit `433c93ff864b0fb114119ddfe859cf421d01d6fb` extends the degraded-superblock self-test in both directions: primary invalid / backup valid, and backup invalid / primary valid.
- Each direction now attempts file creation and directory creation on the read-only degraded mount. Both operations must return `PROS_VFS_EPERM`; durable disk, volatile cache and dirty-block checksums must remain unchanged.
- Exact commit build/boot-test [#39928003595](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39928003595): **PASS**. ISO build, ISO QEMU boot and native BIOS QEMU boot all succeeded, including the ProsenjitFS format/mount/allocation/journal/recovery marker and the ATA/MBR persistent-mount marker.
- Exact commit security architecture checks [#39928003550](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39928003550): **PASS**.
- This verifies the two degraded-superblock recovery directions reject create/mkdir mutation without mock-media changes. It does not prove all VFS mutation operations, all I/O fault paths, or real-device power-loss durability.


## Verification update — 2026-10-23: rename commit-write failure and remount

- PR #197 adds a regression for a failed journal commit-record write during `vfs_rename`.
- The test simulates power loss and remount, checks that the original path and payload remain intact, confirms the destination path is absent, and runs the report-only integrity check.
- Exact PR head `13cf8cc0201ba642be8960475f866d3a732029cb` passed build/boot-test [#39946623102](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946623102), including ISO build, ISO QEMU boot and native BIOS QEMU boot. Security architecture [#39946623071](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946623071) also passed.
- Scope: this closes one focused namespace-rollback/remount acceptance case under the mock block device. Other mutation classes and write/flush failure boundaries still need a matrix-driven review; mock-device/QEMU coverage is not proof of physical power-loss durability.


## Verification update — 2026-10-23: unlink commit-write failure and remount

- PR #198 adds a regression for a failed journal commit-record write during `vfs_unlink` on a populated file.
- The test simulates power loss and remount, checks the original path and payload remain intact, and runs the report-only integrity scan.
- Exact PR code head `ed197bf77328ae419f8f45480640da0063641537` passed build/boot-test [#8194](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946938083), including ISO build, ISO QEMU boot and native BIOS QEMU boot. Security architecture [#7730](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39946938090) also passed.
- Scope: this closes one focused unlink rollback/remount case under the mock block device. Other mutation classes and read-error classification still need review; mock-device/QEMU coverage is not proof of physical power-loss durability.


## Verification update — 2026-10-23: unlink read-error classification

- Fixed `pfs_vnode_unlink()` so an invalid entry LBA returns corruption while a block-device read failure propagates as `PROS_VFS_EIO`.
- Added a deterministic fault-injection case for the directory-entry read; it verifies unchanged durable/volatile/dirty media, preserved file bytes, a writable mount, and a clean integrity scan.
- Exact code head `1c051dc7f4d2561352e7728e1b0da86cadfa3c54` passed build/boot-test [#8199](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947309841), including ISO and native BIOS QEMU boot. Security architecture [#7735](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947309844) also passed.
- This closes one specific I/O-error-classification defect. Caller-side scan/mutation serialization, broader mutation rollback coverage, and physical-device durability remain open acceptance work.


## Verification update — 2026-10-23: truncate commit-write failure and remount

- PR #200 adds a populated-file regression for a journal commit-record write failure during shrink-to-zero.
- The test verifies `PROS_VFS_EIO`, fail-closed read-only state, simulated power loss/remount, preservation of the original file length and bytes, and a clean report-only integrity scan.
- Exact PR code head `2c8409065afb624f6bf115fb0aece5095f777355` passed build/boot-test [#8205](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947772776), including ISO and native BIOS QEMU boot. Security architecture [#7741](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39947772767) also passed.
- Scope: this closes one focused truncate rollback/remount case under mock-device fault injection. Broader mutation-path coverage and physical-device durability remain separate acceptance work.


## Verification update — 2026-10-23: mkdir commit-write failure and remount

- PR #201 adds a commit-record write-failure regression for `vfs_mkdir` across its multi-image metadata update.
- The test verifies `PROS_VFS_EIO`, fail-closed read-only state, simulated power-loss remount, absence of the uncommitted directory, and a clean report-only integrity scan.
- Exact PR code head `af7cbf0187aa717d483d146878051bd5c2cf584a` passed build/boot-test [#8211](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948217344), including ISO and native BIOS QEMU boot. Security architecture [#7747](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948217395) also passed.
- Scope: this closes one focused mkdir rollback/remount case under mock-device fault injection. Other mutation cases and physical-device durability remain separate acceptance work.


## Verification update — 2026-10-23: rmdir commit-write failure and remount

- PR #202 adds a commit-record write-failure regression for `vfs_rmdir` on an existing empty directory.
- The test verifies `PROS_VFS_EIO`, fail-closed read-only state, simulated power-loss remount, persistence/openability of the original directory, and a clean report-only integrity scan.
- Exact PR code head `f137d83af1df8fdfbff9f3c21672d65827d198a9` passed build/boot-test [#8216](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948709380), including ISO and native BIOS QEMU boot. Security architecture [#7752](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39948709352) also passed.
- Scope: this closes one focused rmdir rollback/remount case under mock-device fault injection. Other mutation cases and physical-device durability remain separate acceptance work.


## Verification update — 2026-10-23: file-create commit-write failure and remount

- PR #203 adds a commit-record write-failure regression during file creation through `vfs_file_open(..., PROS_VFS_O_CREATE)`.
- The test verifies `PROS_VFS_EIO`, fail-closed read-only state, simulated power-loss remount, absence of the uncommitted file, and a clean report-only integrity scan.
- Exact PR code head `4f25d7d711d799bac1974049dc7972e57eb52378` passed build/boot-test [#8221](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39949042895), including ISO and native BIOS QEMU boot. Security architecture [#7757](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39949043002) also passed.
- Scope: this closes one focused file-create rollback/remount case under mock-device fault injection. Physical-device durability and other mutation/failure combinations remain separate acceptance work.


## Verification update — 2026-10-23: integrity-scan caller serialization audit

- Reviewed the integrity API declarations/documentation, all in-source scan callsites, and the CPL3 entry interrupt-flag contract.
- All in-source calls to `prosenjitfs_check_integrity_report()`/`prosenjitfs_check_integrity()` are within `prosenjitfs_selftest()`; repository search found no separate production kernel caller. The documented caller-serialization precondition is therefore not currently exercised by a production callsite.
- `pfs_io_lock()` only serializes append writes; it is not a filesystem-wide lock. `boot/entry.asm` keeps IF clear for CPL3 because timer preemption is not yet scheduler-safe.
- Current callsite audit is complete, but **general concurrent VFS safety remains unproven**. Before enabling timer preemption/SMP or production concurrent scans, add a reviewed locking boundary and tests for lock ordering/recursion and mutation-versus-scan exclusion.
- No automatic repair/reclamation was added. Physical-device power-loss durability remains outside mock-device/QEMU proof.


## Pending verification — recovery-test failure cleanup (2026-10-23)

- Source audit found an unexpected-failure branch in the stage-10/11 recovery interruption regression that zeroed `fs` directly rather than using `pfs_test_crash_reset()`. The helper also discards volatile mock-device writes, deactivates the mount, and frees transaction/scratch buffers.
- Commit `ce2d39f71aaf1f33545b67e4f3cb18c3fb3b611d` changes only that failure path to use the helper; it preserves the same failing assertion and does not weaken the regression.
- Follow-up documentation commit `372ed6eed56d3b7f8323753a08900160616a4c4d` records the source finding in `docs/DEBUG_LOG.md`.
- Build/boot-test run [#8227](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39956134203): **PASS** on self-hosted runner `office`; toolchain checks, desktop/DML unit tests, ISO build, ISO QEMU boot, and native BIOS QEMU boot all passed. Both QEMU logs contain the ProsenjitFS format/mount/allocation/journal/recovery success marker; the native BIOS log also contains the ATA PIO, LBA-boundary, and ATA → MBR → ProsenjitFS persistent-mount markers.
- Security architecture run [#7763](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39956134201): **PASS**.
- Exact tested PR head: `f2df43395bbf2f07c56b6796df2818cc75aec220`. This validates the test cleanup and current automated gates only; it does not establish physical-device power-loss durability or production completion.


## Additional regression coverage in review — 2026-10-23

- PR #211 adds two adversarial namespace integrity tests: duplicate inode references across distinct directory entries, and duplicate valid names across distinct inodes. Both tests recompute valid entry checksums, require `PROSENJITFS_ECORRUPT`, verify durable/volatile/dirty mock-media CRCs are unchanged by the report-only scan, restore the original entry, and require a clean rescan.
- PR #212 adds a bounded read-failure countdown sweep around `vfs_rename`. For each read position reached, it requires rename failure, unchanged durable/volatile/dirty media CRCs, a writable mount, intact original path/payload, absent destination, and a clean integrity scan. The sweep stops at the first countdown not reached.
- PR #211 merged as `e8a27899d4fe6de72a6e0959fa9d04659ca34eec`; exact head `28446ed40aeb748e8e0a5b3172a3e86f799f4567` passed Build/boot-test [#39980888372](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39980888372) and Security architecture [#39980888146](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39980888146). PR #212 merged as `b6b6eaf409c9407a822b0a6016147087edb6c365`; fixed head `1204deed4e8899448422e5cc7702eca776de3c6f` passed Build/boot-test [#40072397646](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397646) and Security architecture [#40072397702](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397702).
- These are mock-device regression tests. They do not prove physical-device power-loss durability, full concurrent VFS safety, or production filesystem completion.


## Pending verification — rename read-failure sweep fix (2026-10-24)

- Build/boot run [#39981319506](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39981319506) failed the ISO QEMU ProsenjitFS self-test at source line 10605 (stage 0x296D, failure -21); build/ISO and prior diagnostic steps passed. Security architecture run [#39981319411](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/39981319411) passed.
- Root cause: the rename read-failure sweep left the final not-reached read countdown armed while running the report-only integrity scan and unmount. Those follow-up operations could consume an injection intended only for vfs_rename.
- Fix in commit a7a7a5b7eaca35dfdf91788d44e37daef433f2a0 disarms the countdown before integrity/unmount only when the requested read position was not reached by rename. Failure-path assertions remain intact.
- Debug log updated in commit f497ca14d967a4107f6f686d241187dc87067a4a.
- **Status: verified** on fixed exact PR head `1204deed4e8899448422e5cc7702eca776de3c6f`: Build/boot-test [#40072397646](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397646) passed ISO QEMU and native BIOS QEMU; Security architecture [#40072397702](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072397702) passed. PR #212 is merged.


## Verification update — rename write-failure sweep (2026-10-24)

- Added mock-device one-shot block-write failure injection and a bounded countdown sweep across `vfs_rename`.
- Each reached failure is followed by simulated power loss and remount; acceptance requires exactly one of the original/destination paths, intact payload bytes, and a clean report-only integrity scan.
- The first countdown not reached is disarmed before integrity/unmount checks so those operations cannot consume a rename-specific fault.
- Source commit: `68b04cad273d9521c271942ae828926b0bcb3ab6`; PR #222 merged as `2b14a2b67e7c3543e95140af033f603410c66bc5`.
- Exact PR head `c897fa782f805778b0de051ca2b338a39be7d7ce` passed Build/boot-test [#40072964418](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072964418), including ISO QEMU and native BIOS QEMU, and Security architecture [#40072964417](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40072964417).
- This regression does not prove physical hardware durability or concurrent VFS safety.


## Pending verification — unlink read-failure sweep (2026-10-24)

- Added a bounded read-failure sweep around `vfs_unlink`. Each reached read fault must return `PROS_VFS_EIO`, preserve all mock-media CRCs, keep the mount writable, preserve the file payload, and pass the report-only integrity scan.
- The first not-reached countdown is disarmed before follow-up checks to avoid leaking an operation-specific fault into integrity/unmount.
- Source commit: `126a4786dbd9b0427625149cee3999efd4c6dbba`.
- Initial ISO QEMU run [#40073247679](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073247679) exposed a source-proven error-classification bug: `pfs_vnode_unlink()` collapsed `pfs_read_extent()` device I/O failures into corruption. Fix commit `ab3c659d4784f82fa83b87f54e7e1d84122d8dd6` preserves the original error. Fixed exact PR head `895ee35f7a2413e569b2d42cbc52ca0efc580928` passed Build/boot-test [#40073371338](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073371338), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073371343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073371343). PR #223 merged as `30acc248df1258fa6ac65a6b5eb8e8ef47e2ceb5`. This does not prove physical-device durability or general concurrent VFS safety.


## Pending verification — unlink write-failure sweep (2026-10-24)

- Added a bounded block-write failure sweep around `vfs_unlink`; each injected failure is followed by simulated power loss and remount.
- Acceptance allows either the original file with unchanged payload or the fully committed unlink, but rejects both/neither namespace inconsistencies, altered payload, or an integrity-scan failure.
- The first not-reached countdown is disarmed before follow-up checks.
- Source commit: `a18e2a55b36c6a445657bb9786dd172fb0eaf95c`.
- **Verified:** exact PR head `c536d9e03f2e21ee559d73ecb1d1a4d92e30a74b` passed Build/boot-test [#40073592946](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073592946), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073592829](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073592829). PR #224 merged as `714fa8e66d3a46991cb80446d769ab4538a63120`. Physical-device durability and general concurrent VFS safety remain unproven.


## Pending verification — rmdir cyclic directory-entry classification (2026-10-24)

- Fixed `pfs_vnode_rmdir()` to classify a bounded directory-chain cycle as corruption rather than I/O failure.
- Added a valid-checksum self-cycle regression that checks the VFS error result, no operation-side media mutation, restoration, clean integrity scan, and normal rmdir afterward.
- **Verified:** fixed exact PR head `5a2dd5d63813498b0bd83f2aa4b6fed1f69e8e2a` passed Build/boot-test [#40073950609](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073950609), including ISO QEMU and native BIOS QEMU, and Security architecture [#40073950625](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40073950625). PR #225 merged as `0cdaab0ca75c286e839494d37c24420f74a5c039`.


## Pending verification — rmdir read-failure sweep (2026-10-24)

- Added a bounded read-failure sweep around `vfs_rmdir`. Reached faults must propagate as I/O errors, preserve durable/volatile/dirty mock-media state, keep the mount writable, and leave the directory intact.
- After each injected failure, the directory is reopened and integrity-scanned; the first not-reached countdown confirms the sweep end and is disarmed before follow-up operations.
- Source commit: `aefcac64b5ee80cf58c62c04e0a312d8878ab8dd`.
- **Verified:** exact PR head `5522c822d9bce6d01ca6c0c06b572c8032e63c5e` passed Build/boot-test [#40074188166](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40074188166), including ISO QEMU and native BIOS QEMU, and Security architecture [#40074188142](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40074188142). PR #226 merged as `84a707c354065d6f1c9aa0f418cf043f6d55bdd3`.


## Verified — truncate block-write failure sweep (2026-10-24)

- Added a bounded write-failure sweep across `vfs_file_truncate` and simulated power-loss remount after each reached fault.
- Recovered content must be either the unchanged original 64-byte payload or a fully truncated empty file; intermediate EOF, altered bytes, or integrity failures are rejected.
- **Verified:** exact PR head `2586aa0403d5c984cb64b962c5bfdd1bf360cfe4` passed Build/boot-test [#40077700893](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40077700893), including ISO QEMU and native BIOS QEMU, and Security architecture [#40077700948](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40077700948). PR #228 merged as `e6c391ae009b94dd8645140cfe86a571167ff54f`.



## Verification update — home-block flush failure recovery (2026-10-25)

- PR #239 merged into `main` as `1b87449daf9555d15723dea284d3725b87e354d0` after exact-head Build/boot-test [#40156447788](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447788) and Security architecture [#40156447759](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447759) both passed.
- FS-18 now covers failure of transaction flush #4 after durable commit publication and during home-block persistence. The current mount must report zero written bytes and become read-only; simulated power-loss remount must replay the committed payload completely and pass integrity validation.
- Remaining work includes systematic coverage of other journal interruption points, broader malformed journal metadata, concurrency/object lifetime verification, and physical hardware durability testing. The current pass is mock-device/QEMU evidence, not hardware certification.


## Verification scope update — journal recovery (2026-10-25)

- FS-26 ambiguous pre-commit journal-images flush passed exact PR #250 head `32541b0d67e9a077b10bf8896a0881bf862c01cd`: Build/boot-test [#40168090685](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40168090685) and Security architecture [#40168090633](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40168090633). Merged as `4cb57fc7e80b4a9cca6ecfb0a5cdf7b668c60055`.
- FS-27 adds malformed-but-CRC-valid duplicate journal target coverage. The regression now simulates power loss (not normal unmount) and recomputes both header and commit checksums, so acceptance depends on structural duplicate-target rejection. The duplicate-target regression passed exact PR #271 head CI: Build/boot-test [#40674196387](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196387) and Security architecture [#40674196316](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196316); merged as `965e70902c725caae280b6403f851c93ed8d7059`.
- Neither test establishes physical-drive power-loss durability; concurrency/object-lifetime guarantees also remain unverified as a general VFS property.

## Release roadmap matrix — 1.0 and 1.1 (2026-10-25)

**Version note:** Product releases 1.0/1.1 are distinct from the existing v1 on-disk format.

| Capability | 1.0 target | 1.1 target | Current documentation assessment |
|---|---|---|---|
| Persistent file I/O, allocation, extents, directories | Release-critical correctness and tests | Maintain compatibility | Implemented in core; exact-head release verification required |
| Journal/recovery | Complete failure-point and retry coverage | Maintain recovery guarantees | Core implemented; broader sweeps remain open |
| Metadata CRC32C and superblock redundancy | Validate corruption/degraded modes | Maintain compatibility | Implemented; boundary coverage continues |
| File-data end-to-end checksums | Not required for 1.0; document limitation | Design and implement with corruption tests | Not established by metadata CRCs; planned |
| Read-only integrity scan | Validate scan correctness and serialization requirements | Improve diagnostics | Implemented API; caller must serialize against mutations |
| Hard links / symlinks / xattrs | Not required | Specify and implement selected features | Full feature plan/verification not established here |
| Sparse files | Not required | Define semantics and test; implementation gated by design | No complete implementation contract identified |
| Permissions / ACL | Preserve VFS security boundary | Specify enforcement; ACL conditional | UID/GID/mode metadata exists; ACL is future work |
| Formatter / host tool | Release-critical target safety and round-trip validation | Expand malformed-image tests | Completion must be checked against current exact-head evidence |
| Performance benchmarks | Establish correctness-first baseline if practical | Reproducible benchmark suite | Planned |
| Copy-on-Write / snapshots | Deferred | Deferred unless separately approved | Future roadmap |
| Compression / encryption | Deferred | Deferred unless separately approved | Future roadmap |
| Self-healing / automatic repair | Detection and conservative recovery only; no silent destructive repair | Explicitly scoped, logged, verified recovery design | Design requirements exist; complete behavior not claimed |
| Foreign FAT32/exFAT/NTFS support | Keep provider boundary separate | Separate providers and compatibility tests | Externalized by design; not native ProsenjitFS format |

### Evidence rules

- Every Verified item must cite the exact source commit and passing Build/boot-test plus Security architecture runs where required.
- Old passing CI runs do not verify newer source changes.
- QEMU and deterministic mock-device tests are not physical power-loss certification.
- A metadata checksum is not a file-data checksum; detection is not repair; a planned design is not an implementation.

## Release-specification cross-reference — 1.0 / 1.1

The authoritative detailed scope, required tests, acceptance gates, non-goals and release decision checklist are in [FILESYSTEM.md](FILESYSTEM.md). Keep each capability's current status evidence-based: planned is not implemented, implemented is not necessarily tested, and tested on an earlier commit is not verified on the release candidate. Version 1.1 feature status must be updated only after the code and exact-head CI evidence are available.

## Mandatory security acceptance update — permissions and encryption

- **Permissions / UID-GID-mode enforcement:** mandatory Version 1.0 release blocker; implementation status must be confirmed against current VFS source and tests. Metadata fields alone do not qualify.
- **Basic encryption:** desired but conditional on reviewed cryptography, key management, authenticated I/O, recovery integration and exact-head CI. Otherwise keep it planned/experimental and defer shipment.
- Acceptance details and tests: [PROSENJITFS-PERMISSIONS-AND-ENCRYPTION.md](PROSENJITFS-PERMISSIONS-AND-ENCRYPTION.md).



---

# Historical appendix: PROSENJITFS-1.0-1.1-RELEASE-SPEC.md

> Preserved from the former separate filesystem document during consolidation. Historical status and CI references apply only to the revisions explicitly named there; use the current summary and exact-head verification for present release decisions.

# ProsenjitFS Product Release Specification: 1.0 and 1.1

**Status:** Release planning and acceptance contract; this document does not claim that planned features are already implemented.  
**Last updated:** 2026-10-25  
**Scope:** Native ProsenjitFS in ProsenjitOS Runner.

## 1. Versioning rules

Product releases **ProsenjitFS 1.0 / 1.1** are separate from **on-disk format version 1**. Do not change the disk-format version merely because the product release changes. Any incompatible on-disk change requires a separately reviewed format-version and migration/compatibility plan.

Use these status labels consistently:
- **Planned:** specified but not implemented.
- **In progress:** code exists but the acceptance criteria are not all met.
- **Implemented:** code is present; tests/evidence may still be incomplete.
- **Tested:** named tests passed on a named source revision.
- **Verified:** the exact source revision and required CI gates passed; limitations are documented.
- **Deferred:** explicitly not part of the release.

A feature must not be called complete because it appears in a roadmap, has a reserved field, or passes only a narrow unit test.

## 2. Release strategy at a glance

| Area | Version 1.0 — first dependable release | Version 1.1 — reliability and feature expansion |
|---|---|---|
| Existing v1 format | Freeze and validate layout; no silent incompatible changes | Preserve compatibility; add only explicitly versioned compatible extensions |
| Core file operations | Format, mount, create/open/read/write/truncate, directories, unlink, rename | Preserve behavior; add selected namespace features after contract and tests |
| Allocation/extents | Persistent bitmap allocation, bounded extent handling, safe reclamation | Sparse-file semantics and improved allocation policy, if approved and implemented |
| Journal | Ordered redo transaction, committed replay, torn transaction handling, fail-closed error handling | Broader recovery diagnostics and exhaustive fault matrix; no weakened durability contract |
| Integrity | Metadata CRC32C, redundant superblock handling, read-only consistency scan | File-data checksum design/implementation and corruption-detection tests |
| Security | VFS remains authoritative; malformed metadata fails closed | Explicit permission semantics; ACL only if kernel authorization support is ready |
| Tooling | Safe formatter, round-trip tests, image inspection/test fixtures | Better diagnostics, malformed-image corpus, reproducible benchmarks |
| Advanced storage features | Deferred | Individual feature gates; no automatic promise of CoW/snapshots/compression/encryption |

## 3. Version 1.0 — release contract

### 3.1 Required capabilities

- [ ] Stable format definitions and explicit bounds/endianness/CRC rules; reject unsupported format versions and unknown feature bits.
- [ ] Formatter that validates device size, block size, region arithmetic, non-overlap and minimum capacity before writing; refuses unsafe targets according to the tool's explicit policy.
- [ ] Mount/probe validation of both superblocks, region boundaries, bitmap dimensions, inode-table limits, root inode, journal structure, extents and referenced metadata.
- [ ] Correct persistent inode/block allocation and reclamation across unmount/remount; reserved and tail bitmap bits validated.
- [ ] File create/open/read/write/truncate with correct byte counts, EOF, zero-length behavior, offset/size overflow checks, disk-full behavior and error propagation.
- [ ] Directory lookup/enumeration/create/mkdir/rmdir; unlink/rename including cross-directory moves; reject cycles, invalid names, duplicate names and non-empty directory removal.
- [ ] Explicit UTF-8 byte-sequence naming policy: case-sensitive, no normalization in format v1, reject invalid UTF-8, NUL and separators.
- [ ] Journal ordering and recovery: replay only structurally valid committed transactions; discard torn/uncommitted transactions safely; fail closed on ambiguous/corrupt records; do not continue writable after uncertain commit state.
- [ ] Flush/barrier contract accurately represented by the block-device API. A device with no durable flush primitive must not be described as power-loss durable.
- [ ] Metadata CRC32C and single-superblock degraded/read-only behavior; conflicting valid superblocks rejected.
- [ ] Read-only integrity scan verifies reachable namespace, parent relationships, allocation agreement and extent ownership without repairing or mutating the volume.
- [ ] VFS permission/capability boundary preserved; stale inode handles rejected; append and truncate semantics retain their documented contracts.
- [ ] Provider boundary maintained: FAT32/exFAT/NTFS support is not represented as native ProsenjitFS functionality.
- [ ] Exact-head build, unit/self-tests, ISO QEMU boot, native BIOS/ATA/MBR/ProsenjitFS persistence test, and Security architecture workflow pass.
- [ ] Release notes list known limitations, on-disk format compatibility, recovery behavior, and test coverage.

### 3.2 Required 1.0 test matrix

- [ ] Format/mount/remount on minimum, typical and boundary device sizes; invalid block sizes and arithmetic overflow.
- [ ] Primary/backup superblock independently corrupt, both corrupt, one valid/one invalid, and both valid but disagreeing.
- [ ] Bitmap, inode, directory-entry, extent and journal corruption, including checksum-valid but semantically invalid structures.
- [ ] File sizes and offsets at zero, block boundaries, extent boundaries, maximum supported values and overflow edges.
- [ ] Disk full, allocation failure, short/failed reads and writes, flush failure at each transaction phase.
- [ ] Crash/recovery before commit, after commit, during home-block writes, during journal retirement, and interrupted recovery; remount must produce a valid pre-commit or post-commit state, never a hybrid.
- [ ] Mutation fault coverage for create, write, truncate, mkdir, rmdir, unlink and rename; preserve payload and namespace invariants.
- [ ] Read-only mount must not mutate a dirty journal; recovery requirements are explicit.
- [ ] Integrity scan must not alter durable, volatile or dirty mock-media contents.
- [ ] Re-run all required gates on the exact release candidate commit, not a nearby or earlier commit.

### 3.3 Explicit 1.0 non-goals / limitations

- File-data end-to-end checksums are not implied by metadata CRC32C.
- Automatic destructive repair, orphan reclamation and silent self-healing are not enabled by default.
- General concurrent VFS safety, SMP/timer-preemption safety and filesystem-wide scan/mutation serialization are not considered proven without a reviewed locking contract and tests.
- QEMU/mock-device crash injection is not physical hardware power-loss certification.
- ACLs, xattrs, hard links, symlinks, sparse files, snapshots, CoW, compression and encryption are not 1.0 promises unless their code and acceptance tests are separately completed and explicitly approved.

## 4. Version 1.1 — detailed feature backlog

**Release rule:** 1.1 is not “all features implemented” until each selected feature below has an owner, design, code, tests and exact-head evidence. The following is the complete proposed 1.1 planning backlog, not a statement of current implementation.

### 4.1 Data integrity — priority P0

- [ ] Specify file-data checksum granularity (per block or per extent), checksum storage, update atomicity, checksum-tree/metadata overhead and format-compatibility strategy.
- [ ] Decide how checksums are stored without silently changing format-v1 interpretation; if an incompatible layout is needed, define a new on-disk feature/version and mount compatibility behavior first.
- [ ] Detect silent data corruption on reads and during an explicit scrub/verification operation.
- [ ] Test partial writes, overwrite, truncate, extent split/merge, preallocation, remount, checksum metadata corruption and injected data-bit flips.
- [ ] Return a clear integrity error; never return corrupted bytes as verified data.
- [ ] Document that CRC32C detects accidental corruption and is not cryptographic authentication.

### 4.2 Recovery and diagnostics — priority P0

- [ ] Build a documented fault-injection table for every journal write/flush/read phase and every metadata mutation family.
- [ ] Improve recovery reports: failure phase, transaction ID, reason, read-only/degraded state and whether a retry/remount may help.
- [ ] Add bounded diagnostics for invalid journal records, duplicate targets, stale sequence numbers, malformed lengths and arithmetic overflow.
- [ ] Ensure recovery is idempotent and interrupted recovery can safely restart.
- [ ] Keep repair separate from detection; every repair action must be explicit, logged and followed by a full integrity check.
- [ ] Add tests for no mutation on failed read-only checks and for preservation of ambiguous user data.

### 4.3 Namespace extensions — priority P1, individually gated

- [ ] **Hard links:** define link-count invariants, same-volume restriction, unlink-last-link semantics, directory prohibition and crash-atomic updates. Implement only after inode reference accounting and integrity scan support are designed.
- [ ] **Symbolic links:** define stored target encoding/length, readlink API, traversal limit, relative-path resolution, loops, dangling targets and authorization behavior. The VFS must explicitly support symlink operations before ProsenjitFS exposes them.
- [ ] **Extended attributes (xattrs):** define namespace, size limits, storage allocation, atomic update behavior, malformed-record handling and permission policy. Reserved inode fields alone do not count as xattr support.
- [ ] Add adversarial tests for cycles, duplicate references, stale inode generations, overlong names/targets and crash points.

### 4.4 Sparse files and allocation — priority P1

- [ ] Define sparse-file semantics: holes read as zero, logical size versus allocated bytes, hole punching policy, allocation accounting and interaction with truncate/preallocation.
- [ ] Define whether sparse extents use explicit hole records or absence of extents; validate compatibility with the existing extent reader.
- [ ] Implement seek/data operations only if the VFS API supports them; do not advertise SEEK_HOLE/SEEK_DATA without API support.
- [ ] Test sparse writes across block boundaries, remount, truncate, disk-full, checksums and integrity scan.
- [ ] Revisit allocation heuristics and fragmentation measurements without sacrificing bounded execution or journal limits.

### 4.5 Permissions and security — priority P1

- [ ] Document the precise meaning of uid, gid and mode in the existing VFS permission model.
- [ ] Confirm that kernel authorization, capabilities and mount policy remain authoritative; on-disk metadata cannot grant itself privilege.
- [ ] Add permission-denied tests for read/write/create/unlink/rename and verify output parameters are initialized on failure.
- [ ] ACL support is conditional: first specify inheritance, precedence, ownership, default ACLs, serialization, malformed ACL handling and integration with kernel authorization.
- [ ] If kernel ACL enforcement is not ready, explicitly defer ACL implementation rather than storing unenforced security metadata.

### 4.6 Formatter, inspection and operations — priority P1

- [ ] Improve host-side formatter and image-inspection utility design; ensure shared format definitions avoid host/kernel drift.
- [ ] Validate target type, capacity, alignment and destructive-operation intent before formatting.
- [ ] Add reproducible format → mount → write → unmount → remount → verify round-trip tests.
- [ ] Add a read-only image inspector for superblock copies, regions, journal status, feature flags and checksums.
- [ ] Provide structured error codes and human-readable diagnostics without leaking unsafe or misleading recovery claims.

### 4.7 Performance and observability — priority P2

- [ ] Establish reproducible baselines for sequential/random reads and writes, create/lookup/readdir, large directory behavior, truncate and mount/recovery time.
- [ ] Record compiler/build options, emulator/device configuration, image size and workload so results are comparable.
- [ ] Add tests for directory hash collision chains, long chains, bounded lookup and adversarial names.
- [ ] Optimize only after correctness baselines; every optimization must preserve error handling and transaction bounds.

### 4.8 Self-healing — priority P2, conservative by default

- [ ] Implement detection and reporting before any automatic repair.
- [ ] Separate safe journal replay from repair of inconsistent namespace/allocation metadata.
- [ ] Keep destructive repair disabled by default and require explicit recovery-mode authorization.
- [ ] Use bounded repair transactions, logs, post-repair scans and read-only fallback on uncertainty.
- [ ] Do not automatically reclaim an orphan unless ownership ambiguity and user-data-loss risk have been resolved by a reviewed policy.
- [ ] Test repair interruption and recovery from failed repair; no claim of self-healing until these tests pass.

### 4.9 Features explicitly deferred beyond the 1.1 baseline

These are future research tracks, not guaranteed 1.1 features:
- Copy-on-Write data/metadata and snapshots.
- Compression.
- Encryption/key management.
- Full online resize and online defragmentation.
- Deduplication, replication and multi-device storage pools.
- Full NTFS/ext4/ZFS/Btrfs feature parity.
- Automatic repair that can destroy or rewrite ambiguous user data.

Each can be promoted only by a separate design proposal with threat model, on-disk compatibility plan, resource-cost analysis, failure model and test matrix.

## 5. 1.1 acceptance gates

- [ ] Every selected P0 feature has a design review, implementation, deterministic regression tests and documented failure behavior.
- [ ] Every P1 feature included in the release has an API contract and negative/adversarial tests; otherwise it remains deferred.
- [ ] New metadata structures are bounded, versioned, checksummed where appropriate and rejected safely when malformed.
- [ ] Existing v1 images remain readable unless a separately approved format change explicitly says otherwise.
- [ ] Feature flags accurately describe capabilities; unsupported features are rejected rather than guessed.
- [ ] Build/boot-test and Security architecture CI pass on the exact release candidate commit.
- [ ] Benchmarks, if published, are reproducible and do not substitute for correctness or durability proof.
- [ ] Release notes distinguish implemented behavior from planned, experimental and deferred behavior.

## 6. Release decision checklist

A release manager must answer “yes” to all applicable items:
1. Is the feature implemented in the repository at the release candidate commit?
2. Are success and failure paths tested?
3. Are corrupt/malicious on-disk inputs handled safely?
4. Is crash/recovery behavior specified and tested where persistence changes?
5. Does it preserve VFS security and the provider boundary?
6. Does the exact candidate pass required CI?
7. Are limitations and hardware-validation boundaries stated honestly?

If any answer is “no” or unknown, mark the feature **not release-verified**.

## 7. Current known boundaries to retain in all release notes

- Metadata CRC32C does not provide file-data end-to-end checksums.
- A read-only integrity scan does not repair corruption and requires serialization against mutations.
- Mock-device and QEMU tests do not certify physical-drive power-loss behavior.
- General concurrent VFS safety is not proven merely by append serialization or a passing single-threaded test.
- Passing historical CI does not verify later source changes; evidence must match the exact commit.

## Mandatory security release gate — permissions and encryption

The security contract in [PROSENJITFS-PERMISSIONS-AND-ENCRYPTION.md](PROSENJITFS-PERMISSIONS-AND-ENCRYPTION.md) is normative. **Permission enforcement is a Version 1.0 release blocker.** UID/GID/mode fields alone do not count: the trusted VFS/security layer must enforce permissions for all relevant operations and the negative tests must prove denied operations do not mutate disk state. Basic encryption is a goal, but it must not be advertised as shipped until crypto design, key management, authenticated integrity, boot/recovery integration and exact-head tests pass. If those gates are not met, ship 1.0 with permissions and clearly defer encryption; never use home-grown cryptography.


---

# Historical appendix: PROSENJITFS-TEST-MATRIX.md

> Preserved from the former separate filesystem document during consolidation. Historical status and CI references apply only to the revisions explicitly named there; use the current summary and exact-head verification for present release decisions.

## FS-23 — CRC-valid inode semantic admission (**PASS** on exact source head `70ed44cd45987682ac50f9fbfd1b82a510e25033`)

- Seven malformed but checksummed variants: unknown type, unknown flag, zero generation, invalid parent, `UINT64_MAX` size, unaligned allocation bytes, and allocation beyond capacity.
- Reads return `PROSENJITFS_ECORRUPT`; the valid root is restored after each case; integrity scan passes.

## FS-24 — Directory-index owner identity (**PASS** on exact source head `70ed44cd45987682ac50f9fbfd1b82a510e25033`)

- The root index is changed to another in-range owner while recalculating its CRC; the reader returns `PROSENJITFS_ECORRUPT`; the original index is restored through the journal transaction interface; integrity scan passes.

## FS-25 — Namespace child-reference validation (**PASS** on exact source head `70ed44cd45987682ac50f9fbfd1b82a510e25033`)

- A CRC-valid directory-entry type mismatch is rejected by both `vfs_path_lookup()` and directory enumeration with `PROS_VFS_ECORRUPT`; a wrong but in-range child parent is rejected by lookup.
- Original entry/inode records are restored and the integrity scan passes.

Exact-head evidence for FS-23/24/25: [Build/boot-test #8548](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283756) and [Security architecture #8084](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40165283791). v1 disk layout unchanged. Physical-device durability and general concurrent mutation/scan behavior remain unverified.
## Home-block flush failure regression — exact-head CI passed (2026-10-25)

- [x] Added a mock-device test that fails flush #4, after the commit record has been flushed and while home-block writes are being persisted.
- [x] The test requires the failed write to report zero bytes and freeze the current mount read-only; after simulated power loss, remount must replay the committed transaction to the complete new payload and pass integrity validation.
- [x] Exact PR #239 head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf` passed [Build/boot-test #40156447788](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447788) and [Security architecture #40156447759](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447759); PR #239 merged as `1b87449daf9555d15723dea284d3725b87e354d0`.
- Scope: deterministic mock block device and QEMU CI only; no physical-device durability certification.

| ID | Area / risk | Existing test surface | Required acceptance | Status |
|---|---|---|---|---|
| FS-18 | Home-block flush failure after durable commit | `kernel/prosenjitfs.c`, native ProsenjitFS self-test | Fail fourth transaction flush; report zero bytes and freeze mount; simulated power-loss remount replays complete new payload; integrity scan passes | **PASS** on exact PR #239 head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf`; Build #40156447788 and Security #40156447759 passed; merged as `1b87449daf9555d15723dea284d3725b87e354d0`. Mock/QEMU only |
| FS-19 | Allocator geometry, exhaustion and boundary bits | `kernel/prosenjitfs.c`, ProsenjitFS self-test | Reject block sizes 0/511/513/4097/8192 before division/indexing; preserve output sentinels/media; return ENOSPC on exhausted bitmaps and find final valid bit | **PASS** on PR #242 exact head `96418d7bf512ee49e42eb325d0864738781891ab`; Build #40160091435 and Security #40160091518 passed. Merged as `2a979bed55cc9783859ecd4c3278c46d2abb885d`. |
| FS-20 | Allocator bitmap read-error propagation and cleanup | `kernel/prosenjitfs.c`, `prosenjitfs_selftest()` | Inject a bitmap read error into the inode finder/public allocator and each data-bitmap finder/query; require `EIO`, untouched output sentinels/media state, writable mount, and successful integrity scan | **PASS** on exact PR #242 head `96418d7bf512ee49e42eb325d0864738781891ab`; Build #40160091435 and Security #40160091518 passed; merged as `2a979bed55cc9783859ecd4c3278c46d2abb885d`. |
| FS-21 | Total path bounds and VFS path resolution | `kernel/vfs.c`, `vfs_vnode_api_selftest()` | Reject path length `>= PROS_VFS_PATH_MAX`; handle repeated separators and `.`/`..`, clamp root `..`, return ENOENT/ENOTDIR for invalid traversal, preserve result pointer on errors | **PASS** on exact PR #242 head `96418d7bf512ee49e42eb325d0864738781891ab`; Build #40160091435 and Security #40160091518 passed; merged as `2a979bed55cc9783859ecd4c3278c46d2abb885d`. |
| FS-23 | CRC-valid inode semantic admission | `kernel/prosenjitfs.c`, `prosenjitfs_selftest()` | Reject unknown type/flags, zero generation, out-of-range parent, over-capacity size, unaligned allocation accounting and over-capacity allocated bytes; restore root and pass integrity scan | **PASS** on source head `70ed44cd45987682ac50f9fbfd1b82a510e25033`; Build #8548 and Security #8084 passed. |
| FS-24 | Directory-index owner identity | `kernel/prosenjitfs.c`, `prosenjitfs_selftest()` | Reject checksum-valid index whose embedded owner differs from caller's directory inode; restore original index and pass integrity scan | **PASS** on source head `70ed44cd45987682ac50f9fbfd1b82a510e25033`; Build #8548 and Security #8084 passed. |
| FS-25 | Namespace child-reference validation | `kernel/prosenjitfs.c`, `pfs_vnode_lookup()`, `pfs_vnode_readdir()` | Reject dangling or mismatched type/parent child references in lookup and enumeration; restore corrupted metadata and pass integrity scan | **PASS** on source head `70ed44cd45987682ac50f9fbfd1b82a510e25033`; Build #8548 and Security #8084 passed. |
- [x] **FS-26 | Ambiguous pre-commit journal-images flush** | `kernel/prosenjitfs.c`, native ProsenjitFS self-test | Flush #2 persists journal header/images but returns `EIO`; write reports zero bytes, current mount stays writable, simulated crash/remount exposes the complete old payload and integrity scan passes | **PASS** on exact PR #250 head `32541b0d67e9a077b10bf8896a0881bf862c01cd`; [Build/boot-test #40168090685](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40168090685) and [Security architecture #40168090633](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40168090633) succeeded; merged as `4cb57fc7e80b4a9cca6ecfb0a5cdf7b668c60055`. Deterministic mock-device/QEMU only; no physical-device durability claim.

## Failed build follow-up — 2026-10-25

- Build/boot-test #40159887034 **failed during ISO compilation**: `kernel/prosenjitfs.c:10624:18: error: 'found_lba' undeclared`; boot-test steps were skipped. Security architecture #40159887021 passed on that pre-fix head, but does not verify the corrected source or later integration changes.
- Cause: the rmdir recovery-cleanup hunk was accidentally matched to the earlier unlink-read sweep because both had an identical integrity/unmount condition. The source patch now restores that unlink check and limits directory-removal cleanup to the actual rmdir write-sweep scope.
- Historical compile failure was resolved by the integration correction; PR #242 exact-head Build #40160091435 and Security #40160091518 passed. The later Part 2 hardening is separately verified above.

## FS-28 — Truncate-shrink tail isolation (**PASS** on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- **Risk:** bytes beyond a non-aligned EOF remained in the retained physical block after shrink; a later write beyond EOF could expose the old tail.
- **Test:** truncate `/hello.bin` from 700 to 100 bytes, seek to 200, write `0xE7`, then read bytes 100–200 and require 100 zero bytes followed by the marker.
- **Implementation:** clear the retained block suffix in the same redo transaction as the inode size and related extent/bitmap updates. Reject target collisions; reset staged transactions on failure.
- **Status:** regression passed in the self-test marker observed in ISO and native BIOS QEMU runs; [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332) passed on the code/test head.

## FS-29 — Inode allocator failure preserves caller output (**PASS** on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- **Risk:** `prosenjitfs_alloc_inode()` assigned the candidate before its inode-bitmap transaction could fail.
- **Test:** fail the first journal write during direct inode allocation; require `PROSENJITFS_EIO`, unchanged `UINT64_MAX` output sentinel, unchanged durable/volatile/dirty mock-media CRCs, writable mount state and a passing integrity check.
- **Implementation:** publish the candidate only after `pfs_inode_bitmap_update()` succeeds.
- **Status:** source and regression passed on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`; [Build/boot-test #8584](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895331) and [Security architecture #8120](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40172895332) passed.

## FS-30 — Journal-retirement error classification (**PASS** on code/test head `5aa0569a24093b11aee7685c7f3f136f02aaaded`)

- **Risk:** `pfs_journal_clear()` used undocumented `-101..-106` codes, obscuring actual write/flush EIO during mount recovery.
- **Test:** interrupt a write at stage 4 (durable header/images, no matching commit), inject journal-retirement write failure and flush failure separately, require mount EIO, retry mount, verify old bytes and a clean integrity scan.
- **Implementation:** propagate `pfs_lba_add`, block-write and flush status directly; map invalid state to named filesystem errors.
- **Status:** the injected retirement write and flush failures returned EIO; retry recovery preserved the old payload and integrity passed. [Build/boot-test #8590](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503632) and [Security architecture #8126](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173503601) passed.

## FS-31 — Committed-journal replay write failure (**PASS** on code/test head `73bff7fe8aad278e04f08ee7594b901bb11da6f9`)

- **Risk:** home-block I/O can fail partway through committed-journal replay; the committed journal must remain available for a retry.
- **Test:** interrupt unlink at stage 5 after commit publication, fail the second replay write, simulate power loss, retry mount, require the unlink fully applied and integrity clean.
- **Status:** the second replay home-write failed with EIO; simulated power loss discarded the partial volatile replay; retry mount completed the committed unlink and integrity passed. [Build/boot-test #8593](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866752) and [Security architecture #8129](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40173866715) passed.

# ProsenjitFS Test Matrix — 2026-10-24

## Latest verified execution — 2026-10-25

- PR #235 merged as `d82d1010f3c58696c15dfb09c326ba2b6d612e43`.
- Exact PR-head `89a35608bcd278b01977f5abded40f2208208d9e` passed [Build/boot-test run #40119299299](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119299299) and [Security architecture run #40119299311](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40119299311).
- The passing Build workflow completed desktop unit tests, ISO build, ISO QEMU boot-test, native BIOS QEMU boot-test, and the ProsenjitFS diagnostic step.
- These results validate CI/QEMU/mock-device coverage only; they do not certify physical-device power-loss durability or general concurrent VFS safety.

## Journal failure-path follow-up — verified

- [x] Added a targeted regression for failure of transaction flush #2 (journal header/images flush): the write fails with zero bytes reported, the mount remains writable, retry succeeds, and readback plus integrity scan match the retry payload.
- [x] Exact PR-head Build/boot-test and Security architecture CI passed (links above).
- Scope is the mock block device only; this is not physical-device durability certification.

| ID | Area / risk | Existing test surface | Required acceptance | Status |
|---|---|---|---|---|
| FS-17 | Journal-images flush failure and retry | `kernel/prosenjitfs.c`, native ProsenjitFS self-test | Second transaction flush fails before commit publication; failed write reports zero bytes, mount remains writable, subsequent write/readback succeeds, integrity scan passes | **PASS** on exact PR #235 head `89a35608bcd278b01977f5abded40f2208208d9e`; Build/boot-test #40119299299 and Security #40119299311 passed |

## Part 2 follow-up — VFS read output-count contract (2026-10-24; verified 2026-10-25)

- [x] Audited `vfs_file_read()` and fixed stale `bytes_read` on rejected reads by clearing the output count before argument/permission checks and dispatch.
- [x] Added permission-denied and filesystem-`EIO` read regressions using a nonzero sentinel.
- [x] Exact PR-head Build/boot-test #40119299299 and Security architecture #40119299311 passed on `89a35608bcd278b01977f5abded40f2208208d9e`.
- Scope: VFS output contract only; no on-disk format change, no crash-atomicity or hardware-durability claim.

| ID | Area / risk | Existing test surface | Required acceptance | Status |
|---|---|---|---|---|
| FS-16 | VFS read error output count | `kernel/vfs.c`, `vfs_vnode_api_selftest()` | Permission-denied and filesystem-`EIO` reads with a nonzero sentinel return the expected error and leave `bytes_read=0` | **PASS** on exact PR #235 head `89a35608bcd278b01977f5abded40f2208208d9e`; Build/boot-test #40119299299 and Security #40119299311 passed |


## Part 2 regression added — pending exact-head CI

| ID | Area / risk | Existing test surface | Required acceptance | Status |
|---|---|---|---|---|
| FS-15 | VFS write error output count | `kernel/vfs.c`, `vfs_vnode_api_selftest()` | A filesystem write callback returning `PROS_VFS_EIO` must return the error and set `bytes_written=0`, even when caller input was a nonzero sentinel | **PASS** on exact PR #235 head `89a35608bcd278b01977f5abded40f2208208d9e`; Build/boot-test #40119299299 and Security #40119299311 passed |


**Baseline commit:** `cfe04a8520ec917a90b284bbc726a8bd0cc83a2d`  
**Status source:** [Completion matrix](FILESYSTEM.md)  
**Work plan:** [Implementation plan](FILESYSTEM.md)

The matrix separates implementation, current-baseline runtime evidence, and unmerged test-branch evidence. A test is green only when the exact tested SHA and result are recorded.

| ID | Area / risk | Existing test surface | Required acceptance | Status at audit |
|---|---|---|---|---|
| CI-01 | Toolchain and strict C build | `.github/workflows/build.yml`, `scripts/build.sh`; GCC `-Wall -Wextra -Werror -O2` | Exact SHA builds ISO and native BIOS image; all required tools are present | **PASS** on main `cfe04a8`, run #8426 |
| CI-02 | ISO boot and broad in-kernel regressions | Main workflow serial milestones, ISO QEMU | QEMU may time out only after required success markers; no missing filesystem/other subsystem milestone | **PASS** on main `cfe04a8`, run #8426 |
| CI-03 | Native BIOS storage path | Native BIOS image with IDE/ATA PIO, MBR/partition discovery, native QEMU | Boot reaches partition/storage discovery and ProsenjitFS self-test success marker | **PASS** on main `cfe04a8`, run #8426 |
| CI-04 | Security architecture contract | `.github/workflows/security-design.yml` | Exact SHA passes architecture assertions | **PASS** on main `cfe04a8`, run #7962 |
| FS-01 | Format / mount / probe | `prosenjitfs_format`, `prosenjitfs_probe`, `prosenjitfs_mount`; mock block device | Valid image mounts; unsupported/malformed images fail safely; no out-of-range I/O | Baseline boot self-test pass; expand malformed-input boundaries |
| FS-02 | Redundant superblocks / region geometry | Superblock validation, CRC32C, region bounds | Each copy independently validated; conflicting valid copies rejected; single valid copy marked degraded/read-only; overflow/overlap rejected | Existing documented self-tests; maintain exact-head CI evidence |
| FS-03 | Block and inode allocation | Persistent bitmaps; allocation/free/preallocation APIs | Re-mount preserves allocation; tail bits and metadata-reserved bits rejected; repeated allocate/free does not leak or double-own blocks | Implemented; continue boundary/exhaustion coverage |
| FS-04 | Extents and file-data mapping | Extent chain reader, read/write/truncate/preallocate code | Reject loops/overlaps/out-of-range mappings; read/write binary bytes exactly; sparse gaps return zeros; truncation reclaims only valid blocks | Implemented; expand malformed chain and I/O-fault coverage |
| FS-05 | Directory and UTF-8 names | 32-bucket index, linked entries, VFS lookup/readdir/create/mkdir/rmdir/unlink/rename | Reject corrupt chains, bad hash/bucket, duplicate names or references, invalid UTF-8, root escape and directory cycles | Core tests exist; not all mutation failure paths merged/verified |
| FS-06 | Journal ordering and crash recovery | Redo journal, optional block flush, mock durable/volatile/dirty layers | Pre-commit crash yields old state; committed transaction replays to complete new state; re-replay is idempotent; uncertain commit keeps mount read-only | Deterministic mock-device coverage exists; no physical-device certification |
| FS-07 | Append read-failure sweep | PR #233, `kernel/prosenjitfs.c:10465` | Every reached read fault returns EIO, media CRCs unchanged, writable mount, original payload intact, clean integrity scan | **PASS on exact PR head `84d2b84948065011cf0c94291e8bcaf5d9f7079c`**: [Build/boot-test #40116501666](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40116501666) and [Security #40116501659](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40116501659) passed; full ProsenjitFS self-test marker observed. Fix keeps `pfs_make_vnode()` filesystem errors in the correct namespace and maps them at VFS callbacks. PR #233 merged into main as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`. |
| FS-08 | Truncate read-failure sweep | PR #233, truncate sweep and writable-open path | Same invariants as FS-07; stop at first unreached countdown and disarm it before verification | **PASS as part of the full exact-head self-test** on `84d2b84948065011cf0c94291e8bcaf5d9f7079c`; exact-head Build/boot-test and Security checks passed. Individual truncate test rows do not have separate serial markers; this status is based on the enclosing test suite reaching its final success marker. PR #233 merged into main as `9c23331d6afdb090f2fd7d7d22e766c04ccbad6c`. |
| FS-09 | Create/overwrite write-failure sweep | PR #230, `kernel/prosenjitfs.c` | Reached failures propagate correctly; old file content and durable/volatile/dirty media remain stable; integrity passes | **PASS** on exact PR #230 head `22da8502bce69a3c69c6ddf222eb643ffa89aab0`; Build #40088773369 and Security #40088773288 passed; merged into main as `3df77093edec9a6961a507fe05f4c32420d73391` |
| FS-10 | Rmdir write-failure sweep | `kernel/prosenjitfs.c`, native ProsenjitFS self-test | Sweep block-write failures through `vfs_rmdir`; after simulated crash, the directory is either valid/present or fully removed; integrity passes | **PASS** through PR #230 exact-head Build #40088773369 and Security #40088773288. Equivalent sweep is already in `main`; open PR #232 duplicates this test and is not needed to add coverage. |
| FS-11 | Other read/write/flush failure paths | Mock-device fault injection in ProsenjitFS self-test | Sweep each reachable superblock/journal/bitmap/inode/extent/directory read/write/flush; no stale injection contaminates verification | **PARTIAL**; inventory and add targeted cases in implementation plan |
| FS-12 | Integrity scan read-only guarantee | `prosenjitfs_check_integrity`, report API | Check ownership/reachability/checksums/bounds; report orphan candidates without mutating media; distinguish EIO from malformed on-disk data | Implemented API; caller serialization and full adversarial coverage remain open |
| FS-13 | Concurrency and object lifetime | VFS vnode refs, file/mount state, `io_lock` | Explicit lock contract and interleaving tests cover mutation vs scan and rename/unlink vs open file lifetime | **NOT VERIFIED** as general thread-safe VFS behavior |
| FS-14 | Actual hardware durability | ATA PIO and hardware-backed device | Power-cut testing on target hardware with declared cache/flush assumptions | **NOT TESTED**; QEMU/mock-device results are not hardware certification |

| FS-18 | Home-block flush failure after durable commit | PR #239, native ProsenjitFS self-test | Fail flush #4 after durable commit; write reports zero bytes and current mount becomes read-only; simulated power-loss remount replays complete new payload and integrity passes | **PASS on exact PR head `7faba8cb996e3f5ae8b329af9f44bf0a24f597cf`**: [Build/boot-test #40156447788](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447788) and [Security #40156447759](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40156447759) passed; PR #239 merged as `1b87449daf9555d15723dea284d3725b87e354d0`. Mock-device/QEMU only. |

## Reproducible commands on a provisioned Linux self-hosted runner

Run these in a disposable checkout/workspace, not against a real block device:

```sh
set -euo pipefail
make desktop-unit-test
./scripts/build.sh

timeout 20s qemu-system-x86_64 \
  -cdrom build/prosenjitOS.iso \
  -display none \
  -serial file:build/qemu-serial.log \
  -no-reboot -no-shutdown \
  -d int -D build/qemu-debug.log || test "$?" -eq 124

timeout 20s qemu-system-x86_64 \
  -drive format=raw,file=build/prosenjit-native-bios.img,if=ide \
  -display none \
  -serial file:build/native-qemu-serial.log \
  -no-reboot -no-shutdown \
  -d int -D build/native-qemu-debug.log || test "$?" -eq 124
```

A return code of 124 is acceptable only if all mandatory serial milestones from `.github/workflows/build.yml` appear in the relevant log. Do not treat the command's timeout alone as a pass. Prefer the workflow itself as the canonical gate because it also validates all required markers.

## Safety rules for tests

- Use the mock block device and disposable generated image for formatter, corruption and crash tests.
- Never point a formatter or write test at a real disk, partition, or existing filesystem.
- Fault injection must be scoped to the target operation and disabled before readback, integrity scans, remount checks or cleanup.
- A passing mock-device/QEMU test does not establish physical-device power-loss durability or concurrent VFS safety.


- [ ] **FS-27 | CRC-valid journal header with duplicate targets** | `kernel/prosenjitfs.c`, native ProsenjitFS self-test | Create a durable committed transaction stopped before home-block replay, duplicate two journal target LBAs, recompute both header and commit checksums, and require mount recovery to return `PROSENJITFS_ECORRUPT` before replay | **PASS** on exact PR #271 head `88516324a1007753aa43b213bd17bb0c5ea2700b`; Build/boot-test [#40674196387](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196387) and Security [#40674196316](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196316) passed; deterministic mock-device only, no hardware durability claim.



---

# Historical appendix: filesystem integration boundary

> Preserved from the former standalone document. Its status claims are historical and must not override the current source audit at the top of this file.

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

---

# Historical appendix: NTFS write status

> Preserved from the former standalone document. The NTFS write claims are not evidence of current support because the current branch has no NTFS implementation source/build target; see the reconciliation audit above.

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


---

# Historical appendix: ProsenjitFS encryption design

> Preserved as design history only. It is not proof of a crypto implementation or a release-ready encryption feature.

# ProsenjitFS Encryption Architecture — Development Plan

Status: Design baseline (no kernel integration yet)

## Goal

Add strong, standards-based encryption to Native ProsenjitFS without modifying the kernel initially. The crypto implementation and filesystem encryption format will be developed and tested as an isolated layer, then integrated with the kernel/VFS only after the design and test suite are stable.

## Security principles

- Do not invent a custom cipher or cryptographic primitive.
- Prefer well-reviewed, standard authenticated-encryption primitives.
- Encryption must provide confidentiality and integrity/authentication.
- Never reuse a nonce with the same encryption key.
- Key material must have an explicit lifecycle and must not be logged.
- Filesystem corruption and intentional tampering must be detectable.
- Encryption must remain compatible with ProsenjitFS transaction/journal/recovery semantics.
- Crypto must not own LBA translation. Storage/LBA remains the responsibility of the filesystem/block-device layers.
- No GitHub-hosted CI dependency is required for development; local Debian/VM testing remains the preferred verification path.

## Proposed cryptographic stack

### Primary data encryption

AES-256-GCM.

Reasons:
- standardized and widely reviewed;
- authenticated encryption (confidentiality + integrity);
- good performance on CPUs with AES acceleration;
- suitable as a block/file/extent encryption primitive when nonce management is designed correctly.

### Alternative backend

ChaCha20-Poly1305 may be implemented as an alternative backend for CPUs without useful AES acceleration.

The filesystem format should define an algorithm identifier so the crypto backend can be selected without redesigning ProsenjitFS.

### Password-based unlock

Argon2id for deriving a key-encryption/unlock key from a user password.

The password itself is never stored in the filesystem.

### Randomness

Use a cryptographically secure random-number abstraction for keys and nonces. The standalone implementation must not silently fall back to weak pseudo-randomness.

## Key hierarchy

Do not encrypt the entire filesystem directly with a password or a single long-lived data key.

Target hierarchy:

    User password
        |
        v
      Argon2id
        |
        v
    Unlock / KEK
        |
        v
   Volume Master Key
        |
        +---- Metadata protection keys
        |
        +---- Data/file/extent keys

The exact key hierarchy and wrapping format must be finalized before on-disk format freeze.

## Filesystem integration boundary

The crypto layer must be independent of LBA translation.

    VFS
      |
    ProsenjitFS
      |
    Crypto interface
      |
    block/storage layer
      |
    volume-relative sector/LBA
      |
    device/ATA

The crypto layer receives logical data/metadata buffers and crypto context; it must not calculate physical disk LBAs.

## Encrypted data model

The design should support authenticated encryption of filesystem data units while preserving crash consistency.

Each encrypted unit should have enough authenticated context to prevent:
- ciphertext substitution;
- replay of an older valid ciphertext;
- cross-file/cross-block substitution;
- metadata/data confusion.

Associated authenticated data (AAD) should bind ciphertext to its filesystem identity/context without unnecessarily exposing sensitive information.

## Metadata protection

Determine separately which metadata must be:
1. encrypted and authenticated;
2. authenticated but left readable for mount/recovery;
3. public structural information.

Do not blindly encrypt metadata required to discover the encrypted volume.

## Journal and crash consistency

Encryption must be transaction-aware.

Required cases:
- interrupted encrypted write;
- torn ciphertext;
- torn authentication tag;
- journal replay;
- abandoned transaction;
- replay of stale ciphertext;
- metadata update followed by crash;
- key metadata update followed by crash.

Recovery must never accept partially committed encrypted state as valid.

## Nonce/IV requirements

Nonce uniqueness is a hard security invariant for AES-GCM.

The implementation must define:
- nonce allocation;
- persistence/recovery behavior;
- interaction with transactions;
- behavior after remount;
- behavior after rollback;
- behavior during block relocation;
- behavior during snapshots/copies if later supported.

No nonce reuse may occur for the same key, including after crash/recovery.

## Key rotation

The format should be designed so that future key rotation is possible without requiring a full filesystem redesign.

At minimum, distinguish:
- volume/master key;
- data encryption keys;
- metadata keys;
- key version/generation.

## Testing plan before kernel integration

Host-side/standalone tests:

1. Known-answer cryptographic tests.
2. Encrypt/decrypt round trips.
3. Randomized input/property tests.
4. Empty, tiny, sector-sized and multi-megabyte inputs.
5. Boundary-size and alignment tests.
6. Wrong-key rejection.
7. Authentication-tag corruption detection.
8. Ciphertext bit-flip detection.
9. AAD modification detection.
10. Replay/stale-ciphertext rejection.
11. Nonce uniqueness tests.
12. Crash-point simulation.
13. Journal/recovery interaction tests.
14. Key rotation tests.
15. Fuzz testing of serialized crypto metadata.
16. Sanitizer/static-analysis builds where available.

## Kernel integration plan

Only after the standalone crypto layer and encrypted-format tests are stable:

1. Add a narrow ProsenjitFS crypto interface.
2. Connect VFS/file I/O to the interface.
3. Connect authenticated metadata handling.
4. Connect journal/recovery paths.
5. Connect the kernel CSPRNG source.
6. Preserve the existing LBA/storage contract.
7. Add kernel-level regression tests.
8. Verify remount/persistence/corruption/crash behavior in the Debian VM/QEMU environment.

## Non-goals

This phase does not:
- replace ProsenjitFS allocation;
- redesign VFS;
- redesign LBA translation;
- modify bootloader/graphics/networking;
- invent a new encryption algorithm;
- require GitHub Actions;
- claim encryption is complete before runtime and corruption/recovery verification.

## Definition of done

Encryption is complete only when:
- the on-disk format is documented and versioned;
- cryptographic primitives are standard and correctly composed;
- nonce/key invariants are enforced;
- authentication failures are handled safely;
- journal/recovery semantics are proven;
- corruption and replay tests pass;
- persistence/remount tests pass;
- kernel integration is source-audited;
- Debian VM/QEMU runtime tests pass;
- no secret material appears in logs or diagnostics.


---

# Historical appendix: permissions and encryption release contract

> Preserved from the former standalone document. Its permission tests remain a release requirement; encryption stays deferred until all design, implementation and verification gates pass.

# ProsenjitFS Release Security Contract — Permissions and Basic Encryption

**Status:** Mandatory release requirement for ProsenjitFS 1.0; encryption is a scoped optional capability unless the key-management and recovery gates below can be completed safely.  
**Last updated:** 2026-10-25  
**Applies to:** Native ProsenjitFS, VFS integration, formatter/mount, tests and release notes.

## 1. Non-negotiable release decision

A filesystem release is not acceptable if it has no enforceable access-control model. **Permission enforcement is a Version 1.0 release blocker, not a later enhancement.** Version 1.0 must not ship with UID/GID/mode fields that are merely stored but ignored by the VFS.

Basic encryption is desirable, but insecure or incomplete encryption is worse than clearly documented non-support. Do not advertise encrypted volumes until key handling, authentication, recovery, and all relevant I/O paths are implemented and tested. If these gates cannot be met safely for 1.0, ship permissions in 1.0 and keep encryption as an explicitly deferred/experimental feature for 1.1.

## 2. Permission model — required for Version 1.0

### 2.1 Ownership and mode semantics

- [ ] Define and document UID, GID and mode-bit meanings for files and directories.
- [ ] Define the effective credentials used by each VFS operation; do not trust caller-supplied credentials without the kernel's authorization boundary.
- [ ] Define owner/group/other read, write and execute/search behavior, including directory search versus listing permissions.
- [ ] Define ownership and mode inheritance for file creation, mkdir, rename and cross-directory moves.
- [ ] Define root/privileged-process behavior through the existing kernel capability/security model; avoid ad hoc filesystem-only privilege bypasses.
- [ ] Define umask behavior if supported by the OS; otherwise explicitly document that it is not yet exposed.
- [ ] Define mount policy for read-only mounts and ensure it cannot be bypassed by a filesystem mutation path.
- [ ] Keep on-disk metadata descriptive only: authorization must be enforced by trusted kernel/VFS code.

### 2.2 Operations that must enforce permissions

Every relevant operation must be audited and tested, including:
- [ ] Path lookup and directory traversal/search.
- [ ] File open for read and write; read, write, append and truncate.
- [ ] File creation and directory creation.
- [ ] Directory enumeration/listing, independently of search permission.
- [ ] Unlink and rmdir, including sticky-directory semantics only if the OS supports them.
- [ ] Rename and cross-directory moves; check source and destination directory rights and any applicable ownership/sticky rules.
- [ ] Metadata queries/updates and ownership/mode changes, if those APIs exist.
- [ ] Mount, format and recovery operations at the appropriate privileged boundary.
- [ ] Read-only integrity scans and diagnostics, ensuring no mutation occurs and access to diagnostic information follows policy.

### 2.3 Security invariants

- [ ] Permission denial occurs before any persistent mutation.
- [ ] Failed operations return the correct error and initialize output parameters (e.g. byte counts) deterministically.
- [ ] No operation can obtain privilege by crafting an inode, directory entry, UID/GID, mode, generation or checksum-valid but semantically invalid metadata.
- [ ] Symlink traversal, hard links and ACLs must not be enabled until their interaction with permission checks is specified and tested.
- [ ] Permissions must be enforced in the VFS/security boundary, not only in a shell, desktop UI or convenience wrapper.
- [ ] Document the limits of discretionary access control: it is not encryption and does not protect data from an attacker who can read raw disk blocks or boot another OS.

### 2.4 Required permission tests

- [ ] Owner/group/other allow and deny cases for file reads and writes.
- [ ] Directory search versus enumeration behavior.
- [ ] Create/mkdir/unlink/rmdir/rename allowed and denied cases.
- [ ] Cross-directory rename permission checks and ownership edge cases.
- [ ] Read-only mount mutation rejection.
- [ ] Privileged capability behavior and unprivileged denial.
- [ ] Malformed metadata and permission-denial fault injection; confirm media does not change on denied operations.
- [ ] Remount persistence of ownership/mode and consistent behavior after reboot.
- [ ] Run tests at the exact release candidate commit and required Build/boot-test and Security architecture CI gates.

**Release gate:** if the kernel/VFS cannot reliably enforce the model, mark Version 1.0 blocked; do not claim permissions are supported merely because inode fields exist.

## 3. Basic encryption — design and scope

### 3.1 Recommended initial target

Prefer **whole-volume/data-region encryption behind a block-device or volume-crypto layer** rather than adding ad hoc encryption calls to individual filesystem operations. This reduces the chance of forgetting a read/write path and keeps ProsenjitFS metadata layout concerns separate. The exact layer must be chosen after inspecting the current block-device, boot, recovery and key-input architecture.

A reasonable initial design should aim for:
- Confidentiality of stored file contents when the volume is offline and the key is unavailable.
- Authenticated integrity where the chosen construction supports it, with a documented threat model.
- No hard-coded key, predictable default key, plaintext fallback or silent downgrade.
- A clear distinction between encrypted data and metadata; whole-volume encryption can protect both but may complicate boot-time discovery and recovery.
- Compatibility and recovery behavior defined before changing the on-disk format.

### 3.2 Cryptographic requirements

- [ ] Use a reviewed, standard cryptographic library/implementation available and supportable in this OS; do not invent a cipher, mode, KDF or authentication construction.
- [ ] Select an authenticated-encryption design and nonce/tweak strategy appropriate to random-access block storage; document replay/rollback limitations and block-level integrity behavior.
- [ ] Derive or unwrap keys using a reviewed password-based KDF or a future secure key source; never use a password directly as an encryption key.
- [ ] Specify salt, KDF parameters, key version, algorithm identifier and any authentication metadata with bounds and validation.
- [ ] Ensure nonces/tweaks cannot be reused in a way that breaks the chosen construction. Random-access storage makes nonce management a design-critical issue.
- [ ] Keep secrets out of logs, crash reports, debug output and persistent plaintext buffers where practical; define key lifetime and zeroization best-effort behavior.
- [ ] Fail closed on wrong key, invalid authentication tag, unsupported algorithm/version or malformed crypto metadata.
- [ ] Do not silently mount encrypted media as plaintext when key verification fails.
- [ ] State clearly that encryption does not prevent deletion, denial of service, traffic/usage analysis or rollback to an older valid disk image unless additional mechanisms address those threats.

### 3.3 Key management and boot/recovery requirements

- [ ] Decide how a key is provided at boot and how the key is protected in memory.
- [ ] Do not embed a master key in the kernel, bootloader, repository, image or source code.
- [ ] Specify password change, key rotation, recovery-key policy and the consequences of losing all keys.
- [ ] Specify how formatting identifies encrypted versus unencrypted volumes without exposing secrets or allowing downgrade attacks.
- [ ] Ensure journal replay and integrity scans run at the correct layer and cannot bypass authentication.
- [ ] Specify whether the boot partition is encrypted; do not imply encrypted boot if only a data volume is encrypted.
- [ ] Document that software-only encryption cannot protect keys from a fully compromised running kernel or an attacker with access to unlocked memory.
- [ ] Add tests for wrong key, corrupted crypto metadata, modified ciphertext/tag, interrupted writes, remount, key loss and unsupported crypto version.

### 3.4 Release gating for encryption

Encryption is **not considered complete** until all of these are true:
1. Design and threat model reviewed.
2. Cryptographic primitive and library are reviewed and available in the actual build.
3. Key input/storage/lifetime and recovery behavior are implemented.
4. All data I/O paths are encrypted consistently; no plaintext bypass exists.
5. Authentication failures fail closed without exposing corrupted plaintext.
6. Power-loss/journal ordering and crypto metadata updates are tested together.
7. Known-answer tests for primitives and integration tests pass.
8. Exact-head CI passes and documentation accurately defines protection boundaries.

If any item is missing, encryption must be labelled **planned/experimental**, not a release feature. Never implement a home-grown cipher to meet a deadline.

## 4. Suggested delivery order

### Version 1.0 — mandatory baseline
1. Audit the actual VFS permission checks and all filesystem operation entry points.
2. Define UID/GID/mode semantics and directory permission rules.
3. Implement missing enforcement in the trusted VFS/security layer.
4. Add positive, negative, malformed-metadata and no-mutation tests.
5. Verify persistence across remount and exact-head CI.
6. Publish permission semantics and known limitations in release notes.

### Version 1.1 — encryption only after design approval
1. Inspect the current block-device/volume abstraction and boot key-entry options.
2. Write a threat model and compare reviewed authenticated-encryption designs suitable for random-access storage.
3. Define on-disk crypto metadata and versioning/compatibility.
4. Implement key management and crypto layer behind a narrow interface.
5. Test all I/O paths, wrong-key behavior, corruption, interrupted transactions and remount.
6. Keep it experimental until every encryption gate passes.

## 5. Explicit status rule

This document is a security requirement, not proof of implementation. Before release, link each completed item to source files, tests and exact-head CI runs. A planned permission or encryption feature must never be described as shipped.



## FS-36 — Native ProsenjitFS concurrency source audit (2026-10-28)

- **Result: general concurrent I/O is not verified; release claims must remain limited.** The current VFS file object updates `position` in `vfs_file_read()`, `vfs_file_write()`, and `vfs_file_seek()` without per-handle synchronization. Concurrent operations on the same open handle can therefore race on the shared offset even if the filesystem serializes metadata.
- In `kernel/prosenjitfs.c`, `pfs_io_lock()` is a busy-wait `__sync_lock_test_and_set()` spinlock. The current wrapper uses it for append writes; ordinary writes, reads, and truncates do not use this wrapper. Extending this spinlock across block-device I/O, journal commits, or flushes is unsafe unless the device path is proven non-blocking and preemption-safe; that contract has not been established.
- Correct serialization must cover shared transaction/journal staging state and complete inode/extent/bitmap read-modify-write operations, including namespace operations and internal call paths—not just one VFS callback. A scheduler-aware mutex with defined lock ordering, cancellation/error cleanup, and unmount lifetime protection is required before broad locking is introduced.
- Required deterministic tests: append from separate handles; concurrent read/write/seek on one handle; read-vs-truncate and write-vs-truncate; namespace mutation vs open I/O; transaction failure/rollback under contention; and unmount/media removal with active I/O. Tests must assert bounded completion, integrity checks, and unchanged durable/volatile/dirty media on rejected or failed operations.
- **Verification boundary:** source audit only. The local runner is offline; no build, runtime concurrency test, or exact-head CI pass is claimed. PR #265 must not be treated as a safe merge merely because it proposes broader serialization; review its diverged diff and locking primitive before integration.
- **Next gate:** decide and document the scheduler-aware lock/wait contract first, implement the smallest shared transaction lock with explicit lock order, then add tests and check Build/boot-test plus Security architecture workflows on the exact final SHA.

## FS-37 — Trusted authorization API contract (2026-10-28)

- **Decision gate, not an implementation claim:** the current process model has `process_current()` and process capability/syscall policy, but the VFS API has no explicit effective UID/GID or trusted authorization context. Do not add a UID/GID argument supplied by an ordinary caller and trust it.
- Before implementing mode-bit enforcement, define a kernel-owned credential snapshot derived from the authenticated current process (or a separately authorized kernel-internal context). A missing process, unknown credential, invalid mount/vnode, or unsupported authorization state must fail closed for user-originated operations. Kernel boot/storage self-tests and trusted system services must use an explicit, auditable internal-call path; do not silently treat `process_current() == NULL` as root.
- Keep authorization at the VFS boundary so every filesystem provider follows the same policy. The provider may return trusted inode owner/group/mode metadata, but it must not choose the caller's identity or override VFS denial. Revalidate the target and parent relationships within the mutation's serialization boundary to avoid check-then-use races.
- Required operation matrix before coding:
  - Path traversal: execute/search permission on each directory component; root path and `.`/`..` must not bypass checks.
  - Lookup/stat/open: read/search rules defined separately; enforce requested read/write access at open and on every later handle operation if credentials/authorization can change.
  - Directory enumeration: directory read permission; traversal/search is separate.
  - Create/mkdir: parent-directory write + execute/search; validate inherited/default ownership policy before commit.
  - Unlink/rmdir: parent write + execute/search, plus defined sticky-directory and non-empty-directory policy if those semantics are supported; never authorize only from target-file write bits.
  - Rename: authorize source and destination parents, verify both mount/namespace constraints, and define replacement/type/sticky-directory behavior before mutation.
  - Truncate/write: enforce file write permission and mount read-only state; denial must occur before journal/allocator staging.
  - Permission/ownership changes: require owner or a specifically defined privileged capability; validate supported mode bits and prevent caller-selected UID/GID spoofing.
  - Unknown operation, missing credentials, or unsupported ACL/ownership state: deny by default.
- Required tests: owner/group/other allow/deny cases for each operation family; directory traversal denial; forged credential input rejection; kernel-internal call audit; permission change privilege checks; and before/after durable, volatile, and dirty-media CRC comparisons proving every denied mutation is a no-op. Include multi-component path and rename races once concurrency primitives are defined.
- **Current status:** this contract has not yet been implemented. FS-34 remains a 1.0 release blocker. The local runner is offline, so no build/runtime pass is claimed. The safe next code change is to add the credential/authorization API and tests in a narrow PR only after the process lifecycle, syscall entry points, and all VFS call sites have been enumerated.

## FS-38 — Trusted-authorization call-site inventory (2026-10-28)

**Result: source inventory completed; implementation remains blocked on an explicit kernel authorization context and tests.** This inventory was checked against the current `main` source files and is intended to prevent an unsafe partial UID/GID patch.

### Identity and syscall boundary

- `kernel/include/process.h`: `struct pros_process` has PID/parent PID, `security_level`, capability and syscall policies, address-space state, and handles. It has no effective UID/GID, supplementary groups, or filesystem credential object. `process_current()` returns a process pointer; it does not itself provide a validated filesystem identity.
- `kernel/syscall.c`: dispatcher policy checks the current process's syscall allowlist, then handles version, handle operations, and IPC. The inspected dispatcher has no public filesystem open/read/write/seek/truncate/namespace syscall cases. Therefore current process identity cannot yet be assumed to be the credential source for a public filesystem ABI that is not present.
- `kernel/include/vfs.h` and `kernel/vfs.c`: VFS operations are direct kernel C APIs. The current read/write flag checks and read-only mount check are not owner/group/other authorization.

### In-tree kernel callers that must be classified explicitly

- `kernel/init.c`: `pros_init_load()` opens `/init` read-only and reads its image. This is a privileged boot-time loader path and must use an explicit kernel-internal authorization context, not depend on an ambient user process.
- `kernel/log_persist.c`: persistent logging opens/creates its log, checks rotation candidates by opening files, and unlinks/renames rotated logs. This is a kernel service with bounded paths/policy and must receive an explicit service authority; treating it as an ordinary user or silently granting root when no current process exists is unsafe.
- `kernel/kernel.c`: native filesystem read/write/open calls found in the inspected region are boot/self-test persistence checks. They also need an explicit test/kernel-internal context once VFS authorization is enforced.
- Filesystem-provider callbacks and internal self-tests must not accept a caller-supplied UID/GID as authority. The VFS must resolve the credential from a trusted call context and pass a validated snapshot to authorization.

### Safe implementation order

1. Add a kernel-owned authorization-context type with an explicit distinction between authenticated process credentials and narrowly scoped kernel/service/test authority. No user ABI may construct or forge the privileged variant.
2. Audit all call sites of `vfs_path_lookup`, `vfs_file_open`, `vfs_dir_open/read`, `vfs_file_read/write/truncate`, `vfs_mkdir/unlink/rmdir/rename`, and `vfs_file_close`; classify each as process-originated or internal and pass context explicitly rather than relying on nullable ambient state.
3. Define credential lifecycle/defaults before adding UID/GID fields to processes; unknown or missing process credentials must deny user-originated access. Do not equate `PROS_SECURITY_SUPER` with filesystem UID 0 without a documented, tested policy.
4. Enforce authorization at the VFS boundary before provider mutation, with directory traversal and parent-directory checks, then test owner/group/other allow/deny, forged-context rejection, kernel-service least privilege, and unchanged durable/volatile/dirty media after every denial.
5. Build and boot-test the exact candidate and run Security architecture checks before merge. Local runner is offline; this inventory is source review only and does not claim runtime verification.

This section is an implementation checklist, not a claim that authorization, POSIX identity, or public filesystem syscalls are already implemented.

## FS-39 — VFS authorization insertion points (2026-10-28)

This function-level map narrows the implementation surface. It is derived from the current `kernel/include/vfs.h` and `kernel/vfs.c`; it does not imply that checks are implemented.

| API / path | Required authorization point | Important rule |
|---|---|---|
| `vfs_path_lookup()` | During each path component traversal | Require search/execute permission on each directory before looking up the next component; do not reveal a child after traversal denial. |
| `vfs_file_open()` existing file | After resolving each component, before returning a handle | Check requested read/write access against inode mode and trusted credentials. Read/write open flags alone are not authorization. |
| `vfs_file_open()` with `PROS_VFS_O_CREATE` | Before creating the missing final component | Check search on traversed directories and write+search on the parent; check existing-target behavior and flags explicitly. |
| `vfs_dir_open()` / `vfs_dir_read()` | Before opening/enumerating directory entries | Distinguish directory search from listing/read permission. Do not let a previously opened directory bypass a required policy without a defined handle policy. |
| `vfs_file_read()` | At open and/or each operation according to the documented revocation model | The handle must retain a trusted credential/authorization snapshot or a well-defined owner-bound policy; caller-supplied identity is never valid. |
| `vfs_file_write()` / `vfs_file_truncate()` | Before provider mutation | Require write authorization, reject read-only mounts, and ensure denied operations cannot alter metadata or data. |
| `vfs_mkdir()` | Before parent mutation | Require parent write+search and traversal rights. |
| `vfs_unlink()` / `vfs_rmdir()` | Before namespace mutation | Require parent write+search, enforce target type/emptiness, and define sticky-directory semantics if supported. |
| `vfs_rename()` | Before either parent is mutated | Authorize source and destination parents independently; handle replacement, sticky semantics, and cross-mount behavior explicitly. |
| `vfs_file_close()` | Lifetime cleanup, not a new access grant | Must remain possible for a denied/revoked handle so resources can be released. |
| `vfs_file_seek()` / metadata queries | Validate handle and define disclosure policy | Seek is not a permission grant; stat/metadata visibility needs an explicit rule. |
| Provider vnode callbacks | Receive only VFS-validated operation context | Providers may expose owner/group/mode metadata but must not choose the actor or override a VFS denial. |

### Open-handle semantics that must be chosen before coding

The current `struct pros_vfs_file` stores mount, vnode, provider operations, private data, position, flags, and open state; it does not store an authenticated actor or authorization snapshot. The implementation must explicitly choose and document whether permissions are checked only at open time or revalidated for each read/write/truncate operation. Revalidation is safer for permission revocation but requires a trusted actor context at each operation; an open-time snapshot requires a clear rule for what happens after mode/ownership changes. Do not accidentally mix the two models.

### Test gates tied to these insertion points

- Path traversal denied at an intermediate directory; verify child lookup does not proceed.
- Existing file read/write open denied independently for owner/group/other.
- Create and mkdir denied without parent write+search; unlink/rmdir/rename denied without parent authority.
- Directory enumeration denied when listing permission is absent.
- Open handle tested after permissions change under the chosen revocation model.
- Every denial compares durable, volatile, and dirty mock-device state and verifies integrity.
- Internal boot loader, log rotation, and self-test callers use explicit, narrowly scoped contexts; no implicit `NULL == root` fallback.
- Exact-head build/boot and Security architecture checks must pass before this work can be called verified.

 
## FS-40 — PR #290 trusted mode-permission enforcement integration (2026-10-28)

- Integrated permission enforcement on the current `main` tree, preserving FS-37 through FS-39 authorization contract, call-site inventory and insertion-point audit plus the existing inode metadata cache API.
- Added kernel-owned process UID/GID bootstrap identities: super-level processes map to UID/GID 0; limited processes map from their nonzero kernel-assigned PID only while it fits the on-disk 32-bit identity fields. This remains a bootstrap model, not a login/group database.
- Added a filesystem-neutral authoritative VFS metadata callback. ProsenjitFS returns owner/group/mode from the generation-validated inode; VFS authorization does not trust caller-selected IDs. Existing `vfs_vnode_set_metadata()` cached metadata API from #289 remains intact.
- Enforced owner/group/other read, write and execute/search decisions during path traversal, file open/read/write/truncate, directory open/enumeration, create, mkdir, unlink, rmdir, and both parents of rename. Read-only mount and quarantine checks remain independently enforced.
- New files/directories store the validated current identity with existing v1 defaults (files 0644, directories 0755). Unsupported special permission bits, unmapped credentials, quarantined/invalid processes, and limited callers using providers without authoritative metadata fail closed.
- Added policy and in-memory VFS-wrapper self-tests for owner/group/other decisions, search-versus-list semantics, denied writes/truncates before provider callbacks, identity changes after open, unknown credentials, privileged override, and unsupported mode bits. Boot self-tests now require the VFS permission/security suite to pass.
- **Scope/verification boundary:** no chmod/chown, umask, supplementary groups, ACL, sticky-directory policy, or userspace filesystem syscall/descriptor ABI is claimed. The integration self-test uses an in-memory VFS provider; denial/no-mutation CRC assertions against mounted ProsenjitFS media, explicit per-service authorization contexts for every kernel caller, check-then-use serialization, physical power-loss durability, and general concurrent I/O remain separate acceptance gates. Exact-head Build/boot-test and Security CI results are required before merge; this section alone is not a verification claim.


## FS-41 — Media-backed VFS permission-denial regressions (2026-10-28)

- Added coverage to `prosenjitfs_selftest()` using the mounted ProsenjitFS deterministic mock medium and a kernel-created limited process.
- After each denied operation, the test compares CRCs for durable bytes, volatile cache bytes, and dirty-sector tracking, then requires a passing ProsenjitFS integrity scan.
- Covered denied writable open, create, mkdir, unlink, rmdir, rename, write through an already-open handle, and truncate through an already-open handle.
- **Remaining 1.0 acceptance work:** owner/group/other allow/deny matrices against persistent inode metadata; intermediate search denial; directory-list denial based on a persistent no-read mode; denied read against actual ProsenjitFS media; metadata/permission-change authorization; and explicit least-privilege contexts for every internal filesystem caller. The current on-disk fixture has no chmod API, so this test uses default 0644 files and 0755 directories and cannot establish the missing mode cases.
- This is mock-device evidence only. It does not establish physical-drive power-loss durability or general concurrent VFS safety. Exact-head Build/boot-test and Security architecture checks are required before merge.

