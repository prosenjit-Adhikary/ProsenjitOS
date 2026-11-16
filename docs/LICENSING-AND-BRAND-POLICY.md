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

The text in the root LICENSE is currently a draft pending:
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
