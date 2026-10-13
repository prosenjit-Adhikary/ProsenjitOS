# Prosenjit WinBridge — User-Space Runtime Contract

**Status:** implementation contract / design milestone; not an executable runtime.  
**Parent architecture:** [Version 2 Unified Application Runtime](V2-UNIFIED-APPLICATION-RUNTIME.md).  
**Installation requirements:** [Windows Application Installation Pipeline](WINDOWS-APP-INSTALLATION.md).

## Purpose

Define the first safe boundary for a native ProsenjitOS Windows-application runtime. The aim is to make supported Windows applications launch as applications on the ProsenjitOS desktop without Wine or a Windows VM as the normal execution path. This document does not claim that Windows binaries or Microsoft Office currently run.

## Non-negotiable boundaries

- WinBridge is a user-space service/library, not a kernel PE execution feature.
- The kernel PE inspector is metadata inspection only. It must not map or execute a Windows image.
- Native ELF execution must remain unchanged and retain its existing security checks.
- No arbitrary installer runs with elevated privileges by default.
- No direct loading of Windows `.sys` files as ProsenjitOS kernel drivers.
- No Wine/VM fallback hidden behind a launcher.
- Do not bypass Microsoft sign-in, subscription, activation, integrity, or deployment requirements.
- Unsupported features must fail closed with a structured diagnostic, not be silently emulated as success.

## Proposed module boundaries

```text
Software Center / file manager
          |
Unified Application Manager
          |  package identity + consent + transaction
Installer admission service
          |  bounded metadata inspection only
WinBridge user-space supervisor
          |  sandbox profile + process request
PE image reader -> image mapper -> import/DLL resolver
          |                         |
Native process/address-space API   WinAPI compatibility modules
          |                         |
       ProsenjitOS kernel + existing security/capability checks
```

These are logical modules, not a claim that these components exist in the current source tree.

## Stable interfaces to define before execution

Use fixed-width integer fields and explicit structure-size/version fields at all service boundaries. Every input pointer/offset/length combination must be validated before use.

### Image inspection result

The inspector may return only bounded metadata:
- PE32 or PE32+ format and machine architecture;
- optional-header properties and declared directory count;
- image size, entry RVA, section table and section ranges;
- import, relocation, TLS, exception/unwind and resource directory ranges when present;
- a list of unsupported mandatory features;
- parser status and a bounded diagnostic code.

It must not return a trusted/authorized-to-execute verdict. Trust and compatibility are separate decisions.

### Loader request

A future loader request must identify:
- source object/handle and immutable file identity or digest;
- target architecture and supported ABI profile;
- requested argument/environment sizes with strict caps;
- explicit sandbox/capability profile;
- resource limits and cancellation handle;
- caller identity and admission decision reference.

The request must never accept an arbitrary kernel address or let the caller select writable/executable page permissions directly.

### Loader result

Return a structured status, process handle/ID only after successful creation, and bounded diagnostics. On failure, release every partial mapping, handle, reference, temporary file, and loader allocation. A failed launch must not leave a partially registered process.

## PE loader implementation gates

Implement in this order; do not skip ahead to GUI or Office:

1. **Parser library in user space:** DOS/PE signatures, COFF header, optional-header bounds, section table bounds, architecture, directory count and complete directory entries. Test integer overflow and truncated files.
2. **RVA/file-offset translation:** reject ranges that cross sections, exceed raw data, overlap invalid regions, or overflow arithmetic. Distinguish file-backed bytes from zero-filled virtual section tails.
3. **Image layout plan:** validate section alignment, file alignment, image/header sizes, section overlap, entry RVA, and W^X constraints before requesting memory.
4. **Mapping primitive:** map into a newly created isolated process address space only through documented native VM interfaces. Initially reject relocations or image layouts not yet supported.
5. **Relocations:** implement only documented relocation types for the selected architecture; reject unknown types and out-of-image targets.
6. **Imports and DLL resolution:** resolve only from a trusted, versioned runtime directory or explicit per-app dependency set. Never search the current working directory or an uncontrolled writable path for system DLLs. Detect dependency cycles and enforce depth/count/size limits.
7. **Startup contract:** define stack, arguments, environment, process parameters, TLS callbacks, initialization ordering and termination semantics before executing an entry point. Unsupported TLS/exception/unwind requirements must cause rejection.
8. **Minimal API set:** start with a small console-only profile (process termination, basic handles, bounded console I/O, required memory functions). Every API must have tests for success, invalid handles, bad buffers, concurrency and cleanup.
9. **Supervisor and sandbox:** run malformed-image tests and API calls under least privilege; enforce file/network/device access through native capability brokers.
10. **Installer transaction:** only after the runtime can safely launch a controlled test program. Installer custom actions are arbitrary code and require their own sandbox and rollback policy.

## Parser and loader test matrix

- Valid PE32 and PE32+ fixtures for supported machine types.
- Wrong machine type, malformed DOS/PE signatures, truncated COFF/optional headers.
- Invalid optional-header magic and directory count larger than the header can hold.
- Overflow in section count, section-table size, RVA + length, raw offset + size, and image allocation size.
- Sections overlapping headers or each other; raw ranges outside the file; entry point outside executable mapped sections.
- Import directory half-present, malformed descriptors, missing DLL, missing symbol, ordinal import, cyclic dependencies and excessive import count.
- Relocation block truncation, invalid block size, unsupported relocation type and target outside image.
- TLS/exception directory present when unsupported; malformed data must be rejected, not ignored if required for execution.
- Repeated load/fail/unload cycles with allocation and handle accounting.
- Fuzzing under host sanitizers where available, plus deterministic regression fixtures.
- A test that native ELF loading still works and has no behavior change.

Tests must be reproducible without GitHub-hosted runners. A host test passing validates only the code exercised by that test; it does not prove kernel integration or native OS execution.

## Office readiness gate

Microsoft 365 Apps is a separate, high-complexity target. Do not attempt Office installation until all of the following are evidenced:
- PE loader and required architecture are stable;
- required Win32 APIs, DLLs, COM/OLE, registry semantics and Click-to-Run behavior are identified and implemented or licensed;
- authentication and activation work through Microsoft's supported process;
- fonts, file dialogs, graphics, printing/export, updates and crash recovery are tested;
- a compatibility preflight can block installation before it damages an existing profile;
- the exact Office product, build, architecture, channel and ProsenjitOS/WinBridge revision are recorded.

Use Microsoft's official deployment documentation and content endpoints. Keep Current Channel configurable and resolve the latest eligible build at install/update time rather than hard-coding a build number. A browser shortcut to Microsoft 365 is a separate option, not desktop Office compatibility. Do not claim Office support until Word, Excel and PowerPoint launch, activate, open/save representative files and survive a supported update/restart test.

## Current verified status

- The repository has a kernel-side PE **metadata inspector** and a separate user-space PE metadata parser under `user/winbridge/`; neither is a PE executable loader.
- The original user-space parser-only synthetic host test passed with C11, `-Wall -Wextra -Werror -O2` in the development workspace before import-reader additions. The current branch now includes import-reader regressions for PE32 and PE32+ (including malformed PE32+ ordinal flags), but these latest tests have **not** been executed. Run both `sh user/winbridge/test-pe-image.sh` and `sh user/winbridge/test-pe-image-sanitize.sh` from the checked-out PR branch before claiming the current branch is verified. This is not kernel integration, fuzzing, or runtime verification.
- Image mapping, relocations, import walking, DLL resolver, WinAPI runtime, installer manager, desktop integration and Office support remain unimplemented/unverified unless separate source and runtime evidence is recorded.
- Kernel/process/syscall/VFS/security/user-mode prerequisites remain a gating dependency. Do not mark the runtime milestone complete until the repository build, host tests, integration tests and native runtime tests pass.
