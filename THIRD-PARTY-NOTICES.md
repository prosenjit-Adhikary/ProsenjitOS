# Third-party notices

This file indexes third-party components currently identified in the repository review. It is not a complete SBOM and does not replace upstream license files or notices. The inventory must be rechecked against the exact release tree, build manifests, submodules, generated/bundled artifacts, and binary packaging before each release.

## FatFs

- **Component:** FatFs Generic FAT Filesystem Module
- **Upstream author/project:** ChaN
- **Upstream release:** R0.16 (July 22, 2025), with upstream patch-1 and patch-2 as described in the provenance record
- **Integration mirror:** https://github.com/zephyrproject-rtos/fatfs
- **Pinned mirror commit:** `4f60a12c58c3de4246857ad87c40239ecd93349b`
- **Local provenance record:** [third_party/fatfs/UPSTREAM.md](third_party/fatfs/UPSTREAM.md)
- **Upstream README:** [third_party/fatfs/UPSTREAM_README.md](third_party/fatfs/UPSTREAM_README.md)
- **License/redistribution terms:** FatFs source redistribution requires retaining the copyright notice, the stated redistribution condition, and the disclaimer below. The original notice is retained in the upstream source files. Do not replace it with the repository MIT license.
- **Local modifications:** See `third_party/fatfs/UPSTREAM.md` for ProsenjitOS configuration, adapter details, and the documented exFAT mount-hardening patch.
- **Distribution note:** The upstream provenance documentation flags possible Microsoft exFAT patent/licensing requirements depending on product destination and distribution. This is not a legal conclusion; obtain qualified review before shipping where relevant.

### FatFs notice (reproduced from the vendored source header)

Copyright (C) 2025, ChaN, all right reserved.

FatFs module is an open source software. Redistribution and use of FatFs in source and binary forms, with or without modification, are permitted provided that the following condition is met:

1. Redistributions of source code must retain the above copyright notice, this condition and the following disclaimer.

This software is provided by the copyright holder and contributors "AS IS" and any warranties related to this software are DISCLAIMED. The copyright owner or contributors be NOT LIABLE for any damages caused by use of this software.

## Inventory status

FatFs is the third-party component explicitly documented in the reviewed provenance material. A full repository-wide license/SBOM audit has not yet been completed, so this file must not be interpreted as proof that FatFs is the only third-party component. Add each additional component here only after verifying its exact upstream source, version/commit, license text, notices, and local changes. Preserve every component's own license and required attribution.
