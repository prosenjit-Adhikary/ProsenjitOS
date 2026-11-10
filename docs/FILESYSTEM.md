# ProsenjitOS Filesystem — Single Source of Truth

**Canonical filesystem document:** this is the only active planning, architecture, implementation-status, test-matrix, release-scope, and verification document for every ProsenjitOS filesystem.
**Policy:** do not create separate filesystem roadmap, completion-matrix, test-matrix, implementation-plan, or release-spec files. Update this file instead.
**Last consolidated:** 2026-11-10.

### Native ProsenjitFS integration update — 2026-11-10

- **Directory namespace regression matrix:** PR [#534](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/534) merged as `b2ac52c96ca7e5cf9c90d85d88eaba3e077009e0`. The change tightens directory-entry validation, shares strict validation with rmdir scans, uses the public integrity API in the namespace self-test, and splits rename/old-path diagnostics.
- **Atomic keep-size preallocation:** PR [#535](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/535) merged as `27aac4ab1febc91e7046379868148097fd496d88`. Newly reserved data blocks, allocator bitmap updates, extent metadata and inode mapping are staged in one redo-journal transaction. The first reservation now publishes `inode.extent_head` in that same transaction. The self-test reserves eight blocks, checks the extent-chain head and accounting, reads every reserved block to verify zero-fill, runs the integrity checker, remounts, removes the fixture, and checks integrity again.
- **Partial physical extent overlap regression:** PR [#538](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/538) merged as `f4615786632a7ebe265817d1627f5fcdcf5b40f0`. A checksum-valid later extent is redirected to the final physical block owned by an earlier multi-block extent; the read-only integrity scan must reject the overlap and preserve durable, volatile, and dirty media snapshots.
- **Inode identity invariants:** PR [#539](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/539) merged as `f38a2ac42a6b1304386e68f5c561825ae0f80553`. Checksum-valid root-inode records with a non-v1 link count, zero generation, or out-of-range parent inode are rejected; the test restores the inode and requires a clean integrity scan.
- **Failed inactive-mount rollback:** PR [#540](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/540) merged as `de1100188f112782db8b87e56c705aa04dee0eb4`. A root-inode read failure after VFS mount initialization previously called the active-mount deactivation path, which rejected the inactive mount and left initialized pointers behind. The new `vfs_mount_abort()` rolls back that inactive mount; failure-injection coverage checks cleared ownership/buffers, unchanged media snapshots, and a successful retry mount/integrity scan/unmount.
- **Exact merged-main verification:** current `main` commit `de1100188f112782db8b87e56c705aa04dee0eb4` passed Build/boot-test [run #42784822119](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42784822119) and Security architecture [run #42784822573](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42784822573). The Build job's toolchain/dependency checks, desktop/DML checks, timer-frame check, ISO build, ProsenjitFS block-init diagnostic, exFAT formatter check, ISO QEMU boot and native BIOS QEMU boot all completed successfully. The Security job's architecture-contract validation completed successfully.
- **Format publication durability:** PR [#542](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/542) merged as `54c4e46d464080fbafb51ce0882084667d30f289`. `prosenjitfs_format()` now flushes initialized journal/bitmap/inode/root metadata before publishing either valid superblock, then performs the final publication flush. Failure-injection coverage distinguishes pre-persist and persist-then-EIO at both barriers, including simulated power-loss probe/mount/integrity outcomes. Exact PR-head Build/boot-test run [#42785769946](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42785769946) and Security architecture run [#42785770028](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42785770028) both passed before merge. This verifies the configured CI/QEMU path; physical-device power-loss durability remains unproven.
- **Block-I/O error propagation:** PR [#544](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/544) merged as `14c7b06f98e2e98844c9d8479ef2ad1bc9867f0c`; `pfs_flush()` now preserves invalid/unsupported capability errors instead of collapsing them into EIO. Exact PR-head Build run `42787373452` and Security run `42787373464` passed.
- **Block read/write capability propagation:** PR [#545](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/545) merged as `e59125dec18145f01c302c398cdbb965b4308b55`; missing read/write callbacks now return ENOTSUP and block helper errors preserve invalid/unsupported status. Exact PR-head Build run `42787908664` and Security run `42787908740` passed.
- **Higher-level ProsenjitFS error propagation:** PR [#546](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/546) merged as `e30251bcbc6a49813225589a14649219036e8e4b`. The change preserves block-read errors through probe, allocator and integrity scans, maps VFS read failures through the VFS error mapper, preserves format write failures and transaction flush errors, and adds regression coverage for missing device callbacks. The initial exact-head QEMU self-test exposed `selftest_rc=-391`; source review traced this to ENOTSUP being converted to ECORRUPT in integrity traversal. The corrected exact head `3ad0e5bbacf68b958495e65fcb061e80b8dd62c2` passed Build/boot-test run [#42791185684](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42791185684) and Security architecture run [#42791185565](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42791185565), with all job steps inspected before squash merge.
- **Evidence boundary for error propagation:** these exact-head CI runs verify the configured build, deterministic ProsenjitFS self-tests, ISO QEMU boot and native BIOS QEMU boot. They do not prove physical-device power-loss durability, arbitrary concurrent VFS/SMP safety, or physical removable-media behavior.
- **Evidence boundary:** these runs verify the exact merged `main` commit and configured CI/QEMU environment. They do not establish physical-device power-loss durability, arbitrary SMP-safe VFS operation, or physical USB hotplug/removal support. Keep those acceptance gates open until their separate evidence exists.
- **Duplicate PR handling:** PR #536 was closed without merging after its atomic-preallocation patch was found already present on `main` through merged PR #535. Do not count #536 as a separate implementation or merge.
**Important status rule:** code present, planned, tested on an older commit, and verified on the exact candidate commit are different states. Never claim a capability is complete without source and test evidence. Historical appendices below preserve prior audit notes; where they conflict with the current summary at the top, the current summary and exact-head CI evidence take precedence.

### FAT32/exFAT and USB removable-storage status (2026-11-10)

**Overall status: USB MSC block-device registration is implemented and QEMU/CI-verified on merged PR #457. Physical USB validation and hotplug/surprise-removal integration remain pending. Do not label end-to-end physical removable storage complete.**

#### Completed implementation and exact merged-main evidence

- **FAT32/exFAT provider:** both formats are supported by the pinned patched FatFs R0.16 core, the bounded partition-local block adapter, and the VFS provider. Deterministic provider tests cover format probing, mount, file/directory operations, read/write/truncate, sync/flush, remount and persisted readback. Formatting remains disabled. Further FatFs feature work is intentionally paused.
- **USB host/device bootstrap:** PR [#457](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/457) is merged into `main` as `9da428d3b1135483e88c74d1cb0c30e421333e3a`. The integrated path includes bounded xHCI No-op command completion, port reset, Enable Slot, Address Device, descriptor/configuration parsing, MSC bulk endpoint setup, BOT/SCSI INQUIRY and READ CAPACITY.
- **USB MSC block-device backend:** `kernel/usb_msc.c` implements block read/write/flush callbacks, capacity and sector validation, read-only admission, BOT recovery, and `pros_usb_msc_detach()`. The MSC object is initialized only after successful INQUIRY, TEST UNIT READY, READ CAPACITY and block-device initialization.
- **Kernel registry:** `kernel/block.c` provides bounded, lock-protected register/unregister/get/count operations, duplicate rejection, ENOSPC handling, and freed-slot reuse. Its self-test covers registration order, duplicate registration, unregister/ENOENT, capacity exhaustion and slot reuse.
- **Actual xHCI bootstrap publication:** `kernel/kernel.c` calls `pros_xhci_init_mass_storage()`, obtains the MSC block device, calls `block_device_register()`, and checks that the exact pointer can be found in the registry before printing the success marker. Registration failure is logged rather than reported as successful.
- **MSC detach unit/self-test:** `pros_usb_msc_selftest()` registers the MSC device, verifies that detach while BOT is busy returns EBUSY and leaves the registry entry intact, then verifies successful detach removes the entry and clears the device callbacks/private pointer. It also verifies detach of an initialized but never-published read-only device.
- **Exact post-merge Build/boot-test:** run [#9860](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42080509094), run ID `42080509094`, job ID `127431994644`, completed successfully on the exact merge SHA above using the self-hosted `home` runner. BIOS and Native BIOS QEMU logs contain xHCI No-op, Enable Slot, Address Device, USB MSC registry publication (count 1), and BOT/SCSI INQUIRY + READ CAPACITY (524288 sectors, 512-byte sectors).
- **Exact post-merge Security architecture:** run [#9861](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42080509726) completed successfully on the same merge SHA.

#### Remaining acceptance gates — not complete

- [ ] Wire real xHCI Port Status Change / disconnect events into device teardown and call the MSC detach path when the device is actually removed.
- [ ] Prove controller/DMA quiescence before freeing or reusing xHCI slot, transfer-ring, bounce-buffer, and MSC state. Do not free DMA-visible resources merely because the registry pointer was removed.
- [ ] Handle surprise removal during idle and active BOT I/O with bounded failure behavior and no stale registry, mount, partition, or VFS references.
- [ ] Implement/review reconnect and re-enumeration so a reconnected device receives a fresh valid block-device registration without duplicate/stale entries.
- [ ] Physical USB FAT32: enumerate, register, discover partition, mount, create/write/flush, unmount/remount and verify readback.
- [ ] Physical USB exFAT: same end-to-end test on a real exFAT-formatted device.
- [ ] Safe physical eject/unmount, including flush-before-removal and outstanding-handle behavior.
- [ ] Repeat on more than one real USB mass-storage device/controller where available; record device/controller details and serial logs.

#### Scope boundary and completion rule

- **USB MSC registration status:** implementation present; registry publication and detach helper behavior are covered by deterministic self-tests; xHCI bootstrap registration is confirmed by exact post-merge QEMU/CI logs.
- **Not yet proven:** real disconnect event integration, full hotplug/reconnect, safe physical removal, physical USB compatibility, or physical-media durability.
- QEMU synthetic/emulated evidence is not physical hardware validation. Keep the USB registration milestone marked **QEMU/CI VERIFIED** and the overall physical removable-media milestone **PENDING** until the remaining gates above pass.

### FAT32/exFAT provider implementation and verification history (2026-11-04)

- **Provider implementation:** FAT32 and exFAT are implemented through the pinned patched FatFs R0.16 core, the kernel block adapter, and the VFS provider. The provider delegates on-disk format parsing/classification to FatFs; it does not format media.
- **Merged recovery work:** PR [#443](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/443), merged as `4bb8f1904ea30095d667b2743733d431854b29ab`, adds FAT32 FSInfo and exFAT PercentInUse error propagation/retry, retains the allocation-dirty flag until metadata writes and CTRL_SYNC succeed, directory-create failure injection, and FAT32/exFAT allocation rollback coverage.
- **Exact post-merge CI:** Build/QEMU run [#9719](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41694214966) completed successfully on merge commit `4bb8f1904ea30095d667b2743733d431854b29ab`. The ISO and native-BIOS QEMU logs contain the FatFs block-adapter self-test, VFS 64-bit file-I/O self-test, and FAT32/exFAT provider mount/I/O/remount success markers. Security architecture run [#9212](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41694214630) also completed successfully on that commit.
- **Historical evidence boundary:** this earlier run predates the USB MSC and dual-partition QEMU test described above. It must not be substituted for the exact PR #457 run when describing USB integration.


- **Provider implementation:** FAT32 and exFAT are implemented through the pinned patched FatFs R0.16 core, the kernel block adapter, and the VFS provider. The provider delegates on-disk format parsing/classification to FatFs; it does not format media.
- **Merged recovery work:** PR [#443](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/443), merged as `4bb8f1904ea30095d667b2743733d431854b29ab`, adds FAT32 FSInfo and exFAT PercentInUse error propagation/retry, retains the allocation-dirty flag until metadata writes and CTRL_SYNC succeed, directory-create failure injection, and FAT32/exFAT allocation rollback coverage.
- **Exact post-merge CI:** Build/QEMU run [#9719](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41694214966) completed successfully on merge commit `4bb8f1904ea30095d667b2743733d431854b29ab`. The ISO and native-BIOS QEMU logs contain the FatFs block-adapter self-test, VFS 64-bit file-I/O self-test, and FAT32/exFAT provider mount/I/O/remount success markers. Security architecture run [#9212](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41694214630) also completed successfully on that commit.
- **What this proves:** exact-main-commit build and the configured deterministic synthetic-media/QEMU tests passed. It supports normal functional testing of FAT32/exFAT provider operations in the tested environment.
- **What this does not prove:** physical USB device support, USB Mass Storage end-to-end operation, broad hardware compatibility, physical-media power-loss durability, or exhaustive behavior on every malformed/real-world volume. The local runner's independent log review and physical USB tests remain outstanding.
- **Release status:** FAT32/exFAT provider implementation and configured QEMU runtime checks are **implemented and CI-verified**; FAT32/exFAT USB removable-media end-to-end support remains a separate 1.0 release blocker until the USB transport/device path and physical-media tests pass.

### Native ProsenjitFS 1.1 encryption readiness audit (2026-11-07)

- **Status: NOT IMPLEMENTED; design approval and crypto/runtime prerequisites remain open.** No ProsenjitFS encrypted-volume format, encrypted block layer, encrypted metadata/data path, unlock flow, or key lifecycle is present in the current implementation.
- **Current crypto inventory:** the freestanding kernel build explicitly compiles SHA-512 and Ed25519 implementation files for existing security/signature functions. SHA-512 is a hash and Ed25519 is a signature scheme; neither supplies authenticated encryption for filesystem sectors. The current build script does not link an AEAD implementation or a password KDF.
- **Randomness/keying gap:** the current source-tree/build inventory contains no filesystem CSPRNG/entropy provider or secure random-number API suitable for generating volume keys, salts, or per-write nonces. Do not use timestamps, counters alone, PID, UUID derivation, or ordinary PRNG output as a substitute for cryptographic entropy.
- **Storage-format gap:** no defined encrypted-volume header/keyslot format, authenticated sector record, nonce/tweak allocation protocol, key rotation/replacement procedure, or authenticated recovery/journal protocol exists. CRC32C remains accidental-corruption detection only and is not an authenticity check.
- **Architecture gate before code:** compare vetted, maintained cryptographic libraries/constructions that can be built for this freestanding x86-64 target; verify provenance, licensing, constant-time behavior, known-answer tests, and build reproducibility. Separately design random-access sector authentication, nonce uniqueness across overwrite/crash/recovery, metadata confidentiality, journal ordering, wrong-key behavior, and explicit rollback limitations. A password KDF alone does not create a secure entropy source or storage format.
- **Required next sequence:** (1) threat model and attacker capabilities; (2) crypto-library and AEAD construction review; (3) entropy/key provisioning and boot unlock design; (4) on-disk format and crash/recovery specification; (5) isolated host-side known-answer/fuzz/failure tests; (6) kernel integration behind a narrow volume/block-device interface; (7) exact-candidate QEMU and hardware tests.
- **Release rule:** keep encryption clearly marked planned/deferred. Do not add a custom cipher, ad hoc per-file encryption, or partially encrypted volumes. Do not begin implementation until the cryptographic and key-management design is reviewed and the required entropy/key-input path is available.

### Native ProsenjitFS 1.0 permission-enforcement verification (2026-11-07)

- **Status: IMPLEMENTED; exact source candidate verified.** The current VFS obtains filesystem identity from kernel-owned process state; it does not accept caller-selected UID/GID as authority. Missing, invalid, quarantined, or unsupported credentials fail closed.
- **Enforcement scope:** owner/group/other read, write, and execute/search decisions are applied to path traversal, file open and each later read/write/truncate, directory open and each enumeration, create/mkdir/unlink/rmdir, and both parents of rename. Mount read-only policy is independently enforced.
- **Persistent metadata changes:** authorized `chmod` is owner-or-privileged only; `chown` is privileged-administrator only. Only basic rwx mode bits (0000–0777) are supported; v1 inode reads reject every higher mode bit even when the inode checksum is valid. Metadata updates use the ProsenjitFS journal transaction and are checked after unmount/remount.
- **Negative-test evidence:** the native ProsenjitFS mock-media suite checks denied operations against durable-media, volatile-cache, and dirty-sector CRC32C snapshots and runs an integrity scan. It also covers owner/group/other access classes, search-versus-list semantics, open-handle revocation, namespace mutation denial, chmod/chown policy, read-only mounts, and metadata transaction failure recovery.
- **Exact candidate CI:** these permission tests are part of the current ProsenjitFS self-test required by the Build/boot-test workflow. The exact PR #466 source head `32ebb892945b0ec25e73d55b3c97f3df66515935` passed Build/boot-test [run #9874](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42346986668) and Security architecture [run #9367](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42346986646). Subsequent PR #467 changed documentation only.
- **Explicit limits:** this is not full POSIX identity/permission support. Supplementary groups, a user/group database, umask, sticky-directory semantics, setuid/setgid semantics, ACLs, and a public userspace filesystem syscall/descriptor ABI are not implemented. The bootstrap identity mapping is not a login system.
- **Release interpretation:** the documented owner/group/other permission requirement for ProsenjitOS 1.0 is implemented and verified in the configured QEMU/mock-media environment. Do not extend this claim to unsupported POSIX features or general concurrent VFS safety.

### Native ProsenjitFS block-durability barrier update (2026-11-07)

- **Block-layer contract:** PR [#465](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/465), merged as `45a5e27180269882d311d8f5d5418436140ed34e`, makes a flush request fail with `PROS_BLOCK_ENOTSUP` when the device has no explicit durability callback. A missing hardware barrier can no longer be reported as a successful flush.
- **ATA PIO capability selection:** PR [#466](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/466), merged as `114470ea071b54d7c1c66ae17a5d9bd3a5866bf3`, decodes IDENTIFY DEVICE word 83 and selects FLUSH CACHE EXT only when advertised and LBA48 is active; otherwise it uses advertised FLUSH CACHE or fails closed. The command path waits for device readiness, checks pre-command ERR/DF, applies the 400 ns status delay, and checks completion/error status.
- **Exact merged-head verification:** PR #466 head `32ebb892945b0ec25e73d55b3c97f3df66515935` passed Build/boot-test run [#9874](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42346986668) and Security architecture run [#9367](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42346986646). The merge commit is included in the current `main` history.
- **Native filesystem implication:** ProsenjitFS journal commit/replay and journal-clear paths rely on the block-device flush contract. The lower-layer software path is now fail-closed for absent/unsupported flush operations.
- **Evidence boundary:** QEMU/CI verifies the configured emulated path only. Physical ATA device cache behavior, power-loss persistence, and hardware-level durability remain unverified; do not claim certified physical power-loss durability.

## Release feature contract — owner decision (2026-10-25)

This section is the release-scope authority for native ProsenjitFS. Features below are targets and acceptance criteria, not claims that implementation is already complete. Mark a feature complete only after source inspection and tests on the exact release candidate.

### ProsenjitOS 1.0 — native ProsenjitFS

Required:
- Format and mount a ProsenjitFS volume with validated on-disk metadata and version/feature checks.
- Core block/inode allocation and release with bounds checks; detect invalid, duplicate, or out-of-range references.
- Create, lookup, enumerate, rename, and remove files/directories with correct namespace and parent-directory handling.
- File open/read/write/seek/truncate and supported metadata queries; return defined errors for unsupported operations.
- Space accounting and allocation metadata consistency checks.
- Transactional metadata updates using the existing journal/recovery design, with interruption/replay and malformed-journal tests. V1 journal headers require every unused target slot after `record_count` and all bytes after the fixed header structure to be zero; V1 commit blocks require zero-filled bytes after the fixed commit structure. Checksum-valid non-zero unused slots or record padding are corruption. Do not claim physical power-loss durability without appropriate tests.
- Metadata checksums and corruption detection where defined by the current on-disk format. V1 extent blocks require zero-filled bytes after the fixed 44-byte extent record. V1 directory-entry blocks require zero-filled bytes after the fixed 288-byte entry record. V1 directory-index blocks require zero-filled bytes after the fixed 284-byte structure. V1 superblock blocks require zero-filled bytes after the fixed 132-byte header. The v1 inode writer must reject mode bits outside `0777` and non-zero reserved bytes before any block I/O.
- VFS integration for path resolution and consistent error propagation.
- **Mandatory permissions in 1.0:** persistent UID/GID/mode (or the format's documented equivalent), trusted VFS/kernel authorization for every relevant operation, owner/group/other read-write-execute checks, directory traversal/list/create/delete/rename rules, permission changes subject to authorization, and default-deny behavior for unknown credentials or unsupported authorization states. Test allowed and denied cases, including verifying denied operations do not mutate disk state. UID/GID/mode fields alone do not count as enforcement.
- Deterministic self-tests, malformed metadata/journal tests, allocation-boundary tests, and exact-candidate build/boot/persistence/security test evidence.

The exact supported limits (maximum file size, directory size, extent count, link semantics, timestamp precision, concurrency, and durability guarantees) must be documented from the implementation. Do not promise POSIX completeness, hard links, ACLs, quotas, snapshots, compression, or general concurrent safety unless implemented and verified.

### ProsenjitOS 1.1 — mandatory native filesystem encryption

**Release scope: encryption only. Do not expand the 1.0 contract or bundle unrelated advanced features into 1.1.**

Required design and implementation gates:
- Review and approve a threat model and on-disk encryption format before implementation is called release-ready.
- Use a vetted authenticated-encryption construction and maintained implementation suitable for the freestanding x86-64 target; never invent a custom cipher or ad hoc sector encryption.
- Define entropy source, key generation/provisioning, boot-time unlock, key rotation/replacement, failure handling, and secure key lifetime/zeroization policy.
- Protect metadata and file data with authenticated I/O and explicit nonce uniqueness across overwrite, crash, replay, and recovery paths.
- Integrate with allocation, journal transactions/replay, permissions, formatting, mount/unmount, and block-device flush semantics.
- Define wrong-key, damaged-tag, partial-write, interrupted-transaction, recovery, and rollback behavior. Fail closed; never expose unauthenticated plaintext as valid filesystem content.
- Add host-side known-answer, fuzz, malformed-image, tamper, wrong-key, crash/recovery, key-handling-failure, and denied-access/no-mutation tests before kernel integration; then run exact-candidate QEMU and available hardware tests.
- Do not begin kernel implementation until the crypto/entropy/key-provisioning design is reviewed. Encryption is complete only after design review and exact-candidate tests pass.

### Post-1.0 feature roadmap — version-by-version plan

**Version assignment policy:** the versions below are the current planning target, not a promise or fixed ship date. A release must not start until the previous release is complete, reviewed, tested, performance-checked, and verified on its exact candidate commit. Keep one principal filesystem capability per release; split a capability into smaller releases if its design or verification is too large. A version number does not make a feature implemented or approved.

| Target version | Planned scope | Dependency / release gate |
|---|---|---|
| **1.1** | Native filesystem encryption only | Reviewed threat model, vetted AEAD, entropy/key provisioning, nonce safety, authenticated journal/recovery, exact-candidate tests. |
| **1.2** | Extended attributes (xattrs) | Stable metadata format, bounded lookup/update, transaction and corruption tests. |
| **1.3** | Access-control lists (ACLs) | Build on kernel-owned identity and 1.0 default-deny permissions; bounded evaluation and correct cache invalidation. |
| **1.4** | Advanced extent allocation | Define extent format/index and recovery; establish fragmentation and allocator performance baseline before CoW depends on it. |
| **1.5** | Copy-on-Write (CoW) and reference-count foundation | Format migration/compatibility, reference-count integrity, journal ordering, crash consistency, bounded reclamation. |
| **1.6** | Snapshots | Depend on CoW/refcounts; bounded create/delete/reclaim, crash recovery, and snapshot isolation tests. |
| **1.7** | Reflinks | Depend on CoW/refcounts; shared-extent lifetime, clone/truncate/write isolation and space-accounting tests. |
| **1.8** | Sparse files | Define hole-read, allocation, truncate, seek and accounting semantics; bounded metadata behavior. |
| **1.9** | Large-file optimization | Build on extent lookup and 64-bit checked offsets; preserve small-file latency and existing format limits unless separately approved. |
| **1.10** | Compression | Vetted codec, bounded decompression, explicit policy, corruption and worst-case CPU/memory tests. |
| **1.11** | Quotas | Transactionally consistent user/group/project accounting with bounded lookup/update cost. |
| **2.0** | Parallel I/O | Only after lock hierarchy, object/device lifetime, request cancellation, queue backpressure and journal ordering are proven. |
| **2.1** | SSD TRIM / discard | Block-device capability/limits and safe post-commit free-range ordering; unsupported devices remain fully usable. |
| **2.2** | Online resize | Transactional grow first; shrink only if every tail allocation/reference can be proven safe. |
| **2.3** | Storage pooling and explicit redundancy layout | Define device membership/failure domains and supported concatenated/striped/mirrored modes. Pooling without mirroring is not redundancy. |
| **2.4** | Self-healing: online scrub, then verified repair | Detection may precede repair; automatic repair requires independently verified redundant copies from a supported redundancy layout. |

**Ordering and scope clarifications**
- Advanced extent allocation is intentionally placed before CoW/reference counting because scalable extent metadata and allocator invariants may be prerequisites for safe shared extents.
- CoW/refcount foundation precedes snapshots and reflinks. Snapshots and reflinks remain separate release scopes so each can be verified independently.
- Large-file optimization follows extent, reflink, and sparse-file semantics so it does not prematurely lock in incompatible file-layout behavior.
- Storage pooling must explicitly define whether each supported pool is concatenated, striped, mirrored, or another documented layout. Self-healing repair follows only after a verified redundant-copy mechanism exists; checksums alone detect corruption but cannot reconstruct data.
- Richer journaling/durability is a cross-cutting requirement, not a reason to bundle unrelated features into a release. Any identified durability gap must be fixed before the dependent feature ships.
- These are target version slots. If a dependency, test, performance budget, format migration, or hardware requirement is not ready, defer that release item; do not silently ship a partial feature or move several features into one release to preserve numbering.

### Phase 3 — Performance & Enterprise (planned; incremental post-1.0 roadmap)

**Purpose and status:** Phase 3 is the long-term performance and enterprise track, delivered incrementally through the target versions in the release table above. Every item below is a planned candidate, not an implemented capability or a release commitment. Do not move an item into an active release until its design, dependencies, measurable acceptance criteria, compatibility impact, and test plan have been reviewed. Keep the 1.0 contract unchanged and keep 1.1 limited to encryption. Stabilize the existing journaling, VFS, and block-device contracts before features that depend on them. The advanced extent allocator is deliberately scheduled before the CoW/reference-count foundation because it may be a prerequisite; do not interpret the phase label as requiring CoW to precede that allocator work.

#### Phase 3 feature definitions and acceptance gates

1. **Advanced extent allocation**
   - **Goal:** improve allocation locality and reduce fragmentation by allocating, extending, splitting, merging, and releasing multi-block extents efficiently rather than relying only on small isolated allocations.
   - **Required design:** define the on-disk extent representation, maximum extent count/depth, alignment and size classes, free-space indexing, coalescing rules, overflow handling, and compatibility/migration strategy. Allocation metadata updates must remain journaled and recoverable. If the allocator uses trees or indexes, define corruption detection and rebuild/validation rules.
   - **Correctness gates:** test empty, nearly-full, and fragmented volumes; allocation at device/volume boundaries; split/merge/coalesce; extent exhaustion; allocation rollback after injected I/O/metadata failures; mount/replay after interrupted updates; and consistency between free-space accounting and allocated extents.
   - **Performance gates:** compare sequential/random allocation latency, fragmentation, metadata I/O count, CPU cost, and memory use against the recorded baseline. Index traversal, coalescing, and fallback scans must have explicit bounds; ordinary allocation must not trigger an unbounded whole-volume scan.

2. **Large-file optimization**
   - **Goal:** improve throughput and latency for large files while preserving correctness for small files and existing file-size limits unless a separately reviewed format change expands them.
   - **Required design:** document maximum file size and offset arithmetic; use checked 64-bit or wider intermediate arithmetic where required; define extent lookup/caching, read-ahead/write-behind policy if supported, bounded I/O chunk sizes, partial-I/O semantics, sparse-hole behavior if/when supported, and journal interaction for metadata updates. Do not silently change the on-disk format or promise files larger than the implementation can address.
   - **Correctness gates:** test files around every addressing/extent boundary, multi-gigabyte files where the test environment supports them, sparse and fragmented layouts where supported, seek/read/write/truncate at boundary offsets, short I/O, disk-full, device errors, close/flush retry, crash/recovery, and remount/readback with content hashes.
   - **Performance gates:** measure sequential and random throughput, small random reads within large files, extent lookup latency, CPU, and peak memory. Read-ahead/write-behind buffers must be bounded, cancellable on close/unmount, and must not report durable completion before the required flush succeeds.

3. **Parallel I/O**
   - **Goal:** allow independent requests to different files, extents, or devices to make progress concurrently without corrupting shared metadata or starving latency-sensitive operations.
   - **Prerequisites:** a documented lock hierarchy; safe vnode, mount, request, buffer, and device lifetimes; block-layer request completion/cancellation semantics; per-object or per-volume synchronization; and journal transaction serialization rules. Do not introduce parallelism while shared global state has undefined ownership or lifetime.
   - **Required design:** define which operations may execute concurrently and which remain serialized; queue limits and backpressure; request ordering and barriers; fairness; cancellation and timeout behavior; error propagation; unmount/drain semantics; and partial-completion representation. Never hold broad namespace/allocation locks while waiting on slow device I/O.
   - **Correctness gates:** race/stress tests for same-file and independent-file access, concurrent create/rename/unlink, overlapping and disjoint writes, shared metadata, simultaneous flush, device errors, cancellation, unmount, and journal recovery. Use deterministic scheduling/fault injection where possible and host-side race tools when available.
   - **Performance gates:** demonstrate throughput/latency improvement under concurrent workloads without unacceptable single-request regression. Track queue depth, tail latency, lock contention, CPU, and memory. Queues and in-flight buffers must be capped; saturation must apply backpressure or return a defined error, never grow memory without bound.

4. **SSD TRIM / discard**
   - **Goal:** notify supported block devices that filesystem-free ranges are no longer in use so SSDs may reclaim them internally. Discard is an optimization hint, not a correctness or data-erasure guarantee.
   - **Prerequisites:** a block-device capability/limits contract describing support, alignment, granularity, maximum range/transfer, and error behavior. Unsupported devices must continue normal operation without discard.
   - **Required design:** issue discard only for ranges proven free after the relevant metadata transaction is safely committed; define batching/coalescing and ordering relative to journal commits and flushes; split requests to device limits; and decide whether discard is synchronous, queued, or background. Never discard live, reserved, journal, metadata, or not-yet-committed blocks.
   - **Correctness gates:** mock-device range validation, alignment and overflow tests, unsupported-device behavior, partial/failing discard, crash between free-space commit and discard, repeated discard, and proof that later allocations are initialized according to filesystem policy rather than relying on discarded contents.
   - **Performance/safety gates:** bounded batches and queue depth; discard must not stall foreground I/O indefinitely. Measure latency impact and allow safe disablement per device/volume. Do not describe TRIM as secure erase.

5. **Online resize**
   - **Goal:** grow or, only if safely supported, shrink a mounted ProsenjitFS volume without requiring a normal unmount/remount cycle.
   - **Required design:** explicitly separate grow from shrink. Grow must validate new device/partition capacity, update capacity metadata transactionally, and make newly addressable blocks available only after metadata is durable. Shrink must first prove that no allocated, reserved, journal, metadata, pinned, or in-flight block lies beyond the proposed end; relocate data safely if relocation is part of the design. If safe online shrink cannot be guaranteed, support grow-only and document shrink as unsupported.
   - **Concurrency/recovery gates:** coordinate with allocation, free-space indexes, open files, active I/O, journal replay, mount references, and device capacity changes. Define quiesce/barrier behavior and rollback/recovery for interruption at every resize stage.
   - **Tests:** grow and supported shrink at alignment and minimum/maximum boundaries; insufficient space; occupied tail; concurrent allocation/I/O; injected write/flush failures; crash/reboot at each stage; and remount with old/new geometry validation. Never expose blocks outside the actual device/partition.
   - **Responsiveness gates:** use bounded metadata batches and a defined progress/error state. Resize must not hold global locks across lengthy relocation or scans.

6. **Self-healing, online scrub, and repair**
   - **Goal:** detect latent corruption, classify its scope, and repair only when the filesystem has sufficient independently verified information to reconstruct the correct contents.
   - **Detection:** verify metadata/data checksums and structural invariants; distinguish detected corruption from a verified recoverable error; record affected object/block and reason without trusting corrupted pointers.
   - **Repair prerequisites:** automatic repair requires a verified redundant good copy, such as a validated mirror/replica or another explicitly designed redundancy mechanism. A checksum can detect a mismatch but cannot reconstruct original data. Never guess, synthesize, or silently zero damaged user data. Without a verified good copy, report/quarantine the affected object or volume and preserve recovery evidence.
   - **Required design:** define redundancy layout and failure domains, copy-selection rules, repair transaction/journal semantics, write ordering, scrub scheduling, rate limits, pause/resume/cancel, and behavior when copies disagree or all copies fail verification. Do not claim protection against device/controller-wide or correlated failures unless the redundancy design covers them.
   - **Tests:** injected single- and multi-copy corruption, stale/divergent copies, interrupted repair, power-loss/replay simulation, read-only fallback, scrub during foreground I/O, and proof that repair never overwrites the only known-good copy with unverified data.
   - **Responsiveness gates:** scrub/repair must be chunked, resumable, cancellable where safe, rate-limited, and lower priority than foreground I/O. Bound memory, scan batch size, lock duration, and retry count.

7. **Storage pooling**
   - **Goal:** combine capacity from multiple block devices or partitions into a managed storage pool while preserving clear failure, placement, and recovery semantics.
   - **Prerequisites:** stable device identity and lifecycle, per-device capacity/geometry/capability reporting, safe hot-remove/error handling, multi-device metadata ownership, and a defined redundancy policy. Storage pooling is not equivalent to redundancy: striping or aggregation alone does not protect data against device failure.
   - **Required design:** define pool metadata and versioning, device membership/add/remove rules, allocation and placement policy, balancing/rebuild behavior, degraded-mode semantics, failure domains, pool expansion, and recovery when devices are missing. Specify whether a pool is striped, concatenated, mirrored, or uses another explicit layout; do not use ambiguous “combined disk” behavior.
   - **Correctness gates:** test unequal device sizes, sector geometry differences, full/failed/missing devices, partial writes, device replacement, interrupted membership changes, pool import/export, stale metadata, and recovery for each supported failure scenario. Never mount a pool as healthy if required members or metadata are missing.
   - **Performance/safety gates:** measure per-device queueing, throughput, tail latency, CPU, and memory. Bound rebuild/rebalance bandwidth and make it pausable/rate-limited so foreground I/O remains responsive. Do not start dynamic pooling until simpler single-device and multi-device failure contracts are proven.

#### Recommended implementation order and dependency rules

1. **1.4 — Advanced extent allocation:** establish scalable allocation/indexing and fragmentation measurements before CoW/refcount work.
2. **1.9 — Large-file optimization:** build on extent lookup and bounded I/O after file-layout semantics are stable.
3. **2.0 — Parallel I/O:** only after lock/lifetime/journal/block-request contracts are explicit and stress-tested.
4. **2.1 — SSD TRIM/discard:** after free-space transaction ordering and block-device discard capabilities are defined.
5. **2.2 — Online resize:** after allocator, journal, device-capacity, and mount-quiesce rules are stable; grow-only may be the first supported increment.
6. **2.3 — Storage pooling:** define device lifecycle, metadata, allocation, explicit layouts and failure/redundancy behavior.
7. **2.4 — Self-healing:** begin with read-only online scrub/detection; add repair only after the 2.3 redundancy layout provides verified good copies.

This order is a planning preference, not permission to bypass dependencies. Each feature is a separate, reviewable PR or tightly scoped sub-PR; do not bundle multiple Phase 3 features. A feature that cannot meet bounded-latency, bounded-memory, recoverability, and exact-candidate test requirements must remain deferred. No Phase 3 item is part of ProsenjitFS 1.0 or 1.1 by implication.

### Performance, responsiveness, and non-hang architecture requirements

These requirements apply to every release and every new feature. Feature completeness must never be achieved by making common filesystem operations unbounded or unresponsive.

- **Measure before optimizing:** establish a reproducible 1.0 baseline for sequential/random read and write throughput, small-file create/stat/unlink latency, directory lookup/enumeration, mount/recovery time, CPU use, and peak memory. Record workload, volume size, cache state, and test environment.
- **Set budgets from evidence:** define per-operation latency, memory, CPU, and write-amplification budgets after collecting the baseline. Compare each feature branch against that baseline; investigate and document regressions rather than hiding them behind averages. Track at least median and tail latency (p95/p99) where the harness supports it.
- **Bound all work:** no unbounded retry loops, indefinite spin-waits, unbounded queue growth, unbounded recursion, or whole-volume scans on ordinary open/read/write/stat paths. Long scans, scrub, snapshot cleanup, repair, and large metadata work must be chunked, resumable, cancellable where safe, and rate-limited.
- **Keep critical paths short:** avoid holding global registry, namespace, or allocation locks across slow device I/O or long scans. Use narrowly scoped locks, documented lock ordering, and per-volume/per-object synchronization where the kernel primitives safely support it. Do not introduce lock-free structures without a correctness proof and stress tests.
- **Bound memory and stack use:** use explicit limits and checked allocation for caches, extent maps, ACLs, compression buffers, and queues. Avoid large stack buffers; respect the kernel's actual stack budget. Apply backpressure when bounded pools are exhausted instead of allocating indefinitely.
- **Batch carefully:** coalesce adjacent block I/O and metadata updates where safe, but cap batch size and transaction duration so a large request cannot monopolize the filesystem. Preserve ordering and flush guarantees.
- **Cache with correctness:** use bounded caches for hot metadata/data only when there is a clear invalidation and recovery rule. Cache pressure must degrade performance gracefully, never corrupt state or block indefinitely.
- **Do not serialize everything:** avoid a single global filesystem lock for unrelated volumes or independent objects. Any concurrency increase must define lock hierarchy, lifetime/refcount rules, and race tests first.
- **Feature-specific stress tests:** include large files/directories, fragmentation, low-memory/cache-pressure, full-volume, I/O-error, concurrent access where supported, cancellation/unmount, and recovery workloads. Verify bounded completion or a defined error under every injected failure.
- **No unsupported performance claims:** report measured results and test conditions. Do not claim “no lag”, real-time guarantees, or general concurrency safety without repeatable evidence.

### Roadmap governance

- Keep this file as the sole filesystem roadmap and status source; do not create a second filesystem roadmap document.
- Keep the 1.0 required feature list unchanged. New capabilities belong only in the post-1.0 roadmap and must be explicitly marked planned, implemented, tested, or exact-candidate verified.
- Work one feature/PR at a time. Do not start the next feature while the current feature has unresolved correctness, performance, security, or CI failures.
- Each feature PR must update this document with its design decision, on-disk compatibility/migration impact, tests, measured performance impact, limitations, and exact commit/workflow evidence.
- Never merge based on source presence alone; require successful relevant workflows on the exact PR head and review all failed/cancelled jobs.

### Release separation reminder

FAT32 and exFAT are separate compatibility providers targeted for 1.0; EXT/EXT4 and NTFS are deferred to later releases. These formats are not part of native ProsenjitFS. The native feature schedule above applies only to ProsenjitFS.

## REM-12 — FAT32/exFAT and USB storage foundation status (2026-11-01)

- **FAT32 provider:** implemented and tested through the pinned patched FatFs R0.16 adapter. Deterministic fixtures cover mount/probe, create/read/write/truncate, append, file rename, directory enumeration, EOF and 4-GiB boundaries, read-only remount, flush retry, block-I/O failure propagation, corrupt FAT-chain rejection, invalid directory-entry start-cluster rejection, and file-size-versus-allocation-chain mismatch (PR #390). Directory rename remains unsupported until cached descendant vnode paths can be updated safely.
- **exFAT provider:** implemented through the same pinned patched FatFs core with exFAT and 64-bit LBA enabled. Deterministic fixtures cover mount, file I/O, multi-sector/multi-cluster operations, UTF-8/Unicode LFN paths, allocation-bitmap validation and sector boundaries, FAT-chain/bitmap disagreement, invalid next-cluster rejection, entry-set checksum/secondary-entry validation, partial I/O failure, close/flush retry, ASCII/Unicode file rename, truncate/EOF, read-only remount, 64-bit read-at-EOF, and stream-extension lengths exceeding allocated clusters (PR #390).
- **FAT32/exFAT geometry hardening:** PR [#393](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/393) added exFAT FAT/heap overlap protection and was merged as `43da43a324485bf369bb9b15ef2fbe3ecd2c9c75`. This is metadata geometry validation, not physical-media certification.
- **exFAT corruption containment:** PR [#396](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/396) added a runtime read-only disk gate and quarantines mounted exFAT volumes when FatFs reports `FR_INT_ERR`. Its final source head `d9b71b4a89060f3cc20a45915bae45526e7a8989` passed Build/boot-test [run #41321334211](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41321334211) and Security architecture [run #41321334225](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41321334225); it was merged as `9aa1aa5cc014fade46b09cdade176edacef358d9`. This is conservative write containment only; no on-disk repair is performed.
- **USB controller groundwork:** PR [#394](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/394) added PCI USB host-controller classification and merged as `a3f23612c3e935247b4e52fb5d5d924539d798d9`. PR [#395](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/395) added uncached PCI memory-BAR mapping without enabling bus mastering; its exact head `a43efe6fa5936eff671e48844b6eae42b89e2522` passed Build/boot-test [run #41319170933](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41319170933) and Security architecture [run #41319170973](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41319170973), and merged as `c917a4f573be3a77b7b638620cb712b379e99deb`.
- **Verification boundary:** FAT32/exFAT evidence remains deterministic synthetic-media and QEMU coverage, not physical-device certification. PCI USB classification and MMIO BAR mapping are prerequisites only. xHCI/EHCI host-controller initialization, controller DMA/ring setup, real USB device enumeration, USB Mass Storage transport binding, block-device registration/lifecycle, partition-to-provider mounting from a physical USB device, surprise removal, safe physical unmount, and physical FAT32/exFAT drive validation remain outstanding. Do not call USB removable-media support hardware-complete until those gates pass.

## FS-32 — Invalid journal target geometry regression (2026-10-28)

- Added on current-main-based candidate branch `fix/prosenjitfs-invalid-journal-geometry-current-main-20261028`.
- Builds checksum-valid committed journal records whose second target is reserved block 0, reserved block 1, `total_blocks` (out of range), or the journal region start.
- Requires mount recovery to return `PROSENJITFS_ECORRUPT` and preserve durable, volatile, and dirty mock-device CRCs.
- **Status: verified on exact PR #271 head** `88516324a1007753aa43b213bd17bb0c5ea2700b`. Build/boot-test [run #40674196387](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196387) and Security architecture [run #40674196316](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40674196316) both passed; merged into `main` as `965e70902c725caae280b6403f851c93ed8d7059`. The regression covers reserved/out-of-range journal target geometry on a deterministic mock device only; it does not establish physical-drive power-loss durability or general VFS concurrency safety.
- Scope is deterministic mock-device validation; this does not establish physical-device power-loss durability or general VFS concurrency safety.

## FS-34 — Trusted permission-enforcement source audit (historical, 2026-10-28)

- **Historical result at the time of this audit:** Version 1.0 permission enforcement was a release blocker and no fix was yet claimed. This snapshot was superseded by the implementation and verification recorded in FS-40, FS-41, and FS-42; it is retained as the original source-audit rationale. Current `kernel/vfs.c` now checks owner/group/other permissions through the trusted authorization path, in addition to open-handle flags and read-only mount state.
- `kernel/prosenjitfs.c` initializes and persists inode `uid`, `gid`, and `mode` fields, but the current source audit found no use of those fields in authorization checks. `kernel/include/vfs.h` exposes no effective-credential context in its file-operation interfaces; `kernel/include/process.h` has process capability policy, which must be connected through a trusted authorization path rather than inferred from inode metadata.
- Missing acceptance evidence includes owner/group/other read-write-execute/search/list checks, create/delete/rename and permission-change authorization, default-deny for unknown credentials, and denied-operation tests proving durable/volatile/dirty media are unchanged.
- Next safe implementation gate: define the effective credential source and VFS authorization API first; enumerate every path-based and open-handle operation; then add allow/deny tests and exact-head CI. Do not bolt isolated mode-bit checks into ProsenjitFS callbacks or trust caller-supplied UID/GID.
- This is a source-level audit only. No runtime permission enforcement or release readiness is claimed.

## FS-35 — UTF-8 directory-name boundary regression (2026-10-28)

- Added regression coverage for empty/reserved names, overlong encodings (2/3/4-byte forms), isolated continuation, truncated sequence, invalid middle continuation, surrogate scalar, leading byte above U+10FFFF, valid 1–4-byte UTF-8, U+10FFFF, exact 255-byte acceptance and 256-byte rejection.
- Public VFS integration creates/removes valid UTF-8 names and a 255-byte component; malformed UTF-8 and 256-byte components must be rejected before metadata mutation, preserving durable/volatile/dirty mock-media CRCs and a clean integrity scan.
- **Status: PASS — exact-head CI verified.** Merged PR [#283](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/283), exact head `7025ca4f33d958462bb249fd964f7b9a28d5d603`, passed Build/boot-test [run #40709512778](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40709512778) and Security architecture [run #40709512354](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40709512354). Integrated on `main` by merge commit `55a13388ee569b1fbabcab9a902912e255419c9b`.
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

The current `include/vfs_provider.h` and `kernel/vfs_provider.c` provide a kernel-compiled provider registry, ABI v2 probe/mount/sync/unmount callbacks, provider mount pins, and a generic mount lifecycle bridge. `kernel/storage.c` can probe registered partition-aware providers during partition discovery and retain the selected provider in the discovered volume. This remains a **kernel-compiled provider foundation**, not a userspace plug-in runtime: the generic VFS/provider bridge and a concrete FatFs-based FAT32/exFAT provider are present, including vnode/file operations. There is no isolated userspace provider process, IPC request path, sandbox, or device broker. Exact-main Build/QEMU verification for FAT32/exFAT provider mount/I/O/remount passed on merge commit `4bb8f1904ea30095d667b2743733d431854b29ab` (run [#9719](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41694214966)); this is synthetic-media/QEMU evidence, not physical removable-device certification.

### Compatibility filesystem source audit

The former `docs/NTFS-WRITE-STATUS.md` describes NTFS write behavior and QEMU markers, but the current branch's source/build tree does not contain an NTFS implementation source, an NTFS filesystem enum, or an NTFS object in the build. Treat those statements as historical notes, not evidence of current NTFS read/write support. FAT32 and exFAT provider source and configured QEMU mount/I/O/remount checks are verified on the exact post-merge main commit as recorded in the current status summary; physical USB end-to-end support remains unverified and is still a release gate. No unsupported volume may be auto-mounted or written.

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

- **FAT32 provider — implemented; deterministic boot-fixture coverage verified on merged PR heads:** the separate FatFs R0.16-backed provider, VFS file/directory operations, partition-local block adapter, and upstream FatFs classification are present. The synthetic FAT32 fixture exercises mount, create/read/write/truncate, EOF and 4-GiB boundaries, append, file rename, directory semantics, open-handle sync, unmount, flush-failure/unmount-retry, FAT16 rejection, and read-only remount. Additional exact-head regressions cover write-failure propagation (PR #359) and block-read failure/recovery (PR #362). Directory rename remains explicitly unsupported until cached descendant vnode paths can be updated safely. These are deterministic in-memory/QEMU tests, not physical-device certification.
- **EXT/EXT4 provider:** planned for a later release; no current 1.0 requirement. Do not imply it exists or is tested without source and exact-head test evidence.
- **exFAT provider — implemented; initial deterministic mount and file-I/O coverage verified:** upstream FatFs exFAT support is enabled and selected only after upstream FatFs classification. The synthetic valid exFAT fixture now covers mount/unmount and file create, write, seek, readback comparison, close/reopen/readback, unlink, and unmount (PR #361 and #364). Malformed-media rejection and fragmented allocation-bitmap rejection are covered by PR #360 and #363. Unicode LFN and allocation-bitmap/cluster handling are delegated to the pinned FatFs source; dedicated Unicode filename, directory-mutation, large-file, broader corruption/fault-injection, and real-media conformance tests remain outstanding.
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

The v1 inode reader admits a record only after CRC32C and semantic fields validate. Non-free records must use a known v1 type, contain only defined v1 flag bits, have a nonzero generation and an in-range parent inode; logical size and block-aligned allocated-byte accounting must fit within the volume's data-area capacity. Directory inodes must keep file size, allocated file bytes, and extent head at zero because directory contents live in the separate directory-index block. The sparse flag is valid only on regular-file inodes, and only directory inodes may carry a nonzero directory-index pointer. Extent chains and directory index/entry graphs are validated separately by their bounded readers and the read-only integrity scan. These checks do not change the v1 disk layout or make the low-level inode writer a validator; that writer remains usable by corruption-injection tests.

Each directory index's embedded inode ID is checked against the owning inode number supplied by its caller, in addition to validating the index checksum, format version, generation and bucket heads. A checksum-valid index pointing at a different in-range inode is corruption, not a second namespace view. This is a reader-side check within the unchanged v1 layout.

Namespace lookup validates cross-record relationships: an entry must reference an allocated inode, the entry type must match the inode type, and the child's recorded parent must be the containing directory. Direct directory self-links are rejected. The read-only integrity scan remains responsible for global duplicate ownership and longer parent cycles.

Both path lookup and directory enumeration enforce the same child-reference invariant: the directory entry must reference an allocated inode, its declared type must match the inode, and the child's parent must equal the containing directory; direct directory self-links are corruption. This prevents listing from exposing inconsistent metadata merely because it follows the enumeration path rather than normal lookup.

## Directories and Unicode
Directories use deterministic hash buckets pointing to chains of directory-entry blocks rather than a fixed-size entry array.
Each entry contains inode number, node type, UTF-8 byte length, UTF-8 name, name hash, entry checksum and next-entry link. The v1 flags byte is reserved and must be zero; readers reject non-zero values even when the entry checksum is valid.
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
- Persistent inode/block bitmaps back allocation. Files map logical to physical blocks through linked extents. Directories use a 32-bucket hash index and linked entries. Version 1 does not implement hard links: the on-disk inode link-count field must equal one for every allocated inode, and checksum-valid records with any other value are rejected. Names are case-sensitive UTF-8 byte sequences with a maximum of 255 bytes; the documented policy performs no Unicode normalization.
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

- [x] Format publication ordering: PR #542 merged after exact-head Build/boot-test and Security workflows passed. Metadata is flushed before either valid superblock is published, followed by the final publication flush; failure-injection covers both barriers and simulated power-loss outcomes. Physical-device power-loss durability remains a separate acceptance gate.
- [x] Targeted P4 boundary regressions are now present in the current source: block/inode bitmap tail bits; inode semantic admission including mode/reserved bytes, flags, links, generation and parent range; extent-chain cycles, logical overlap, same-start and partial physical overlap, and metadata/data self-alias; plus device-relative LBA overflow and end-of-device rejection. PR #538 adds the partial physical-overlap case and PR #539 adds inode identity cases; both passed exact-head Build/boot-test and Security workflows before merge. The LBA helper tests reject arithmetic overflow and the first sector outside the device. This marks these named test cases covered, not the whole P4 source audit complete.
- PR #499 added checksum-valid regressions for journal/bitmap overlap, zero-length journal, block-bitmap capacity, inode-table capacity, and data_start consistency; exact-head verification passed on `49ba8cf0d186347750ce36f47a7dfefa9d602d1a`: [Build/boot-test #10022](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42493329317) and [Security #9519](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42493329325). PR #500 adds fixed-header admission cases; exact-head verification passed on `6389ec360a1660071608d5db5cc9a4fc52e7633f`: [Build/boot-test #10032](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42504407241) and [Security #9529](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42504407237).
- [x] Source/test coverage for checksum-valid inode type/flag/generation/parent/size/allocated-byte admission passed on exact source head `70ed44cd45987682ac50f9fbfd1b82a510e25033` (Build #8548 and Security #8084). P4 remains open for remaining boundary families.
- [x] Directory metadata corruption coverage: checksum-valid full-name-hash mismatch (PR #496); on-disk malformed UTF-8, zero-length and overlong names (PR #498); wrong buckets; duplicate names/references; self-cycles and chain cycles. Existing self-tests also reject child-entry type/inode-type mismatches, child-parent mismatches, and disconnected parent cycles. PR #498 exact-head verification passed on `ced8e7cfafc210315658801f2db54baeb0d5fb1c`: [Build/boot-test #10019](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42492615135) and [Security #9516](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42492615055).
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