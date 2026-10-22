# Userspace Application Manager — Initial Registry

## Implemented in this branch

- Fixed-size, versioned manifests for Native, Linux, and Windows application backends.
- Bounded application IDs and relative entry-point path validation.
- Registry capacity limits, duplicate detection, ABI and required-feature checks.
- Explicit staged/installed/disabled states and a fail-closed preflight for entries not marked installed.
- A trust-state gate and dependency checks, including missing dependencies, cycles, and cross-ecosystem dependency rejection.
- Host-side regression tests via `make app-unit-test`.

## Important limits

This is an initial **registry and metadata preflight**, not a complete Apps Handler. It does not download or verify packages, install/update/remove files, launch processes, implement a PE/ELF loader, provide Linux or Windows API compatibility, create a sandbox, or integrate with the desktop launcher.

The `trust_state` field is only a result supplied by the trusted manager after a separate provenance/signature verification step. This module does not perform cryptographic verification and must not accept package-controlled manifests directly. Likewise, `pros_app_preflight()` is not proof that an executable is compatible or safe to run.

All backends remain metadata identities until their respective runtime, package-source, permission, process-launch, and integration contracts are implemented and tested. Do not describe Linux or Windows applications as runnable based on this registry alone.

## Verification gate

Run `make app-unit-test` in the approved development environment. This branch has not yet been verified on the required OfficeWindows runner. Do not use the existing Debian/Linux self-hosted workflow as a substitute for that runner requirement. Before merging, run the unit tests on OfficeWindows and complete a source review; boot/runtime compatibility is outside this module's claim.
