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

Source definition is at `kernel/prosenjitfs.c:2484` and is the current implementation previously recorded in this log.

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

## 2026-10-02 22:25 IST update

### New findings

The directory-format definitions and read helpers were inspected.

- `pros_prosenjitfs_dir_index` is a packed 284-byte structure.
- `pros_prosenjitfs_dir_entry` is a packed 292-byte structure.
- Both are smaller than the 4096-byte local read buffers.
- `pfs_read_dir_index()` copies exactly `sizeof(*index)` and validates the structure/checksum.
- `pfs_read_dir_entry()` copies exactly `sizeof(*entry)`, validates the fields/checksum, and then validates the UTF-8 name.
- `pfs_copy()` is a bounded byte-copy loop with no visible overrun mechanism.
- `pfs_utf8_name_valid()` only writes through its caller-provided `length_out` pointer and has no visible out-of-bounds write.

### Revised diagnosis

The proven chain remains:

selftest -> vfs_mkdir() -> pfs_vnode_mkdir() -> pfs_find_dir_entry() result -> -780

However, neither the source nor compiled explicit return paths of `pfs_find_dir_entry()` contain -780. The struct sizes and copy sizes also do not reveal an obvious overflow.

The investigation is therefore narrowed to:
1. the actual runtime EAX value immediately after the `pfs_find_dir_entry()` call;
2. stack/register corruption around that call/return;
3. ABI/prototype or compiled-object mismatch;
4. a lower-level callee in the read path, especially `pfs_read_block()` or the device layer.

### Next exact action

Do not patch source yet. Inspect the exact compiled call site around `pfs_find_dir_entry()` and its entry/exit, then verify the ABI/prototype. If needed, use a narrowly targeted QEMU/GDB check to capture EAX immediately after the call. Only after that will we trace into the specific lower-level callee.

### Time estimate

For this specific -780 mkdir failure, a realistic estimate is:
- 10–20 minutes for call-site/ABI verification;
- 20–45 minutes for targeted runtime/disassembly isolation;
- 15–45 additional minutes to patch, rebuild, and test once the concrete corruption/return-path bug is identified.

Overall: roughly 45–120 minutes to identify and fix this specific failure, assuming it remains confined to the currently traced mkdir path. Completion will only be claimed after a clean rebuild and QEMU self-test confirms the fix.

## 2026-10-02 23:10 IST update — native LBA readback mismatch

The exact failure string reported from the local tree is:

```
kernel/kernel.c:1410: serial_write("ProsenjitOS: ERROR native LBA trace readback data mismatch\\n");
```

The current `main` source around this path was inspected. The diagnostic performs the following checks before the mismatch comparison:

1. Opens `/.__pfs_lba_trace` for read.
2. Reads the complete `trace_read` buffer.
3. Closes the file.
4. Retrieves the filesystem-reported physical read LBA.
5. Verifies the partition-layer parent LBA.
6. Verifies the ATA-layer read LBA.
7. Requires the ATA read LBA to equal the partition parent read LBA.

Only after all of those checks succeed does the code execute the byte-for-byte comparison:

```
for (uint32_t i = 0u; i < sizeof(trace_read); ++i)
    if (trace_read[i] != trace_write[i]) {
        serial_write("ProsenjitOS: ERROR native LBA trace readback data mismatch\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
```

### Analysis

This separates the failure from an LBA-boundary mismatch.

If the observed failure is specifically `native LBA trace readback data mismatch`, the diagnostic has already passed the physical-LBA/partition-LBA/ATA-LBA consistency checks immediately before the byte comparison.

Therefore **do not currently identify LBA mapping as the root cause**.

The direct proven condition is:

```
trace_read[i] != trace_write[i]
```

for at least one byte.

The remaining data-path possibilities include:
- write-side buffer/data preparation;
- transaction commit/journal persistence;
- block-device/ATA write;
- block-device/ATA read;
- filesystem extent/block mapping;
- VFS read reconstruction;
- buffer corruption between layers.

### Required next evidence

Before changing allocator, journal, extent, or device code, capture:

- first mismatching byte index;
- expected byte from `trace_write[]`;
- actual byte from `trace_read[]`;
- physical write LBA;
- partition-parent write LBA;
- ATA write LBA;
- physical read LBA;
- partition-parent read LBA;
- ATA read LBA.

Then compare the same block at progressively lower boundaries:

```
write buffer
  -> transaction/commit
  -> physical block storage
  -> direct physical read
  -> filesystem/extent read
  -> VFS read buffer
```

The first boundary where the byte changes is the responsible layer to investigate.

### Current conclusion

The native LBA trace mismatch is a **real data-integrity/readback failure**, but this evidence alone does not prove allocator or journal corruption. The previous -780 mkdir investigation and this LBA readback failure must remain separate until their causal relationship is demonstrated.

No source weakening, skipped test, changed expected value, or completion claim is justified yet.

## 2026-10-02 23:40 IST update — decisive QEMU/GDB runtime proof

A clean rebuild was performed from HEAD `e9a9afae13933945684cbbfe41a18f19398fdaad`.

ELF symbols after the clean build remained:
- `vfs_mkdir = 0x10d5e0`
- `pfs_find_dir_entry = 0x121f60`
- `pfs_vnode_mkdir = 0x126100`

The full compiled disassembly of `pfs_find_dir_entry()` was inspected. Its explicit return instructions are:
- `0x1220c7: mov eax,0xffffffff` = -1
- `0x122090: mov eax,0xfffffffc` = -4
- `0x1220b0: mov eax,0xfffffffa` = -6
- `0x1220b7: mov eax,0x5` = +5

There is still no compiled literal or explicit return of -780.

A QEMU instance was started paused with a GDB stub, and GDB used a **symbol breakpoint** on `pfs_find_dir_entry` rather than the earlier hard-coded return address.

Runtime result:

```
Breakpoint 1, 0x0000000000121f60 in pfs_find_dir_entry ()

=== ENTERED pfs_find_dir_entry ===
rax = 0x0
eax = 0x0

=== RETURNED FROM pfs_find_dir_entry ===
rax = 0xfffffffa
eax = 0xfffffffa
signed EAX = -6

caller:
0x1277df <pfs_vnode_lookup+223>: cmp $0xfffffffa,%eax
0x1277e2 <pfs_vnode_lookup+226>: je ...
```

### Decisive correction to the earlier model

The runtime proof **disproves the previous assumption that the failing mkdir call itself receives -780 directly from `pfs_find_dir_entry()`**.

The observed invocation of `pfs_find_dir_entry()` was from **`pfs_vnode_lookup()`**, not `pfs_vnode_mkdir()`.

The function correctly returned **-6 (`PROSENJITFS_ENOENT`)**, which is a valid source-level and compiled return value.

Therefore:
- `pfs_find_dir_entry()` is not producing -780.
- The earlier static call-site at `pfs_vnode_mkdir+0x144` is not, by itself, proof that the runtime failure came through that call; the same helper is used from multiple filesystem paths.
- The -780 source remains unresolved.
- The GDB session also confirms QEMU itself stayed alive with no host-side error; the previous apparent QEMU exit was not reproduced in this run.

### Current root-cause boundary

The investigation must now move **up one level** and identify the exact runtime caller/instance that eventually returns -780 to `vfs_mkdir()`.

The next required proof is:
1. break at `pfs_vnode_mkdir`;
2. capture its entry registers/arguments;
3. break at its actual `pfs_find_dir_entry` call/return;
4. capture the returned EAX for that specific invocation;
5. capture the final `pfs_vnode_mkdir` return value;
6. capture `vfs_mkdir` return value immediately before self-test encoding.

Until that chain is captured, **do not attribute the failure to allocator, journal, extent, ATA, or directory corruption**.

### Status

- Clean rebuild: PASS
- ELF/layout consistency: PASS
- GDB runtime connection: PASS
- `pfs_find_dir_entry()` runtime return proof: PASS
- `pfs_find_dir_entry() -> -780`: **DISPROVEN**
- Actual -780 root cause: **NOT YET PROVEN**
- Source patch from this diagnostic: **NONE**
- Test weakening/skipping: **NONE**


## 2026-10-03 00:05 IST update — LBA contract hardening for NTFS and native storage

The LBA path was reviewed end-to-end before the next NTFS runtime attempt. The repository already had LBA tracing in the native PFS, partition, and ATA layers; this work does not claim to have created the original LBA implementation. The goal here was to make the LBA contract explicit and checked so NTFS uses the same device-local sector addressing rules without relying on unchecked arithmetic.

### LBA contract now made explicit

- A block-device LBA is a device-local sector index.
- The partition layer translates local partition LBA to parent-device LBA by adding the partition's first_lba.
- ATA receives the parent-device LBA and validates the complete requested range before programming the controller.
- NTFS cluster-to-LBA conversion produces a volume-device-local LBA; sector-within-cluster addition is now checked through the common block-device LBA helper.
- Range and addition checks reject overflow/out-of-device requests instead of allowing wrapped LBA values.

### Implemented changes

1. kernel/include/block.h — added block_device_lba_range_valid() and block_device_lba_add().
2. kernel/block.c — implemented the checked helpers and extended the block self-test with valid-boundary and invalid-range cases.
3. kernel/partition.c — partition read/write now uses the common range validation and checked local-to-parent LBA addition.
4. kernel/ata_pio.c — ATA request validation now uses the common block-device LBA range check.
5. kernel/ntfs.c — ntfs_cluster_lba() validates the complete cluster span, and sector-within-cluster LBA addition is checked instead of using unchecked arithmetic.

### Why this matters for NTFS

NTFS operates on a partition-local block device. Its MFT, MFTMirr, runlist, index allocation, and file-data accesses therefore need consistent volume-local LBA calculations, while the partition layer owns translation to the parent disk LBA. The hardened path preserves that separation instead of making NTFS aware of the partition offset.

This is defensive correctness work, not evidence that LBA was the previously proven cause of the -780 mkdir failure or the native byte-mismatch failure. The earlier native LBA diagnostic already showed that the partition and ATA read-LBA chain could be consistent while the returned data byte was still wrong.

### CI status at this update

The latest LBA hardening commits have triggered GitHub Actions. Earlier LBA diagnostic commit 62a1dce... completed with failure in both Build and Security workflows; the failure logs were not retrievable through the connector at this time. The newest hardening commit db8b4fd... was queued when this update was recorded. Therefore the new LBA hardening is not yet CI-verified.

### Next runtime proof

When the system is available again, run the existing native LBA trace and NTFS bootstrap/index tests without changing expected values. Required evidence:

NTFS logical cluster -> NTFS volume-local LBA -> partition parent LBA -> ATA LBA -> physical sector -> returned data

Any mismatch must be fixed at the first boundary where the value/data diverges. NTFS should not be disabled merely to hide an LBA failure; temporary NTFS isolation remains a separate fallback only if runtime evidence shows NTFS itself is the blocker.

## 2026-10-03 00:20 IST update — NTFS LBA audit extended

The first LBA hardening pass exposed additional unchecked NTFS sector-index additions in metadata/data paths. These were audited and converted to the common checked device-local LBA addition helper, including bitmap, MFT mirror, metadata write, file growth, file write/read, UpCase, index allocation read/write, and other production paths that previously used `lba += sector_index` or `lba + sector_index` directly.

A test-only use of the new LBA helper was also corrected immediately: the block self-test now stores the helper result in a dedicated `lba_result` variable rather than reusing the health-counter baseline. This keeps the LBA assertions independent of the existing error-counter assertions.

Latest NTFS LBA audit commit: `0a3d02b0341a990c9df7756acd421a5d7ce5b558`.

This is still implementation/hardening, not runtime proof. GitHub Actions for the preceding commits were completing with failure and their job logs were unavailable through the connector. The newest commit has triggered fresh workflows and requires CI verification before it is treated as validated.

## 2026-10-03 — LBA/NTFS semantic contract verification

### Microsoft NTFS semantic basis

Microsoft documentation defines LCN (Logical Cluster Number) as the cluster number relative to the beginning of the volume; the first cluster is LCN 0. VCN is the cluster number relative to the beginning of a file/directory/stream. The NTFS volume data structure separately exposes NumberSectors, TotalClusters, BytesPerSector, BytesPerCluster, MftStartLcn and Mft2StartLcn. Microsoft also documents a retrieval-pointer base as the sector offset to the first LCN relative to the start of the volume.

References:
- Microsoft Learn, NTFS volume data: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-ntfs_volume_data_buffer
- Microsoft Learn, MS-FSCC glossary (LCN/VCN): https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/8ac44452-328c-4d7b-a784-d72afd19bd9f
- Microsoft Learn, volume management control codes / FSCTL_GET_RETRIEVAL_POINTER_BASE: https://learn.microsoft.com/en-us/windows/win32/fileio/volume-management-control-codes

### Established address hierarchy

    NTFS file offset
      -> VCN / file-relative cluster
      -> NTFS runlist
      -> LCN / volume-relative logical cluster
      -> LCN * sectors_per_cluster
      -> NTFS volume-relative LBA
      -> partition-relative LBA
      -> parent-device LBA
      -> ATA

**LCN != physical LBA.** NTFS computes a volume/device-local sector location. The partition view is responsible for adding its first_lba exactly once when translating to the parent device.

### Actual mount-device semantics

ntfs_mount() receives the block device passed by its caller and stores that pointer through vfs_mount_init(). There is no hidden partition offset inside NTFS. When a filesystem is mounted from a partition view, fs->mount.device is therefore partition-relative and partition translation occurs exactly once below it.

### Hardening performed

- NTFS sector-within-cluster additions use block_device_lba_add().
- NTFS LCN-to-LBA multiplication is checked before multiplication and the complete cluster span is range-validated.
- NTFS runlist parser length/offset arithmetic is checked without wrapping uint32_t values.
- NTFS signed runlist LCN deltas are checked for int64_t overflow before accumulation; 8-byte sign extension avoids a shift-by-64 undefined operation.
- Partition entry-array and local-to-parent translation paths use checked LBA operations.
- Partition trace reverse translation is checked rather than performing unchecked parent-LBA addition.
- ATA multi-sector chunk advancement uses the common checked LBA helper.

### Verification added

- LCN 0 -> volume LBA 0.
- LCN 1 -> volume LBA 8 for the synthetic 512-byte/8-sector-cluster geometry.
- Highest valid synthetic LCN 511 -> volume LBA 4088.
- LCN 512 is rejected because its complete cluster lies outside the 4096-sector volume.
- Last sector within a cluster (sector index 7) is accepted; sector index 8 is rejected.
- Independent multiplication-overflow guard test.
- NTFS runlist malformed-length and signed-delta overflow coverage.
- Partition local LBA 0 -> parent LBA 1 and local LBA 1 -> parent LBA 2 are traced explicitly, proving the partition offset is applied once in the test path.

### CI/runtime status

Implementation hardening has been committed, but a fresh build, filesystem regression, ISO/QEMU/native-BIOS verification, security checks, and GitHub Actions results are still required before the LBA/NTFS work can be declared complete. The separate ProsenjitFS -780 mkdir investigation remains unresolved and is not attributed to this LBA work without runtime evidence.


## 2026-10-03 — Kernel filesystem scope deliberately narrowed to native ProsenjitFS

A new architectural decision was applied after reviewing the Linux NTFS3/ntfs-3g model and the ProsenjitOS storage/VFS boundaries:

- The kernel image now contains **only native ProsenjitFS** as a filesystem implementation.
- The previous in-kernel FAT32, exFAT, exFAT Up-case, and NTFS implementations were removed from the kernel source tree and build.
- The VFS filesystem-type contract was reduced to `UNKNOWN` and `PROS_FS_PROSENJIT`.
- `vfs_probe_filesystem()` now probes only ProsenjitFS; legacy BPB/OEM probing was removed.
- Storage discovery now accepts only `PROS_FS_PROSENJIT`.
- FAT32/exFAT/NTFS self-tests and their CI-required serial markers were removed because those implementations are no longer kernel components.
- The build script no longer compiles or links the removed filesystem objects.
- Stale NTFS-specific boot diagnostics were removed from `kernel/kernel.c`.
- Documentation and roadmap were updated to define FAT32/exFAT/NTFS as **future external filesystem providers**, not kernel filesystems.

This does **not** mean FAT32/exFAT/NTFS support is abandoned permanently. The intended future architecture is:

```
VFS
 |
 +-- native ProsenjitFS (kernel)
 |
 +-- filesystem-provider boundary
      +-- future NTFS provider
      +-- future exFAT provider
      +-- future FAT32 provider
```

The compatibility providers must use the common block-device/partition contract and must not put parser-specific filesystem code back into kernel-core.

Current verification state at handoff:
- Main branch contains the architecture change at commit `b0525062012b02285d50a0229a7f161f4f09ed1b`.
- GitHub Actions Build and Security workflows for that latest commit were queued at the time of this report update; green CI has **not** yet been claimed.
- The native ProsenjitFS runtime/LBA investigation remains independent: the previously unresolved mkdir self-test return `-780` has not been attributed to NTFS/FAT32/exFAT code.
