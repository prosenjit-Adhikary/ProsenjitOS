# ProsenjitOS Debug Handoff — 2026-10-03

## Scope

This log records the current filesystem/VFS/CPL3 debugging work so another AI can continue from GitHub without relying on chat history.

## Completed fixes

- CPL3 test address-space setup fixed and synced in `fe8e16d`.
- Missing current-design `vfs_probe_selftest()` restored in `kernel/vfs.c`.
- ProsenjitFS-only probe behavior retained; stale NTFS/exFAT/FAT32 probe tests were not restored.
- `kernel/prosenjitfs.c` declaration/local-variable build blockers were corrected locally after targeted compiler diagnostics.
- Storage volume discovery self-test geometry was corrected to use a 257-sector test disk with a valid MBR partition spanning LBA 1 through LBA 256.
- The storage self-test stack-overflow/double-fault blocker was fixed by making the large `struct storage_test_disk` test buffer static instead of allocating it on the kernel stack.

## Build result

The clean full build now succeeds.

Generated artifacts:

- `build/prosenjitOS.iso`
- `build/prosenjit-native-bios.img`
- Native kernel sectors: 313
- Native ProsenjitFS partition: LBA 2048 + 512 sectors

## Latest QEMU runtime result

After the storage test fixes, a clean rebuild and QEMU run reached:

```
ProsenjitOS: partition discovery self-test passed
ProsenjitOS: storage volume discovery self-test passed
ProsenjitOS: ERROR ProsenjitFS self-test failed rc=0xFFFFFFFFFFFFFFF4 mkdir_stage=0x0000000000000000 mkdir_rc=0x0000000000000000 tx_stage=0x0000000000000000 tx_count=0x0000000000000000
Terminated
```

The return value `0xFFFFFFFFFFFFFFF4` is signed `-12`, which maps to the range-check block in `prosenjitfs_selftest()` around the first `pfs_lba_add()` boundary assertion.

The current `pfs_lba_add()` implementation is:

```c
if (!device || !result || base >= device->sector_count)
    return PROSENJITFS_EINVAL;
if (offset > UINT64_MAX - base)
    return PROSENJITFS_EOVERFLOW;
*result = base + offset;
if (*result >= device->sector_count)
    return PROSENJITFS_EINVAL;
return PROSENJITFS_OK;
```

The self-test currently expects `pfs_lba_add(&range_device, 8, 8, &lba)` with `sector_count = 16` to return `PROSENJITFS_EINVAL` and leave `lba == 0`. Source inspection shows that expectation is consistent with the implementation, so the remaining `-12` failure still requires a focused runtime/source-path correction rather than another declaration or build investigation.

## Root cause identified — ProsenjitFS `-12`

The `-12` self-test failure was caused by a subtle output-parameter contract bug in `pfs_lba_add()`.

For the boundary case `base=8`, `offset=8`, `sector_count=16`, the function correctly detected `16 >= 16` and returned `PROSENJITFS_EINVAL`, but it wrote `16` into `*result` before performing that validity check. The self-test intentionally requires a rejected operation to leave its output unchanged (`lba` must remain `0`). Therefore the return code was correct but the output parameter was incorrectly modified, producing self-test `-12`.

Fix synced in commit `f6c64ce`: validate the computed LBA against the device range before assigning it to `*result`.

This is now a source-level root-cause fix, not a stale-build or error-code mismatch.

## Current status

- Build: PASS
- ISO generation: PASS
- BIOS image generation: PASS
- Early kernel boot: PASS
- Partition discovery self-test: PASS
- Storage volume discovery self-test: PASS
- ProsenjitFS self-test: FAIL with `-12`
- QEMU runtime: FAIL at ProsenjitFS self-test
- The local ProsenjitFS/VFS/storage fixes still need final source synchronization after the runtime path is green.

## Next action

Focus only on the `prosenjitfs_selftest()` `-12` range-check path. Do not repeat the already-fixed storage geometry, stack-overflow, CPL3, or declaration investigations. Resolve the actual runtime mismatch, rebuild, rerun QEMU, and continue until the ProsenjitFS self-test and subsequent self-tests succeed. Keep this log updated at each completed milestone and sync completed source changes to GitHub.


## Synchronization update — 2026-10-03 14:05 IST

GitHub main now contains the actual ProsenjitFS `-12` root-cause fix in `f6c64ce` (`fs: preserve LBA result on rejected range`) and the analysis was verified against the self-test contract.

The local checkout has four modified files and is still based on `ea22517`; the attempted `git pull --ff-only origin main` was safely aborted because local `kernel/prosenjitfs.c` would be overwritten. Current local modifications are `kernel/kernel.c`, `kernel/prosenjitfs.c`, `kernel/storage.c`, and `kernel/vfs.c`. No local work was lost.

Do not force-reset or discard these files. Reconcile the local changes with GitHub `main` first, then perform a clean build and QEMU runtime test. The post-fix runtime has not yet been verified, so ProsenjitFS must not yet be marked PASS.

## Diagnostic milestone — 2026-10-06 15:25 IST

The current ProsenjitFS self-test runtime failure was narrowed further using the compiled ELF, source, and exact transaction return paths.

Runtime output:

```
ProsenjitOS: ERROR ProsenjitFS self-test failed rc=0xFFFFFFFFFFFE19F1 mkdir_stage=0x0000000000000000 mkdir_rc=0x0000000000000000 tx_stage=0x0000000000000009 tx_count=0x0000000000000004
```

Decoded self-test result:

- `0xFFFFFFFFFFFE19F1` = `-123407`
- Encoder base = `123000`
- `mkdir_stage = 0`
- `tx_stage = 9`
- `tx_count = 4`
- The residual encoded raw value is `317`

The compiled `pfs_tx_commit()` return paths were inspected. The relevant common return paths contain:

- `0x10f6b0: eax = -1`
- `0x10f73f: eax = -8`
- `0x10f756: eax = 0xffffd601`
- `0x10f77a: eax = -2`
- `0x10f784: eax = -1`

There is no compiled `pfs_tx_commit()` return path producing `-317`. Therefore the `317` embedded in `-123407` is not evidence of a real `-317` transaction error.

The compiled transaction path also proves that stage 9 is reached and then tail-jumps to `pfs_journal_clear()`. The current `pfs_journal_clear()` implementation only has explicit error returns `-101` through `-106`, so it likewise cannot directly generate `-317`.

The current self-test source is:

```c
mkdir_rc = vfs_mkdir(&fs.mount, "/dir1");
if (mkdir_rc != PROS_VFS_OK) {
    uint32_t raw = pfs_mkdir_diag_rc < 0 ? (uint32_t)(-pfs_mkdir_diag_rc) : 0u;
    return -(123000 + pfs_mkdir_diag_stage * 1000u +
             pfs_tx_diag_stage * 10u + raw);
}
```

The diagnostic globals are direct globals:

```c
static uint32_t pfs_mkdir_diag_stage;
static int pfs_mkdir_diag_rc;
static uint32_t pfs_tx_diag_stage;
```

and `pfs_mkdir_diag_rc` is reset to `PROSENJITFS_OK` at both diagnostic-reset and `pfs_vnode_mkdir()` entry.

The compiled self-test assembly around the failure was also inspected. On the failing path, the assembly loads a pre-call stack value into `ecx`; if `pfs_mkdir_diag_rc >= 0`, the branch skips the instructions that replace `ecx` with `-pfs_mkdir_diag_rc`. The later encoder subtracts `ecx`. This matches the observed residual value `317` and explains why the encoded error can disagree with the printed diagnostic globals.

The build script confirms that `kernel/prosenjitfs.c` is compiled with GCC `-O2` and freestanding/kernel flags:

```
CFLAGS=(-m64 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -I"$ROOT/kernel/include" -I"$ROOT/include" -Wall -Wextra -Werror -O2)
```

### Conclusion at this milestone

Do **not** change `pfs_tx_commit()` or `pfs_journal_clear()` based on `-123407`. Their compiled return paths do not support a `-317` error.

The immediate diagnostic defect is in the self-test failure encoding path: the runtime `317` does not come from `pfs_mkdir_diag_rc`, which is reported as zero. The next investigation should prove the exact generated code/source interaction and the actual `vfs_mkdir()` return value before changing filesystem transaction logic.

Current status remains: **ProsenjitFS self-test FAIL; root cause of the actual mkdir return is not yet proven.** No source fix has been made for this milestone.


## Build blocker — 2026-10-06 17:34 IST

A fresh clone of GitHub `main` failed the clean build in `kernel/elf_loader.c:56` because `eh->phnum` is `uint16_t`, so the comparison against `UINT64_MAX / sizeof(struct elf64_phdr)` is provably always false under GCC `-Werror=type-limits`. The check was redundant because `ELF_MAX_PHNUM` already bounds `phnum` to 128. The impossible overflow check was removed in commit `a0c6b846` so the clean GitHub tree can build without suppressing the warning.
