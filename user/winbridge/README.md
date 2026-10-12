# WinBridge PE parser (first user-space milestone)

**Status: bounded metadata parser + host fixtures only. Not a PE loader.**

This directory is independent of kernel code and does not execute Windows instructions. It is the first user-space parser foundation for Prosenjit WinBridge.

## Local host test

From the repository root:

```sh
sh user/winbridge/test-pe-image.sh
```

The test runner uses the local C compiler with C11, `-Wall -Wextra -Werror -O2`; it does not require GitHub-hosted Actions.

Run the AddressSanitizer/UndefinedBehaviorSanitizer variant with:

```sh
sh user/winbridge/test-pe-image-sanitize.sh
```

A libFuzzer entry point is provided in `fuzz_pe_image.c`; actual fuzz campaigns are still pending and require a compatible Clang/libFuzzer toolchain.

## What is implemented

- Bounded DOS/PE/COFF and optional-header reads.
- Supported metadata pairs: I386 + PE32 and AMD64 + PE32+.
- Limits for file size, section count, directory count, and image size.
- Checks that the declared data-directory count fits inside the optional header.
- Section raw/virtual bounds, overlapping section rejection, and rejection of writable+executable sections.
- Entry point must land in file-backed executable section bytes.
- Import/base-relocation directory metadata consistency, image-range checks, and rejection of directories that point into non-file-backed virtual tails.
- RVA-to-file-offset translation for headers and raw-backed section data; virtual zero-fill tails are deliberately not treated as file bytes. The public translation helper enforces the parser's maximum file/image bounds, rejects zero-length mapping requests, validates every caller-supplied section range, and rejects virtual/raw overlap across the complete section map before returning a mapping; an early matching section cannot hide malformed later entries.
- Bounded import-descriptor reading (the validated directory is mapped once per lookup, the descriptor count is capped at 4096, a terminating all-zero descriptor is required, and any nonzero descriptor after that terminator invalidates the table), ASCII DLL-name extraction, and import-thunk inspection for named imports (hint + symbol name) and ordinal imports. Thunk-prefix lengths use wide arithmetic and are bounded before narrowing or scanning. Caller-controlled string capacity is capped at 4096 bytes to bound repeated RVA mapping work; malformed, unterminated, and out-of-range names fail closed. This does not resolve symbols to addresses or load DLLs.
- Failure clears the output structure; metadata is published only after successful validation.
- Synthetic host tests cover PE32/PE32+, architecture/magic mismatch, out-of-range offsets, image/entry bounds, directory-count corruption, W^X conflicts, zero-filled entry points, RVA translation, bounded import descriptor/name reading, PE32 and PE32+ named/ordinal thunk parsing (including reserved-bit rejection for PE32+ ordinals), half-present import-directory metadata, bounded output buffers, unterminated name rejection, malformed caller-supplied metadata rejection (including full raw/virtual section-range checks, a forged later section hidden behind an otherwise matching first section, zero-length mapping requests, oversized public-helper file-size arguments, and over-cap string capacities and forged import-directory ranges that exceed the image or file-backed section, and attempts to skip the all-zero import-descriptor terminator or import-thunk terminator, a missing import-descriptor terminator, hidden nonzero descriptors after the terminator, oversized import tables, extreme thunk indices, thunk prefixes crossing a section's raw-data end, and forged image-size metadata that could otherwise truncate a thunk-prefix byte count), and cleared output on failure. The latest additions are not verified until both host scripts pass.

## Explicitly not implemented

- Memory mapping, relocations, import walking, DLL resolution, TLS callbacks, exception/unwind support, process startup, Win32 APIs, GUI, MSI/EXE installer execution, or sandbox enforcement.
- Authenticode verification or an execution-trust decision.
- Compatibility claims for arbitrary applications or Microsoft Office.

The parser is untrusted-input handling code and must receive fuzz/property testing and review before it is used on installer input. The host tests validate only their fixtures; they do not prove OS integration or runtime safety. Do not connect this parser to an executable launch path until the native process/address-space interfaces and security admission contracts have been audited.
