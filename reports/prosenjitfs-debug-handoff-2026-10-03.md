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
