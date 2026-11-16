# ProsenjitOS license and project-name policy

## 1. Project licensing direction

ProsenjitOS uses the MIT License. The project owner wants the software to
remain easy to use, study, modify, fork, redistribute, and use commercially,
without royalty payments or project-imposed naming restrictions.

## 2. Author's humble request about the Prosenjit name

The author and developer respectfully ask anyone who contributes to,
continues, modifies, forks, or independently develops this project or a
derivative operating-system project to consider retaining **Prosenjit** as
part of the operating system's name.

This is a humble, non-binding request intended to honor the project's
origin, identity, and development history. It is **not a condition of the
MIT License**, does not restrict any permission granted by that license,
and creates no legal obligation to use the Prosenjit name. Fork authors
and independent developers remain free to choose another name, subject to
applicable third-party rights and law.

The request does not require unrelated applications, packages, libraries,
tools, services, or projects that interoperate with ProsenjitOS to use the
Prosenjit name.

## 3. License status and interpretation

The root [LICENSE](../LICENSE) contains the standard MIT License followed
by the author's non-binding request. The request is deliberately separate
from the license grant and must not be interpreted as an additional
restriction, condition, or enforcement right.

The project must not describe the naming request as mandatory or claim that
a fork violates the MIT License by choosing another name. Any separate use
of project marks or branding remains subject to applicable law and verified
rights; this document does not assert trademark registration or ownership.

Previously published MIT-licensed versions remain under the MIT permissions
already granted. Third-party components remain governed by their own
licenses and notices.

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


## 9. Project-owner authorship statement (2026-11-16)

The project owner states that the ProsenjitOS work completed to date was written by the project team itself. This statement is recorded as the owner's representation; it is not independent verification of every file's provenance or a legal determination of copyright ownership.

The statement does not include FatFs or other separately identified third-party material, which remains under its own terms. It does not close the audit gaps for generated outputs, embedded assets, cryptographic source provenance, historical releases, or external build dependencies. Retain available authoring records and complete those checks before adopting the custom license.

The requested mandatory **ProsenjitOS** primary name for every distributed derivative OS remains in the draft. The statement does not activate the draft or revoke permissions already granted under earlier MIT distributions.


## 10. Superseding licensing decision (2026-11-16)

The project owner has chosen to replace the earlier proposed mandatory
same-name licensing condition with a **non-binding Author's Humble Request**.
The root LICENSE on this branch now uses the standard MIT License, followed
by a clearly separate request asking derivative OS developers to consider
retaining **Prosenjit** in the OS name. The request expressly creates no
legal obligation and does not limit the freedom to rename forks.

This decision supersedes contrary mandatory-name wording in earlier audit
notes and draft descriptions. PR #646 remains unmerged; its description and
historical audit sections should be read together with this superseding
section. The existing MIT license on the default branch is not being
replaced by a more restrictive license.
