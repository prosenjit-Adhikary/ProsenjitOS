# ProsenjitOS license and project-name policy

## 1. Owner's requirement

The project owner's requirement is that everyone should be able to use,
study, modify, fork, redistribute, and commercially use ProsenjitOS without
paying a license fee, while every distributed derivative operating system
must retain **ProsenjitOS** as its exact primary OS name.

This is a same-name condition in the software license itself, not merely a
request that forks use a different name or a separate trademark guideline.

## 2. License status and permissions

The repository root [LICENSE](../LICENSE) contains the project-specific
ProsenjitOS Community Use and Same-Name License draft.

The intended permissions are broad: private and organizational use,
modification, forking, source and binary redistribution, and commercial use
are allowed without royalty payments, subject to the license conditions.

**Mandatory condition:** if a person modifies, forks, adapts, or derives
ProsenjitOS and distributes the result as an operating system, OS
distribution, or installable/bootable OS product, the primary OS name must
remain exactly **ProsenjitOS**. A secondary edition, maintainer, build, or
organization label may be added if it does not replace the primary name or
falsely imply upstream endorsement.

The name condition applies to the OS product identity. It does not require
unrelated applications, packages, libraries, tools, services, or projects
that merely interoperate with ProsenjitOS to use that name.

## 3. Important license classification and adoption gate

This is a custom, project-specific license condition. It is **not the MIT
License**, and it is **not an OSI-approved Open Source license**. The
mandatory same-name condition may make the license incompatible with the
Open Source Definition and with some downstream projects' policies. The
project must not describe this license as MIT, OSI-approved, or
unrestricted open source.

The text in the root LICENSE is a proposal only, not an adopted license. Do not rely on it as an operative grant until the relevant rights holders formally authorize it. The text is pending:
- qualified legal review, especially of the same-name condition, termination,
  cure, and commercial redistribution wording;
- confirmation that the project owner and all relevant copyright holders
  can license the covered material on these terms;
- a repository-wide audit separating original code from third-party code;
- a decision on how previously MIT-licensed commits, releases, and copies
  are treated. A new license cannot automatically revoke permissions already
  granted under earlier distributions.

Do not claim that this draft is enforceable in every jurisdiction or that
the ProsenjitOS name is a registered trademark unless that is independently
verified. Do not issue enforcement threats based only on this draft.

## 4. Third-party components

Third-party source, binaries, generated assets, documentation, and
submodules remain subject to their own licenses and notices. This license
does not relicense third-party material or remove upstream obligations.

- Preserve original copyright notices, license texts, disclaimers, and
  required attribution.
- Record each dependency's exact upstream, version/commit, license, source,
  local modifications, and redistribution conditions.
- Maintain [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).
- The current notice inventory is not a complete SBOM. Complete a
  repository-wide source, build-manifest, submodule, generated-artifact,
  and release-package audit before declaring it exhaustive.
- Review patent and distribution concerns, including the exFAT note in the
  FatFs provenance record, with qualified counsel where relevant.

## 5. Release checklist

Before publishing a source or binary release:

1. Confirm the final adopted license and its copyright-holder authority.
2. Verify that every distributed derivative OS retains **ProsenjitOS** as
   its exact primary OS name under the license.
3. Preserve all applicable original and third-party license notices.
4. Identify substantial modifications and avoid false upstream endorsement.
5. Audit dependencies and packaged artifacts against their actual licenses.
6. Record the exact release commit and unresolved legal/compliance issues.

## 6. Scope and limitations

This document records the owner's requested licensing direction. It is not
legal advice, trademark registration, patent clearance, or a guarantee of
enforceability. The draft must not be treated as final until the adoption
gates above are satisfied.


## 7. Repository audit findings (2026-11-16)

A tree-level audit was performed against PR #646 head `61f285a342fb1719c4935b4a578b37b1235330da`. GitHub returned 238 paths with `truncated=false`. The only separately vendored directory found was `third_party/fatfs/`; no `.gitmodules` entry was present. The root build metadata found was `Makefile`; the audited tree had no Cargo, CMake, npm, Go, or Meson manifest. No common compiled/binary/archive file extensions were found in tracked paths. The root `LICENSE` is the draft on this feature branch, while base `main` still carries the prior MIT license.

FatFs R0.16 is separately documented with its pinned Zephyr mirror commit, imported files, license notice, local changes, and exFAT distribution caveat in [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) and [third_party/fatfs/UPSTREAM.md](../third_party/fatfs/UPSTREAM.md). The repository's references to ReactOS, Wine, Linux and commercial security products are documented as architectural/behavioral references; this audit did not identify them as vendored source dependencies.

**Audit boundary:** this is a tracked-tree/path and documentation/provenance review, not proof of line-by-line source originality, full asset clearance, exhaustive generated-code provenance, CI download inventory, or a release-package SBOM. GitHub code search used in this pass indexes the default branch, not the PR head. Those checks remain mandatory before release. See the exact snapshot and limitations in [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).

## 8. Adoption blockers that cannot be closed by repository edits

The draft is not ready to become the operative license solely because documentation and CI checks pass. The following require actions/evidence outside this repository edit:

1. Qualified legal counsel must review the same-name condition, commercial redistribution grant, termination/cure mechanism, warranty/liability wording, governing law, and compatibility with applicable law.
2. The rights holder for every covered contribution must expressly authorize the custom terms. The repository cannot infer contributor consent from a commit or pull request.
3. Counsel/rights holders must resolve the relationship between the mandatory product-name condition and any trademark/name rights. This draft does not assert trademark ownership or registration and does not itself establish that the project owner controls all rights needed to authorize the required name use.
4. Prior MIT grants and copies must be treated according to the permissions already granted; this draft cannot retroactively revoke them.
5. The source/header/asset and release-package provenance audit must be completed beyond the tree-level snapshot.
6. The project owner must approve the final counsel-reviewed text before any license replacement or release.

Until these gates are evidenced, keep the custom text explicitly labelled as a proposal and do not describe it as the operative license.


## 9. Detailed audit report

The detailed 2026-11-16 repository licensing and provenance report is maintained at [LICENSING-AUDIT-REPORT-2026-11-16.md](LICENSING-AUDIT-REPORT-2026-11-16.md). It records the exact audited commit, tree inventory, FatFs provenance, sampled first-party source/header review, contributor-rights limitations, CI evidence, and all outstanding legal/adoption gates.

The source sample did not reveal SPDX or copyright notices in the opening 80 lines of the sampled files. That is not proof of originality or ownership. A complete file-by-file contributor/provenance attestation remains necessary, with particular attention to cryptographic implementations and generated/downloaded content.
