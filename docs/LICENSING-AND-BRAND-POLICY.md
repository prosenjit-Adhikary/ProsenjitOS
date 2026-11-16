# Licensing, attribution, and project identity policy

## 1. Goals

ProsenjitOS is intended to be freely usable by everyone. The project welcomes use, study, modification, redistribution, and commercial use under the repository's MIT License. This policy documents how the original project identity and third-party notices should be treated.

This document is a project policy, not legal advice. Trademark rights and enforceability depend on applicable law and actual rights; do not represent an unregistered mark as registered.

## 2. ProsenjitOS original code

- The root `LICENSE` applies to original ProsenjitOS code and documentation to the extent they are owned by the project and not identified as third-party material.
- Preserve the MIT copyright and permission notice in copies or substantial portions of the original covered material.
- Do not remove or falsify original copyright, authorship, provenance, or attribution information.
- Do not imply that a modified distribution is an official release or is endorsed, maintained, or certified by the original ProsenjitOS project unless the project has expressly authorized that claim.

## 3. Project name and identity

- The official project name is **ProsenjitOS**. The original project, its official repository, and official releases should retain this name and identify their origin as ProsenjitOS.
- When redistributing the project substantially as ProsenjitOS, retain the project name and clear attribution to the original ProsenjitOS project.
- Anyone may fork, modify, and redistribute the code under the MIT License. A fork may use a distinct distribution name, but should clearly state that it is derived from ProsenjitOS and preserve required copyright/license notices. It must not falsely present itself as the official project.
- The MIT License itself does not require derivatives to keep the same name and does not grant trademark rights. This naming/identity policy expresses the project's branding expectations; it does not add a same-name condition to the MIT copyright license.
- The project name and any logos/marks are not automatically licensed for unrestricted endorsement or official-affiliation claims by the MIT License. Obtain permission before using branding in a way that suggests official status. Seek qualified trademark advice before asserting exclusive rights or taking enforcement action.

## 4. Third-party components

- Third-party source, binaries, generated assets, and documentation remain subject to their own applicable licenses and notices. The root MIT License does not relicense them.
- Keep upstream copyright notices, license conditions, disclaimers, attribution files, and required source/binary redistribution materials intact.
- Before adding or updating a dependency, record its upstream project, exact version/commit, source URL, license, required notices, local modifications, configuration, and update procedure.
- Review license compatibility, redistribution obligations, and patent/export or other distribution considerations before shipping. Escalate uncertain legal questions for qualified review.
- Maintain `THIRD-PARTY-NOTICES.md` as the human-readable notice index. The index is not a substitute for the original license files/notices shipped with components.
- Do not claim the third-party inventory is exhaustive until a repository-wide dependency and release-artifact audit has verified source files, build manifests, submodules, generated/bundled material, and packaged outputs.

## 5. Release checklist

Before publishing a source or binary release:

1. Confirm the root MIT license and its scope are clear.
2. Regenerate/review the third-party dependency inventory and notices against the exact release commit.
3. Verify that required third-party license texts and notices are included in source and binary distributions as applicable.
4. Review local patches and provenance records against the pinned upstream versions.
5. Check branding and release metadata identify the official project accurately and do not imply unauthorized endorsement.
6. Review patent and other jurisdiction-specific distribution concerns, including the exFAT note in the FatFs provenance record, with qualified counsel where needed.
7. Record the exact release commit and any unresolved compliance limitations.

## 6. Scope and limitations

This policy is operational project guidance, not a legal opinion, trademark registration, patent clearance, or certification of repository-wide compliance. The project should remain freely usable under the MIT License while respecting the independent terms attached to third-party materials.
