# ProsenjitFS Debugging Handoff Log

Date: 2026-10-02
Repository: prosenjit-Adhikary/ProsenjitOS
Area: ProsenjitFS mkdir/transaction diagnostics and rename/unlink debugging

## Current objective

Continue debugging the ProsenjitFS self-test failure without weakening or skipping tests. Do not patch source until the exact failing operation and return path are proven.

## Current runtime failure

Latest QEMU self-test result:
- rc = 0xFFFFFFFFFFFE1C7C
- signed rc = -123780
- mkdir_stage = 0
- mkdir_rc = 0
- tx_stage = 9
- tx_count = 4

The first /dir1 self-test encodes a mkdir failure as:
- `-(123000 + (uint32_t)(-mkdir_rc))`

Machine code confirmed this is actually:
- call `vfs_mkdir`
- `lea r14d,[rax-0x1e078]` where 0x1e078 = 123000
- therefore `-123780` proves `vfs_mkdir()` returned **-780**.

## VFS correlation

`vfs_mkdir()` calls the filesystem vnode mkdir callback and returns its rc unchanged. The callback is `pfs_vnode_mkdir()` at 0x126100.

Therefore the -780 is not introduced by VFS.

## pfs_vnode_mkdir() complete-symbol inspection

`nm -S -n` reports:
- `pfs_vnode_mkdir` start = 0x126100
- size = 0xC11 bytes
- end ≈ 0x126d11

The full function was then inspected for the literal -780 byte pattern. Searching for `fcf4` / `f4 fc ff ff` produced **no matches**.

Conclusion: -780 is not embedded as a literal constant in `pfs_vnode_mkdir()`; it is being produced dynamically or propagated from another call.

## Common mkdir error path

Disassembly around 0x126a30-0x126a80 shows:

```
0x126a34 call 0x1220d0 <pfs_inode_bitmap_stage>
0x126a45 je  0x126b1e
0x126a4b mov rdi,[r14]
0x126a4e mov pfs_mkdir_diag_stage,0xF
0x126a58 mov pfs_mkdir_diag_rc,eax
...
```

Thus the common error path copies the failing operation's `eax` directly into `pfs_mkdir_diag_rc`; it does not transform -780 there.

The same common error target 0x126a4b is reached by:
- `pfs_inode_bitmap_stage()`
- two `pfs_allocator_stage()` calls
- four `pfs_tx_add()` calls
- `pfs_tx_commit()`

All branch to 0x126a4b when their return value is nonzero.

The normal transaction sequence reaches four `pfs_tx_add()` calls and then `pfs_tx_commit()`.

## Important diagnostic implication

Because the runtime printed `mkdir_stage=0` and `mkdir_rc=0`, the -780 failure appears **not** to have gone through the common `pfs_vnode_mkdir()` error path that writes stage 15/other stages.

However, this is not yet enough to declare the source/runtime inconsistent: we still need to identify the exact return instruction/path of `pfs_vnode_mkdir()` and determine whether -780 can escape without updating the diagnostics.

## Previously established

- tx_stage=9 means `pfs_tx_commit()` reached the journal-clear call; it does not by itself prove journal clear failed.
- tx_count=4 confirms four transaction blocks were staged before commit.
- `pfs_vnode_mkdir()` resets mkdir and transaction diagnostics at entry.
- The source contains normal VFS error mappings; no literal -780 was found in source or function machine code.
- Existing local changes remain untouched.
- No source patch has been made during this investigation.

## Next exact action

Inspect the complete `pfs_vnode_mkdir()` return/exit control flow, especially:
1. all `ret` instructions;
2. the value in `eax` immediately before each ret;
3. the paths at 0x126a4b and 0x126d09;
4. whether any error can reach a ret without setting `pfs_mkdir_diag_stage/rc`.

Do not modify source until the -780 producer/escape path is proven.
