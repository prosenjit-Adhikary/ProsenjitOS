# Userspace Application Manager — Initial Registry

## Implemented in this branch

- Fixed-size, versioned manifests for Native, Linux, and Windows application backend identities.
- Bounded application IDs and relative entry-point path validation.
- Bounded content-based ELF32/ELF64, PE32/PE32+, and OLE-container header classification; malformed/truncated known headers are rejected.
- Format-family/backend consistency checks (ELF for Native/Linux identities, PE for Windows identities); this is not a runtime compatibility check.
- Registry capacity limits, duplicate detection, ABI and required-feature checks.
- Explicit lifecycle: registration accepts only staged/unverified manifests; a trusted verifier caller must mark trust; install commit requires trusted metadata and installed same-backend dependencies.
- Disable and uninstall registry transitions; uninstall refuses to remove a package still referenced by another registered package.
- Fail-closed preflight for entries not marked installed/trusted; dependency graph checks cover missing dependencies, cycles, and cross-ecosystem dependency rejection.
- Host-side regression tests via `make app-unit-test`.

## Important limits

This is an initial **registry, format-detection, and metadata preflight**, not a complete Apps Handler. The format detector does not validate the full binary format or signatures and does not authorize execution; OLE detection is not MSI validation. The registry uninstall only removes an in-memory entry. It does not download or verify packages, install/update/remove filesystem objects, launch processes, implement a PE/ELF loader, provide Linux or Windows API compatibility, create a sandbox, or integrate with the desktop launcher.

The `trust_state` field is only a result supplied by the trusted manager after a separate provenance/signature verification step. `pros_app_mark_verified()` is a trust-boundary API, not cryptographic verification: only the trusted verification service may call it, and package-controlled manifests must never be allowed to self-attest. This in-memory registry cannot enforce caller identity by itself. Likewise, `pros_app_preflight()` is not proof that an executable is compatible or safe to run.

The install/disable/uninstall operations currently update only in-memory registry metadata. They do **not** copy or remove filesystem objects, create a transaction journal, recover after power loss, or provide atomic package rollback. Do not present these APIs as a real package installer or filesystem uninstall implementation.

All backends remain metadata identities until their respective runtime, package-source, permission, process-launch, and integration contracts are implemented and tested. Do not describe Linux or Windows applications as runnable based on this registry alone.

## Verification gate

Run `make app-unit-test` on the project-designated OfficeWindows/self-hosted runner. This branch has not yet been verified because the runner is currently unavailable. A queued or not-started workflow is not a pass, and an unrelated local build must not be reported as runner verification. Before merging, inspect the test logs from the required runner and complete a source review; boot/runtime compatibility is outside this module's claim.
