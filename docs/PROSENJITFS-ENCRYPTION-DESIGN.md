# ProsenjitFS Encryption Architecture — Development Plan

Status: Design baseline (no kernel integration yet)

## Goal

Add strong, standards-based encryption to Native ProsenjitFS without modifying the kernel initially. The crypto implementation and filesystem encryption format will be developed and tested as an isolated layer, then integrated with the kernel/VFS only after the design and test suite are stable.

## Security principles

- Do not invent a custom cipher or cryptographic primitive.
- Prefer well-reviewed, standard authenticated-encryption primitives.
- Encryption must provide confidentiality and integrity/authentication.
- Never reuse a nonce with the same encryption key.
- Key material must have an explicit lifecycle and must not be logged.
- Filesystem corruption and intentional tampering must be detectable.
- Encryption must remain compatible with ProsenjitFS transaction/journal/recovery semantics.
- Crypto must not own LBA translation. Storage/LBA remains the responsibility of the filesystem/block-device layers.
- No GitHub-hosted CI dependency is required for development; local Debian/VM testing remains the preferred verification path.

## Proposed cryptographic stack

### Primary data encryption

AES-256-GCM.

Reasons:
- standardized and widely reviewed;
- authenticated encryption (confidentiality + integrity);
- good performance on CPUs with AES acceleration;
- suitable as a block/file/extent encryption primitive when nonce management is designed correctly.

### Alternative backend

ChaCha20-Poly1305 may be implemented as an alternative backend for CPUs without useful AES acceleration.

The filesystem format should define an algorithm identifier so the crypto backend can be selected without redesigning ProsenjitFS.

### Password-based unlock

Argon2id for deriving a key-encryption/unlock key from a user password.

The password itself is never stored in the filesystem.

### Randomness

Use a cryptographically secure random-number abstraction for keys and nonces. The standalone implementation must not silently fall back to weak pseudo-randomness.

## Key hierarchy

Do not encrypt the entire filesystem directly with a password or a single long-lived data key.

Target hierarchy:

    User password
        |
        v
      Argon2id
        |
        v
    Unlock / KEK
        |
        v
   Volume Master Key
        |
        +---- Metadata protection keys
        |
        +---- Data/file/extent keys

The exact key hierarchy and wrapping format must be finalized before on-disk format freeze.

## Filesystem integration boundary

The crypto layer must be independent of LBA translation.

    VFS
      |
    ProsenjitFS
      |
    Crypto interface
      |
    block/storage layer
      |
    volume-relative sector/LBA
      |
    device/ATA

The crypto layer receives logical data/metadata buffers and crypto context; it must not calculate physical disk LBAs.

## Encrypted data model

The design should support authenticated encryption of filesystem data units while preserving crash consistency.

Each encrypted unit should have enough authenticated context to prevent:
- ciphertext substitution;
- replay of an older valid ciphertext;
- cross-file/cross-block substitution;
- metadata/data confusion.

Associated authenticated data (AAD) should bind ciphertext to its filesystem identity/context without unnecessarily exposing sensitive information.

## Metadata protection

Determine separately which metadata must be:
1. encrypted and authenticated;
2. authenticated but left readable for mount/recovery;
3. public structural information.

Do not blindly encrypt metadata required to discover the encrypted volume.

## Journal and crash consistency

Encryption must be transaction-aware.

Required cases:
- interrupted encrypted write;
- torn ciphertext;
- torn authentication tag;
- journal replay;
- abandoned transaction;
- replay of stale ciphertext;
- metadata update followed by crash;
- key metadata update followed by crash.

Recovery must never accept partially committed encrypted state as valid.

## Nonce/IV requirements

Nonce uniqueness is a hard security invariant for AES-GCM.

The implementation must define:
- nonce allocation;
- persistence/recovery behavior;
- interaction with transactions;
- behavior after remount;
- behavior after rollback;
- behavior during block relocation;
- behavior during snapshots/copies if later supported.

No nonce reuse may occur for the same key, including after crash/recovery.

## Key rotation

The format should be designed so that future key rotation is possible without requiring a full filesystem redesign.

At minimum, distinguish:
- volume/master key;
- data encryption keys;
- metadata keys;
- key version/generation.

## Testing plan before kernel integration

Host-side/standalone tests:

1. Known-answer cryptographic tests.
2. Encrypt/decrypt round trips.
3. Randomized input/property tests.
4. Empty, tiny, sector-sized and multi-megabyte inputs.
5. Boundary-size and alignment tests.
6. Wrong-key rejection.
7. Authentication-tag corruption detection.
8. Ciphertext bit-flip detection.
9. AAD modification detection.
10. Replay/stale-ciphertext rejection.
11. Nonce uniqueness tests.
12. Crash-point simulation.
13. Journal/recovery interaction tests.
14. Key rotation tests.
15. Fuzz testing of serialized crypto metadata.
16. Sanitizer/static-analysis builds where available.

## Kernel integration plan

Only after the standalone crypto layer and encrypted-format tests are stable:

1. Add a narrow ProsenjitFS crypto interface.
2. Connect VFS/file I/O to the interface.
3. Connect authenticated metadata handling.
4. Connect journal/recovery paths.
5. Connect the kernel CSPRNG source.
6. Preserve the existing LBA/storage contract.
7. Add kernel-level regression tests.
8. Verify remount/persistence/corruption/crash behavior in the Debian VM/QEMU environment.

## Non-goals

This phase does not:
- replace ProsenjitFS allocation;
- redesign VFS;
- redesign LBA translation;
- modify bootloader/graphics/networking;
- invent a new encryption algorithm;
- require GitHub Actions;
- claim encryption is complete before runtime and corruption/recovery verification.

## Definition of done

Encryption is complete only when:
- the on-disk format is documented and versioned;
- cryptographic primitives are standard and correctly composed;
- nonce/key invariants are enforced;
- authentication failures are handled safely;
- journal/recovery semantics are proven;
- corruption and replay tests pass;
- persistence/remount tests pass;
- kernel integration is source-audited;
- Debian VM/QEMU runtime tests pass;
- no secret material appears in logs or diagnostics.
