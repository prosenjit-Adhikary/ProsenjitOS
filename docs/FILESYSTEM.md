# ProsenjitOS Filesystem — Single Source of Truth

**Canonical filesystem document:** this is the only active planning, architecture, implementation-status, test-matrix, release-scope, and verification document for every ProsenjitOS filesystem.
**Policy:** do not create separate filesystem roadmap, completion-matrix, test-matrix, implementation-plan, or release-spec files. Update this file instead.
**Last consolidated:** 2026-11-14.

### xHCI deferred port-event pump — 2026-11-13 (PR implementation)

- Added a bounded, fail-fast serialized `pros_xhci_poll_port_events()` API. It consumes deferred Port Status Change Events only between command/transfer transactions, records valid one-based port IDs in the existing pending bitmap, and acknowledges each consumed port event. A non-port event is inspected without consuming it, so command/transfer completions remain available to their owning transaction.
- Added a deterministic fake-MMIO/event-ring self-test covering one valid port event, pending-bit consumption, an empty ring, preservation of an unexpected non-port event, and invalid zero event budget.
- This is an event-ring service primitive, **not yet a hotplug worker**. There is still no persistent kernel runtime consumer after boot enumeration, no disconnect-to-MSC detach/re-enumeration path, and no controller-quiescence proof permitting DMA release. Do not claim USB hotplug or safe controller detach complete from this API alone.
- The self-test must pass in the exact-head Build/QEMU workflow before the change can be considered verified. Hardware disconnect/reconnect and DMA ownership fault injection remain separate release gates.


### Mutation-preflight scanner feasibility audit — 2026-11-12 (PR #573)

- Audited the exact PR-head implementation of `pfs_consistency_preflight()` and its shared `pfs_selftest_consistency()` checker. The preflight is deliberately a whole-volume consistency gate: it validates inode allocation/records, directory ownership and references, extent ownership, reservation registries, and the complete block-allocation bitmap before a public mutation proceeds.
- A per-inode/per-range targeted scan cannot preserve the current rejection contract by itself: it cannot discover an unrelated orphan inode/block or a corrupt ownership alias elsewhere on the volume. Replacing the full scan with a local check would silently weaken the guarantee that mutation is refused when pre-existing unrelated corruption exists.
- **Decision:** retain the full-volume preflight for the current v1 release; do not ship a targeted-scan shortcut or trust an unpersisted cache as authoritative. A future scalability redesign requires an authoritative ownership index with explicit mount validation, mutation-generation/invalidation rules, crash-recovery semantics, and corruption/fault-injection tests proving it cannot become stale. The public read-only integrity API remains a full scan.
- Current checker complexity is bounded by volume blocks, inode count, directory entries, and extent blocks; it allocates a volume-sized ownership bitmap plus inode-bounded graph/name-index scratch buffers. Allocation failure is fail-closed (preflight returns an error before mutation); performance and low-memory behavior still need measured stress coverage.
- **Verification:** source-level audit only. Exact-head Build/boot-test and Security workflow results are tracked separately; this documentation decision does not claim additional runtime coverage or close FS-11/FS-12/FS-13/FS-14.







### Allocation mutation preflight hardening — 2026-11-12

- Added the same ownership-consistency preflight to the public low-level allocation wrapper that already guards preallocation and free operations.
- Allocation now refuses to mutate a volume containing pre-existing unrelated orphan blocks, invalid ownership, or directory/inode consistency failures. Existing exact current-mount block reservations remain admissible through the registry.
- The check runs under the filesystem I/O lock and before allocator scanning/transaction staging, preventing a new reservation from being layered onto an already inconsistent volume.

### Orphan-range isolation regression fixture — 2026-11-12

- Extended the ambiguous-commit recovery fixture to reserve two contiguous blocks, then crash/remount so the volatile registry is empty.
- The fixture first attempts to free only the first block. The preflight must reject the operation because the adjacent second block remains an unrelated unregistered orphan, and a follow-up bitmap check requires both blocks to remain allocated.
- It then frees the complete two-block recovery range and requires a clean public integrity scan. This guards against accidentally broadening the one-range recovery exception into blanket orphan acceptance.
- Fixture added to source; execution remains pending runner availability.

### Mount publication hardening — 2026-11-12

- Reordered mount initialization so the exact block-reservation registry is allocated and zeroed before `vfs_mount_activate()`. An active mount is now published only after its volatile allocator proof storage is ready.
- Allocation/overflow/activation failure paths release the registry, abort the not-yet-published mount, and free transaction/scratch buffers. This removes the previous activate-then-deactivate rollback path and avoids ignoring a deactivation failure.
- Source-level cleanup-path review completed; runtime mount fault-injection remains pending runner availability.

### Block reservation recovery hardening — 2026-11-12

- Cross-check found a recovery-path interaction: after an ambiguous allocator commit and remount, the volatile registry is intentionally empty, so the strict orphan scan would reject the very orphan range that the caller is trying to reclaim. The allocator recovery fixture (FS-75) already exercises this case.
- Updated mutation preflight to exempt only the caller-supplied free range from orphan rejection. The range is bounds-validated and every target block is separately checked against the ownership map before any bitmap update. All unrelated unregistered orphan blocks remain rejected; public integrity scans remain strict.
- Static call-path review confirms the ambiguous-commit recovery path can now reach its ownership check and atomic free transaction. Runtime fixture execution remains pending the offline runner.

### Exact volatile block reservation registry — 2026-11-12 (PR #573)

- Added a mount-scoped bit registry indexed by the exact physical block number. The registry is sized from the validated on-disk `total_blocks` geometry, zero-initialized on mount, and released on unmount; it is never persisted.
- The low-level contiguous block allocator checks that the candidate range has no stale reservation bits before committing the bitmap, then registers every exact block only after a successful allocator transaction. A registry inconsistency after a durable allocation fails closed by forcing the mount read-only.
- Mutation preflight no longer accepts every unowned allocated block indiscriminately. It admits an unowned allocated block only when that exact block is present in the current-mount reservation registry. Public integrity scans remain strict.
- Successful low-level frees clear only the freed physical range after the bitmap transaction commits. This supports partial-range release without erasing reservation bits for the still-allocated remainder. Failed/ambiguous frees retain the volatile proof; an ambiguous transaction already forces read-only operation.
- The keep-size preallocation path remains an atomic bitmap+zero-data+extent+inode transaction and does not use the separate low-level reservation registry because it publishes ownership in the same transaction.
- Added source fixtures for exact registration, duplicate/overlap rejection, remount invalidation, and partial-range release. Runtime execution remains **PENDING RUNNER**; no build, self-test, QEMU, or CI was run while the local runner is offline. Keep PR #573 Draft and do not merge until exact-head runtime verification succeeds.

### `write_inode()` semantic admission — 2026-11-12 (PR #573 source implementation)

- Extracted the v1 inode semantic contract into one shared validator used by both `prosenjitfs_read_inode()` and the public writer.
- The writer now rejects invalid inode type/flags, unsupported mode bits, non-zero reserved bytes, invalid v1 link count, zero generation, out-of-range parent, directory size/allocation/extent misuse, sparse flags on non-files, non-directory index ownership, over-capacity size, and unaligned/over-capacity allocated-byte accounting.
- Validation occurs before the inode-table pre-read, transaction reset, or journal staging. Invalid caller input returns `PROSENJITFS_EINVAL` without changing durable, volatile, or dirty media.
- FREE records remain writable only in the release representation: zeroed metadata with the incremented generation retained. The generation tombstone is required to prevent stale vnode identity from being reused.
- Added a private self-test raw-media injector so checksum-valid malformed records can still be injected deliberately without weakening the public writer. Converted link-count, general semantic, and allocated-FREE corruption fixtures to this path.
- Expanded writer-admission coverage to 11 root-directory semantic cases, plus invalid link-count (zero/multiple), non-directory directory-index, and malformed allocated-FREE record cases. Each public-writer rejection requires unchanged durable/volatile/dirty media. The tests also require read/integrity rejection of the deliberately injected malformed records and a clean integrity scan after restoration.
- Source-level checks completed on code commit `e549adc50bebae8031ac33ed0117a7963a81d2e8` (documentation subsequently updated on the PR branch). No compiler, build, self-test, QEMU, or CI execution was performed because the self-hosted runner is offline per user instruction. Runtime verification remains **PENDING RUNNER**; keep PR #573 Draft and do not merge before exact-head checks pass.


### Native ProsenjitFS source-audit checkpoint — 2026-11-10 (initial pass)

**Source snapshot inspected**
- `kernel/prosenjitfs.c` blob: `78af66f311d1cef92270143de00f877a3717d758`.
- `include/prosenjitfs.h` blob: `58d910c7fdf87aeef3e87b74a65d1da7ddd80875`.
- The current `main` tip at the time of this inspection was documentation commit `e63b5750a35ccbae422ac2b9f32b92ae18e7d68e`; the source file itself was unchanged by that documentation commit.

**Initial source areas reviewed**
- Journal recovery and commit admission: record-count bounds, journal-range exclusion, duplicate-target rejection, commit transaction-ID/count matching, zeroed unused target slots and reserved padding, checksum validation, replay flush, and journal retirement.
- Allocator admission and range checks: data-region bounds, device geometry, bitmap index bounds, and transactional staging.
- Integrity scan: inode bitmap versus inode records, directory-entry ownership, duplicate physical block detection, extent logical/physical range overflow checks, inode allocated-byte accounting, orphan inode/block reporting, and allocation-bitmap-to-owner comparison.
- Truncate: sparse growth ordering, tail-zero-before-EOF-publication, shrink preflight against the bounded journal, and transactional metadata publication.
- Namespace mutation: unlink and rename code paths were sampled for their transaction construction and parent/entry updates; this is not yet a complete crash-stage audit.

**Findings from this pass**
- No reproducible defect has been established from the reviewed sections, so no speculative code change was made.
- Journal target validation correctly treats blocks 0 and 1 as reserved and excludes the journal region; the final physical block is not a backup superblock in this v1 layout. Do not add a blanket last-block exclusion.
- The integrity checker is read-only and builds ownership from inode/directory/extent references before comparing it with the block bitmap. Orphan reports are diagnostic candidates only; they do not authorize automatic reclamation.
- Existing truncate implementation deliberately bounds affected extents before staging a shrink transaction. This needs targeted failure-injection review, not an assumption that all truncate crash states are already covered.

**Next audit actions — pending**
1. Finish unlink/rmdir/rename transaction review, especially same-directory bucket moves, cross-directory moves, replacement semantics, and inode-generation/reference behavior.
2. Build a crash-stage matrix for truncate and unlink, including every journal flush boundary and persist-then-EIO outcomes; prepare tests but do not execute CI while the self-hosted runner is offline.
3. Continue allocation ownership review with deliberately corrupted but checksum-valid inode/extent/directory fixtures, including shared extent metadata, duplicate data ownership, and orphan reservations.
4. Record a defect only when source tracing yields a concrete violated invariant and a deterministic regression fixture. Then implement a focused patch and keep its verification state **PENDING RUNNER** until exact-head Build/QEMU and Security checks pass.

**Execution constraint:** no CI workflow was triggered for this audit. The local self-hosted runner remains offline per the user's instruction.

### Native ProsenjitFS checkpoint — 2026-11-10 (runner offline; source work checkpoint)

**Verified merged implementation through FS-75**
- PR [#569](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/569) and PR [#570](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/570) complete FS-74 public inode-read error handling. Read failures and checksum/semantic validation failures do not partially overwrite the caller's inode output; the tests require the expected error and unchanged output. PR #570 exact-head Build/boot-test run [#42821076730](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42821076730) and Security architecture run [#42821076342](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42821076342) passed before merge.
- PR [#571](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/571) reconciles the filesystem documentation with the verified FS-74 results; merged as `12203efbde3f920c335d8afa62e6241d44ea7fea`.
- PR [#572](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/572) completes FS-75 ambiguous allocation-commit recovery. Exact tested PR head `da8964b7a8c02a36bf5823dd7538182b00f1fe74` passed Build/boot-test run [#129859090746](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/129859090746) and Security architecture checks before squash merge as `9f539df168b0b7bdb48e638dffbf7c2fc79e9e8`. The regression injects a persist-then-EIO at the third transaction flush, simulates crash/reboot, remounts, confirms journal replay retained the allocation bit, reclaims the unowned reservation, and requires a clean integrity scan. This is mock-device/QEMU evidence only, not physical power-loss certification.
- The FS-75 API contract distinguishes a definite allocation failure from an ambiguous commit: on EIO with the mount forced read-only, the candidate block is preserved as indeterminate; callers must not use it before remount/recovery and may reclaim it only after recovery confirms it is allocated and unowned.

**Current verification boundary**
- These statements describe merged PRs and their exact-head pre-merge workflow evidence. Do not reinterpret older passing runs as fresh verification of a later main commit.
- Local self-hosted runner is currently offline and is expected to return in approximately two days (user-reported). Therefore, no new Build/QEMU or Security workflow has been triggered for this checkpoint. New changes made during this interval must remain unverified/pending until the runner is online and exact-head checks pass.
- PR [#459](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/459) is the self-hosted-runner-only workflow change. Its previously inspected exact-head checks passed, but it is not evidence that a new workflow run can execute while the local runner is offline. Do not fall back to GitHub-hosted runners.

**Next native-filesystem audit queue — not yet completed**
1. Re-audit allocation bitmap versus inode-owned extent accounting, including orphan reservations, duplicate ownership, reserved/system blocks, and bounds/overflow.
2. Audit journal sequence/replay invariants and recovery behavior across every transaction barrier.
3. Audit truncate crash stages, rename/link atomicity, dirty-unmount behavior, and mount/unmount rollback.
4. For every confirmed defect, create a focused implementation + regression-test change; keep one-PR-at-a-time merge discipline and do not merge until exact-head Build/QEMU and Security checks pass.
5. FatFs work remains intentionally paused; this queue is for native ProsenjitFS only.

**Progress accounting rule:** FS-74 and FS-75 are implemented and merged. The audit items above are remaining work, not completed features. Do not publish a new overall completion percentage until the remaining requirements are checked against current source, tests, and release gates.

### Native ProsenjitFS integration update — 2026-11-10

- **Directory namespace regression matrix:** PR [#534](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/534) merged as `b2ac52c96ca7e5cf9c90d85d88eaba3e077009e0`. The change tightens directory-entry validation, shares strict validation with rmdir scans, uses the public integrity API in the namespace self-test, and splits rename/old-path diagnostics.
- **Multi-block write crash recovery:** PR [#567](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/567) adds FS-72, a two-block write matrix targeting each of the second block transaction's five flush barriers in pre-persist and persist-then-EIO modes. It verifies the first committed block remains intact, the second block is either absent or fully replayed, and remount/integrity checks succeed. Exact-head Build/boot-test run [#42817588937](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42817588937) and Security architecture run [#42817588961](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42817588961) passed on `5df665764fe7ded83a6b6744a0dc2c9f09427233`; PR #567 was squash-merged as `f2d12b9ee06184a577af99d31af13a35b9dff81f`.
- **Three-block write crash recovery expansion:** PR [#568](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/568) adds FS-73, extending the matrix across all three per-block transactions, all five flush barriers, both pre-persist and persist-then-EIO modes, plus a three-block success/remount control. Exact final PR head `e3904c0d6803104c3753b8bba485838267ab7a8a` passed Build/boot-test run [#42819255106](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42819255106), including ISO and native BIOS QEMU boot, and Security architecture run [#42819254990](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42819254990). PR #568 was squash-merged as `0031cc4b2e8eb824fa15a86cb011021ff48f64f8`. Mock-device/QEMU evidence only; no physical power-loss certification.
- **Public inode I/O read-failure regression:** PR [#569](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/569) adds FS-74 coverage that `prosenjitfs_read_inode()` preserves its output buffer and returns `PROSENJITFS_EIO` on a device read failure; `prosenjitfs_write_inode()` must not stage a transaction or mutate durable/volatile/dirty media when its read-modify-write pre-read fails. It also verifies a successful retry and integrity scan. PR #569 merged as `3b433c342fc252e74e31c1b49352ca7751c7cab3`.
- **Inode read failure atomicity:** PR [#570](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/570) extends FS-74 so `prosenjitfs_read_inode()` decodes into a private candidate and copies to the caller only after checksum and semantic validation. A checksum-corruption injection requires `PROSENJITFS_ECORRUPT` and byte-for-byte unchanged caller output. Exact-head Build/boot-test run [#42821076730](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42821076730) passed ISO build, ProsenjitFS diagnostic, exFAT formatter verification, ISO QEMU boot and native BIOS QEMU boot; the runtime logs contain the ProsenjitFS format/mount/allocation/journal/recovery self-test success marker on both boot paths. Exact-head Security architecture run [#42821076342](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/42821076342) passed. PR #570 squash-merged as `c385232b7c16862ff4b7aa870f1825e38d49c42b`. Mock-device/QEMU evidence only; no physical power-loss certification.
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
`prosenjitfs_alloc_inode()` initializes its output to zero before lock acquisition
and consistency preflight. Ordinary failures leave zero; only an error after the
ambiguous-commit boundary may preserve the candidate, with the mount forced read-only.


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
- [x] Historical behavior on this 2026-10-25 head preserved the caller's sentinel on pre-commit failure. The 2026-11-12 contract supersedes it: initialize to zero, keep zero on ordinary failures, and preserve a candidate only for ambiguous durable-commit EIO.
- [x] Current-head regressions check zero output plus unchanged durable/volatile/dirty media for preflight and pre-commit failures, and preserve the candidate only for persist-then-EIO.
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

- `prosenjitfs_alloc_inode()` now zeroes its output before lock/preflight; ordinary failures leave zero, while ambiguous durable-commit EIO preserves the indeterminate candidate and forces read-only state.
- Current-head preflight and pre-commit regressions require zero output and unchanged mock-media CRCs.
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
| On-disk format / superblock geometry | Implemented; validator enforces region order/bounds, bitmap and inode-table capacity, root inode, and supported 16..64-block journal bounds. PR #579 adds checksum-valid undersized/oversized journal fixtures for both redundant copies. | Continue the feature/version/region-overlap corruption matrix and exact-head verification. |
| Mount and journal recovery ordering | Merged PR #179 includes journal-slot invalidation, contradictory commit rejection, fail-closed handling, and recovery-restart regression coverage; consolidated build/boot and security workflows passed. | Continue adversarial recovery/failure-injection review and real-device durability validation; QEMU/mock-device tests cannot prove physical power-loss behavior. |
| Block bitmap and allocation ownership | Integrity checker marks metadata references, compares allocated bits to ownership, checks metadata bits and out-of-range tail bits, and reports unowned allocated data blocks in report mode. | PR #191 merged as `a843f4ac1d62ef27c6fe3a06d03359783a50b5c2`; its exact head passed build/boot-test #7824 and security architecture #7360. Checksum-valid block-bitmap and inode-bitmap tail-bit regressions verify out-of-range allocation bits are rejected without scan-side media mutation, then pass after restoration. Continue boundary and read-failure coverage. |
| Inode bitmap and inode metadata | Merged PR #179 validates allocation state, inode checksums/types, parent range, bitmap tail bits, and inode sizes against volume capacity; checksum-valid oversized-inode regression passes. | PR #530 extends the seven checksum-valid malformed-root-inode cases to the public read-only integrity-report API, requiring `ECORRUPT` for every case and unchanged durable/volatile/dirty media views before restoring the valid root. Malformed root-inode preflight failures now carry a bounded source-line `first_error_location`; the seven checksum-valid root cases assert nonzero, in-range report locations. Continue broader generation/checksum and stable report-detail review. |
| Directories and names | Checker validates directory-index and entry bounds, name length/UTF-8/hash/checksum, duplicate names in a bucket, child type/parent, duplicate references, and parent chains to root. | PR #184 adds a checksum-valid directory-entry self-cycle regression, checking scan-side media immutability and clean rescan after restoration. PR #187 merged as `6e9e70676617d8f128ca03134cd1c8ebe0eca829`; its exact head passed build/boot-test #7802 and security architecture #7338. The checker now rejects entries whose hash does not map to their containing bucket, with a checksum-valid wrong-bucket regression and read-only media-state checks. PR #192 merged as `2abc3feffba5a5d2941b8580e7af06c3f5427c99`; its exact head passed build/boot-test #7831 and security architecture #7367. It creates two distinct filenames that intentionally collide in the same directory bucket, corrupts one entry into a duplicate name with a valid checksum, and verifies read-only rejection and clean rescan after restoration. PR #585 strengthens the repeated-inode-reference fixture with a nonzero bounded source error location, zero orphan counters, deterministic repeated-report checks, and clean-report field reset after restoration. Corruption-at-read-boundary cases remain follow-up. Review symlink namespace semantics against the actual API. |
| File extents and block ownership | Merged PR #179 validates bounded extent chains, logical ordering/arithmetic, volume-capacity bounds, block ownership and allocated-byte totals. PR #181 adds pre-iteration logical/physical range validation and a checksum-valid oversized-block-count regression; build/boot and security workflows passed on exact PR head `a237ebd02e77609b47258ff18a4677857bbd8350`. | PR #182 adds checksum-valid zero-length and out-of-volume physical-start regressions; build/boot and security workflows passed on exact PR head `666a7e2f96f6c5dd0c7b11d1cf16602b08dafbaa`. PR #185 merged as `0e8e996f55df9da7840502d4dba4e5d0cb0934a8`; its exact head passed build/boot-test #7795 and security architecture #7331. The checksum-valid duplicate physical-block ownership regression checks read-only scan state and a clean rescan after restoration. PR #188 merged as `e5af0af4df3cf0d67eadbcedf799fb384f22d5de`; its security architecture check passed (run #7347). The exact PR build job was marked cancelled despite the listed ISO/native BIOS steps completing successfully, so do not count that run alone as a clean pass. The descendant `main` commit `569fb12b786a1bef4ba7ef8a13357b66af66707c` subsequently passed build/boot-test #7834, including ISO build, ISO QEMU boot, and native BIOS QEMU boot. The cross-inode regression is therefore covered by a passing descendant build, but exact-head PR verification remains distinct. PR #189 merged as `2029eb371c6865a839f282b0538c59030f3c8e2c`; its exact head passed build/boot-test #7814 and security architecture #7350. The checksum-valid overlapping-logical-range regression confirms corruption is rejected without scan-side media mutation. PR #190 merged as `0026779c6006a72f21c1a824d2fd8ec403a05a17`; its exact head passed build/boot-test #7818 and security architecture #7354. The checksum-valid extent-chain self-cycle regression confirms repeated extent metadata is rejected without scan-side media mutation. PR #529 adds a checksum-valid extent metadata/data self-alias regression, verifying that one physical block cannot be claimed simultaneously as extent metadata and file data; the read-only scan leaves all mock-media views unchanged and succeeds after restoration. |
| Orphan data-block detection | Implemented as report-only candidate detection in the inspected branch. Report mode completes the bounded bitmap scan and returns corruption when candidates exist. | Preserve zero mutation of durable, volatile, and dirty state on all outcomes; no reclamation without a separately proven crash-safe design. |
| Orphan inode detection | Implemented as report-only candidate detection for allocated, structurally valid non-root inodes with no directory reference and a parent chain reaching root. | Test unusual namespace/link semantics and ensure candidates are never treated as safe-to-free solely from this report. |
| Structured integrity report | Existing report includes status, first error location, inode and bitmap scan counts, and first/count of orphan block and inode candidates. | Ensure error classification is stable and documented; verify bounded memory and report initialization for every early-return path. |
| Create/read/write/append/truncate/delete | Core mutation paths and deterministic self-tests exist in the current implementation. | Run normal, zero-length, block/extent-boundary, max-size, invalid-input, persistence and remount tests on the exact final commit. |
| Directory create/remove/rename/path traversal | Core directory and mutation paths exist. Rename and unlink now have read/write failure sweeps; rmdir has a cyclic-chain classification regression and read-failure sweep. Exact PR heads passed ISO QEMU, native BIOS QEMU, and Security architecture checks. | Continue mutation rollback/persistence tests for mkdir/create/truncate/append and remaining path/name edge cases; test allocation failures and all transaction boundaries. |
| Journal commit / replay / flush failures | Merged PR #179 invalidates reused commit slots, rejects contradictory matching-ID commit records, forces uncertain post-commit failures read-only, and tests recovery restart after interrupted replay. Combined build/boot and security workflows pass. | Maintain the physical power-loss caveat for simulated QEMU/mock-device tests; add real-device validation separately. |
| Failed I/O and resource cleanup | Accepted-then-EIO sweeps cover format (PR #578), mkdir, create-open, truncate-to-zero, same-size overwrite, unlink, append, rmdir, rename, and keep-size preallocation (PR #577). PR #581 sweeps every measured block-read position in empty-file preallocation, checks no media mutation on EIO, and verifies the zero-filled success/unlink control. Rename/unlink read/write and rmdir read-failure sweeps are also present. USB BOT mock tests exercise short IN/OUT transfers, CSW residue mismatch, stalls, and reset recovery; xHCI bulk paths report actual lengths/residuals and handle short IN packets; ATA PIO maps per-sector DRQ/status failures to block I/O errors. | Add controller-level injected mid-command ATA failures and xHCI/DMA ownership/transport fault tests; complete systematic read/flush injection across all mutation and journal boundaries, allocation/resource-exhaustion and rollback-cleanup tests; physical durability remains a separate gate. |
| Concurrency and serialization | VFS open-file operations, directory-handle read/close, mount lookup admission/unmount draining, per-mount namespace mutation admission, ProsenjitFS file/metadata I/O, integrity scans, directory lookup/readdir, and unmount now have scoped fail-fast gates (PRs #308–#316 merged). | Integrity scans must still be externally serialized against namespace mutations: mutator callbacks are protected by the VFS namespace gate, not the provider I/O gate, because authorization re-enters metadata callbacks. Atomic vnode refs still require an owned live reference or lifetime guard. No general SMP-safety claim. |
| Read-only/degraded mount behavior | Degraded-superblock read-only behavior is a stated architecture constraint in Issue #166. | Test that uncertain metadata/recovery states remain read-only and cannot mutate media. |
| VFS/provider and boot integration | Code-bearing PRs #223–#226 passed exact-head Build/boot-test (including ISO QEMU and native BIOS QEMU) and Security architecture CI; documentation logs and matrix were refreshed afterward. | Re-run full CI on any subsequent code changes; continue provider-boundary, resource cleanup, and hardware-backed validation. |
| CoW, snapshots, reflink, ACLs/xattrs, links, quotas, compression, dedup, online resize, relocation | Listed as long-term targets in README, not established as implemented by this audit. | Explicitly defer until design, API/on-disk compatibility, recovery semantics and tests are implemented. |
| Repair / orphan reclamation | **Deferred for safety.** The inspected scan is read-only and report-only. | Require proven reachability/link semantics, journaled or otherwise crash-safe repair, resumable interrupted repair, explicit operator-controlled mode, dry run, and a clean follow-up scan before implementation. |

## VFS concurrency hardening — 2026-10-30

- PR #308 merged as `2c6c3dadb990ad198fb5d4c5fd031ccd21f85919`: fail-fast serialization for shared open-file read/write/truncate/seek/close operations.
- PR #309 merged as `13a1cc96223211caa790cf6e588ead7431d3c768`: atomic vnode retain/release arithmetic with zero-resurrection and overflow rejection.
- PR #310 merged as `eff97aaa05f81dae84fb0f248f1d4178220f768f`: directory-handle read/close serialization.
- PR #311 merged as `02867378c532a4a2fb8f423a5bc54bb115166006`: atomic mount lifecycle admission and in-flight path-lookup draining before unmount.
- PR #312 merged as `6d67b40fbada4647705f4fd610ef7298580d63e7`: fail-fast per-mount serialization for unlink, mkdir, rmdir, rename and O_CREATE.
- Exact PR-head Build/boot-test and Security architecture workflows passed for each of these changes. PR #312's merged-main Build/boot-test and Security architecture workflows also passed on merge commit `6d67b40fbada4647705f4fd610ef7298580d63e7`.
- These changes do not make raw unowned vnode pointers safe, do not serialize provider inode data paths or read operations against namespace mutation, and do not remove the integrity scanner's caller-serialization precondition. They are not a general SMP-safe filesystem claim.

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
- General concurrent VFS safety, SMP/timer-preemption safety, scheduler-aware wait/wakeup and complete vnode/object-lifetime safety remain unproven. Scoped serialization between ProsenjitFS provider I/O, namespace mutations and integrity scans is verified in FS-46/47; it is not a general SMP-safety claim.
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

## FS-29 — Inode allocator output initialization and failure contract (historical verification on code/test head `520be738325a14025e6b00d56a5d940c13e9ce4f`)

- **Risk:** `prosenjitfs_alloc_inode()` assigned the candidate before its inode-bitmap transaction could fail.
- **Test:** fail the first journal write during direct inode allocation; require `PROSENJITFS_EIO`, zero output, unchanged durable/volatile/dirty mock-media CRCs, writable mount state and a passing integrity check.
- **Implementation:** zero output on entry; publish the candidate before the bitmap commit only so ambiguous durable EIO can be recovered.
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
| FS-11 | Other read/write/flush failure paths | Mock-device fault injection in ProsenjitFS self-test | Sweep reachable superblock/journal/bitmap/inode/extent/directory read/write/flush paths; no stale injection contaminates verification | **PARTIAL**; accepted-then-EIO recovery sweeps are merged for journal replay (PR #479), mkdir (PR #481), regular-file create-open (PR #483), unlink and `O_APPEND` (PR #484), `rmdir`/rename (PR #485), and truncate-to-zero/same-size overwrite (PR #486). Each bounded sweep simulates power loss/remount and checks complete old-or-committed-new state plus integrity. PR #488 adds unmount flush-failure retry coverage and format failure sweeps. The follow-up fault tests add initial invalidation-flush persist-then-EIO coverage (after power loss neither old nor new volume is probeable) and unmount persist-then-EIO coverage (dirty data is confirmed durable while mount state/buffers remain retryable): old superblocks are invalidated and flushed before metadata initialization; new superblocks are published only after journal, bitmaps, inode table and root directory are initialized, with final flush as commit point. The format sweep covers write positions, initial/final flush failure, persist-then-EIO ambiguity, post-power-loss probe and integrity. Existing operation-specific read-failure sweeps cover namespace/data paths. Flush-failure matrices cover all five transaction boundaries for metadata update (FS-48), unlink (FS-51), rename (FS-52), regular-file create (FS-53), directory creation (FS-55), append (FS-56), truncate (FS-57), and rmdir (FS-58). Verified lower-level coverage includes direct `pfs_read_block()`/`pfs_write_block()`/`pfs_flush()` error propagation, no-partial-read-buffer, unchanged-media, invalid-argument and successful-round-trip checks; deterministic partition probe/read outputs on MBR/GPT failures and malformed scans (FS-59–FS-61); primary/backup superblock probe/mount read-failure cleanup (FS-62); journal-recovery read-failure sweep across every generated transaction image with power-loss retry (FS-63); and writable-create preflight read-failure sweep with absent-path/media-preservation assertions and explicit disarming of the first unreached fault (FS-64). **Transfer limitation:** current block-device callbacks return status only, not transferred-byte counts, so ProsenjitFS cannot assert partial-sector byte counts at this API boundary; USB MSC short-transfer handling is a separate transport-level concern. Broader reachable lower-level fault inventory, USB transport audit, and cleanup/rollback audit remain open. FS-11 stays PARTIAL until every applicable reachable path is audited and exact-head CI passes.
| FS-12 | Integrity scan read-only guarantee | `prosenjitfs_check_integrity`, report API | Check ownership/reachability/checksums/bounds; report orphan candidates without mutating media; distinguish EIO from malformed on-disk data | **PASS for the bounded report-only mock-device acceptance matrix on exact descendant head `42053cb2c1f8c212a6daab759d47410b4cffdd67`**: symlink extent ownership/cross-inode directory aliasing (PR #573), multiple orphan blocks (PR #576), multiple orphan inodes (PR #584), repeated inode-reference report determinism/reset (PR #585), and sequential integrity-scan read-failure EIO/media-immutability sweep (commit `2ddc86ac087a90be018d8ab14a877d26be4bb060`). Exact-head Build/boot-test #10427 and Security architecture #9924 passed; Build log confirms ProsenjitFS self-test milestone in ISO QEMU and native BIOS QEMU. Report remains read-only/report-only: no orphan reclamation or repair. This closes FS-12's scoped gate only; FS-11, FS-13 and physical FS-14 remain separate. |
| FS-13 | Concurrency and object lifetime | VFS vnode refs, file/mount state, `io_lock` | Explicit lock contract and interleaving tests cover mutation vs scan and rename/unlink vs open file lifetime | Scoped fail-fast gates cover per-handle file operations, directory-handle read/close, mount lookup/unmount admission, namespace mutation, provider I/O and integrity scans (PRs #308–#321). **PR #344 is merged and exact-main CI verified:** a mount lifecycle pin spans the complete namespace-mutation wrapper, with regression assertions for unmount rejection and pin release; busy partition mounts are rejected before caller partition-view mutation, and `partition_device_init()` validates parent sector size/read callback before mutating the view. Exact-main Build/boot-test [#41106219343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41106219343) and Security [#41106219279](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41106219279) passed. **PR #474 adds FS-67:** a caller-held vnode returned by `vfs_kernel_path_lookup()` blocks unmount with `EBUSY`; the mount remains usable for metadata authorization, releasing the vnode permits unmount, and the same device remounts. Exact PR-head Build/boot-test and Security checks passed; QEMU ISO and native BIOS QEMU boot tests passed. This specifically covers a held file vnode and its parent-reference chain, not arbitrary concurrent teardown. **Documented concurrency limit remains:** callers must serialize operations on each mounted ProsenjitFS instance; do not assume general multi-threaded or SMP-safe filesystem access. Existing gates reject detected same-handle, namespace or provider-I/O contention with `EBUSY`; these scoped gates are not a global lock. The VFS refcount primitives require the caller to already own a live reference or an external lifetime guard; they do not make an unowned raw pointer safe. Complete object-lifetime guarantees under arbitrary concurrent teardown remain NOT VERIFIED.
| FS-14 | Actual hardware durability | ATA PIO and hardware-backed device | Power-cut testing on target hardware with declared cache/flush assumptions | **NOT TESTED**; QEMU/mock-device results are not hardware certification |

### FS-14 — physical power-loss acceptance campaign (2026-11-13)

**Status: NOT TESTED — hardware execution is a mandatory release gate.** This section makes the physical test reproducible; it is not itself evidence of durability and must not be marked PASS from QEMU, a software reset, a VM snapshot, mock-media cache discard, or injected I/O errors.

#### Existing software guarantees checked before hardware testing

- The block layer rejects a flush request when the device has no explicit flush callback (PROS_BLOCK_ENOTSUP); it must never report a missing barrier as success.
- ATA PIO selects FLUSH CACHE / FLUSH CACHE EXT only from valid IDENTIFY DEVICE capability bits and fails closed when the device does not advertise a supported command. The completion path checks BSY, ERR, DF, and DRQ terminal state.
- ProsenjitFS journal transaction barriers, redo replay, format publication barriers, and crash-recovery cases have extensive deterministic mock-device coverage (including FS-72/73 multi-block writes and FS-75 ambiguous allocation commit). Those tests establish software ordering/recovery behavior under their modeled device semantics, not whether a particular physical drive honors flushes through sudden power removal.
- No filesystem source change is justified solely by the absence of physical test equipment. Do not weaken journal barriers, skip flushes, or treat a successful ATA command completion as independent proof that the drive's firmware and volatile cache behaved correctly.

#### Mandatory lab setup

1. Use a sacrificial, independently powered SATA HDD/SSD dedicated to this test. Never use the host OS disk, a production disk, or a disk containing the only copy of data. Verify the target by model, serial, capacity, and controller port before every run.
2. Use a hardware power-cut relay or programmable power controller that removes power from the target drive itself. A kernel panic, reboot, ATA reset, SATA cable removal, or host power-button event is not an equivalent test. The relay must default to power-OFF on controller failure and must not interrupt the host/runner disk.
3. Record drive model, firmware, serial (store securely in lab records), interface/controller, power source, cache policy, IDENTIFY DEVICE flush capability, whether volatile write cache is enabled, and any advertised power-loss protection. Do not silently change drive write-cache settings between runs.
4. Use a disposable test volume with a known-good baseline image and a separate host-side oracle/log on storage that is not being power-cycled. Keep a verified restore image and confirm it can be restored before starting.
5. Capture the exact ProsenjitOS commit, build artifact hash, kernel serial log, operation sequence number, relay trigger timestamp, and recovery result for every cycle. A run without a complete record is INVALID.

#### Workload and cut-point matrix

- Establish a baseline volume, mount it, create deterministic files with per-record sequence numbers and CRC32C, and record the complete expected namespace and payload hashes on the independent host.
- Run a continuous workload that alternates: create + write + flush; append + flush; overwrite + flush; truncate grow/shrink; rename; unlink; mkdir/rmdir; and multi-block writes spanning data and extent metadata. Each logical record must include a monotonically increasing generation and checksum so recovery can distinguish a complete old state from a complete committed new state.
- Trigger real drive-power removal at randomized offsets across each operation, including immediately before a filesystem flush request, during the device flush command, immediately after flush completion, and during sustained write traffic. The relay event must be measured independently; software timestamps alone are insufficient.
- Perform at least 100 cuts per workload family as an initial screening campaign, then repeat on each supported drive/controller/cache configuration. This count is a minimum test plan, not a statistical proof or a substitute for coverage of all cut points. Record every attempt, including failures and invalid runs.
- After each cut, restore power, allow the drive's documented power-on recovery interval, cold-start the machine, and mount through the normal ProsenjitOS storage path. Do not run host filesystem repair tools or modify the medium before collecting the first recovery image and kernel logs.

#### Per-cycle acceptance checks

- The filesystem must either recover the last fully committed state or the immediately preceding valid state according to the journal commit boundary. No partially published namespace operation or mixed metadata generation is allowed.
- All recovered file records must pass their CRC32C and generation checks. Acknowledged-and-flushed records must not disappear; an operation interrupted before its durable commit may resolve to its old or new atomic state only where the documented transaction boundary permits it.
- The ProsenjitFS read-only integrity scan must pass after every recovery. No duplicate physical ownership, orphan allocation/inode, bitmap/extent mismatch, malformed directory graph, invalid journal state, or silent repair is acceptable.
- A failed/unsupported flush must be surfaced as an error and must not be counted as a durable acknowledgement. A drive that advertises flush support but loses acknowledged data after the flush completion is a failure for that drive/configuration.
- Repeat mount, read, integrity scan, and clean unmount at least twice after each cut to detect non-idempotent replay or latent corruption.

#### Stop conditions and reporting

Immediately stop the campaign and preserve the medium image/logs if any acknowledged-and-flushed data is lost, integrity scan fails, the drive becomes intermittently undiscoverable, the relay affects the host disk, or the observed result cannot be classified as old-valid/new-valid. Do not automatically repair or reformat a failed specimen.

FS-14 may be changed from NOT TESTED to PASS only after the lab report identifies the exact tested commit and drive/controller/cache configuration, includes the complete cut count and invalid-run count, provides independent relay timing evidence, and shows zero failed acceptance checks. PASS is configuration-scoped; it does not imply every SATA/SSD model or every power-loss-protection policy is certified. Any change to journal ordering, block flush, ATA PIO, or storage transport invalidates the relevant evidence and requires rerunning the affected campaign.

**Current result:** no physical drive/relay campaign has been executed in this session. The software-level checks above are source/test coverage only. FS-14 remains **NOT TESTED** until the required hardware evidence is attached to this canonical document.

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

Filesystem providers consume struct pros_block_device and its read/write operations. ProsenjitFS additionally uses the optional flush primitive introduced on its feature branch: block_device_set_flush() and block_device_flush(). Existing block-device initializers remain source-compatible; a device without a flush callback causes `block_device_flush()` to return `PROS_BLOCK_ENOTSUP`, so callers cannot mistake a missing durability barrier for a successful one.

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

## FS-37 — Trusted authorization API contract (historical design gate, 2026-10-28)

- **Historical decision gate:** at the time of this design, the VFS had no explicit effective UID/GID or trusted authorization context. This contract was subsequently implemented for native filesystem operations in FS-40 through FS-42. Do not add a UID/GID argument supplied by an ordinary caller and trust it.
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

## FS-38 — Trusted-authorization call-site inventory (historical baseline, 2026-10-28)

**Historical result:** the source inventory identified the kernel authorization-context gap before implementation. FS-42 later added explicit `vfs_kernel_*` entry points for boot image loading, persistent kernel logs, and filesystem/VFS self-tests, while ordinary operations use validated process credentials. This inventory remains useful for understanding why those paths were classified separately.

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

## FS-39 — VFS authorization insertion points (historical implementation map, 2026-10-28)

This function-level map records the original insertion points derived from `kernel/include/vfs.h` and `kernel/vfs.c`. The principal permission checks were subsequently implemented and verified in FS-40 through FS-42; the table is retained as an audit map, not as a claim that the listed checks are absent.

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
- **Scope/verification boundary at the time of FS-40:** chmod/chown, umask, supplementary groups, ACL, sticky-directory policy, and a userspace filesystem syscall/descriptor ABI were not implemented. FS-41 later added media-backed denial/no-mutation CRC assertions; FS-42 added explicit kernel-internal VFS entry points and exact-head verification; FS-44 added basic chmod/chown. Remaining gates include full POSIX permission semantics, check-then-use safety for namespace operations, physical power-loss durability, and general concurrent VFS safety.


## FS-41 — Media-backed VFS permission-denial regressions (2026-10-28)

- Added coverage to `prosenjitfs_selftest()` using the mounted ProsenjitFS deterministic mock medium and a kernel-created limited process.
- After each denied operation, the test compares CRCs for durable bytes, volatile cache bytes, and dirty-sector tracking, then requires a passing ProsenjitFS integrity scan.
- Covered owner/group/other read and write allow/deny cases using persistent inode metadata, execute-only traversal versus directory listing, and permission revocation on an already-open directory handle.
- Covered denied writable open, create, mkdir, unlink, rmdir, rename, write through an already-open handle, truncate through an already-open handle, and read through an already-open handle after permission revocation.
- **Exact-head verification: passed and merged in PR #294** at merge commit `bb9a439830347d8dff47488c96f4073223dbfc2d`. Build/boot-test [run 40744206887](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40744206887) and Security architecture [run 40744206926](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40744206926) both passed on candidate head `dca3ce6b18557e61483c6379c634a8c97293eabe`.
- **Historical remaining work at FS-41:** no public chmod/chown API existed yet. FS-44 subsequently added and verified basic authorized chmod/chown and FS-42 introduced explicit kernel-internal VFS entry points. The fixture described here changes persistent inode metadata directly to exercise restrictive modes; that is test setup, not itself a public permission-change interface.
- This is mock-device evidence only. It does not establish physical-drive power-loss durability or general concurrent VFS safety.

## FS-42 — Explicit authorization context implementation (2026-10-28)

- This section records the PR #295 implementation; FS-41 remains the independently verified media-backed permission-denial regression baseline.
- Legacy `uid/gid` remain for compatibility, while VFS authorization uses kernel-owned `fs_uid/fs_gid/fs_identity_valid`. A privileged security label alone is not filesystem-root authority.
- Ordinary VFS calls require a non-null, live, non-quarantined process with a valid filesystem identity. Missing/invalid identity, malformed context, unsupported mode bits, and absent metadata from an untrusted provider fail closed.
- VFS passes a validated credential snapshot into native namespace mutation callbacks. ProsenjitFS revalidates parent write+search authority before mutation and stores the authorized actor's filesystem UID/GID on newly created objects.
- Explicit `vfs_kernel_*` entry points are used for boot image loading, persistent kernel logs and filesystem/VFS self-tests; ordinary calls do not infer root from `process_current() == NULL`.
- File/directory handles retain the opener identity. File read/write/truncate and directory enumeration revalidate authorization; path lookup checks search permission on root and traversed directories, while namespace mutations check parent write+search and rename checks both parents.
- The PR adds media-backed mock-device denial checks for write-open, create, mkdir, unlink, rmdir, rename, writes/truncates through open handles, and reads after permission revocation. Each denial compares durable, volatile and dirty-sector CRC32C snapshots and runs the filesystem integrity scan.
- Namespace regressions cover limited-user create/mkdir/unlink/rmdir and cross-directory rename with ownership metadata checks, plus denial when either rename parent lacks write permission. Read-only mounts reject writable open/create, mkdir, unlink and rename before media mutation.
- The Build workflow requires the VFS permission/security marker in ISO QEMU and native BIOS QEMU logs. **Verification was subsequently completed:** PR #295's exact-head Build/boot-test and Security architecture workflows passed and the authorization implementation was merged; see the verification record below.
- Bootstrap limitation: filesystem IDs derive from the kernel-assigned nonzero PID when it fits the 32-bit on-disk UID/GID range. This is not a user/group database; supplementary groups, ACLs, `chmod/chown`, `umask`, sticky-directory rules, setuid/setgid semantics and a public filesystem syscall/descriptor ABI are not implemented.
- **Verification:** the authorization-context implementation was subsequently verified and merged in PR #295 at merge commit `002b91813242629162a656438e7cd4d4294566ca`. Exact-head Build/boot-test [run #8942](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40851541441) and Security architecture [run #8437](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40851541436) passed, including ISO QEMU and native BIOS QEMU.
- **Remaining release gates:** scheduler-aware locking, general concurrent VFS/handle-position safety, mount-vs-operation quiescence, physical-device power-loss durability and conservative crash repair/reclamation remain separate; the merge does not establish these properties.



## FS-44 — Authorized chmod/chown implementation (verified 2026-10-29)
- Added VFS `vfs_chmod()` and `vfs_chown()` entry points that resolve the target through the normal permission-checked path walk and derive actor credentials only from kernel-owned process state.
- Policy: chmod accepts owner or privileged administrator; chown requires privileged administrator. The API accepts only basic rwx mode bits (0000–0777); special bits, ACLs, umask and supplementary groups are not implemented.
- Added an optional provider `set_metadata` operation. Providers without a persistent metadata writer return `ENOTSUP`; the VFS cache is updated only after the provider reports a successful persistent update.
- ProsenjitFS writes UID/GID/mode to the validated inode-table block through the existing journal transaction path under the filesystem I/O lock; inode generation/type/checksum are revalidated before commit. On-disk v1 format is unchanged.
- Added mock-provider owner/non-owner chmod policy checks, administrator-only chown denial, read-only mount rejection, unsupported mode validation, media-backed denied-operation CRC assertions, and persistent inode-table UID/GID/mode checks.
- Added a full unmount/remount regression that changes UID/GID/mode through the authorized VFS APIs, remounts the same mock device, and verifies committed inode metadata on the remounted filesystem.
- Added a metadata-transaction device-write failure sweep: after each injected write failure, simulated crash recovery must mount a valid filesystem and recover either the complete old mode or the complete new mode, with UID/GID and inode type intact.
- **Verification: passed and merged in PR #297** at merge commit `8f98ba9ecb580e47e9d1c59824bf4c1030db67e0`. Exact-head Build/boot-test [run #8949](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40854171208) passed ISO build, ISO QEMU and native BIOS QEMU; Security architecture [run #8444](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40854171206) also passed on tested head `4ef6981e3c3721e23534b39747db7b2f1996fa86`.
- This mock-device coverage does not prove physical-drive power-loss behavior or general concurrent VFS safety; those remain separate release gates.
- Timestamp semantics are unchanged by this initial candidate; it does not claim POSIX-complete metadata semantics or general concurrent filesystem safety.

## FS-46 — Provider I/O, integrity scan and unmount admission (verified 2026-10-30)

- PR #315 merged at `5960e14843bf41421686fd3a92f21ca912aefa73`: integrity scans and vnode metadata reads share the provider I/O gate with file read/write/truncate and metadata updates. Contention returns `EBUSY`; a deterministic regression verifies the scan does not start while the gate is held.
- PR #316 merged at `3f3a9670fd34df1b69181dd15c32d8877bb49d16`: directory lookup/readdir callbacks share the provider I/O gate; unmount acquires that gate, revalidates mount state after admission, samples root vnode references atomically, and fails fast if another provider operation is active. The self-test verifies unmount contention returns `EBUSY`.
- PR #315 exact-head Build/boot-test and Security architecture checks passed. PR #316 exact-head Build/boot-test passed ISO build, ISO QEMU and native BIOS QEMU; Security architecture passed. Post-merge main checks for #316 were still running when this record was prepared.
- **Updated by PR #320:** ProsenjitFS mkdir/rmdir/unlink/create/rename callbacks now also share the provider I/O gate with file-data I/O, metadata I/O, lookup/readdir, and integrity scans. The previous provider-level namespace-mutation gap is closed; contention remains fail-fast as `EBUSY`.
- **Authorization ordering:** the PFS callback validates parent write+search authority before taking the provider I/O gate. To prevent chmod/chown from changing parent mode/ownership in that interval, PR #320 also places all four VFS metadata mutation entry points (`vfs_chmod`, `vfs_chown`, `vfs_kernel_chmod`, `vfs_kernel_chown`) under the same per-mount VFS namespace gate as namespace mutators. The lock order is VFS namespace gate, then provider I/O gate. No provider lock is recursively acquired from metadata lookup.
- **Exact-head verification:** PR #320 head `65f3f0ab4551ece3607c9633ac113a5db09557e2` passed Build/boot-test [run #9075](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41042066107), including ISO build, ProsenjitFS diagnostics, ISO QEMU boot, and native BIOS QEMU boot; Security architecture [run #8570](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41042066106) passed. PR #320 merged as `e83c9a893905771a14777a1c6ae091a69fe4b43b`.
- **Concurrency boundary:** these fail-fast gates provide scoped serialization for the enumerated paths; they do not implement scheduler-aware wait/wakeup or justify a general SMP-safety claim. Vnode reference counts are atomic, but callers still need an owned live reference or external lifetime guard.
- The provider I/O gate is fail-fast, not a sleeping mutex. Contention is surfaced as `EBUSY`; callers must retry or otherwise handle contention. This change does not implement scheduler-aware wait/wakeup.
- Vnode reference counts are atomic, but atomic arithmetic does not make an unowned raw vnode pointer safe. Callers must hold a live reference or an external lifetime guard.
- These changes do not establish physical-device power-loss durability, full POSIX semantics, ACLs, xattrs, encryption, or crash-safe repair/reclamation.

## FS-47 — Serialize namespace mutations with provider I/O (verified 2026-10-30)

- PR #320 merged at `e83c9a893905771a14777a1c6ae091a69fe4b43b`.
- ProsenjitFS mkdir, rmdir, unlink, create and rename callbacks now acquire the per-filesystem fail-fast I/O gate and delegate to unlocked helpers. This makes namespace mutations mutually exclusive with provider file-data I/O, metadata operations, directory lookup/readdir and integrity scans.
- Authorization is validated before provider-lock admission. The VFS chmod/chown and kernel chmod/chown entry points now acquire the per-mount namespace gate, preventing a concurrent permission/ownership update from invalidating a namespace mutation's authorization decision between check and provider-lock acquisition.
- Added VFS self-test coverage requiring all four metadata mutation entry points to return `EBUSY` while the namespace gate is held.
- Exact-head Build/boot-test [run #9075](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41042066107) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Exact-head Security architecture [run #8570](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41042066106) passed.
- The provider and namespace gates remain fail-fast, not sleeping locks. This is scoped serialization for the audited paths, not general SMP safety, scheduler-aware wait/wakeup, or proof of physical-device power-loss durability.

## FS-45 — Serialize file-data I/O operations (2026-10-29)

- Replaced the stale PR #265 implementation branch with mainline-based PR #299 to avoid merging a branch that had substantially diverged from current `main`.
- Reads, writes (including non-append writes), and truncates now acquire the existing per-filesystem I/O lock in wrapper functions and delegate to unlocked helpers. This centralizes lock acquisition and avoids recursive locking.
- The first exact-head build exposed an unused append-flag variable after widening the lock scope. That warning-as-error was fixed in a follow-up commit before merge.
- **Verification:** PR #299 merged at `9b793cb6f3850253f934a61ea7b83a56fe758a8f`. Exact-head Build/boot-test run [#40859764029](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40859764029) passed ISO build, ISO QEMU boot-test and native BIOS QEMU boot-test; the run included ProsenjitFS format/mount/allocation/journal/recovery and ATA PIO-to-MBR-to-ProsenjitFS persistent-mount markers. Security architecture run [#40859763946](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40859763946) passed.
- **Limits:** file-data operations are serialized by the provider I/O gate, and same-handle read/write/seek/truncate/close operations use a fail-fast per-handle gate. PRs #316/#320 additionally serialize provider namespace mutations with file I/O and integrity scans. This does not establish general SMP safety, scheduler-aware sleeping locks, complete vnode/object-lifetime safety, or physical-device power-loss durability.
## FS-48 — Transaction flush-failure recovery matrix (verified 2026-10-30)

- PR #321 merged as `fedb1cd15658a5bafa08f34d9dc723834bbb878e`.
- The mock block device now exercises both flush-failure modes: fail before persistence (`fail_flush_countdown`) and report an ambiguous error after persistence (`fail_flush_after_persist_countdown`).
- The regression injects failures at all five `pfs_tx_commit` flush boundaries: old commit-slot retirement, journal header/images, commit record, home-block application and journal retirement. It also uses countdown position six as a no-failure control proving the transaction has exactly five flushes.
- Every case simulates power loss, remounts, checks that the inode mode is exactly the complete old or complete new value, runs the integrity scan, and removes the fixture. It verifies the mount becomes read-only from the point the commit may be durable; earlier failures leave it writable.
- Exact-head Build/boot-test [run #9083](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41044058603) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8578](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41044058604) passed.
- **Scope:** this matrix currently covers a metadata-update transaction. It does not close the remaining FS-11 need for mutation-family-specific write/flush sweeps or certify physical hardware power-loss behavior.

## FS-49 — Directory-creation transaction write-failure sweep (verified 2026-10-30)

- PR #322 merged as `3d33663cff77286296e65539da9d71a8409a81f5`.
- The ProsenjitFS self-test injects a block-write failure at every reachable write position in a `mkdir` transaction, stopping at the first countdown position not reached to establish the successful-write boundary.
- Each injected failure is followed by simulated power loss and remount. The expected commit boundary is checked: failures before the home-block phase recover to an absent directory; failures after the commit record is durable recover to a complete directory. The test checks the mount's read-only transition when commit-record write is attempted.
- The recovered directory inode type and parent are validated, the integrity scan runs before and after fixture cleanup, and the fixture is removed before the next position.
- Exact-head Build/boot-test [run #9086](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41044953533) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8581](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41044953268) passed.
- **Scope:** this sweep covers the `mkdir` transaction only. It does not replace the remaining FS-11 write/flush sweeps for other mutation families or establish physical-device power-loss durability.

## FS-50 — Regular-file creation transaction write-failure sweep (verified 2026-10-30)

- PR #324 merged as `e2cedfc7290cac07ab0c9938e8445d9f4eabaf8a`.
- The ProsenjitFS self-test injects a device block-write failure at every reachable write position in `vfs_kernel_file_open(O_WRITE | O_CREATE)`, stopping at the first countdown position not reached to establish the successful-write boundary.
- Each injected failure is followed by simulated power loss and remount. The test asserts the new file is absent before the durable commit/home-block phase, or is a complete empty regular file after the commit becomes durable. It validates inode type, parent, zero size, zero allocated bytes and no extent head.
- The mount read-only transition is checked at the commit-record write boundary. Integrity is checked before and after unlinking any recovered fixture.
- Exact-head Build/boot-test [run #9090](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41045586872) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8585](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41045586915) passed.
- **Scope:** this sweep covers regular-file creation only. It does not replace remaining FS-11 sweeps for rmdir, unlink, rename, truncate or append, nor establish physical-device power-loss durability.

## FS-51 — Unlink transaction flush-failure recovery matrix (verified 2026-10-30)

- PR #326 merged as `ea31d79752cff924c4a40f37239101c9991f7e14`.
- The ProsenjitFS self-test exercises all five `pfs_tx_commit` flush boundaries plus a no-failure control for `vfs_kernel_unlink`.
- Every boundary is tested twice: a flush error reported before persistence and an ambiguous error reported after persistence. The matrix checks the read-only transition when the commit may be durable.
- After each injected error, the test simulates power loss and remount. It asserts that the file is either fully present with its original payload or fully absent according to the durable commit boundary, runs the integrity checker, and removes any recovered file before unmounting.
- Exact-head Build/boot-test [run #9095](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41048017139) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8590](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41048017116) passed.
- **Scope:** mock block device only. FS-51 does not prove physical-device power-loss durability or replace targeted flush-failure matrices for the remaining mutation families.

## FS-52 — Rename transaction flush-failure recovery and VFS error mapping (verified 2026-10-30)

- PR #328 merged as `59166dfae6c1b2d68f4eabf5156f1bfc26156f80`.
- The ProsenjitFS self-test exercises all five `pfs_tx_commit` flush boundaries plus a no-failure control for `vfs_kernel_rename`.
- Every boundary is tested with a failure reported before persistence and an ambiguous failure reported after persistence. After simulated power loss and remount, exactly one of the old/new paths must exist and contain the original payload; the test also checks the read-only transition, integrity and fixture cleanup.
- The regression exposed an error-contract bug: same-bucket, same-directory bucket-move and cross-directory rename transaction paths could return internal `PROSENJITFS_*` errors directly. All three paths now route transaction staging/commit failures through `pfs_vfs_error()` before returning to VFS.
- Exact-head Build/boot-test [run #9102](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41049781975) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8597](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41049781965) passed.
- **Scope:** mock block device only. FS-52 does not establish physical-device power-loss durability or replace targeted flush-failure matrices for the remaining mutation families.

## FS-53 — Regular-file creation transaction flush-failure recovery (verified 2026-10-30)

- PR #330 merged as `88ea03297894c9d7376e49053d5f0006b9b85e23`.
- The ProsenjitFS self-test exercises all five `pfs_tx_commit` flush boundaries plus a no-failure control for `vfs_kernel_file_open(O_WRITE | O_CREATE)`.
- Every boundary is tested with a flush error reported before persistence and an ambiguous error reported after persistence. After simulated power loss and remount, the file must be absent before the durable commit boundary or present as a complete empty regular file after the commit becomes durable.
- The test verifies the read-only transition, inode type/parent/size/allocation metadata, integrity before and after fixture cleanup, and remounts the test fixture for subsequent regressions.
- Exact-head Build/boot-test [run #9106](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41050303819) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8601](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41050303873) passed.
- **Scope:** mock block device only. FS-53 does not establish physical-device power-loss durability or replace targeted flush-failure matrices for the remaining mutation families.

## FS-54 — O_APPEND transaction write-failure recovery sweep (verified 2026-10-30)

- PR [#333](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/333) merged as `e7868772fd329cb66f51111ad7d59d4c4e70b998`; exact code/test head was `13c78c22f52036518d5a9c8b937435b4e9f4270b`.
- Adds a bounded mock-device block-write failure sweep for `O_APPEND` against an existing three-byte file. Every reachable write position is faulted until the first countdown position not reached, which proves the successful append control.
- After each injected error, volatile state is discarded and the volume is remounted. Before a durable commit record, the original three-byte payload and EOF must remain visible. Once the commit record has been successfully persisted, recovery must expose the complete four-byte payload and new EOF. A failed commit-record write itself forces the current mount read-only but must not be mistaken for a durable commit.
- The test validates the read-only boundary, verifies the complete recovered payload/EOF, runs integrity before and after fixture deletion, and executes at the end of the self-test so its format/reset cycle cannot erase fixtures used by earlier regressions.
- Exact-head Build/boot-test [run #9115](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41053324807) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8610](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41053324566) passed.
- **Scope:** deterministic mock-device/QEMU evidence only. This does not certify physical-device power-loss durability or general concurrent VFS safety.

## FS-55 — Directory-creation transaction flush-failure matrix (verified 2026-10-30)

- PR [#335](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/335) merged as `f1d4058f64871db9eda27dfd0ae6f0672b78d36b`; exact code/test head was `5c7d24cabcc30ce292349c5e58cf1ce65475997a`.
- The ProsenjitFS self-test covers all five `mkdir` transaction flush barriers: commit-slot retirement, journal header/images, commit record, home-block application and journal retirement.
- Each barrier is tested twice: a flush that fails before persistence and an ambiguous flush that persists the data but reports EIO. A sixth countdown position is the successful-operation control.
- After each failure, volatile state is discarded and the volume remounted. The test checks the exact commit boundary: before-persist failure at the commit-record flush leaves the directory absent; a persisted commit record, or any later flush failure, recovers the complete directory. It also verifies the read-only transition from the commit-record attempt onward.
- The recovered directory inode type and parent are checked; integrity runs before and after cleanup, and each iteration unmounts cleanly. The successful-control countdown is disarmed before cleanup so it cannot contaminate later operations.
- Exact-head Build/boot-test [run #9121](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41054529140) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8616](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41054529157) passed.
- **Scope:** deterministic mock-device/QEMU evidence only. This does not certify physical-device power-loss durability or general concurrent VFS safety.

## FS-56 — O_APPEND transaction flush-failure matrix (verified 2026-10-30)

- PR [#338](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/338) merged as `73ad7c9fd52dc68531f8ebe2011c2f0af794b1d4`; exact code/test head was `8ec58e0c8bd563b214b70dbbf26bf69971a86ab8`.
- Covers all five append transaction flush barriers: commit-slot retirement, journal header/images, commit record, home-block application and journal retirement.
- Every barrier is exercised in both modes: fail before persistence and persist the flush data but report EIO. A sixth countdown position is the successful append control.
- After each failure, the test discards volatile state and remounts. It requires the original three-byte payload/EOF before a durable commit and the complete four-byte payload/new EOF after a committed transaction is recovered. The commit-record flush failure forces the current mount read-only but does not itself imply that the commit became durable.
- It checks the expected read-only transition, validates the recovered bytes and EOF, runs integrity before and after fixture unlink, and cleanly unmounts each iteration.
- Exact-head Build/boot-test [run #9126](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41055158313) passed ISO build, ProsenjitFS diagnostics, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #8621](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41055158206) passed.
- **Scope:** deterministic mock-device/QEMU evidence only. This does not certify physical-device power-loss durability or general concurrent VFS safety.

## FS-57 — Truncate-shrink transaction flush-failure matrix (verified 2026-10-30)

- PR [#340](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/340) merged as `5e33aad3726954c8f3c7a5e78bd48107495eb44b`; exact code/test head was `6c66c5ba4ed17e7d79e081d2c345017c894aa003`.
- Covers all five truncate transaction flush barriers: commit-slot retirement, journal header/images, commit record, home-block application and journal retirement.
- Every barrier is exercised with a failure before persistence and an ambiguous failure after persistence; a sixth countdown position is the successful truncate control.
- The test starts with a persisted 64-byte payload, truncates to zero, simulates power loss, remounts and checks the exact old-or-empty result at each commit boundary. It also verifies read-only transition, clean integrity, cleanup and unmount.
- Exact-head Build/boot-test [run #41055845013](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41055845013) passed ISO build, ProsenjitFS self-test milestone, ISO QEMU boot and native BIOS QEMU boot. Security architecture [run #41055844926](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41055844926) passed.
- **Scope:** deterministic mock-device/QEMU evidence only. This does not certify physical-device power-loss durability or general concurrent VFS safety.

## FS-58 — Rmdir transaction flush-failure matrix (verified 2026-10-30)

- PR [#342](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/342) merged as `b5b793d06028a91210de753af7b73a60937c6539`; exact code/test head was `1712f8a8da4203ccd920b117bff6b13e1ec2e9f3`.
- Covers all five rmdir transaction flush barriers, with failures before persistence and ambiguous failures after persistence, plus a sixth-position successful-removal control.
- After simulated power loss and remount, the directory must remain before durable commit or be fully removed after commit recovery. The test verifies read-only transition, surviving directory inode type/parent, integrity before/after cleanup and clean unmount.
- Exact-head Build/boot-test [run #41055842441](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41055842441) and Security architecture [run #41055842416](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41055842416) passed.
- **Scope:** deterministic mock-device/QEMU evidence only. This does not certify physical-device power-loss durability or general concurrent VFS safety.



## FS-59 — VFS lookup callback reference ownership (verified 2026-10-30)

- Defined the VFS lookup callback contract: success transfers exactly one owned vnode reference through the output pointer; failure transfers no reference and must leave the output pointer NULL.
- VFS now ignores malformed output pointers on callback errors rather than releasing a pointer whose ownership is not transferred. This avoids consuming the traversal's live reference when a provider returns a borrowed alias such as the current vnode.
- On a successful but invalid result (NULL, uninitialized, or wrong mount), VFS releases the transferred result reference when it is initialized, including when it aliases the current vnode, then releases the traversal reference independently.
- Added a deliberately malformed self-test provider that returns a borrowed parent pointer together with EIO; the path-lookup regression checks that lookup returns EIO, exposes no result, and preserves the root's mount-owned reference count.
- ProsenjitFS lookup now clears the output pointer before attempting provider-I/O gate admission; previously, an `EBUSY` lock failure could return while leaving a caller-supplied stale pointer in the output. Added a deterministic contention regression that seeds the output with the root vnode and verifies `EBUSY` leaves it NULL.
- **Verification:** merged in PR [#344](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/344); exact-main commit `eaca2521bc7246497b6fa36a951347457665027c`. Build/boot-test [#41106219343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41106219343) and Security architecture [#41106219279](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41106219279) passed. This does not establish general concurrent vnode reclamation safety.


## FS-60 — Partition discovery error-output contract (verified 2026-10-30)

- `partition_probe()` now stages the detected table type locally and writes the caller's output only after the full probe succeeds. A protective-MBR GPT-header read failure no longer leaves a misleading MBR result behind.
- `partition_read_mbr()` and `partition_read_gpt()` set `*count = 0` before I/O and validation. Any read or metadata error therefore cannot leave a stale nonzero count that looks like a valid result.
- The partition API documents that partition-array entries are unspecified unless the call succeeds and the resulting count includes them; callers must not consume partially written entries after an error.
- Added deterministic mock-device fault injection covering MBR read failure, GPT probe header read failure, GPT header read failure, and malformed GPT extent/entry-array cases. Each verifies the output count remains zero; probe verifies the prior caller value is preserved.
- **Verification:** merged in PR [#344](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/344); exact-main commit `eaca2521bc7246497b6fa36a951347457665027c`. Build/boot-test [#41106219343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41106219343) and Security architecture [#41106219279](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41106219279) passed.


## FS-61 — Partition scan partial-output cleanup (verified 2026-10-30)

- MBR parsing now clears any earlier decoded entries if a later partition extent is invalid.
- GPT parsing clears entries already decoded if a later entry read, LBA calculation, range check, or partition-extent validation fails. The public count remains zero on every failure.
- Regression coverage constructs a valid first MBR/GPT entry followed by a failing second entry and verifies that the earlier result slot is cleared, preventing callers from accidentally consuming a partial scan.
- This is a deterministic output-safety contract; it does not validate GPT CRCs or backup-header consistency, which remain separate parser-hardening work.
- **Verification:** merged in PR [#344](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/344); exact-main commit `eaca2521bc7246497b6fa36a951347457665027c`. Build/boot-test [#41106219343](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41106219343) and Security architecture [#41106219279](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/41106219279) passed.


## FS-64 — Regular-file create-open read-failure sweep (verified 2026-10-31)

- Adds a bounded mock-device read-failure sweep around `vfs_kernel_file_open(O_WRITE | O_CREATE)`, targeting the metadata and path preflight before a new regular-file entry can be published.
- Every reached failure must return `EIO`, leave the new path absent, preserve durable/volatile/dirty media, keep the mounted filesystem writable, and pass the integrity scan before clean unmount.
- The first countdown position not reached by the operation is disarmed before a successful create/open control, verifying the end of the read sequence without contaminating later checks.
- **Verification: PASS — exact PR #348 head verified before merge.** Build/boot-test #9215 passed DML unit tests, ISO build, ProsenjitFS block-init diagnostic, ISO QEMU boot-test and native BIOS QEMU boot-test. Security architecture #8710 passed. PR #348 merged into `main` as `47204e34a17016be5d1c14a558ed29477ccc1ba4`. Deterministic mock-device/QEMU evidence only; FS-11 remains PARTIAL and no physical-device durability claim is made.


## FS-65 — Bounded truncate extent-chain regression (verified 2026-10-31)

- Ported the bounded-truncate regression onto current `main` in PR [#351](https://github.com/Prosenjit-Adhikary/ProsenjitOS/pull/351), exact code/test head `641ad0e3b6e2f23fef38a563c8db42fcca2ba52e`.
- The fixture is checked to contain exactly eight independent extents before truncation. A truncate that exceeds the v1 transaction target budget must return `PROS_VFS_EOVERFLOW`; after closing the vnode, the test uses the saved inode number rather than dereferencing vnode-private data, then verifies unchanged file size and a clean integrity scan.
- Exact-head Build/boot-test run #9221 passed DML unit tests, ISO build, ProsenjitFS block-init diagnostic, ISO QEMU boot-test and native BIOS QEMU boot-test. Security architecture run #8716 passed.
- PR #351 merged into `main` as `3b56fc82dc57d95900f162151e0c56d1063d5df7`. Duplicate PRs #350 and #162 were closed as superseded; neither was separately merged.
- **Scope:** deterministic mock-device/QEMU regression evidence only. This does not certify physical-device power-loss durability or general concurrent VFS safety.


## REM-01 — FAT32/exFAT removable-media provider bootstrap (2026-10-31)

**Status: implementation workstream opened; no FAT32/exFAT runtime support is claimed yet.**

### Owner-confirmed scope
- Target filesystems are **FAT32 and exFAT only** for removable media (USB pendrives and external HDD/SSD).
- FAT16 is not a product target. A candidate library may contain FAT16 code internally; the provider must reject FAT16 volumes and must not advertise FAT16 support.
- Native ProsenjitFS remains the only native kernel filesystem. Compatibility filesystem code must stay behind the external provider/VFS boundary.
- Required user-facing operations: safe mount/unmount, directory listing, file open/read/write, create/delete/rename where supported, sync/flush, and safe handling of surprise media removal.

### Initial exact-head source audit
- Existing provider registry (include/vfs_provider.h, kernel/vfs_provider.c) is scaffolding with only probe/mount/unmount callbacks; it does not yet provide a complete filesystem operation bridge.
- kernel/include/block.h exposes sector size/count plus read/write/flush callbacks. This is the intended narrow disk-I/O adapter boundary for a portable FAT implementation.
- kernel/include/storage.h and the current partition view provide a starting point for partition-scoped devices. Current storage evidence does not establish USB mass-storage hardware/transport support.
- kernel/include/vfs.h contains the common vnode operations and mount lifecycle; integration must preserve VFS permission checks, file-admission/security policy, lifecycle pins, and error semantics.
- docs/FILESYSTEM.md remains the single source of truth. Do not create a second FAT status/plan document.

### External implementation research
- Primary candidate for evaluation: ChaN FatFs, because its upstream project documents FAT and exFAT support and a platform-independent disk-I/O boundary. Enable exFAT only with its required LFN/Unicode configuration and 64-bit addressing as required by device size.
- Linux kernel exFAT and VFAT code are reference material for mount validation, directory semantics, edge cases, and removable-media behavior; do not copy Linux GPL code into this project without an explicit license/architecture review.
- Before vendoring any library: pin an upstream release/commit, record its license and notices, review exFAT patent/licensing implications for intended distribution, and document local modifications. No third-party source has been imported in this bootstrap step.

### Ordered implementation gates
1. Complete the provider/VFS operation contract and mount ownership/lifetime model; define read-only fallback and provider failure behavior.
2. Pin and vendor the selected FAT32/exFAT implementation with license notices and a minimal configuration; reject FAT16 at provider probe/mount.
3. Implement the disk-I/O adapter over pros_block_device/partition views with strict LBA, sector-size, overflow, and flush validation.
4. Implement non-destructive FAT32/exFAT probing and mount validation; malformed/unsupported volumes fail closed.
5. Implement VFS vnode/file/directory operations, Unicode/LFN conversion, permission/security integration, and safe unmount.
6. Add mock-media tests, malformed-image/fuzz tests, FAT32/exFAT fixtures, read/write/remount tests, and exact-head CI evidence.
7. Separately complete USB controller + USB Mass Storage transport + device registration + partition discovery + hotplug/media-removal path; do not confuse filesystem-provider tests with physical USB support.

### Current checkpoint
- Branch: feature/fat32-exfat-removable-provider
- Completed: repository/document audit; scope corrected to FAT32 + exFAT; candidate upstream library identified for evaluation; implementation sequence recorded.
- Not yet completed: no FAT parser/provider code, no VFS operation bridge, no USB mass-storage driver, no physical-device test, and no runtime support claim.
- Next action: inspect and extend the VFS/provider contract safely before importing third-party filesystem source.


## REM-02 — provider mount-lifetime pin foundation (2026-10-31)

**Status: provider pin API implemented; not yet wired into VFS mount/unmount dispatch.**

- Extended `struct pros_vfs_provider` with an atomic live-mount counter. Its high bit is reserved as an unregistering state; the remaining bits count active mounts.
- Added `pros_vfs_provider_mount_acquire()` and `pros_vfs_provider_mount_release()`. Acquire fails for an unregistered or unregistering provider and rejects counter overflow. Release rejects null providers, underflow, and unregistering state.
- Provider unregister now atomically transitions a zero-mount provider into unregistering state. It returns busy while any mount pin exists, preventing a registered provider from being removed while a mount holds a reference.
- Extended the provider registry self-test to verify acquire, unregister-while-pinned rejection, balanced release, underflow rejection, successful unregister after release, and acquire-after-unregister rejection.
- **Integration limitation:** no VFS/provider mount dispatcher currently acquires/releases these pins. The next milestone must connect pin acquisition to successful mount ownership and release only after successful full unmount/teardown. Until then, this is a tested-contract candidate, not proof of provider lifetime safety in a running filesystem.
- **Concurrency limitation:** registry membership operations still have no general registry lock. Registration/removal must remain externally serialized as documented in `include/vfs_provider.h`; the atomic mount counter protects the pin/unregister state transition but does not make arbitrary concurrent registry mutation safe.
- No FAT parser, FAT32/exFAT probing, VFS operation bridge, USB transport, or physical-media support was added in this milestone.


## REM-03 — provider-to-VFS mount lifecycle bridge (2026-10-31)

**Status: bridge API and VFS ownership guard implemented; compile/runtime verification pending. No FAT provider is registered yet.**

- Bumped the provider ABI to `0x00020000` because provider operations now include a required `sync` callback in addition to probe, mount, and unmount.
- Added provider mount bridge APIs:
  - `pros_vfs_provider_mount_device()`: pins a registered provider, invokes its mount callback, validates that it prepared an initialized inactive mount with a directory root and expected block-device view, associates the provider with the mount, and activates it.
  - `pros_vfs_provider_unmount_device()`: calls sync before VFS teardown; if sync fails, the mount remains active. It then drains VFS references and releases the root vnode before invoking provider cleanup. A cleanup failure retains the provider association and pin so cleanup can be retried.
- Added an owning-provider pointer to `struct pros_vfs_mount`. Ordinary `vfs_mount_deactivate()` rejects provider-owned mounts; the bridge uses the provider-specific teardown path so callers cannot accidentally bypass flush/cleanup/pin release.
- Added guarded rollback for a provider mount that was prepared but not activated. Rollback requires the mount to be inactive and the mount root to have no outstanding references beyond its mount-owned reference.
- Callback contract: mount callbacks prepare (but do not activate) a valid mount and must roll back partial allocations on failure. The bridge owns activation. Sync must preserve a usable mount on failure. Unmount cleanup runs after root vnode teardown and must be retry-safe. Mount objects must be zero-initialized.
- Provider registry membership operations remain externally serialized; this bridge does not solve registry-wide concurrent registration/removal.
- **Storage discovery integration:** added provider probe dispatch with required capability flags; partition discovery now accepts only providers declaring `PROS_VFS_PROVIDER_PARTITION_AWARE`, records the selected provider in `pros_storage_volume`, and exposes provider-backed mount/unmount helpers. With no provider registered, existing unsupported-volume behavior remains unchanged.
- **Not implemented:** a concrete FAT32/exFAT provider, FatFs vendoring/configuration, VFS vnode/file operation adapter, and USB Mass Storage. This remains generic provider infrastructure only.
- **Verification status:** source/header/docs were fetched from the feature branch after edits. No build, unit-test execution, QEMU boot, or hardware test is claimed yet. ABI v2 requires any future provider implementation to supply the new sync callback.


## REM-04 — provider-aware storage discovery (2026-10-31)

**Status: provider probe selection and storage mount/unmount entry points implemented; no provider registered and no FAT format supported yet.**

- `pros_vfs_provider_probe_device()` now accepts a required capability mask. Storage discovery requests `PROS_VFS_PROVIDER_PARTITION_AWARE`, so an unrelated non-partition provider cannot mask a later valid partition provider.
- `pros_storage_discover()` tries native ProsenjitFS first, then registered compatible providers only when the native probe reports an ordinary unsupported format. Probe I/O/validation errors are not silently converted into no-match.
- Provider-backed volumes retain the parent block device, selected provider, partition metadata, and partition block-device view. Added `pros_storage_mount_volume()` and `pros_storage_unmount_volume()`.
- Corrected an important ownership issue while integrating the volume record: `partition_device_init()` stores a back-pointer to its view in `device.private_data`. A stack-local view must not be copied into the returned volume without rebinding that pointer. Discovery now rebuilds the partition view in its final array slot, and the storage header documents that discovered volume entries must remain at stable addresses.
- No provider is registered yet, so the current storage self-test's unsupported-format case should still discover zero volumes. This expectation has not been executed in this environment.
- No FAT32/exFAT parser, file/directory operation adapter, FatFs import, USB transport, or physical-device support is included in this milestone.
- Verification remains source-level only; no build, unit test, QEMU, or hardware pass is claimed.


- **Provider object lifetime constraint:** the current registry has no module-unload or discovery-reference protocol. Provider objects and ops tables must therefore have boot/static lifetime; unregister is a registry operation, not permission to free provider memory while discovered volume records may still retain its pointer. Dynamic provider unloading is unsupported.


## REM-05 — provider bridge lifecycle self-test coverage (2026-10-31)

- Extended `pros_vfs_provider_selftest()` with a synthetic provider and in-memory block device/root vnode. The test exercises actual bridge mount activation, direct-deactivate rejection, sync failure preserving an active mount, successful sync followed by root teardown, provider cleanup failure retaining the pin, and successful cleanup retry releasing the pin.
- Provider ABI validation now tests the required sync callback independently from probe, mount, and unmount callback validation.
- These are added self-test cases only; they have not yet been compiled or executed. No runtime pass is claimed.


## REM-06 — patched ChaN FatFs source and block-device adapter (2026-10-31)

**Status: upstream source vendored and build integration added; exact-head compiler and boot verification pending.**

- Pinned FatFs R0.16 from `zephyrproject-rtos/fatfs` commit `4f60a12c58c3de4246857ad87c40239ecd93349b`. The mirror carries upstream R0.16 patch-1 and patch-2 plus a documented local zero-fill hardening change. Do not substitute the unpatched R0.16 release: upstream's patch page and the mirror README describe security issues affecting the original release.
- Vendored upstream `ff.c`, `ff.h`, `diskio.h`, and `ffunicode.c` with their copyright/license notices intact. Added a ProsenjitOS-specific `ffconf.h` and upstream provenance/security notes in `third_party/fatfs/UPSTREAM.md` and `UPSTREAM_README.md`.
- Configuration: exFAT and 64-bit LBA enabled; UTF-8 LFN API; 255 UTF-16 code-unit LFN capacity and 1020-byte UTF-8 filename buffer; 8 logical drives; 512–4096-byte sectors; 32 open-object locks; formatting and trim disabled; no RTC dependency. `FF_FS_REENTRANT=0`; the provider serializes FatFs calls with a fail-fast global gate.
- Added `kernel/fatfs_disk.c` to map FatFs physical drives to bounded partition-local `pros_block_device` instances. Read-only mode is enforced, LBA/count ranges are checked, sector geometry and CTRL_SYNC are exposed, and the adapter has an in-memory self-test.
- Added the upstream core, Unicode module, and disk adapter to the kernel build and linked objects. Boot self-test and CI log markers were added.
- FatFs itself includes FAT12/16 code, but the ProsenjitOS provider must accept only FAT32/exFAT; FAT12/16 are explicitly rejected by the provider's format probe/mount classification. Formatting is not exposed.

## REM-07 — FAT32/exFAT provider and 64-bit VFS file path (2026-10-31)

**Status: source implementation present; exact-head build and runtime tests pending.**

- Added a registered `fat32-exfat` provider. It probes exFAT boot-sector signatures and validates FAT32 BPB geometry/cluster-count classification; FAT12/16 are not accepted. Final mount classification is checked against FatFs `FS_FAT32` or `FS_EXFAT`.
- Provider mount binds the partition-local block view to FatFs, mounts without formatting, and creates a VFS root vnode. Provider sync flushes the block device before teardown; unmount detaches FatFs and releases the disk slot.
- Implemented VFS vnode operations for lookup, directory enumeration, create, mkdir, unlink, rmdir, rename, synthesized metadata, plus per-open file state and read/write/truncate/close operations using FatFs. UTF-8 paths are passed to the FatFs UTF-8 API. The VFS currently limits a single component to 255 UTF-8 bytes; longer encoded names are rejected rather than truncated.
- FatFs calls, probe-sector access, and shared path scratch buffers are serialized by one fail-fast global gate because kernel thread stacks are only 4 KiB and the current kernel has no FatFs-compatible mutex API. Large path/sector buffers therefore live in provider-owned static storage rather than on thread stacks. Concurrent operations may return `EBUSY`; this is safe but deliberately conservative. Directory reads currently reopen and rescan the directory for each cookie, so large-directory enumeration is O(n²); optimize only after correctness tests.
- Extended VFS file ops with optional per-open setup and 64-bit read/write callbacks. Legacy ProsenjitFS callbacks remain 32-bit; FatFs uses the 64-bit path. VFS seek and truncate now allow offsets/sizes above 4 GiB only where the 64-bit callbacks exist.
- FAT volumes do not carry POSIX uid/gid/mode. The provider synthesizes uid/gid 0 and permissive 0777 metadata for ordinary writable directories (0777) and files (0666), or 0555/0444 for FAT read-only directories/files; mount-level read-only remains separately enforced by VFS. POSIX chmod/chown persistence is not claimed.
- **Historical test-plan snapshot (2026-10-31):** this branch originally added a synthetic FAT32 block-image fixture for provider probe/mount, file create/write/read/truncate, directory enumeration, sync/unmount, and read-only remount/readback. These tests are now exercised by the exact-main Build/QEMU workflow documented in the 2026-11-04 status summary; the older snapshot is retained for history.
- **Historical status at this 2026-10-31 snapshot:** no compiler run or QEMU boot had completed for that branch. This note is superseded by the 2026-11-04 exact-main Build/QEMU evidence in the current status summary. Surprise-removal testing, end-to-end physical USB support, and physical USB disk validation remain outstanding.
- **Licensing:** FatFs source redistribution requires retaining its source notice and disclaimer. The upstream application note warns that Microsoft exFAT patent/licensing obligations may apply depending on product destination/distribution; review before shipping binaries or commercial products.


## REM-08 — FatFs build integration and disk-binding consolidation (2026-10-31)

- Audited the build script after the FatFs provider landed. It compiled and linked the FatFs core and Unicode translation units twice, and included two competing implementations of the same FatFs `disk_*` and bind/unbind symbols. This would cause duplicate-symbol linker failures; source presence alone was not a valid build verification.
- Consolidated the build to one FatFs core object, one Unicode object, and one disk adapter object. The selected `kernel/fatfs_disk.c` adapter protects active disk operations with an in-flight counter and blocks unbind until all I/O calls have left the device slot. The simpler duplicate `fatfs_diskio.c` implementation and header were removed from the branch.
- Updated provider and boot self-test includes/calls to use `fatfs_disk.h` and `pros_fatfs_disk_selftest()` consistently.
- Source-level re-fetch confirms the build script references one `ff.c`, one `ffunicode.c`, and one disk adapter; provider and kernel call sites use the guarded adapter. No build or QEMU run is claimed.


## REM-09 — Delegate filesystem probing to upstream FatFs (2026-10-31)

- Replaced the provider's handwritten FAT32/exFAT BPB/VBR parser with a temporary read-only FatFs mount using the pinned upstream FatFs implementation. The provider now accepts only when FatFs reports `FS_FAT32` or `FS_EXFAT`; FAT12/FAT16 remain rejected by the same upstream classification result.
- Removed synthetic unit tests for the deleted handwritten parser. The existing provider integration fixture still calls the registered provider probe and then exercises mount, create/write/read/truncate, directory enumeration, sync/unmount, read-only remount, and persisted readback through FatFs itself.
- Added the vendor root to the compiler include search path so the pinned FatFs configuration header is found alongside the upstream headers. The active configuration is `third_party/fatfs/include/ffconf.h`.
- This intentionally keeps ProsenjitOS code limited to the platform glue: bounded block-device binding, provider lifecycle, VFS vnode/file adaptation, locking, and policy. FAT/exFAT on-disk parsing, allocation, directory/file semantics, and metadata validation are delegated to FatFs.
- Source-level changes are committed; compilation, boot self-tests, malformed-image testing, and real-device testing remain pending until the runner completes. Do not treat this milestone as verified or bug-free yet.
- Upstream review: ChaN documents FatFs as a platform-independent FAT/exFAT module with a separate disk-control adapter; exFAT requires LFN and 64-bit LBA configuration. The upstream patch page documents security fixes in patch-2 for CVE-2026-6682, CVE-2026-6683, and CVE-2026-6687. The pinned mirror also carries additional downstream hardening; keep its provenance pinned and re-review before upgrades.


## REM-10 — Upstream-only FAT/exFAT format probing (2026-10-31)

- Corrected the provider probe so all on-disk FAT12/16/32 and exFAT parsing/classification is performed by the pinned, patched ChaN FatFs R0.16 implementation. ProsenjitOS no longer parses BPB/VBR geometry in its provider probe.
- The provider binds the candidate partition read-only for probing, calls FatFs mount/classification, accepts only `FS_FAT32` and `FS_EXFAT`, then detaches and unbinds before returning. Formatting remains disabled.
- If FatFs detach or disk unbind fails, the temporary volume slot is deliberately retained rather than reusing memory that may still be referenced by FatFs.
- Probe tests now exercise upstream FatFs recognition of the FAT32 fixture, invalid-media rejection, and I/O-error propagation. The existing end-to-end fixture covers FAT32 create/write/read/truncate/directory enumeration/sync/unmount/read-only remount. A full valid exFAT mount/write fixture and real USB-device tests remain outstanding.
- **Verification status:** implementation committed as `4b26184496baf187bd81986ada4ec6765e6a8c48`; canonical documentation update follows in the same PR. Build, QEMU and physical removable-media validation are not yet confirmed on the exact candidate head.


## FS-75 — Ambiguous data-block allocation commit recovery

- The raw contiguous allocator now preserves the candidate start block when a journal commit may have reached durable media but the final flush reports EIO and forces the mount read-only. Failures before that ambiguous-commit boundary still clear the output.
- The API contract explicitly marks that candidate as indeterminate: callers must stop using it, remount/recover, and reclaim it only if recovery confirms it is allocated and it was not attached to an inode.
- Added a mock-device regression that fails the third transaction flush after persistence, simulates power loss, remounts, verifies the candidate bitmap bit was replayed, reclaims the unowned reservation, and requires a clean integrity scan.
- This is a mock-device durability-ordering test; it does not certify physical-device power-loss behavior.

### Exact inode reservation registry — 2026-11-12

The inode allocator maintains a bounded, volatile 1024-bit registry for reservations in the active mount. Mutation preflight admits an allocated inode with a FREE record only when the exact inode number was registered by a successful `alloc_inode()` call and the FREE-record generation, checksum, and zero-field invariants still validate. A valid FREE record plus an allocated bitmap bit is not sufficient.

Reservation lifecycle:
- Successful inode-bitmap allocation registers the exact candidate after commit.
- A successful non-FREE inode write publishes the inode and clears its reservation bit.
- A successful inode-bitmap release clears its reservation bit.
- The public integrity checker remains strict and does not treat registered reservations as durable ownership.
- Mount/crash reset clears the volatile registry. An allocated FREE inode surviving a crash is an unregistered orphan allocation and fails preflight until explicitly recovered.
- The candidate is published before the bitmap commit so persist-then-EIO remains indeterminate, matching the block allocator contract. The mount stays read-only and no registry entry is created for an ambiguous commit.

Self-test fixtures now cover exact registered admission, duplicate-registration rejection, wrong-inode/stale-published-inode rejection, missing-registry rejection, remount invalidation, stale registry rejection after release, free-inode discovery and bitmap-staging rejection of stale bits, publication clearing, and persist-then-EIO allocation recovery. These are source-level additions only until the exact PR head passes Build/QEMU and Security workflows; no runtime pass is claimed while the self-hosted runner is offline.


### Exact block reservation registry consistency — 2026-11-12

- Mutation preflight now verifies every registered block reservation against both the ownership map and on-disk allocation bitmap. A registered block must be an allocated, unowned data block; registry bits that overlap inode/extent/directory ownership, point at a free block, or refer to metadata space are corruption.
- This closes a stale-proof gap where a low-level reservation could otherwise overlap newly published inode ownership or remain registered after an independent bitmap release, causing future allocation skips or unsafe release accounting.
- Added source fixtures for reservation/owned-directory-block overlap and reservation-on-free-block rejection. These fixtures are not runtime-verified until the exact PR head completes Build/QEMU and Security checks.


### Allocator staging/release and mutation-path audit — 2026-11-12

- Re-fetched the complete ProsenjitFS source and enumerated every `pfs_allocator_stage()` and `pfs_allocator_update()` call site. Raw allocator updates are confined to the public low-level allocate/free path; inode-owned allocation/reclamation is staged inside the file write, truncate, preallocate, mkdir, rmdir, unlink, create, and rename transactions.
- Verified at source level that the VFS file-write and truncate wrappers acquire the provider I/O gate and run consistency preflight before entering their transaction helpers. The mkdir/rmdir/unlink/create/rename wrappers do the same. Keep-size preallocation and public low-level allocation/free each acquire the gate and run preflight before staging.
- The internal allocation paths publish bitmap, data/extent blocks, and inode/directory ownership metadata in one transaction. Their allocator staging therefore does not create a separate raw reservation bit. Internal reclamation first passes the global ownership preflight; a registered raw reservation cannot be treated as an owned extent/directory block, and the consistency scan rejects registry bits on free or already-owned blocks.
- Added a source regression fixture that keeps a two-block raw reservation live while performing file create/write/truncate/unlink and directory create/rename/rmdir. It then requires both exact registry bits and both on-disk allocation bits to remain set and requires mutation preflight to remain clean. This is a source fixture only; it has not been executed.
- Scope limitation: source audit is not a substitute for the offline runner. Build, compiler diagnostics, self-tests, QEMU, and crash-injection runtime verification remain pending.


### Public inode ownership publication contract — 2026-11-12

- The public `prosenjitfs_write_inode()` interface updates an inode-table record only; it does not atomically stage allocator bitmap and extent/directory ownership metadata. It cannot be used to publish a newly allocated inode that claims raw block reservations or to change ownership-bearing fields of an existing inode.
- A freshly allocated inode reservation may be published through this writer only as a blockless inode (`extent_head == 0`, `dir_index == 0`, `allocated_bytes == 0`) while its current record is FREE. Existing published inode records may be updated only when `extent_head`, `dir_index`, and `allocated_bytes` remain unchanged. Ownership-changing operations stay on atomic VFS transaction paths.
- Added a regression fixture that attempts to publish a reserved data block as a newly allocated inode's extent. It expects rejection before journal staging and verifies both inode and block reservation proofs remain intact. Source-only; not executed because the local runner is offline.


### Public allocator admission and output contract — 2026-11-12

- Both public low-level allocation APIs now initialize their output parameters to zero before lock acquisition or consistency preflight. Callers cannot mistake a stale sentinel/output value for an allocated candidate after lock contention or a preflight rejection.
- alloc_inode() now runs the same locked ownership-consistency preflight as alloc_blocks() before scanning or staging the inode bitmap. It accepts only exact current-mount reservations already proven by the volatile registries; unrelated orphan blocks or invalid ownership reject the operation before mutation.
- Removed the inode allocator's read of the caller's prior output value. Ordinary pre-commit failures leave zero; a candidate remains visible only when commit ambiguity forces the mount read-only, preserving the existing recovery contract.
- Added a mock-media regression that creates a raw block reservation, simulates power loss/remount (thereby invalidating its volatile proof), and verifies both public allocators reject the orphan with ECORRUPT, zero their sentinel outputs, and leave durable/volatile/dirty media unchanged. The fixture then explicitly reclaims the block and requires a clean integrity scan.
- Source implementation and static call-path checks only. Build, compiler diagnostics, self-test execution, QEMU, and CI remain pending the user's offline-runner restriction.


### Public FREE-inode reservation release and recovery — 2026-11-12

- Added public prosenjitfs_free_inode() for releasing an unpublished FREE inode reservation. It rejects inode zero and the root inode, and it cannot release a published FILE/DIR/SYMLINK inode.
- For crash recovery, the caller-supplied inode number is temporarily admitted as the sole additional inode reservation during the full ownership preflight. The scan still validates the allocated bitmap bit, FREE-record generation/checksum/zero fields, all other inode reservations, every block reservation, and all unrelated ownership/orphan state before the bitmap-clear transaction.
- The temporary proof is removed before staging. The normal bitmap transaction clears the live registry only after successful commit; on ambiguous EIO the mount remains read-only and the caller can retry the same explicit candidate after remount.
- The ambiguous inode-allocation fixture now recovers through the public API rather than a private bitmap helper. Additional source fixtures cover live reservation release, root-inode rejection without media changes, and I/O-lock gating.
- Source-only until exact-head Build/boot-test, self-test, QEMU and Security workflows can run. No runtime pass is claimed.


### Reservation-registry geometry and integrity validation — 2026-11-12

- Mount now rejects a reservation-registry byte count of zero or greater than UINT32_MAX before allocation/zeroing. This matches the 32-bit pfs_zero() length and integrity-scanner allocation contract; no truncated initialization can be published as a writable mount.
- Every consistency scan verifies that the runtime registry pointer exists and its recorded byte length exactly equals ceil(total_blocks / 8). A missing, shortened, or stale registry fails closed before ownership or allocator mutation checks.
- Added a source regression that temporarily corrupts the recorded registry length, requires public integrity checking to return ECORRUPT, restores the exact length, and requires the integrity scan to succeed again.
- This enforces the current 32-bit registry/owner-buffer geometry limit. Memory allocation can still fail on a machine without sufficient RAM; mount aborts and releases all allocated resources. Runtime fault injection/build verification remains pending runner availability.


### Allocator bitmap-sector scan caching — 2026-11-12

- The inode free-slot finder and the public inode allocator now retain the currently loaded inode-bitmap sector while scanning. The free-data-block finder and both exclusion variants do the same for data-bitmap sectors.
- This removes repeated identical device reads for every inode/block bit. A full exhaustion scan now performs at most one read per bitmap sector touched, while preserving the same candidate ordering, exclusion semantics, I/O error propagation and sentinel behavior of the private find helpers.
- Added a mock-device read-call counter and a regression that fills the inode/data bitmap images in memory, runs all affected full-scan paths, and asserts the read count equals the number of bitmap sectors rather than the number of inode/block candidates. The original media images are restored before integrity checking.
- Static implementation only; runtime test/build/CI remain pending runner availability.


### Mock crash-reset reservation-registry cleanup — 2026-11-12

- Source audit found the mock power-loss reset freed transaction images and scratch memory but did not release the mount-scoped block reservation registry before zeroing the filesystem object.
- Added matching registry deallocation and pointer/length reset. Repeated crash/remount regression sweeps no longer leak one reservation bitmap per reset; normal unmount and mount-abort cleanup remain unchanged.
- Source-level ownership/lifecycle cross-check only; allocator/leak instrumentation and runtime tests remain pending runner availability.


### Inode allocator read-only flag parity — 2026-11-12

- Cross-check aligned the inode allocator's ordinary-failure output reset condition with the block allocator: the indeterminate candidate is preserved only when both filesystem and VFS mount read-only flags are set. A partial/inconsistent flag state now clears the output instead of being misclassified as an ambiguous commit.
- The public wrapper already rejects either read-only flag before preflight; this change hardens the unlocked allocator's internal error contract as well.


### Block-reservation registry tail-bit validation — 2026-11-12

- Added an exact registry validator that checks pointer presence, overflow-safe expected byte length, the current 32-bit allocation ceiling, and zero-only unused tail bits in the final registry byte.
- The global ownership/integrity scan now rejects a registry with any out-of-volume reservation bit instead of silently ignoring it because block lookup bounds-checks against total_blocks.
- Added a focused synthetic registry fixture for a non-byte-aligned total-block count; it sets one out-of-range tail bit, requires validator rejection, clears it, and confirms the valid registry is accepted. The real mounted media is not altered by this probe.


### Inode-table block cache in the ownership scanner — 2026-11-12

- Factored inode-image checksum/semantic decoding into one shared validator and added a caller-owned one-block cache keyed by inode-table block index. The public read API still uses a private one-call cache and retains its no-partial-output behavior.
- The global ownership scan now reuses the cached inode-table block across sequential inode records and directory-child validation. It no longer performs a device read for every inode record when several records share one physical table block.
- Added an integrity-scan read-count assertion to the mock-device scan-cache fixture. On the clean 1024-inode image, the scan must complete with fewer block reads than inode records; the fixture also verifies the bitmap helper scans read no more than one read per bitmap sector.
- Source-level performance regression only; runtime execution and CI remain pending runner availability.


### FREE inode-record integrity and generation validation — 2026-11-12

- Source audit found that the public inode reader returns ENOENT immediately for a FREE record. The global scanner previously treated that status as sufficient for unallocated slots, so stale timestamps or a bad FREE-record checksum could evade integrity validation and fail only when the inode was later reused.
- Added a shared FREE-record validator covering type, all zero-only metadata (including atime/mtime/ctime), reserved bytes, and checksum rules. The allocator now uses the same validator before deriving a new generation.
- The ownership scan validates every unallocated FREE record from its cached inode-table image, and validates registered allocated FREE reservations from that same image before admitting them. Generation exhaustion remains a valid unallocated state but cannot be allocated.
- Added a mock-media regression that injects a checksum-valid FREE record with a nonzero timestamp, requires integrity rejection, restores a valid FREE record, and requires integrity success.
- Runtime verification remains pending; no build/test pass is claimed.


### Public write_inode API is metadata-only — 2026-11-12

- A record-only writer cannot atomically add/remove a directory entry, update parent links, or publish/free extent ownership. Allowing it to publish a new allocated inode or alter type/parent/size/extent fields could create an orphan or namespace mismatch that blocks all later preflight operations.
- The public write_inode API is now explicitly metadata-only for an already-published inode. Type, flags, links, generation, parent, size, allocated bytes, extent head, and directory index must match the current validated record. Mode, owner IDs, and timestamps remain mutable subject to semantic validation.
- The public wrapper acquires the I/O lock and runs the ownership-consistency preflight before the metadata transaction. The unlocked writer refuses registered FREE reservations and all structural-field changes with EPERM.
- Self-tests that deliberately create orphan inodes, symlinks, parent cycles, or impossible extents now use the test-only raw inode injection helper and explicitly clear the volatile reservation proof when simulating a committed publication. New checks verify the public writer refuses reservation publication and parent mutation.
- Runtime verification remains pending; the PR stays Draft until the exact head passes build, self-test, QEMU and CI.


### Public inode-reclamation negative-path hardening — 2026-11-12

- Added a source self-test proving prosenjitfs_free_inode() cannot reclaim an inode that is already published as /hello.bin. The recovery API temporarily admits the requested inode only for the full consistency scan; the published non-FREE record is rejected as corruption.
- The regression requires the temporary reservation bit to be removed after rejection, durable/volatile/dirty mock media to remain byte-for-byte unchanged (CRC32C), and the public integrity scan to remain clean.
- Added a public alloc_blocks() read-fault regression with a nonzero output sentinel. A preflight I/O error must return EIO, clear the output to zero, and leave the mount writable.
- Static source/call-path review only. The local runner remains offline per user instruction; compiler/build/self-test/QEMU and exact-head CI execution remain pending. PR #573 remains Draft and must not be merged before those checks pass.

- Extended the same public allocator failure matrix to cover a pre-commit journal-write failure in alloc_blocks(): a nonzero output sentinel must become zero, the mount must remain writable, and durable/volatile/dirty media CRCs must remain unchanged. Together with the existing read-fault, orphan-preflight, success, and persist-then-EIO fixtures, both public allocation APIs now have explicit source coverage for their output-state contract.


### FREE-inode API negative-path regression — 2026-11-12

- Extended the allocator regression to call `prosenjitfs_free_inode()` on a known unallocated FREE slot. It must return `PROSENJITFS_ECORRUPT`, remove its temporary reservation proof, and leave durable, volatile, and dirty mock-media CRCs unchanged.
- The same check continues to reject the root inode with `PROSENJITFS_EINVAL`; a final integrity scan must still pass before unmount.
- This is source-level regression coverage only. The workflow is queued but has not been executed to completion; no compiler or runtime pass is claimed.


### Inode semantic-writer validation matrix — 2026-11-12

- Expanded the public `write_inode()` rejection matrix from two malformed cases to twelve independent cases: unsupported mode bits, non-zero reserved bytes, unknown inode type, unsupported flags, zero link count, zero generation, out-of-range parent, illegal non-directory `dir_index`, unaligned `allocated_bytes`, directory size/extent invariant violation, sparse flag on a symlink, and an oversized file length.
- Each case requires `PROSENJITFS_EINVAL` before any durable, volatile, or dirty media changes. The fixture then bypasses the public writer to inject a checksum-valid malformed record and confirms read, path lookup, and integrity validation reject it, restoring the original block unconditionally afterward.
- This strengthens static regression coverage for the same semantic constraints used by the inode read path. Build/self-test/QEMU/CI execution remains pending; source inspection is not a runtime pass.


- Added an explicit `UINT64_MAX` file-size case to the semantic-writer matrix so capacity arithmetic is exercised independently of the directory-size invariant.


### Collision-safe directory duplicate-name scan optimization — 2026-11-12 (PR #573)

- Replaced the integrity scanner's repeated reread of every prior directory-entry block with a bounded in-memory open-addressed name index.
- The index stores each validated child inode's parent, 32-bit name hash, exact name length, and complete UTF-8 name. Hash/length matches are confirmed byte-for-byte, so hash collisions do not become false duplicate-name reports; the same name in different parent directories remains valid.
- The table capacity is the next power of two at least twice the v1 inode count. The v1 maximum of 1024 inodes bounds the index to at most 2048 slots and approximately 300 KiB of temporary name/index storage. All allocations are checked and released on every scanner exit path.
- Added a focused source self-test for same-parent duplicate rejection, deliberate same-hash/same-length distinct-name acceptance, and same-name/different-parent acceptance.
- Static cross-check confirms the old nested prior-entry disk-read loop is removed and each directory entry is indexed only after its block checksum, padding, UTF-8, bucket, child inode, and parent/type checks pass.
- This reduces duplicate-name validation's repeated directory-entry I/O from quadratic rereads to one read per entry. Open-addressing probe work remains bounded by the v1 inode ceiling; this does not remove the full-volume ownership scan from mutation preflight.
- **Verification:** source-level only. No compiler, build, self-test, QEMU, or CI execution was performed because the self-hosted runner remains offline. Keep PR #573 Draft; exact-head runtime and Security verification remain required before merge.


### Partial-block truncate crash recovery matrix — 2026-11-12 (PR #573)

- Added FS-57b source coverage for shrinking a 64-byte file to a 17-byte EOF, exercising the path that zeros the retained final data block's newly unreachable suffix in the same redo transaction as the inode size update.
- Sweeps all five transaction flush barriers in both pre-persist and persist-then-EIO modes, plus the successful control. After simulated power loss/remount, the fixture requires the old 64-byte contents when the commit was not durable, or the new 17-byte EOF when the commit was durable.
- For the committed new state, the fixture reads the physical data block and verifies every byte from offset 17 through the block end is zero. This specifically guards against stale data becoming visible after a later file extension.
- Each case also checks read-only transition semantics, integrity before/after cleanup, and successful unmount.
- **Verification:** source fixture and static call-path review only. The local runner remains offline; no compiler, build, self-test, QEMU, or CI execution has been performed. The existing FS-57 truncate-to-zero sweep remains unchanged.


### Sparse-growth two-transaction crash recovery matrix — 2026-11-12 (PR #573)

- Added FS-57c for growth from a 64-byte file to one block plus one sparse byte. The fixture deliberately poisons the old EOF-to-block-end suffix before the operation so the tail-zero transaction has observable work.
- Sweeps all five flush barriers of both transactions, in pre-persist and persist-then-EIO modes, plus the successful control. This covers the tail-zero transaction and the later inode-EOF publication transaction independently.
- After power-loss remount, the fixture requires the old EOF until the inode-size commit is durable, even when the preceding tail-zero transaction has already committed. When the larger EOF is durable, all newly exposed bytes must read as zero.
- It also inspects the physical block: the poisoned suffix must remain unchanged if the first transaction did not commit, and must be zero once that transaction is durable. Integrity and cleanup are checked for every case.
- **Verification:** source-level fixture only. No compiler/build/self-test/QEMU/CI execution was performed while the local runner is offline.


### Native BIOS CI failure reconciliation — 2026-11-12

- Build/boot-test run [#43134511165](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43134511165), job 130906479516, failed at the native BIOS handoff marker; the ISO QEMU boot step had already passed. Security architecture run [#43134511291](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43134511291), job 130906480581, passed.
- The Build log checked out the PR merge ref 8c8aaa7f4b3d17c34a34744ce15b5a6ca56fb63e, with merge commit 1099886 joining PR head 8de5dd9081f63c3dc5c6a62d47e5e6871ab27c0e into base 28d507dea945e9525dd047d32ca0e87dfff0f427. That tested snapshot still used the fixed KERNEL_LOAD + 24 jump and did not derive the entry offset from the linked ELF symbol.
- The current PR branch source has since changed the native path: scripts/build.sh resolves _start with nm -n, validates the offset against the flat kernel image, and passes it to stage-2; boot/native_stage2.asm jumps to KERNEL_LOAD + KERNEL_ENTRY_OFFSET; boot/entry.asm loads the kernel GDT before enabling IA-32e paging.
- Therefore run #43134511165 is a real failure of its checked-out merge snapshot, but it is not exact-head verification of the current branch source. Do not count it as a pass or as a confirmed failure of the newer entry-symbol/GDT changes.
- Current acceptance state: fresh exact-head Build/ISO+native-BIOS QEMU and Security checks are still required. PR #573 remains Draft; do not merge until both checks pass and the native serial log contains the handoff marker and subsequent kernel self-test markers.


### Exact-head Build/QEMU and Security verification — 2026-11-12

- Candidate PR #573 merge ref 4b4d3ba62b6d857b963fca460cf72aa515aeb063 was built from PR head db9c28551a1afb80dbb04779014f8390c38c450c into base 28d507dea945e9525dd047d32ca0e87dfff0f427.
- Exact candidate Build/boot-test [#43198126137](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43198126137), job 131114223235, **PASSED** on self-hosted runner Office1. Every step passed: toolchain, desktop/DML, timer IRQ, ISO build, block-init diagnostic, exFAT formatter, ISO QEMU, and native BIOS QEMU.
- Exact candidate Security architecture [#43198126144](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43198126144), job 131114223172, **PASSED**.
- Native BIOS log confirms the linked _start offset was 32, the native bootloader handoff was accepted, ProsenjitFS format/mount/allocation/journal/recovery self-test passed, and the ATA PIO -> MBR -> ProsenjitFS persistent mount test passed. The QEMU timeout exit 124 occurred after the required milestones and was accepted by the workflow.
- This supersedes the earlier stale-merge-ref failure reconciliation above for the newer candidate. It does not certify physical-device power-loss durability, physical USB hotplug/removal, or arbitrary SMP/concurrent VFS safety.
- PR #573 remains Draft and has not been merged. The current source candidate is exact-head Build/QEMU + Security verified; any later source change requires fresh exact-head checks.
- Remaining native-filesystem release gates: FS-11 remains PARTIAL for the broader reachable read/write/flush fault inventory, USB transport short-transfer audit, and cleanup/rollback paths; FS-12 still needs full adversarial integrity-scan coverage; FS-13 does not claim arbitrary concurrent teardown/SMP safety; FS-14 physical hardware power-cut durability remains NOT TESTED. Keep these as explicit pending items rather than treating the PR's passing CI as filesystem-wide completion.

### Native BIOS kernel destination E820 validation — 2026-11-12 (PR #573)

- Source audit found that stage-2 validated the low-memory BIOS staging buffer but copied the loaded kernel to physical address 1 MiB without proving that the destination range was covered by firmware-reported usable RAM.
- Updated `boot/native_stage2.asm` to validate both the complete sector-rounded staging range and the complete sector-rounded 1 MiB destination range against enabled E820 type-1 entries before issuing the kernel disk read.
- The range helper rejects incomplete coverage, disabled entries, non-usable entries, and nonzero high base addresses. It accepts a range whose end crosses 4 GiB only when the E820 length high dword or 32-bit addition carry proves that it extends beyond the requested sub-4-GiB end.
- This is deliberately conservative: each requested range must fit inside one enabled usable E820 entry; adjacent usable entries are not combined.
- **Verification:** source and diff cross-check only. The new exact-head Build/boot-test and Security workflows are queued; no NASM assembly, compiler, QEMU, or CI result is claimed for this commit. Keep PR #573 Draft pending both exact-head checks and inspection of native BIOS serial markers.


### USB Mass Storage BOT short-transfer and OUT-stall audit — 2026-11-12 (PR #573)

- Cross-checked the BOT command path against the xHCI bulk transport: CSW length, signature, tag, status, residue bounds, and the data-actual-plus-residue equation are checked before a command can succeed. The xHCI adapter derives actual bytes from the Transfer Event residual length, treats a short bulk-IN packet as termination of that data phase, and rejects short bulk-OUT completion.
- Identified and fixed a transfer-accounting gap: the Bulk OUT transport callback now reports actual bytes transferred on both success and STALL. BOT validates `actual + CSW residue == data length` for OUT as it already did for IN; the xHCI adapter accumulates completed chunks plus the terminal partial chunk and rejects impossible counts.
- Reviewed the endpoint STALL path: BOT clears the affected endpoint halt before reading the CSW, and a malformed/missing CSW enters BOT reset recovery. xHCI timeout/unknown completion poisons the bulk path rather than freeing/reusing DMA still potentially owned by the controller.
- Added mock-media regressions for zero-byte and partial (256/512-byte) Bulk OUT STALL, checking endpoint clear-halt, valid residue accounting, failed-command sense retrieval, and unchanged sector contents. Added inconsistent OUT-residue rejection (BOT reset recovery), clear-halt failure recovery, and unchanged-media checks.
- Existing regressions continue to cover bulk-IN STALL/sense retrieval, inconsistent short-IN transfer/residue recovery, malformed CSW/tag recovery, failed BOT reset poisoning, and detach/reference admission.
- **Verification:** source/diff cross-check only; the expanded transport API and fixtures have not yet been compiled or executed. Exact-head Build/boot-test and Security workflows must run against the final commit before this audit is considered validated. FS-11 remains PARTIAL pending exact-head CI execution and broader xHCI hardware/DMA failure and physical disconnect validation.


### Referenced-block allocation-bitmap mismatch regression — 2026-11-12 (PR #573)

- Extended the adversarial integrity fixture to clear the allocation bit for a physical data block still referenced by a valid file extent. The fixture corrupts both durable and volatile mock-media views so the result is independent of cache dirtiness.
- The report-only integrity scan must return corruption while leaving durable, volatile, and dirty media unchanged. The original bit is restored and a clean follow-up integrity scan must pass.
- Static cross-check ties the fixture to the scanner's explicit `!set && owned` rejection branch. Bitmap-LBA addition and byte-offset multiplication are checked before indexing the mock media.
- **Verification:** source-level only; the new fixture has not been compiled or run. FS-12 remains PARTIAL until exact-head Build/boot-test executes the expanded self-test and the broader integrity test inventory is reviewed against the v1 format invariants.


### Post-merge verification and current release gates — 2026-11-13

- ProsenjitFS ownership/reservation hardening from PR #573 is present on `main` through merge commit `8bf56cc209f538376253c6649be3fec3584a1658`. PR #573 was merged on 2026-11-13; historical notes above that say it was still Draft describe the state at the time they were written.
- PR #574 was merged as `6b690d0c52d432b3e9526e6ff7671fc4f3e07e9b`. Its exact-head Build/boot-test and Security architecture checks passed before merge.
- Post-merge Build/boot-test run [#43382900464](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43382900464), job `131744588708`, passed every step on merge commit `6b690d0c52d432b3e9526e6ff7671fc4f3e07e9b`, including ISO build, ISO QEMU boot, native BIOS QEMU boot, and post-checkout.
- The post-merge native BIOS log contains `native bootloader handoff accepted` and `x86-64 long mode active`. The ProsenjitFS format/mount/allocation/journal/recovery self-test passed in both ISO and native BIOS boot paths.
- Post-merge Security architecture run [#43382900538](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43382900538) also passed.
- These results verify the merged source against the configured self-hosted build/QEMU gates. They do not establish physical-media power-cut durability, physical USB disconnect/hotplug reliability, arbitrary SMP safety, or completion of every fault-injection matrix.

**Remaining native-filesystem release gates (still open):**

1. **FS-11 — fault/recovery completeness:** complete the inventory of reachable read/write/flush failure boundaries across mutation families, cleanup/rollback paths, and USB transport/DMA failure paths. Existing mock-device matrices are not evidence of physical-device durability.
2. **FS-12 — adversarial integrity coverage:** finish the v1 invariant-by-invariant scanner test inventory, including malformed ownership graphs, bitmap/extent mismatches, duplicate namespace entries, orphan allocations, and repeated scans. Keep detection read-only; no automatic destructive repair.
3. **FS-13 — concurrency/lifetime:** current fail-fast VFS/provider gates and per-handle serialization are narrower than general scheduler-aware locking, arbitrary concurrent teardown, or SMP safety. Audit the actual lock/lifetime contract before claiming broader guarantees.
4. **FS-14 — physical durability:** real hardware power-cut testing remains NOT TESTED. Simulated crash injection must not be presented as proof of physical power-loss behavior.

The next implementation work should address one release gate at a time, with source/diff cross-check, exact-head Build/boot-test and Security checks, and no merge until all required checks pass.


### FS-12 multi-orphan block report regression — 2026-11-13

- Extended the read-only integrity self-test with two simultaneously allocated-but-unowned data blocks placed in distinct bitmap bytes.
- The report must return corruption, count both orphan blocks, identify the lowest block deterministically, report no orphan inodes, and provide a nonzero source error location.
- The test snapshots durable, volatile, and dirty mock-media CRCs to prove the scan itself performs no repair or mutation. It restores both injected bits and requires a clean follow-up report with all orphan counters and first-item fields reset.
- This is source-level coverage only until the exact-head Build/boot-test executes the kernel self-test. It does not close the full FS-12 inventory or physical durability gate.


### FS-11 accepted-write coverage extension — 2026-11-13

- Added an operation-scoped `fail_write_after_accept` sweep for `prosenjitfs_preallocate_blocks()`.
- Each reached write position must return EIO, then after simulated power loss/remount the file must have either no preallocation or the complete two-block zeroed extent; partial allocation, leaked ownership, and corrupted integrity state are rejected.
- The sweep disarms any not-reached fault before verification, requires a successful-operation control, checks integrity before and after unlink, and cleans up the mount on every tested path.
- This extends mock-device coverage only. FS-11 remains open for the full reachable failure-boundary inventory, transport/DMA faults, and physical-device durability evidence.


### xHCI runtime port-event poll contention regression — 2026-11-13

- Extended `pros_xhci_port_event_poll_selftest()` to hold the shared event-ring lock while a port-event poll is attempted.
- The poll must fail fast with `PROS_USB_XHCI_EIO`, report zero processed events, leave the lock owned by the original transaction, and preserve the queued non-port completion TRB without consuming or acknowledging it.
- This verifies the event-pump serialization contract in the deterministic ring model. It does not add a runtime worker, prove controller DMA quiescence, or complete USB disconnect/reconnect hotplug.

### FS-11 accepted-write format recovery extension — 2026-11-13

- Added an operation-scoped `fail_write_after_accept` sweep around reformatting an existing valid volume.
- The regression models a write accepted into volatile device cache that returns EIO, then simulates power loss and probes the durable medium.
- Failures before the invalidation barrier must preserve the prior valid filesystem; failures after durable invalidation must leave the volume undiscoverable rather than expose a partially formatted filesystem.
- The sweep requires a successful-format control and multiple reached failure positions. This is mock-device evidence only; physical-media durability remains NOT TESTED and FS-11 remains open for other mutation, transport/DMA, cleanup, and rollback boundaries.


### Superblock journal geometry admission regression — 2026-11-13

- The v1 superblock validator now rejects checksum-valid journal lengths below the format minimum or above the format maximum before accepting metadata-region geometry.
- The mock-device self-test rewrites both redundant superblock copies with recomputed CRC32C and internally consistent shifted metadata-region offsets, then checks that probe and mount reject both undersized and oversized journals.
- A valid-format/remount/integrity control runs after the negative cases. This is structural-admission coverage; it does not change the v1 disk layout or claim physical-media durability.


## 2026-11-13 filesystem gate reconciliation

- PR #577 merged as `2d4425944834cf92161386ddead28ef9d8d98d2d`. Exact PR-head Build/boot-test #10409 and Security architecture #9906 passed. It adds accepted-then-EIO coverage for keep-size preallocation.
- PR #578 merged as `69fb8d0626b421c6a4eaece5fdff9c55e00ad04f`. Exact PR-head Build/boot-test #10412 and Security architecture #9909 passed. It adds accepted-then-EIO coverage for format invalidation/publication.
- PR #579 merged as `fcd53617c4f532a533733af961b5e0687cf8164c`. Exact PR-head Build/boot-test #10414 and Security architecture #9911 passed. It rejects checksum-valid superblocks with journal sizes outside the v1 16..64 block contract and tests both redundant copies with valid recomputed checksums and consistent shifted metadata-region geometry.
- The accepted-then-EIO mock-device mutation coverage now includes format, mkdir, regular-file create-open, truncate-to-zero, same-size overwrite, unlink, append, rmdir, rename, and keep-size preallocation. This is not a claim that every read/short-transfer/flush/DMA boundary is covered.

- PR #581 merged as `16f5a0b0a79afdcfb347f0e24744fce4e42d4dcc`. Exact PR-head Build/boot-test #10419 and Security architecture #9916 passed. The Build log confirms the ProsenjitFS self-test milestone in both ISO QEMU and native BIOS QEMU boot paths. Its preallocation read-failure sweep measures the successful operation's read count, injects EIO at every ordinal read, and checks durable/volatile/dirty media invariance before the successful zero-fill and unlink control.
- FS-11 remains open for the full reachable failure-boundary inventory, transport/DMA faults, cleanup/resource-exhaustion and rollback cases, and physical-device durability evidence. FS-12/FS-13 remain separate gates; no global completion claim is made.


### FS-11 transport short-transfer source audit — 2026-11-13

- The block-device callback ABI returns a status code, not a completed-sector count. A provider must therefore return success only when the full requested range has completed; any partial/ambiguous completion must surface as an I/O error.
- USB BOT's `issue()` path requires a complete CSW, bounds the residue, and checks `data_actual + residue == data_len`. The fake BOT self-test covers short data-IN, partial/stalled data-OUT, inconsistent residue, reset recovery, and subsequent I/O.
- xHCI bulk-IN/OUT paths return actual byte counts and check transfer residuals; bulk-IN treats a short packet as the end of that data phase. This is source-level transport-path evidence, not hardware DMA fault-injection evidence.
- ATA PIO reads/writes wait for DRQ per sector and check completion status before returning success. The later 2026-11-14 correction below records the existing two-sector injected-failure self-test; broader terminal/final-completion and chunk-boundary sweeps remain open.
- Therefore the earlier broad phrase “short-transfer/transport/DMA fault coverage” is too coarse: USB BOT short-transfer mock coverage and bounded ATA fake-I/O mid-command injection exist, while xHCI/controller DMA fault injection and real-device durability remain open.


### FS-12 multiple orphan-inode report regression — 2026-11-13

- Extended the report-only integrity self-test to stage two allocated, checksum-valid, unreferenced file inodes whose parent chains both reach the root.
- The scan must report an exact orphan-inode count of two and the lowest inode number deterministically across repeated scans, while leaving durable, volatile, and dirty mock-media views unchanged.
- The fixture then removes the higher-numbered orphan and requires the next scan to report exactly the remaining lower-numbered inode, proving count/first-item state is recomputed rather than retained.
- This is a regression-test addition; it does not authorize orphan reclamation or close the full FS-12 gate. Exact-head Build/boot-test and Security verification remain required.


### FS-11 ATA PIO terminal-status hardening — 2026-11-13

- Hardened the per-sector DRQ wait path to classify absent (0x00) and floating (0xFF) status as immediate device loss rather than burning the full polling timeout.
- Centralized terminal status classification so device ERR/DF is returned as I/O failure before DRQ readiness is considered; BSY and ordinary not-yet-DRQ states continue polling.
- Added deterministic classifier self-tests for absent, floating, ERR, DF, BSY, RDY, and valid DRQ states.
- Scope is intentionally limited: this is status-classification coverage, not a controller-level injected mid-command failure test. ATA multi-sector partial-command fault injection, xHCI/DMA ownership faults, systematic mutation/journal read/flush sweeps, cleanup/resource-exhaustion tests, and physical-media durability remain open.

### Kernel-owned scheduler thread foundation (2026-11-13)

- Added an explicit thread owner kind so supervisor-only kernel threads are not
  represented as fake user processes.
- Added `thread_init_kernel()` with a NULL process owner and the same guarded,
  supervisor-only kernel-stack allocator used by process threads.
- Scheduler eligibility accepts only explicitly tagged kernel-owned threads
  with no process pointer; malformed owner/process combinations fail closed.
- Added a boot self-test for kernel-thread initialization, run-queue admission,
  removal, blocked-to-ready wakeup, process-context clearing/restoration, and
  guarded stack reclamation.
- Added owner-aware blocked-state handling so a kernel worker remains wakeable
  when no other thread is runnable, plus `thread_reap_kernel()` to reclaim only
  dead, detached kernel-owned threads after active-stack checks.
- Follow-up scheduler audit: both interrupt-switch rollback paths must use the
  same owner-aware blocked/dead classification; otherwise a kernel worker could
  be incorrectly retired if candidate stack activation or CR3 switching fails.
- This validates the kernel-thread lifecycle primitive only: no persistent
  worker, USB event consumer, disconnect teardown, or DMA quiescence is claimed.



## FS-12 gate reconciliation — 2026-11-14

- **Status: PASS for the ProsenjitFS v1 adversarial integrity-scanner acceptance matrix.** This supersedes the older 2026-11-13 pending wording above; it does not declare the entire filesystem release complete.
- Source audit at main commit `9246c86884a4597f7b147ce0bf8076add49f749e` cross-checked the scanner and fixtures in `kernel/prosenjitfs.c`. Coverage includes checksum-valid malformed inode semantics (type, mode, link count, generation, parent index), allocated-but-unlinked symlink and extent ownership, cross-inode directory ownership, duplicate namespace references/names and hash collisions, malformed parent graphs/cycles, duplicate and intersecting extent ownership, referenced-block bitmap disagreement, orphan block/inode reporting, bitmap tail/reservation invariants, deterministic repeated reports/reset behavior, and report-only media immutability.
- The read-only scanner's device-read failure matrix injects EIO at every reached read ordinal and requires EIO classification plus unchanged durable, volatile, and dirty mock-media views. Corruption fixtures restore their original metadata and require a clean follow-up scan.
- Exact-head Build/boot-test run **43577039351** and Security architecture run **43577039338** both passed for source head `d8b18cf2055372f59fa701fe3cec351a2c35a739`, which was then squash-merged as `9246c86884a4597f7b147ce0bf8076add49f749e`. Build logs confirm the ProsenjitFS format/mount/allocation/journal/recovery self-test milestone in both ISO QEMU and native BIOS QEMU boot paths.
- Integrity checking remains read-only: it reports corruption/orphan candidates and does not automatically reclaim or repair them. These are deterministic mock-device/QEMU results, not physical-media certification.
- **Remaining gates:** FS-11 remains PARTIAL for the remaining reachable transport/DMA and cleanup/resource-exhaustion fault inventory; FS-13 remains limited to its documented lock/lifetime guarantees; FS-14 physical power-cut durability remains NOT TESTED.


### FS-11 ATA multi-sector injected-failure evidence correction — 2026-11-14

- **Correction to the 2026-11-13 transport audit:** the statement that no ATA multi-sector partial-command fault test was present is stale. At main commit `6bfdf83ce03b14f770255a0434145dc37e2f5233`, `kernel/ata_pio.c` contains `ata_multi_sector_fault_selftest()`, called by `pros_ata_pio_selftest()`.
- The test uses the existing injectable `pros_ata_io_ops` fake port-I/O backend and exercises the real public block read/write callbacks. For a two-sector request it allows sector one to transfer (exactly 256 16-bit data words), injects ATA ERR on sector two before its data phase, and requires both public operations to return `PROS_BLOCK_EIO`; it also checks that no second-sector data words were transferred.
- This is deterministic fake task-file/status injection, not a physical ATA controller fault or a real drive test. It covers a mid-command per-sector DRQ failure before the second sector's data phase; it does not yet sweep failures at every status poll, after a sector's data phase, during final command completion, or across multi-command chunks.
- **Revised FS-11 gap:** retain broader ATA terminal/final-completion and chunk-boundary fault injection, xHCI/controller DMA ownership fault injection, transport cleanup/resource-exhaustion cases, and physical-device durability as open items. Do not claim hardware DMA or power-loss validation from this self-test.


### FS-11 xHCI DMA ownership and cleanup source audit — 2026-11-14

- Source audit at main commit `d9e64f055f2d4564d5e36047655ec217aad937b4` confirms that a bulk transfer posts a normal TRB against the persistent bounce buffer and rings the endpoint doorbell only after validating controller RUN/Halted/HCE state. Accepted transfer events are checked for event type, slot ID, endpoint DCI, aligned TRB pointer, expected transfer-ring dequeue address, nonzero completion code, and residual length.
- For bulk-IN, bytes are copied from the DMA bounce buffer into the caller buffer only after an accepted completion event. STALL records the endpoint halted state and reports the actual byte count; successful short bulk-IN terminates the current BOT data phase. Bulk-OUT requires the full requested byte count.
- If event dequeue/decoding/acknowledgement is uncertain, or the posted transfer times out, `bulk_io_poisoned` is set. The timeout path explicitly retains the posted TRB and bounce buffer because the controller may still own them. Clear-halt and BOT reset recovery refuse poisoned slots or outstanding transfer-ring entries; recovery is serialized by the shared event-ring lock.
- Controller DMA resources are permanently pinned after `hardware_referenced` is set. The release API takes the event lock and refuses to free any such resources. Controller BAR detach separately requires PCI bus mastering off, RUN clear, and HCHalted set. These are conservative source-level ownership rules, not a controller-level DMA fault-injection test or physical quiescence proof.
- **Cleanup audit correction — 2026-11-14:** the earlier statement that the xHCI pre-publication rollback paths discarded `pros_usb_dma_free()` results is stale. Current `main` checks each free result in `xhci_release_unpublished_slot_dma()` and `xhci_release_unpublished_bulk_dma()`, preserves any descriptor whose page return fails, and retries cleanup before reusing the slot/rings. The bulk path tracks `bulk_cleanup_pending`; the slot path retries retained descriptors before allocating again.
- **PR #612 xHCI halted-endpoint recovery — 2026-11-14:** merged as `bac1c92af600c585eb249b9fb95875730891e3ba` after exact-head Build/boot-test run `43583890892` and Security architecture run `43583890898` both passed. The Build job completed ISO build, ISO QEMU boot, and native BIOS QEMU boot. The change blocks new bulk TD submission while the selected endpoint's halted flag is set; latches `bulk_io_poisoned` after failures in the multi-stage CLEAR_FEATURE/RESET_ENDPOINT/SET_TR_DEQUEUE/BOT Mass Storage Reset recovery path; validates controller/resources before BOT recovery; and adds halted IN/OUT transfer-gate self-tests. This is software-gate and QEMU evidence only, not physical endpoint recovery or hardware disconnect/reconnect validation.
- **PR #614 xHCI unpublished DMA cleanup fault-injection — 2026-11-14:** merged as `ce6543937e4174b9cbbc33c0fc806543b0993838`. Exact-head Build/boot-test run `43590992223` and Security architecture run `43590992211` both passed. The build log confirms ISO build, ISO QEMU boot, native BIOS QEMU boot, ProsenjitFS and ATA self-test milestones, and xHCI ownership/SMI/halt-reset plus polling No-op command completion. The new `xhci_unpublished_dma_cleanup_selftest()` is wired into `pros_xhci_selftest()` and injects a one-shot free failure for one EP0 slot descriptor and one bulk-ring descriptor. It checks that failed descriptors remain owned, independent descriptors are released, retry clears the retained descriptor, and the physical free-page count returns to baseline. This is software self-test and QEMU evidence, not physical USB-device recovery or power-loss durability evidence.
- **Remaining FS-11 gaps:** deterministic failure injection for event acknowledgement and endpoint recovery; broader ATA terminal/final-completion and chunk-boundary fault sweeps; and physical-device durability. Keep FS-11 PARTIAL.
### FS-11 xHCI controller DMA ownership setup fault injection — 2026-11-14

- Extended the in-file xHCI DMA-programming self-test with deterministic PCI callback failures at two ownership-sensitive boundaries: PCI bus-master enable failure after controller DMA pointers have been programmed, and PCI command-register readback failure immediately after the enable attempt.
- Both cases require the setup API to return EIO, retain resources.hardware_referenced, and reject pros_xhci_dma_resources_release() while the controller may still reference the buffers. The test then clears the synthetic pin only after explicitly treating its static MMIO/PCI model as incapable of DMA, releases all allocations, and checks the physical free-page count returns to baseline.
- The existing success path remains covered and verifies DCBAAP, CRCR, ERSTBA, ERSTSZ, interrupter state, and controller RUN/Halted invariants.
- This is deterministic software callback/MMIO-model injection. It does not emulate a real PCI configuration-space failure, actual controller DMA fetch, hardware bus-master quiescence, or physical disconnect/power-loss behavior. Exact-head Build/boot-test and Security workflows are required before accepting the extension.


### FS-11 xHCI slot and bulk-ring allocation exhaustion — 2026-11-14

- Refactored the unpublished EP0 slot-resource allocation and paired bulk-ring allocation into rollback-safe helpers used by the real address-device and Configure Endpoint paths.
- Added a lock-protected, one-shot USB DMA allocator test seam that returns ENOMEM after a caller-selected number of successful allocation calls. The seam is disarmed after the injected failure and can be explicitly cleared by the self-test.
- The xHCI self-test injects ENOMEM at each of the four EP0 resource allocation positions (output context, input context, EP0 transfer ring, control buffer) and both bulk-ring allocation positions (bulk IN and bulk OUT). Every failure must return EIO, clear every descriptor/ring state, and restore the physical free-page count to its pre-test baseline.
- Successful control cases exercise each helper and release all allocations, checking the same free-page baseline. The test is deterministic allocator-level resource exhaustion, not proof of real-world system-wide memory pressure under concurrent workloads.
- Exact-head Build/boot-test and Security workflows are required before accepting this extension. FS-11 remains PARTIAL for the other listed transport/ATA gaps and physical-device durability.

## FS-11 transport and ATA fault-injection reconciliation — 2026-11-14

- **PR #617 — xHCI event-ring acknowledgement failure injection:** merged as `3258641ce4606e690f1557bc10bb23318072f1f8`. Exact-head Build/boot-test run `43593857030` and Security architecture run `43593857034` passed. The deterministic one-shot event-ack failure is injected after event validation/ERDP calculation and before the MMIO acknowledgement write; the self-test requires EIO and a poisoned/fail-closed event ring. This is software-model injection, not actual hardware MMIO readback failure.
- **PR #618 — xHCI endpoint recovery-stage failure injection:** merged as `031e41555551aace917462c1a5b619facb4faaaa`. Exact-head Build/boot-test run `43594624807` and Security architecture run `43594624959` passed. Reset Endpoint, CLEAR_FEATURE, and Set TR Dequeue recovery-stage failures are injected deterministically; tests require EIO, poisoned bulk I/O, and retained halted state. The Set TR Dequeue injection models failure at that stage before actual command submission; it is not a physical controller command-failure test.
- **PR #619 — xHCI controller DMA ownership setup faults:** merged as `1f3cad8ce97ccf0c4ed35e264e14dbd1b9c51248`. Exact-head Build/boot-test run `43595214138` and Security architecture run `43595214072` passed. PCI bus-master enable and command-register readback failures are injected through the software callback/MMIO model; DMA resources remain pinned while controller-visible pointers may be referenced. This is not proof of actual hardware DMA quiescence.
- **PR #620 — xHCI slot/bulk-ring allocation exhaustion:** merged as `ecdcbc355e5ef56d410cfa72e4fbaf2892ef4313`. Exact-head Build/boot-test run `43595693617` and Security architecture run `43595693619` passed. Deterministic ENOMEM injection covers all four unpublished EP0 allocations and both bulk-ring allocations; rollback must clear resource state and restore the free-page baseline. This is allocator-level fault injection, not concurrent system-wide memory pressure.
- **PR #616 — ATA final completion and 256-sector chunk boundary:** merged as `8a1b64853e0ef91357dc68a0c8bb877a011ac00d`. Exact-head Build/boot-test run `43592971198` and Security architecture run `43592971178` passed. Read/write final completion ERR after the 256-word data phase and the 257-sector 256+1 command boundary are covered by fake-I/O tests.
- **PR #621 — ATA status-poll fault sweep:** merged as `8b4b0a9d4e6bd821b0a1965e74d9a10d1d4fff2e`. Exact tested head `5ef91d6632870c87c70adf10e8e96b8b3f3449ca`; Build/boot-test run `43596070483` and Security architecture run `43596070465` passed. The fake ATA status backend measures the successful two-sector read's reached status-poll count, injects sticky ERR at each ordinal, and requires public read/write callbacks to return EIO without transferring more than the requested two sectors. Build logs also confirm ISO build, ProsenjitFS block-init diagnostic, exFAT formatter, ISO QEMU boot, and native BIOS QEMU boot.
- **Reconciled remaining FS-11 boundary:** the previously listed deterministic event-acknowledgement, endpoint-recovery, controller DMA ownership setup, slot/bulk allocation exhaustion, ATA final-completion, chunk-boundary, and status-poll software fault-injection gaps are now covered by the merged changes above. FS-11 remains **PARTIAL** because the complete reachable failure-boundary inventory is not thereby proven exhaustive and physical-device durability/real hardware transport fault behavior remain unverified. Do not interpret QEMU or static software models as physical-device or power-loss certification.

## FS-11 software fault-matrix reconciliation — 2026-11-14

**Software-only acceptance: PASS for the currently enumerated ProsenjitFS, ATA PIO, FatFs disk-adapter, and xHCI software failure paths. Overall FS-11 remains PARTIAL until physical-device durability and real-hardware transport behavior are validated.** This section supersedes older pending wording above where it lists software gaps now covered by the merged changes below.

### ProsenjitFS block-I/O and transaction coverage

The current `kernel/prosenjitfs.c` self-test includes deterministic mock-device failure coverage across these operation families:

- **Read paths:** format probe/mount, inode and data allocation/preflight, preallocation, integrity scan (including populated-image scans), unlink, append, overwrite, truncate, mkdir, create-open, directory enumeration, rename, public file reads/partial reads, and rmdir.
- **Write paths:** format, metadata mode update, mkdir, regular-file creation, unlink, append, truncate, overwrite, rename, rmdir, and keep-size preallocation. Accepted-then-EIO cases explicitly verify the ambiguous-write boundary rather than assuming an error means no media mutation.
- **Flush/recovery paths:** metadata transaction, format publication/invalidation, mkdir, create, unlink, rename, append, truncate, rmdir, preallocation, multi-block file writes, journal replay, and unmount durability barrier. Tests distinguish failure before persistence from an error reported after persistence, discard volatile state, remount, and verify the old-or-complete-new transaction outcome.
- **Allocator/integrity invariants:** full inode/data-bitmap exhaustion, reservation/ownership consistency, report-only integrity behavior, read-failure propagation, and unchanged durable/volatile/dirty media on rejected operations.

### Transport and adapter coverage

- **USB BOT/xHCI:** BOT short data-IN and partial/stalled data-OUT residue accounting; endpoint clear-halt and reset recovery; unpublished DMA cleanup retry; event acknowledgement failure; endpoint recovery-stage failures; controller DMA ownership setup faults; and EP0/bulk-ring allocation exhaustion. These are deterministic software/controller-model tests, not real hardware DMA/disconnect tests.
- **ATA PIO:** per-sector mid-command failure, final completion ERR after the data phase, 256-sector chunk boundary, and a status-poll ordinal sweep for public read/write callbacks.
- **FatFs disk adapter:** PR #625 replaces split in-flight/unbinding atomics with a single packed exclusive-gate plus active-operation count. Tests cover active-I/O rejection of bind/unbind/read-only transitions and fail-closed read/write/ioctl/status behavior while the transition gate is held.
- **Heap/resource rollback:** PR #626 adds one-shot deterministic `kmalloc()` failure injection. The ProsenjitFS mount test sweeps allocation failures through scratch/transaction buffers, root vnode creation, and reservation-registry publication, requiring heap usage to return to baseline and mock media to remain unchanged. The integrity scanner test sweeps all eight temporary graph/index allocations and requires EIO, unchanged heap usage, an active mount, and unchanged durable/volatile/dirty media.

### Exact-head verification

- **PR #625 — FatFs disk admission/unbind serialization:** merged as `a4a14fb4378db2f2c27c3830086cc3654cb6a1ac`. Exact-head Build/boot-test run `43597527608` and Security architecture run `43597527621` passed. QEMU logs include the FatFs disk-adapter and FAT32/exFAT provider self-test markers.
- **PR #629 — FatFs disk-I/O argument and geometry contract tests:** merged as `2ab6042b996c172956b663fb160a40284e02cd87`. Added software self-test assertions for sector count/size, unsupported block-size geometry, null ioctl/read/write buffers, zero-sector requests, invalid physical-drive indices, and fail-closed behavior after unbind. Exact-head Build/boot-test run `43601998880` and Security architecture run `43601998898` passed; both included successful ISO and native BIOS QEMU boot. Tests use only the four-sector in-memory mock device; physical FatFs validation remains deferred to the combined hardware campaign.
- **PR #626 — ProsenjitFS heap-allocation rollback sweeps:** merged as `ffed8eff93eaa56368c697e7300d0a5f2792aa18`. Exact tested head `bd7ec7053cb256e0f1dbf0cfa73b7f2e474b7ed7`; Build/boot-test run `43598475913` and Security architecture run `43598475907` passed. ISO build, ProsenjitFS block-init diagnostic, exFAT formatter, ISO QEMU boot, and native BIOS QEMU boot passed. The ProsenjitFS format/mount/allocation/journal/recovery self-test marker passed in both QEMU paths.

### VFS handle-object lifetime boundary — 2026-11-14

- The per-file and per-directory `operation_busy` fields are fail-fast operation gates. They serialize API operations only while the caller-provided handle structure remains allocated and stable.
- The VFS does not own or reference-count the storage containing `struct pros_vfs_file` or `struct pros_vfs_dir`. A caller sharing one handle across execution contexts must keep that structure alive and at a stable address until every API call using it has returned, including calls rejected with `EBUSY`.
- Therefore these gates do **not** make concurrent close-versus-free, arbitrary handle destruction, or arbitrary SMP teardown safe. Callers needing shared-handle lifetime must provide an outer ownership/reference protocol before invoking VFS APIs.
- Vnode references are a separate mechanism: `vfs_vnode_get()` requires an already-owned reference or another lifetime guard, and cannot make an unowned raw pointer safe against concurrent provider reclamation.
- Existing callback re-entry self-tests verify fail-fast rejection while a handle operation is in progress. They are deterministic single-CPU tests, not a proof of arbitrary SMP interleavings.

### Remaining boundary — intentionally deferred

- **FS-11 overall remains PARTIAL:** software fault injection does not prove physical drive power-loss durability, real ATA device/controller failure behavior, real xHCI DMA quiescence, physical USB disconnect/hotplug, or controller behavior under actual bus timing.
- **FS-12:** PASS for the ProsenjitFS v1 adversarial integrity-scanner acceptance matrix; detection remains report-only and does not automatically repair or reclaim.
- **FS-13:** accepted only for the documented single-CPU fail-fast VFS/provider lifetime contract. Scheduler-aware blocking locks, arbitrary concurrent teardown, and SMP safety are not claimed.
- **FS-14:** physical power-cut durability remains NOT TESTED and is deliberately deferred to the later combined physical-test campaign with FatFs, as requested.

Do not mark physical durability complete from mock-device fault injection or QEMU. Keep the physical campaign separate from the now-passing software regression gates.

## Native ProsenjitFS non-physical completion reconciliation — 2026-11-14

This section is the current release-gate interpretation and supersedes older checkpoint wording above where an earlier entry still says that a software-only test is pending. Native filesystem means **ProsenjitFS only**. FatFs is a separate compatibility provider and is not counted toward native ProsenjitFS feature completion.

| Gate | Non-physical acceptance | Current status |
|---|---|---|
| FS-01–FS-10 | Format/probe/mount, redundant metadata admission, allocation/extents, directories/names, journal recovery, file I/O, mutation/read/write/flush failure handling | Implemented and covered by the merged deterministic regression suite; retain exact-head CI as the merge gate for future changes |
| FS-11 | Enumerated ProsenjitFS mock-device read/write/flush/recovery failures; ATA PIO and xHCI/BOT software-model fault paths; resource-exhaustion and rollback tests | **SOFTWARE-ONLY PASS** for the currently enumerated paths. Physical-drive durability, real controller/device timing, actual DMA quiescence, and physical disconnect/reconnect remain outside this status |
| FS-12 | Adversarial read-only integrity scanner, ownership graph, duplicate/shared extent and namespace references, orphan reporting, deterministic results, read-failure handling, media immutability | **PASS** for the ProsenjitFS v1 acceptance matrix; scanner remains report-only and never repairs or reclaims |
| FS-13 | Supported VFS/provider lifetime and serialization contract | **PASS within the documented single-CPU fail-fast contract**: callers serialize each mounted instance and keep caller-owned handle storage alive/stable during API calls; mount/namespace/provider gates and vnode reference ownership rules are enforced. This is not a claim of arbitrary concurrent close/free, scheduler-aware blocking locks, or SMP safety |
| FS-14 | Physical power-cut durability on a declared real drive/controller/cache configuration | **NOT TESTED — physical campaign only** |

### Software completion boundary

- The latest software-only matrix includes ProsenjitFS operation-specific read/write/flush and accepted-then-EIO recovery coverage for format, metadata, create, mkdir, unlink, append, overwrite, truncate, rename, rmdir, preallocation, multi-block writes, journal replay and unmount barriers.
- Supporting transport software tests include ATA per-sector/final-completion/status-poll/chunk-boundary faults and xHCI/BOT event acknowledgement, endpoint recovery, DMA ownership setup, unpublished-resource cleanup, and allocation-exhaustion models. These are software-injected models, not hardware certification.
- Mount allocation-failure and integrity-scratch allocation-failure sweeps verify heap/resource rollback and media immutability.
- FS-12's ownership graph covers malformed parent graphs/cycles, duplicate and intersecting extent ownership, symlink extent ownership, cross-inode directory aliases, bitmap disagreement, orphan blocks/inodes, and deterministic report reset.
- FS-13 intentionally specifies the current single-CPU supported contract. Do not silently broaden it into SMP or arbitrary concurrent object-destruction guarantees.

### Physical-only work retained

- Real drive power-cut and recovery campaign (FS-14).
- Real ATA/controller fault behavior and physical cache/flush semantics.
- Physical xHCI DMA quiescence, USB disconnect/reconnect/hotplug, and safe physical-media removal.

These items require physical equipment and remain pending by design. They must not block completion of the software-only ProsenjitFS v1 gates, and software/QEMU evidence must never be substituted for physical evidence.

**Merge policy:** one focused PR at a time. Cross-check the diff against current `main`, run exact-head Build/boot-test and Security architecture checks, inspect relevant QEMU serial milestones, and merge only after both required checks pass. Keep the physical campaign PR open until real hardware evidence is recorded.


## Final ProsenjitFS v1 non-physical status and exact-head verification — 2026-11-14

This is the latest status snapshot for the current `main` revision. It clarifies the release boundary: **all currently defined ProsenjitFS v1 non-physical completion gates are accepted; physical-device validation remains deliberately deferred.** This does not claim that every long-term capability listed in the repository's design goals (for example snapshots, CoW, encryption, quotas, compression, online resize, or SMP-safe VFS) is implemented in v1.

### Exact current-main verification

- Verified `main` commit: `19a86627a57b75b15f1611650e6da897132b6e42`.
- Build/boot-test check: **SUCCESS** — workflow run [43616515858](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43616515858), job [132512678727](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43616515858/job/132512678727).
- Security architecture check: **SUCCESS** — workflow run [43616515843](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43616515843), job [132512685583](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43616515843/job/132512685583).
- The build log contains the required ProsenjitFS format/mount/allocation/journal/recovery, native LBA-boundary, ATA PIO → MBR → ProsenjitFS persistent-mount, FatFs block-adapter, FAT32/exFAT mount/I/O/remount, and USB FAT32/exFAT partition readback success markers.
- QEMU was intentionally run under a bounded timeout; exit status 124 was accepted by the workflow after the required serial milestones were observed. This is software/QEMU evidence, not physical-device evidence.
- Runner selection used the GitHub-hosted runner (`RUNNER_MODE: github-hosted`); this verification does not claim that a local runner was used.

### Current v1 scope and filesystem roles

- **ProsenjitFS** is the only native filesystem and the only filesystem counted toward native filesystem v1 completion.
- **FatFs R0.16** is a separate kernel-integrated compatibility provider for FAT32/exFAT, connected through the existing kernel VFS provider interface. Its provider self-tests and software/QEMU USB partition tests are present in the current build.
- **NTFS and other compatibility filesystems** are not part of the current v1 completion claim. They remain future provider work and must not be conflated with ProsenjitFS or the FatFs provider.
- The separate generic user-space provider ABI proposal in PR #107 is not a dependency for current FatFs operation. That PR was closed unmerged; its branch is retained as a future architecture backup.

### Release-gate conclusion

- FS-01–FS-10: implemented and covered by the merged deterministic regression suite.
- FS-11: **software-only acceptance PASS for the currently enumerated paths**; overall hardware/transport validation remains partial until the physical items below are tested.
- FS-12: PASS for the ProsenjitFS v1 adversarial integrity-scanner acceptance matrix.
- FS-13: PASS only within the documented single-CPU fail-fast VFS/provider lifetime contract; no arbitrary SMP or concurrent handle-destruction guarantee is made.
- FS-14: **NOT TESTED — physical power-cut campaign only**.

### Physical campaign — the only intentionally retained v1 validation work

1. Real-drive power-cut and recovery testing under the documented FS-14 acceptance plan.
2. Real ATA/controller fault behavior and physical cache/flush semantics.
3. Physical xHCI DMA quiescence, USB disconnect/reconnect/hotplug, and safe physical-media removal.
4. Physical FAT32/exFAT media validation as part of the same combined hardware campaign.

Keep these validations pending until real hardware and recorded evidence are available. Never infer physical durability or hardware safety from QEMU, mocks, or software fault injection. The FS-14 campaign remains tracked separately in [PR #602](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/602).

**Conclusion:** no additional non-physical ProsenjitFS v1 completion gate is currently identified as pending. Future code changes must still pass the one-PR-at-a-time source review and exact-head Build/boot-test plus Security checks before merge.
