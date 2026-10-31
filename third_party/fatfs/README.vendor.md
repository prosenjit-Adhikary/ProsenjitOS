# FatFs vendor snapshot

- Project: ChaN FatFs, Generic FAT Filesystem Module
- Version: R0.16 with upstream patch-1 and patch-2, plus the downstream security hardening present in the pinned Zephyr mirror.
- Source repository: https://github.com/zephyrproject-rtos/fatfs
- Pinned commit: 4f60a12c58c3de4246857ad87c40239ecd93349b
- Commit date: 2026-08-27
- Upstream README identifies the source release as R0.16 and documents its patch/security state.
- Imported source files: `ff.c`, `include/ff.h`, `include/diskio.h`, and `option/ffunicode.c`.
- Local configuration: `ffconf.h` (FAT32 + exFAT, UTF-8 LFN API, 64-bit LBA, variable 512–4096-byte sectors, no formatting API).
- License: retain the copyright notice and disclaimer in every imported source file. FatFs permits source redistribution provided the source notice and disclaimer are retained. No warranty is provided.
- This is a pinned source snapshot, not a git submodule. Re-baseline only after reviewing upstream release notes, patches, CVEs, and the local diff.

## Security provenance

The pinned Zephyr mirror states that its `ff.c` contains R0.16 patch-1 and patch-2, and a local fix for CVE-2026-6686 (zeroing the newly exposed region when `f_lseek()` extends a file). It also documents the CVE-2026-6685 range-check expression as intentional and harmless. Preserve this provenance and review the exact source diff before upgrading.

## ProsenjitOS policy

- The provider must reject FAT12 and FAT16 even though FatFs can parse them.
- `FF_USE_MKFS=0`: no formatting API is built or exposed.
- `FF_FS_NOFSINFO=3`: do not trust free-cluster or next-cluster hints from FAT32 FSInfo.
- `FF_FS_REENTRANT=0`: the ProsenjitOS provider serializes all FatFs calls globally because no kernel mutex interface is available yet.
- The FatFs disk-I/O API is an adapter boundary; it must map only to the mounted partition view, never to the parent disk without checked bounds.
- The pinned library is integrated through `kernel/fatfs_disk.c` and `kernel/fatfs_provider.c`, with FAT32/exFAT probe, mount, VFS file/directory operations, and a synthetic FAT32 mount/read/write/remount fixture. The fixture and boot self-tests are added but still require exact-head compiler/QEMU execution; a full exFAT mount fixture and physical removable-media test remain outstanding.
