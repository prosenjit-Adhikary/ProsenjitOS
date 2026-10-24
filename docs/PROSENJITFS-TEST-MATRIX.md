# ProsenjitFS Test Matrix — 2026-10-24

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
| FS-09 | Create/overwrite write-failure sweep | PR #230 merged | Create-open and overwrite block-write fault sweeps (plus mkdir/rmdir write cases) require recovered state to be fully old/new or absent/present as specified, with clean integrity | **PASS on main `3df77093edec9a6961a507fe05f4c32420d73391`**: [Build/boot-test #40118316719](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40118316719), [Security #40118316931](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40118316931) |
| FS-10 | Rmdir write-failure sweep | Baseline sweep in PR #230; strengthened assertion port in current branch | After each block-write fault and simulated power loss, recovered directory must be absent or a valid root child; clean integrity before cleanup | **Baseline PASS** on main `3df77093edec9a6961a507fe05f4c32420d73391` with Build/boot-test #40118316719 and Security #40118316931. Enhanced parent/cleanup assertions are **PENDING** on `eefb231ba4577d989b2170d245010685cf4d3493`. |
| FS-11 | Other read/write/flush failure paths | Mock-device fault injection in ProsenjitFS self-test | Sweep each reachable superblock/journal/bitmap/inode/extent/directory read/write/flush; no stale injection contaminates verification | **PARTIAL**; inventory and add targeted cases in implementation plan |
| FS-12 | Integrity scan read-only guarantee | `prosenjitfs_check_integrity`, report API | Check ownership/reachability/checksums/bounds; report orphan candidates without mutating media; distinguish EIO from malformed on-disk data | Implemented API; caller serialization and full adversarial coverage remain open |
| FS-13 | Concurrency and object lifetime | VFS vnode refs, file/mount state, `io_lock` | Explicit lock contract and interleaving tests cover mutation vs scan and rename/unlink vs open file lifetime | **NOT VERIFIED** as general thread-safe VFS behavior |
| FS-14 | Actual hardware durability | ATA PIO and hardware-backed device | Power-cut testing on target hardware with declared cache/flush assumptions | **NOT TESTED**; QEMU/mock-device results are not hardware certification |

| FS-15 | Allocator geometry and bitmap boundaries | Current branch `fix/prosenjitfs-allocator-geometry-currentmain-20261024` | All allocator/index helpers reject 0, 511, 513, 4097, and 8192-byte geometry via the shared `pfs_block_size_valid()` predicate before division/indexing; output sentinels and durable/volatile/dirty mock state stay unchanged. Exhaustion returns ENOSPC; last valid inode/data bit is handled correctly. | **PENDING exact-head CI** at `eefb231ba4577d989b2170d245010685cf4d3493` |

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
