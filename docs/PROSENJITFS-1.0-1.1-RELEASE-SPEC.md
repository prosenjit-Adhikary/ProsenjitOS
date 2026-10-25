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