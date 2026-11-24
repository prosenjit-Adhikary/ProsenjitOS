# FatFs vendored source provenance

- Upstream: ChaN, FatFs Generic FAT Filesystem Module
- Upstream integration mirror: https://github.com/zephyrproject-rtos/fatfs
- Pinned commit: 4f60a12c58c3de4246857ad87c40239ecd93349b
- Upstream release: R0.16 (July 22, 2025)
- Source baseline: R0.16 with upstream patch-1 and patch-2 applied by the Zephyr mirror.
- Security review: the mirror documents the 2026 R0.16 issues CVE-2026-6682, CVE-2026-6683 and CVE-2026-6687 as fixed by upstream patch-2; it also documents the local zero-fill hardening for CVE-2026-6686 and the CVE-2026-6685 readability-only change. See https://elm-chan.org/fsw/ff/patches.html and the pinned mirror README.
- FatFs source license: source redistribution must retain the copyright notice, license condition and disclaimer. The original notice is retained verbatim in each upstream source file.
- exFAT note: FatFs documentation notes that Microsoft exFAT patent/licensing requirements may apply depending on product destination and distribution. Review before shipping binaries or products.
- Local files: ff.c, include/ff.h, include/diskio.h, include/ffconf.h, option/ffunicode.c.
- Local changes: `ffconf.h` and the kernel disk/provider adapters are ProsenjitOS-specific. `ff.c` includes a documented ProsenjitOS exFAT mount hardening patch: validate that the allocation bitmap `DataLength` covers at least one bit per cluster, does not exceed the cluster-heap byte capacity, is backed by a contiguous FAT chain with sufficient capacity, and marks the root-directory and bitmap-file clusters as allocated. exFAT sector-size and sectors-per-cluster shift fields are range-checked before shifting, preventing undefined behavior on malformed boot sectors. This closes malformed-short-bitmap and out-of-range chain cases that the pinned upstream mount path did not fully validate. The pinned Zephyr mirror's documented downstream deltas (including the CVE-2026-6686 zero-fill hardening and CVE-2026-6685 readability-only change) remain part of the selected source baseline; see UPSTREAM_README.md.
- Configuration: `FF_FS_NOFSINFO=3` ignores untrusted FAT32 free/next-cluster hints; read/write enabled; exFAT and 64-bit LBA enabled; UTF-8 LFN enabled; 255 UTF-16 code-unit LFN capacity and 1020-byte UTF-8 FILINFO buffer; variable sector size 512-4096; one logical FatFs drive per storage partition slot; formatting disabled; no trim; no RTC dependency; non-reentrant FatFs calls must be serialized by the provider.
- FAT16/FAT12 are not supported by the ProsenjitOS provider even though the upstream library contains their parsers. The provider must reject them after mount classification.
- Upstream source file blob IDs:
  - ff.c: 739077c068e298eb96e40b40de53b1710c988cc1
  - include/ff.h: ef678fb7a685d6bf0da807f0edbdec8b45b47250
  - include/diskio.h: c3cda9661e5f9f1b5d3080dd3cb4692896ee8549
  - option/ffunicode.c: 54ad526710595e64716d0bc6d01e8461e1b3821d
- Upstream README was reviewed and is retained as UPSTREAM_README.md.
