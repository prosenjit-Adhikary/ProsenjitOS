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
- therefore -123780 proves `vfs_mkdir()` returned **-780**.

## VFS correlation

`vfs_mkdir()` calls the filesystem vnode mkdir callback and returns its rc unchanged. The callback is `pfs_vnode_mkdir()` at 0x126100.

Therefore the -780 is not introduced by VFS.

## pfs_vnode_mkdir() complete-symbol inspection

`nm -S -n` reports:
- `pfs_vnode_mkdir` start = 0x126100
- size = 0xC11 bytes
- end ≈ 0x126d11

The full function was inspected for the literal -780 byte pattern. Searching for `fcf4` / `f4 fc ff ff` produced **no matches**.

Conclusion: -780 is not embedded as a literal constant in `pfs_vnode_mkdir()`; it is being produced dynamically or propagated from another call.

## pfs_vnode_mkdir() return-slot breakthrough

The complete return sequence is:

```
0x1263b7: mov eax,DWORD PTR [rsp+0x8]
...
0x1263cc: ret
```

There is only one actual `ret` in the complete 0x126100..0x126d11 function.

The return value is therefore whatever 32-bit value is present at `[rsp+0x8]` immediately before the final epilogue.

The complete function search for writes to `[rsp+0x8]` found the key producer:

```
0x126244: call 0x121f60 <pfs_find_dir_entry>
0x126249: mov DWORD PTR [rsp+0x8],eax
```

Therefore the first /dir1 mkdir failure can be traced as:

```
pfs_find_dir_entry()
        -> returns -780
pfs_vnode_mkdir [rsp+0x8] = -780
pfs_vnode_mkdir() -> -780
vfs_mkdir() -> -780
selftest -> -123780
```

This also explains why the failure can occur with `mkdir_stage=0` and `mkdir_rc=0`: the return-slot assignment happens before the later mkdir diagnostic stages.

Other return-slot writes seen in the function include explicit VFS/PFS error values such as -1, -5, -7, -9, -11, -12 and success 0. No literal -780 is present in these writes.

## pfs_find_dir_entry() inspection

Symbol:
- start = 0x121f60
- size = 0x16e bytes
- end = 0x1220ce

Complete disassembly shows explicit return values:
- `0x1220c7: eax = -1`
- `0x122090: eax = -4`
- `0x1220b0: eax = -6`
- `0x1220b7: eax = +5`

There is no `mov eax,-780` or equivalent literal in this function.

Source definition is at `kernel/prosenjitfs.c:2484` and is:

```
static int pfs_find_dir_entry(const struct pros_prosenjitfs *fs,
                              const struct pros_prosenjitfs_inode *dir_inode,
                              const char *name,
                              struct pros_prosenjitfs_dir_entry *found)
{
    struct pros_prosenjitfs_dir_index index;
    struct pros_prosenjitfs_dir_entry entry;
    uint32_t name_length;
    uint32_t hash;
    uint64_t current;
    uint64_t steps = 0u;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    if (pfs_read_dir_index(fs, dir_inode, &index) != PROSENJITFS_OK)
        return PROSENJITFS_ECORRUPT;
    hash = pfs_name_hash((const uint8_t *)name, name_length);
    current = index.heads[hash % PROSENJITFS_DIR_BUCKETS];
    while (current != 0u) {
        if (++steps > fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_dir_entry(fs, current, &entry) != PROSENJITFS_OK)
            return PROSENJITFS_ECORRUPT;
        if (entry.name_hash == hash && entry.name_length == name_length) {
            uint32_t equal = 1u;
            for (uint32_t i = 0u; i < name_length; ++i)
                if (entry.name[i] != (uint8_t)name[i]) {
                    equal = 0u;
                    break;
                }
            if (equal) {
                if (found)
                    pfs_copy(found, &entry, sizeof(*found));
                return PROSENJITFS_OK;
            }
        }
        current = entry.next;
    }
    return PROSENJITFS_ENOENT;
}
```

Thus the source-level function also has no -780 return path.

## Important revised diagnosis

At this point, -780 is proven to enter `pfs_vnode_mkdir()` through the return value of `pfs_find_dir_entry()`, but `pfs_find_dir_entry()` source and compiled explicit return paths do not generate -780.

The strongest remaining hypotheses are:
1. stack/register corruption around the `pfs_find_dir_entry()` call/return;
2. corruption of `pfs_find_dir_entry()`'s saved return-state or caller return slot;
3. ABI/prototype mismatch or incorrect compiled object/interface;
4. memory overwrite in a callee used by `pfs_find_dir_entry()`, especially `pfs_utf8_name_valid`, `pfs_read_dir_index`, or `pfs_read_dir_entry`.

A normal source-level `return` from `pfs_find_dir_entry()` should not produce -780, so the next diagnostic must establish the actual value of `eax` at the call return and verify the caller's stack slot immediately before and after the call.

## Previous transaction findings

The common mkdir error path is:

```
0x126a34 call 0x1220d0 <pfs_inode_bitmap_stage>
0x126a45 je  0x126b1e
0x126a4b mov rdi,[r14]
0x126a4e mov pfs_mkdir_diag_stage,0xF
0x126a58 mov pfs_mkdir_diag_rc,eax
```

The common error target 0x126a4b is also reached by:
- two `pfs_allocator_stage()` calls
- four `pfs_tx_add()` calls
- `pfs_tx_commit()`

The normal transaction sequence reaches four `pfs_tx_add()` calls and then `pfs_tx_commit()`.

`tx_stage=9` means `pfs_tx_commit()` reached the journal-clear call; it does not by itself prove journal clear failed.

`tx_count=4` confirms four transaction blocks were staged before commit.

## Existing local changes

Existing local changes remain untouched:
- `kernel/prosenjitfs.c`: 24 insertions, 11 deletions
- `setup-prosenjitos-agent.sh`: mode 100644 -> 100755, no content change

No source patch has been made during this investigation.

## Next exact action

Do **not** patch source yet.

First inspect the source and compiled layout of:
1. `struct pros_prosenjitfs_dir_index`
2. `struct pros_prosenjitfs_dir_entry`
3. `pfs_read_dir_index()`
4. `pfs_read_dir_entry()`

The immediate objective is to determine whether any buffer/size mismatch can corrupt the caller's stack or whether the compiled code/prototype is inconsistent.

After that, use a narrowly targeted runtime/disassembly check around the `pfs_find_dir_entry()` call to prove the actual `eax` value at return.

