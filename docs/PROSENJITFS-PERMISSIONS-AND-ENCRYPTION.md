# ProsenjitFS Release Security Contract — Permissions and Basic Encryption

**Status:** Mandatory release requirement for ProsenjitFS 1.0; encryption is a scoped optional capability unless the key-management and recovery gates below can be completed safely.  
**Last updated:** 2026-10-25  
**Applies to:** Native ProsenjitFS, VFS integration, formatter/mount, tests and release notes.

## 1. Non-negotiable release decision

A filesystem release is not acceptable if it has no enforceable access-control model. **Permission enforcement is a Version 1.0 release blocker, not a later enhancement.** Version 1.0 must not ship with UID/GID/mode fields that are merely stored but ignored by the VFS.

Basic encryption is desirable, but insecure or incomplete encryption is worse than clearly documented non-support. Do not advertise encrypted volumes until key handling, authentication, recovery, and all relevant I/O paths are implemented and tested. If these gates cannot be met safely for 1.0, ship permissions in 1.0 and keep encryption as an explicitly deferred/experimental feature for 1.1.

## 2. Permission model — required for Version 1.0

### 2.1 Ownership and mode semantics

- [ ] Define and document UID, GID and mode-bit meanings for files and directories.
- [ ] Define the effective credentials used by each VFS operation; do not trust caller-supplied credentials without the kernel's authorization boundary.
- [ ] Define owner/group/other read, write and execute/search behavior, including directory search versus listing permissions.
- [ ] Define ownership and mode inheritance for file creation, mkdir, rename and cross-directory moves.
- [ ] Define root/privileged-process behavior through the existing kernel capability/security model; avoid ad hoc filesystem-only privilege bypasses.
- [ ] Define umask behavior if supported by the OS; otherwise explicitly document that it is not yet exposed.
- [ ] Define mount policy for read-only mounts and ensure it cannot be bypassed by a filesystem mutation path.
- [ ] Keep on-disk metadata descriptive only: authorization must be enforced by trusted kernel/VFS code.

### 2.2 Operations that must enforce permissions

Every relevant operation must be audited and tested, including:
- [ ] Path lookup and directory traversal/search.
- [ ] File open for read and write; read, write, append and truncate.
- [ ] File creation and directory creation.
- [ ] Directory enumeration/listing, independently of search permission.
- [ ] Unlink and rmdir, including sticky-directory semantics only if the OS supports them.
- [ ] Rename and cross-directory moves; check source and destination directory rights and any applicable ownership/sticky rules.
- [ ] Metadata queries/updates and ownership/mode changes, if those APIs exist.
- [ ] Mount, format and recovery operations at the appropriate privileged boundary.
- [ ] Read-only integrity scans and diagnostics, ensuring no mutation occurs and access to diagnostic information follows policy.

### 2.3 Security invariants

- [ ] Permission denial occurs before any persistent mutation.
- [ ] Failed operations return the correct error and initialize output parameters (e.g. byte counts) deterministically.
- [ ] No operation can obtain privilege by crafting an inode, directory entry, UID/GID, mode, generation or checksum-valid but semantically invalid metadata.
- [ ] Symlink traversal, hard links and ACLs must not be enabled until their interaction with permission checks is specified and tested.
- [ ] Permissions must be enforced in the VFS/security boundary, not only in a shell, desktop UI or convenience wrapper.
- [ ] Document the limits of discretionary access control: it is not encryption and does not protect data from an attacker who can read raw disk blocks or boot another OS.

### 2.4 Required permission tests

- [ ] Owner/group/other allow and deny cases for file reads and writes.
- [ ] Directory search versus enumeration behavior.
- [ ] Create/mkdir/unlink/rmdir/rename allowed and denied cases.
- [ ] Cross-directory rename permission checks and ownership edge cases.
- [ ] Read-only mount mutation rejection.
- [ ] Privileged capability behavior and unprivileged denial.
- [ ] Malformed metadata and permission-denial fault injection; confirm media does not change on denied operations.
- [ ] Remount persistence of ownership/mode and consistent behavior after reboot.
- [ ] Run tests at the exact release candidate commit and required Build/boot-test and Security architecture CI gates.

**Release gate:** if the kernel/VFS cannot reliably enforce the model, mark Version 1.0 blocked; do not claim permissions are supported merely because inode fields exist.

## 3. Basic encryption — design and scope

### 3.1 Recommended initial target

Prefer **whole-volume/data-region encryption behind a block-device or volume-crypto layer** rather than adding ad hoc encryption calls to individual filesystem operations. This reduces the chance of forgetting a read/write path and keeps ProsenjitFS metadata layout concerns separate. The exact layer must be chosen after inspecting the current block-device, boot, recovery and key-input architecture.

A reasonable initial design should aim for:
- Confidentiality of stored file contents when the volume is offline and the key is unavailable.
- Authenticated integrity where the chosen construction supports it, with a documented threat model.
- No hard-coded key, predictable default key, plaintext fallback or silent downgrade.
- A clear distinction between encrypted data and metadata; whole-volume encryption can protect both but may complicate boot-time discovery and recovery.
- Compatibility and recovery behavior defined before changing the on-disk format.

### 3.2 Cryptographic requirements

- [ ] Use a reviewed, standard cryptographic library/implementation available and supportable in this OS; do not invent a cipher, mode, KDF or authentication construction.
- [ ] Select an authenticated-encryption design and nonce/tweak strategy appropriate to random-access block storage; document replay/rollback limitations and block-level integrity behavior.
- [ ] Derive or unwrap keys using a reviewed password-based KDF or a future secure key source; never use a password directly as an encryption key.
- [ ] Specify salt, KDF parameters, key version, algorithm identifier and any authentication metadata with bounds and validation.
- [ ] Ensure nonces/tweaks cannot be reused in a way that breaks the chosen construction. Random-access storage makes nonce management a design-critical issue.
- [ ] Keep secrets out of logs, crash reports, debug output and persistent plaintext buffers where practical; define key lifetime and zeroization best-effort behavior.
- [ ] Fail closed on wrong key, invalid authentication tag, unsupported algorithm/version or malformed crypto metadata.
- [ ] Do not silently mount encrypted media as plaintext when key verification fails.
- [ ] State clearly that encryption does not prevent deletion, denial of service, traffic/usage analysis or rollback to an older valid disk image unless additional mechanisms address those threats.

### 3.3 Key management and boot/recovery requirements

- [ ] Decide how a key is provided at boot and how the key is protected in memory.
- [ ] Do not embed a master key in the kernel, bootloader, repository, image or source code.
- [ ] Specify password change, key rotation, recovery-key policy and the consequences of losing all keys.
- [ ] Specify how formatting identifies encrypted versus unencrypted volumes without exposing secrets or allowing downgrade attacks.
- [ ] Ensure journal replay and integrity scans run at the correct layer and cannot bypass authentication.
- [ ] Specify whether the boot partition is encrypted; do not imply encrypted boot if only a data volume is encrypted.
- [ ] Document that software-only encryption cannot protect keys from a fully compromised running kernel or an attacker with access to unlocked memory.
- [ ] Add tests for wrong key, corrupted crypto metadata, modified ciphertext/tag, interrupted writes, remount, key loss and unsupported crypto version.

### 3.4 Release gating for encryption

Encryption is **not considered complete** until all of these are true:
1. Design and threat model reviewed.
2. Cryptographic primitive and library are reviewed and available in the actual build.
3. Key input/storage/lifetime and recovery behavior are implemented.
4. All data I/O paths are encrypted consistently; no plaintext bypass exists.
5. Authentication failures fail closed without exposing corrupted plaintext.
6. Power-loss/journal ordering and crypto metadata updates are tested together.
7. Known-answer tests for primitives and integration tests pass.
8. Exact-head CI passes and documentation accurately defines protection boundaries.

If any item is missing, encryption must be labelled **planned/experimental**, not a release feature. Never implement a home-grown cipher to meet a deadline.

## 4. Suggested delivery order

### Version 1.0 — mandatory baseline
1. Audit the actual VFS permission checks and all filesystem operation entry points.
2. Define UID/GID/mode semantics and directory permission rules.
3. Implement missing enforcement in the trusted VFS/security layer.
4. Add positive, negative, malformed-metadata and no-mutation tests.
5. Verify persistence across remount and exact-head CI.
6. Publish permission semantics and known limitations in release notes.

### Version 1.1 — encryption only after design approval
1. Inspect the current block-device/volume abstraction and boot key-entry options.
2. Write a threat model and compare reviewed authenticated-encryption designs suitable for random-access storage.
3. Define on-disk crypto metadata and versioning/compatibility.
4. Implement key management and crypto layer behind a narrow interface.
5. Test all I/O paths, wrong-key behavior, corruption, interrupted transactions and remount.
6. Keep it experimental until every encryption gate passes.

## 5. Explicit status rule

This document is a security requirement, not proof of implementation. Before release, link each completed item to source files, tests and exact-head CI runs. A planned permission or encryption feature must never be described as shipped.
