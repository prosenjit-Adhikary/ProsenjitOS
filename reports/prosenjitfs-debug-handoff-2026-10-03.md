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

Current `vfs_security_selftest()` still calls:

- `vfs_probe_selftest()`
- `vfs_security_admission_selftest()`
- `vfs_vnode_api_selftest()`

but current `kernel/vfs.c` contains only the caller; the three helper definitions are missing.

Git history confirmed these helpers existed previously:

- `430d180` — `vfs: add filesystem probe regression selftest`
  - `vfs_probe_selftest()` at historical line 723.
- `83b6b32` — `vfs: complete vnode path file and directory API`
  - `vfs_security_admission_selftest()` at historical line 153.
  - `vfs_vnode_api_selftest()` at historical line 499.
- `a489089` — `fs: remove legacy filesystems from kernel`

Historical source inspection showed that the missing self-tests were real implementations, not imaginary declarations.

### Historical probe self-test

The `430d180` implementation creates a synthetic 512-byte block device and verifies filesystem probing for:

- NTFS
- exFAT
- FAT32
- unknown/unsupported media
- invalid sector-size metadata

It expects the appropriate `PROS_VFS_*` return codes and filesystem types.

### Historical admission self-test

The `83b6b32` implementation verifies:

- admission before mount activation is rejected
- activation succeeds
- verified/allow admission succeeds
- duplicate activation returns `PROS_VFS_EBUSY`
- deactivation succeeds
- admission after deactivation is rejected

### Historical vnode API self-test

The `83b6b32` implementation verifies:

- vnode initialization
- path lookup including `..\`
- file open/read/seek/close
- directory open/readdir/close
- file-vs-directory type errors
- mount deactivation

## Current status

- CPL3 compile errors identified above have been fixed and synced to GitHub.
- VFS self-test helper implementations have been located in Git history.
- The VFS helpers have **not yet been restored** in the current source at the time this log was written.
- A clean full build and QEMU runtime verification therefore still need to be performed.
- Do not claim the project currently builds or boots successfully until those tests are run.

## Next step

Restore the three historical self-test implementations into the current `kernel/vfs.c`, reconcile them with the current ProsenjitFS-only probe logic, rebuild, then run the QEMU serial self-tests. Update this log again with the actual build/runtime results before the next GitHub sync.
