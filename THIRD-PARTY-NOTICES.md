# Third-Party Notices

This file records third-party components currently identified in the ProsenjitOS
repository during the license audit dated 2026-11-16. It is an engineering
inventory, not a legal opinion or a guarantee that every historical file,
generated artifact, or transitive build dependency has been discovered.

## License scope

The root `LICENSE` file contains the MIT License for ProsenjitOS-original
material authored for this project, to the extent that the relevant material is
owned by the project copyright holder and is not identified as third-party
material.

The root MIT license does **not** replace, relicense, or override the license
terms of third-party components. Each such component remains under its own
applicable license and notices. When redistributing source or binaries, review
the applicable component terms and preserve all required notices.

## Inventoried third-party components

### FatFs — ChaN

- Component: FatFs Generic FAT Filesystem Module
- Upstream: https://elm-chan.org/fsw/ff/
- Integration mirror: https://github.com/zephyrproject-rtos/fatfs
- Pinned upstream integration commit: `4f60a12c58c3de4246857ad87c40239ecd93349b`
- Upstream release: R0.16 with upstream patch-1 and patch-2, plus documented
  downstream/local changes.
- Vendored location: `third_party/fatfs/`
- Provenance, configuration, local modifications and source blob identifiers:
  [UPSTREAM.md](third_party/fatfs/UPSTREAM.md)
- Upstream README retained at
  [UPSTREAM_README.md](third_party/fatfs/UPSTREAM_README.md).

The upstream FatFs source notice states:

> Copyright (C) 2025, ChaN, all right reserved.
>
> FatFs module is an open source software. Redistribution and use of FatFs in
> source and binary forms, with or without modification, are permitted provided
> that the following condition is met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this condition and the following disclaimer.
>
> This software is provided by the copyright holder and contributors "AS IS"
> and any warranties related to this software are DISCLAIMED.
> The copyright owner or contributors be NOT LIABLE for any damages caused
> by use of this software.

The original notice is retained in the vendored upstream source. Do not remove
or replace it with the repository MIT license. Consult the upstream source and
current upstream terms before making redistribution decisions.

**exFAT distribution note:** the FatFs documentation warns that Microsoft
exFAT patent/licensing requirements may apply depending on product destination
and distribution. This repository note is not a determination of patent
clearance. Obtain an appropriate legal review before shipping a product where
that question matters.

## Reference projects — not vendored runtime dependencies

The following projects are referenced for architecture or behavior and are not
identified here as bundled runtime dependencies:

- Wine: architectural/behavioral reference only; ProsenjitOS does not intend
  to embed or require the Wine runtime.
- ReactOS: architecture and behavior reference only. Its source is GPL-licensed;
  do not copy or adapt ReactOS source into ProsenjitOS without a deliberate
  license review and approval of the resulting distribution obligations.

A reference to a project does not grant permission to copy its source, binaries,
branding, documentation, or other protected assets.

## Inventory maintenance requirements

Before adding, updating, or removing a third-party component:

1. Record its upstream project, exact version or commit, source URL, license,
   copyright notices, required notices, and component directory.
2. Record local patches and configuration changes, and keep a reproducible
   provenance trail.
3. Review compatibility with the intended distribution model and relevant
   patent or other non-copyright obligations.
4. Keep the component's original license and notices with the component where
   required, and update this inventory.
5. Re-run the repository license and provenance audit on the exact release
   candidate.

This inventory is based on the repository material inspected for this audit;
it must be refreshed as dependencies and generated/bundled artifacts evolve.
