# ProsenjitOS licensing and provenance audit report

**Audit date:** 2026-11-16  
**Repository:** `prosenjit-Adhikary/ProsenjitOS`  
**Audited PR:** [#646](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/646)  
**Audited head:** `90f18c09f1fb9ee784b8786bcc3821123d3dfc81`  
**Status:** Repository-side documentation and available CI verification complete; legal adoption and complete source-rights clearance remain open.

## 1. Owner's requested outcome

The proposed terms reflect the owner's stated requirement:

- Use, study, run, modify, fork, redistribute, and commercially use covered ProsenjitOS material without a royalty fee.
- If a modified/forked/derivative work is distributed as an operating system or installable/bootable OS product, its exact primary OS name must remain **ProsenjitOS**.
- Truthful secondary edition/build/maintainer labels may be added without replacing the primary name.
- Independent applications, libraries, packages, tools, services, and projects that merely interoperate with ProsenjitOS are outside the OS-name condition.
- Third-party code remains under its own license and notices.

The feature-branch root `LICENSE` records these as a **draft proposal**. It is not an operative grant. The base `main` branch retains the previously published MIT license.

## 2. Repository tree inventory

GitHub's recursive tree for the audited head returned 238 entries (218 blobs and 20 tree entries), with `truncated=false`.

| Finding | Evidence |
|---|---|
| Separately vendored directory | `third_party/fatfs/` only |
| Git submodules | No `.gitmodules` path or submodule entry found |
| Root build metadata | `Makefile` |
| Common build manifests | No Cargo, CMake, npm, Go, or Meson manifest found |
| Tracked image/font/audio/archive/binary assets | No common extensions found by the tree-path filter |
| Tracked source/config files | 152 paths matched the source/config extension filter |
| Documentation | 51 Markdown files |
| Root license-like filenames | Only `LICENSE` |

This is a tree inventory, not proof that no transient CI downloads, release attachments, generated files, embedded assets, or external dependencies exist.

## 3. Third-party component: FatFs

The only separately vendored directory identified is ChaN's FatFs Generic FAT Filesystem Module:

- Version: R0.16 (2025-07-22), with upstream patch-1 and patch-2 and the downstream security changes documented by the pinned Zephyr mirror.
- Pinned integration mirror commit: `4f60a12c58c3de4246857ad87c40239ecd93349b`.
- Imported files: `ff.c`, `include/ff.h`, `include/diskio.h`, `option/ffunicode.c`; `ffconf.h` is project configuration.
- Its own source redistribution condition and disclaimer must be retained. The ProsenjitOS draft does not relicense FatFs.
- The provenance record notes a possible exFAT patent/licensing review requirement depending on product destination/distribution.

The opening headers of `ff.c` and `include/ff.h` were inspected and contain the ChaN copyright, redistribution condition and disclaimer. `include/diskio.h` contains ChaN copyright attribution. The GitHub file reader could not return a readable excerpt for the 1.97 MB `option/ffunicode.c`; its complete header was not independently verified in this audit. This limitation is recorded in `THIRD-PARTY-NOTICES.md`.

## 4. Sampled first-party source/header review

A targeted sample of 47 first-party source/build files was inspected through the PR-head raw files, including boot assembly, ABI headers, core kernel files, filesystem/VFS, process and loader code, security/cryptography, USB, desktop, build scripts and Makefile.

- No SPDX identifier or copyright/license notice was found in the opening 80 lines of the sampled files.
- The sampled cryptographic implementation files include RFC 8032-related Ed25519 comments/test-vector references and SHA-512/SHA-256 algorithm code. A standards reference is not itself evidence of copied code, but the source tree does not provide an explicit per-file provenance declaration for these implementations.
- No source-level copying or third-party ownership conclusion can be made from this limited sample. The absence of a header is not proof that a file is original or that the current repository owner holds all relevant rights.

**Required follow-up:** the project owner/contributors should attest the origin of every first-party file, identify any adapted snippets or generated material, and add appropriate provenance/copyright records. Cryptographic code should receive a specific provenance and independent security review before production use.

## 5. Architecture references

`docs/ORIGINALITY.md` says the project intends independent implementations and treats Windows, Linux, ReactOS, Wine, public standards and observable behavior as architectural/compatibility references rather than permission to copy proprietary implementation code. The tree audit did not identify source directories for ReactOS, Wine, or Linux. A reference in documentation is not proof of code reuse; nor does the absence of a vendor directory prove all code was independently authored.

## 6. Contributor and copyright authority

The draft now avoids presuming contributor consent: contributor-owned material is covered only where the relevant rights holder expressly authorizes the custom terms.

This audit did not establish a complete chain of title for all commits, historical contributors, generated outputs, or assets. Git commit authorship alone does not establish copyright ownership or consent to a new license. The project owner must obtain written authorization or retain each contribution under its existing valid license. Prior MIT grants remain in force for copies/versions already distributed under MIT.

## 7. Proposed license legal status

The same-name condition is central to the owner's request and is included in the draft license itself. However:

- The custom same-name condition means this is not MIT and not an OSI-approved Open Source license; it may conflict with the Open Source Definition and downstream policies.
- Legal review must address the definition and scope of a derivative OS product, how the primary name is determined across installer/boot/metadata/UI/distribution channels, enforcement, termination/cure, commercial redistribution, warranty/liability, governing law, and trademark/name-use authority.
- The draft does not establish registered trademark rights or prove the project owner controls all rights necessary to impose the name condition.
- No license text can retroactively revoke permissions already granted under earlier MIT distributions.
- The license draft is explicitly labelled a proposal and must not be represented as adopted or enforceable until the relevant rights holders formally authorize the final counsel-reviewed text.

## 8. CI verification

For PR head `90f18c09f1fb9ee784b8786bcc3821123d3dfc81`:

- [Security architecture checks — run 43863647368](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43863647368): success.
- [Build and boot-test workflow — run 43863647458](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/43863647458): success.

The build workflow verified the PR checkout and architecture/documentation contracts. Kernel build, QEMU, and runtime jobs were skipped by the documentation-only path classifier. These results are not an OS compile, boot, hardware, legal, or enforceability test.

## 9. Completion decision

### Completed in repository
- Owner's requested same-name condition represented in a custom license proposal.
- Scope boundaries and third-party license separation documented.
- FatFs provenance and available license notice evidence recorded.
- Tree-level inventory, sampled source-header review, audit limitations, and adoption gates documented.
- Exact-head Security and Build workflow runs passed.

### Not complete / requires external evidence
1. Qualified legal review and approval of the final custom license.
2. Written authorization from every relevant copyright holder for material intended to move to the custom terms.
3. Verification of authority to grant the mandatory ProsenjitOS name-use permission and legal review of the same-name condition.
4. Full file-by-file provenance review for all 152 source/config paths, generated material, assets, commit history, build downloads and release packages.
5. Complete review of the FatFs `option/ffunicode.c` notice and exact source diff.
6. Project-owner approval of the counsel-reviewed final text.

**Do not merge PR #646, replace the active MIT license on `main`, or claim the custom license is operative until the above gates are satisfied.** Keeping the PR as draft is intentional and is not a claim that the requested legal outcome has already been achieved.


## 9. Project-owner authorship statement (2026-11-16)

The project owner has stated that, to the best of their knowledge, the ProsenjitOS project work completed to date was written by the project team itself and was not intentionally taken from another contributor's repository or source tree. This is an owner-provided authorship statement, not independent proof of copyright title or a file-by-file provenance certification.

The statement does not cover or relicense separately identified third-party material such as FatFs. It also does not establish ownership of any standards text, generated output, cryptographic implementation, assets, or other material whose origin has not been independently checked. The repository audit's source-sampling and asset limitations in Sections 2–5 remain applicable. Before adoption, preserve available authoring records and verify that the person/entity granting the license owns or controls the necessary rights in every covered file.

For the current licensing proposal, the project owner intends to authorize the custom ProsenjitOS terms for project-owned material only. The same-name requirement remains mandatory for distributed derivative OS products under the proposed terms. This statement does not make the draft operative, change historical MIT grants, or replace legal review.
