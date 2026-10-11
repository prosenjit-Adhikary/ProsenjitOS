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
- Import/base-relocation directory metadata consistency and image-range checks.
- RVA-to-file-offset translation for headers and raw-backed section data; virtual zero-fill tails are deliberately not treated as file bytes. The public translation helper also validates caller-supplied metadata bounds rather than assuming every caller passed parser output.
- Bounded import-descriptor reading, ASCII DLL-name extraction, and import-thunk inspection for named imports (hint + symbol name) and ordinal imports. Malformed, unterminated, and out-of-range names fail closed. This does not resolve symbols to addresses or load DLLs.
- Failure clears the output structure; metadata is published only after successful validation.
- Synthetic host tests cover PE32/PE32+, architecture/magic mismatch, out-of-range offsets, image/entry bounds, directory-count corruption, W^X conflicts, zero-filled entry points, RVA translation, bounded import descriptor/name reading, PE32 and PE32+ named/ordinal thunk parsing (including reserved-bit rejection for PE32+ ordinals), unterminated name rejection, malformed caller-supplied metadata rejection, and cleared output on failure. The latest additions are not verified until both host scripts pass.

## Explicitly not implemented

- Memory mapping, relocations, import walking, DLL resolution, TLS callbacks, exception/unwind support, process startup, Win32 APIs, GUI, MSI/EXE installer execution, or sandbox enforcement.
- Authenticode verification or an execution-trust decision.
- Compatibility claims for arbitrary applications or Microsoft Office.

The parser is untrusted-input handling code and must receive fuzz/property testing and review before it is used on installer input. The host tests validate only their fixtures; they do not prove OS integration or runtime safety. Do not connect this parser to an executable launch path until the native process/address-space interfaces and security admission contracts have been audited.
