# ProsenjitOS Debug Handoff — 2026-10-03

## Scope

This log records the current filesystem/VFS/CPL3 debugging work so another AI can continue from GitHub without relying on chat history.

## Starting point

- Base commit before the current CPL3 source fix: `ec49875` — `fs: restore deterministic native error mapping`.
- Current work is on `main`.

## CPL3 build failure diagnosed and fixed

The kernel build reported:

- `user_space` undeclared in `kernel/kernel.c`
- `user_mode_handle` undeclared; existing handle is `user_test_handle`
- `user_ipc_channel` defined but unused

The source was corrected to:

1. Remove the unused `user_ipc_channel`.
2. Use the already initialized `init_process.address_space`.
3. Allocate private physical pages for the user stack and user test-data page.
4. Map the linked `.user_text` image pages into `PROSENJIT_USER_CODE_VA`.
5. Map the user stack at `PROSENJIT_USER_STACK_VA` with writable + NX.
6. Map the user test-data page at `PROSENJIT_USER_TEST_VA` with writable + NX.
7. Switch to `init_process.address_space`.
8. Enter CPL3 with `user_test_handle`.

This source fix was synced to GitHub in commit `fe8e16d`:
`kernel: fix CPL3 test address-space setup`.

## VFS self-test build blocker diagnosed

The next build failure was:

```
kernel/vfs.c:1111:9: error: implicit declaration of function ‘vfs_probe_selftest’
```

Current `vfs_security_selftest()` calls:

- `vfs_probe_selftest()`
- `vfs_security_admission_selftest()`
- `vfs_vnode_api_selftest()`

Git history confirmed these helpers existed previously:

- `430d180` — `vfs: add filesystem probe regression selftest`
- `83b6b32` — `vfs: complete vnode path file and directory API`
- `a489089` — `fs: remove legacy filesystems from kernel`

Current source inspection showed that `vfs_security_admission_selftest()` and `vfs_vnode_api_selftest()` are already present in the current `kernel/vfs.c`; only `vfs_probe_selftest()` was missing.

## Probe design reconciliation

Current `vfs_probe_filesystem()` is intentionally ProsenjitFS-only:

- invalid device/arguments -> `PROS_VFS_EINVAL`
- valid ProsenjitFS superblock on sector 0 or 1 -> `PROS_VFS_OK`, `PROS_FS_PROSENJIT`
- unsupported media -> `PROS_VFS_ENOSYS`
- native probe errors are preserved as VFS error mappings

Current `prosenjitfs_probe()` validates:

- sector size between 512 and 4096 and power-of-two
- sector count >= 256
- reads sector 0, then sector 1
- returns `PROSENJITFS_OK` when either contains a valid superblock
- returns `PROSENJITFS_ENOTSUP` when neither is valid

The historical probe self-test from `430d180` tested NTFS/exFAT/FAT32 and is stale for the current ProsenjitFS-only kernel design. It must not be blindly restored.

A new current-design `vfs_probe_selftest()` was added to `kernel/vfs.c`. It currently checks:

- null device -> `PROS_VFS_EINVAL`
- null type -> `PROS_VFS_EINVAL`
- undersized device geometry -> `PROS_VFS_EINVAL`
- 256-sector blank device -> `PROS_VFS_ENOSYS` and `PROS_FS_UNKNOWN`

The test uses `256u` directly because `PFS_MIN_BLOCKS` is a private macro defined only in `kernel/prosenjitfs.c`.

## Latest build result

After adding the current-design probe self-test, the build progressed past the previous `vfs_probe_selftest` implicit-declaration blocker, but failed in `kernel/prosenjitfs.c`:

```
pfs_vnode_rename: implicit declaration of function ‘pfs_find_dir_entry_lba’
pfs_vnode_rename: unused variable ‘steps’
pfs_vnode_rename: unused variable ‘current’
static declaration of ‘pfs_find_dir_entry_lba’ follows non-static declaration
pfs_find_dir_entry_lba defined but not used
```

Relevant locations:

- `pfs_vnode_rename()`: around lines 2235–2319
- `pfs_find_dir_entry_lba()` definition: around line 2956
- Existing forward-declaration block is at lines 13–37 and currently declares `pfs_find_dir_entry` but not `pfs_find_dir_entry_lba`.

This is now the active build blocker.

## Current status

- CPL3 source fix is synced to GitHub in `fe8e16d`.
- Debug handoff log was synced in `c017e83`.
- Current VFS probe self-test source patch is present locally but has not yet been synced as a completed fix.
- Clean full build is currently blocked by the `pfs_vnode_rename()` declaration/unused-variable errors above.
- QEMU runtime verification has not yet been completed.
- Do not claim the project currently builds or boots successfully until these tests are run.

## Next step

Fix the `pfs_find_dir_entry_lba()` forward declaration/order and remove or otherwise reconcile the now-unused `current` and `steps` locals in `pfs_vnode_rename()`. Then rebuild. After a green build, run the QEMU serial self-tests, update this log with actual results, and sync the completed changes to GitHub.
