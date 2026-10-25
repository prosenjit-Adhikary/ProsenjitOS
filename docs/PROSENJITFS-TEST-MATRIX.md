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
**Status source:** [Completion matrix](PROSENJITFS-COMPLETION-MATRIX.md)  
**Work plan:** [Implementation plan](PROSENJITFS-IMPLEMENTATION-PLAN.md)

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


- [x] **FS-27 | CRC-valid journal header with duplicate targets** | `kernel/prosenjitfs.c`, native ProsenjitFS self-test | Create a durable committed transaction stopped before home-block replay, duplicate two journal target LBAs, recompute both header and commit checksums, and require mount recovery to return `PROSENJITFS_ECORRUPT` before replay | **PASS** on exact head `935d5ef715f6c81dc502269d584c7fa3f25cf728`: Build/boot-test [#40171924978](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40171924978) and Security architecture [#40171924948](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40171924948); PR #255 merged as `d6e4f0998cbaa57d7234803011e3bbe21680b241`. Deterministic mock-device only; no hardware durability claim.


## FS-32 — Invalid journal target geometry (CI pending)

- [x] Checksum-valid target equal to `total_blocks`.
- [x] Checksum-valid target inside the journal region.
- [x] Recompute header and commit checksums to isolate structural validation.
- [x] Assert recovery returns `PROSENJITFS_ECORRUPT` without durable, volatile, or dirty mock-medium mutation.
- [ ] Exact-head Build/boot-test and Security architecture CI pass.
