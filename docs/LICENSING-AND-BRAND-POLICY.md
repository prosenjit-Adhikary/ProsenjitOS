# Licensing, attribution, and project identity policy

## 1. Goals and current status

The project owner's stated goal is to let everyone use ProsenjitOS freely, including for commercial purposes, while requiring every modified or forked operating-system distribution derived from ProsenjitOS to retain **ProsenjitOS** as its exact primary OS name.

The repository's current root `LICENSE` is still the MIT License. MIT does **not** impose a same-name condition. The proposed custom license is in [LICENSE-CUSTOM-DRAFT.md](../LICENSE-CUSTOM-DRAFT.md) and is **draft only, not active**. Until it is legally reviewed, approved by the rights holders, and formally adopted, the MIT License remains operative for material it covers.

A same-name condition changes the legal permissions materially. The proposed custom license is not an OSI-approved open-source license and must not be advertised as OSI-compliant or unrestricted open source. The Open Source Definition includes criteria for derived works and source identity; see the [Open Source Initiative's definition](https://opensource.org/osd).

## 2. Intended permissions under the proposed model

The proposed model is intended to allow, without a license fee:

- use, study, copy, and modify covered original ProsenjitOS material;
- create and distribute modified and unmodified OS builds;
- distribute source and compiled/binary forms;
- personal, educational, research, public-sector, and commercial use;
- charge for hardware, copies, distribution, installation, support, hosting, and related services.

These are proposed terms only. They do not replace the currently active MIT License until the custom license is formally adopted.

## 3. Mandatory name requirement — proposed

The project owner's requirement is that every Covered OS Distribution derived from ProsenjitOS, whether modified or unmodified, must retain **ProsenjitOS** as its exact and primary operating-system product name.

The draft allows version/edition/build qualifiers after the required name (for example, `ProsenjitOS 2.0` or `ProsenjitOS Community Edition`) but does not allow a fork to replace, translate, abbreviate, remove, or rebrand the OS under a different primary product name.

A modified distributor must also clearly disclose that its build is independently modified and is not an official, endorsed, or supported upstream release unless written authorization says otherwise.

The proposed legal wording is in [LICENSE-CUSTOM-DRAFT.md](../LICENSE-CUSTOM-DRAFT.md). Do not claim this requirement is currently binding: the root MIT License does not create it.

## 4. Adoption gates — mandatory before changing the active license

1. Obtain qualified legal review of the custom license, especially the same-name condition, trademark/name rights, jurisdiction, termination, and downstream distribution.
2. Confirm the identity and authorization of every copyright holder whose contributions would be relicensed. Do not assume the repository owner can relicense contributions owned by others.
3. Define the effective date and treatment of copies/releases already distributed under MIT. Do not retroactively claim that recipients' existing MIT permissions have been withdrawn.
4. Audit all third-party source, binaries, generated assets, documentation, submodules, and release artifacts. The custom license cannot override third-party terms.
5. Approve a final license text and update the root `LICENSE`, README, contributor guidance, notices, and release metadata consistently in a separately reviewed change.
6. Keep the custom license draft clearly marked as draft until all gates are met.

## 5. Third-party components

- Third-party source, binaries, generated assets, and documentation remain subject to their own applicable licenses and notices. Neither MIT nor the proposed custom license automatically relicenses them.
- Keep upstream copyright notices, license conditions, disclaimers, attribution files, and required source/binary redistribution materials intact.
- Before adding or updating a dependency, record its upstream project, exact version/commit, source URL, license, required notices, local modifications, configuration, and update procedure.
- Review license compatibility, redistribution obligations, and patent/export or other distribution considerations before shipping. Escalate uncertain legal questions for qualified review.
- Maintain `THIRD-PARTY-NOTICES.md` as the human-readable notice index. The index is not a substitute for original license files/notices shipped with components.
- Do not claim the third-party inventory is exhaustive until a repository-wide dependency and release-artifact audit has verified source files, build manifests, submodules, generated/bundled material, and packaged outputs.

## 6. Release checklist

Before publishing a source or binary release:

1. Confirm which license is actually effective for each original and third-party component.
2. Regenerate/review the third-party dependency inventory and notices against the exact release commit.
3. Verify that required third-party license texts and notices are included in source and binary distributions as applicable.
4. Review local patches and provenance records against pinned upstream versions.
5. If the custom license has been formally adopted, verify that the OS identity retains the exact primary name **ProsenjitOS** and that modified distributions disclose their unofficial status.
6. Review patent and other jurisdiction-specific distribution concerns, including the exFAT note in the FatFs provenance record, with qualified counsel where needed.
7. Record the exact release commit and unresolved compliance limitations.

## 7. Scope and limitations

This document is project policy and workflow guidance, not a legal opinion, trademark registration, patent clearance, or certification of repository-wide compliance. The proposed custom license requires qualified legal review and rights-holder approval before adoption. The current MIT license does not prevent a fork from changing its OS name.
