# Third-party notices

This file indexes third-party components identified in the repository-tree and provenance review. It is not a complete SBOM and does not replace upstream license files or notices. The inventory must be rechecked against the exact release tree, build manifests, generated/bundled artifacts, and binary packaging before each release.

## FatFs

- **Component:** FatFs Generic FAT Filesystem Module
- **Upstream author/project:** ChaN
- **Upstream release:** R0.16 (July 22, 2025), with upstream patch-1 and patch-2 as described in the provenance record
- **Integration mirror:** https://github.com/zephyrproject-rtos/fatfs
- **Pinned mirror commit:** `4f60a12c58c3de4246857ad87c40239ecd93349b`
- **Local provenance record:** [third_party/fatfs/UPSTREAM.md](third_party/fatfs/UPSTREAM.md)
- **Vendor snapshot record:** [third_party/fatfs/README.vendor.md](third_party/fatfs/README.vendor.md)
- **Upstream README:** [third_party/fatfs/UPSTREAM_README.md](third_party/fatfs/UPSTREAM_README.md)
- **Imported upstream source files:** `ff.c`, `include/ff.h`, `include/diskio.h`, and `option/ffunicode.c`; `include/ffconf.h` is the project-specific configuration.
- **License/redistribution terms:** FatFs source redistribution requires retaining the copyright notice, the stated redistribution condition, and the disclaimer. The original notice is retained in the upstream source files. Do not replace it with the repository's proposed custom license.
- **Local modifications:** See `third_party/fatfs/UPSTREAM.md` for the pinned baseline, local configuration, adapter details, and documented exFAT mount-hardening patch.
- **Distribution note:** The upstream provenance documentation flags possible Microsoft exFAT patent/licensing requirements depending on product destination and distribution. This is not a legal conclusion; obtain qualified review before shipping where relevant.

### FatFs notice (reproduced from the vendored source header)

Copyright (C) 2025, ChaN, all right reserved.

FatFs module is an open source software. Redistribution and use of FatFs in source and binary forms, with or without modification, are permitted provided that the following condition is met:

1. Redistributions of source code must retain the above copyright notice, this condition and the following disclaimer.

This software is provided by the copyright holder and contributors "AS IS" and any warranties related to this software are DISCLAIMED. The copyright owner or contributors be NOT LIABLE for any damages caused by use of this software.


- **Notice sampling:** the opening source headers of `ff.c` and `include/ff.h` were inspected and contain the ChaN copyright, redistribution condition, and disclaimer. `include/diskio.h` carries the ChaN 2025 copyright attribution in its header; the complete applicable FatFs condition/disclaimer is reproduced above in this notice file. The connector did not return a readable line excerpt for the 1.97 MB `option/ffunicode.c` file, so its complete header was not independently verified in this pass. The pinned upstream blob ID for that file is recorded in `UPSTREAM.md`.

## Repository-tree audit snapshot — 2026-11-16

The following is a reproducible **tree-level inventory**, not a legal provenance certification:

- Audited Git tree: `61f285a342fb1719c4935b4a578b37b1235330da` (PR #646 head at the time of audit).
- GitHub recursive tree response: **238 paths**, `truncated=false`.
- Third-party/vendor path identified in the tree: `third_party/fatfs/` only. Its exact files are listed above and in its provenance records.
- No `.gitmodules` path or Git submodule entry was present in that tree.
- Root build metadata found: `Makefile`; no `Cargo.toml`, `CMakeLists.txt`, `package.json`, `go.mod`, or `meson.build` was present in the audited tree.
- No checked-in archive or binary payload with common `.zip`, `.tar`, `.gz`, `.xz`, `.bin`, `.img`, `.iso`, `.efi`, `.dll`, `.exe`, `.sys`, `.a`, or `.o` extensions was found by the tree-path filter.
- Only the root `LICENSE` file matched the tree-path license/notice filename filter. The PR branch's root file is a **draft proposal**; the base `main` branch still has the previously published MIT license. Historical MIT permissions are not retroactively withdrawn.
- The build uses the vendored FatFs include/source paths. The project documentation separately identifies ReactOS, Wine, Linux and vendor security products as architectural/behavioral references; those references are not evidence that their source code is vendored.

### What this audit does not establish

A Git tree path scan cannot prove the provenance or license of every original source line, copied snippet, generated file, font, image, sound, or future downloaded build dependency. It also does not inspect release assets outside the Git tree, transient CI downloads, or every file's embedded copyright header. GitHub code search used during this review indexes the default branch rather than this PR branch, so it was not treated as an exhaustive scan of the PR head. The repository therefore **must not** claim a completed SBOM, complete copyright clearance, or that FatFs is necessarily the only third-party-origin material.

Before release, complete a source/header and asset-level provenance scan on the exact release tree, inspect CI dependency downloads and packaging scripts, compare imported code against declared upstream sources, and verify every notice against its upstream license text.

## Inventory status

FatFs is the only separately vendored component identified by the audited PR-head tree and the reviewed provenance documents. The tree-level inventory is recorded above, but the full source-provenance/SBOM and release-package audit remains open. Preserve every component's own license and required attribution; the ProsenjitOS same-name license proposal does not relicense third-party material.
