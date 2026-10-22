# Apps Handler ↔ ProsenjitFS/VFS Integration Gate

## Status

Design and source audit only. This document does not claim that a filesystem adapter, package installer, or userspace filesystem syscall exists. The current Apps Handler registry is a host-side/userspace module with a callback-based transaction coordinator. The VFS API is private kernel API under `kernel/include/vfs.h`; the public syscall ABI in `include/abi/syscall.h` does not currently expose filesystem operations.

## Source-audited constraints

- `vfs_file_open/read/write/truncate/close`, `vfs_mkdir`, `vfs_unlink`, and `vfs_rename` are kernel APIs that accept a kernel-owned mount object or kernel memory pointers. A userspace caller must not pass arbitrary pointers directly to them.
- The current public syscall set covers version, test completion, handle lifecycle, and IPC. It has no file open/read/write, directory mutation, package install, or spawn/exec syscall.
- The current process/security design includes a per-process syscall allowlist and capability handles. A future filesystem path must integrate with these controls rather than add an unrestricted path-based escape hatch.
- `vfs_rename()` resolves both parent directories and delegates to the filesystem driver's rename callback. The wrapper alone does not prove that every filesystem driver implements atomic rename, replacement semantics, or crash durability.
- The lower block layer already has an optional `block_device_flush()` callback, and ProsenjitFS transaction commit calls its internal `pfs_flush()` at journal-image, commit-record, and home-block stages. Its deterministic self-test documents interrupted-transaction/recovery coverage. However, `kernel/include/vfs.h` exposes no general VFS file-sync API, and the public userspace syscall ABI exposes no filesystem operation. Thus this is not yet an Apps Handler durability API. Devices without a real flush callback cannot provide a physical durability guarantee; QEMU fault-injection is not physical power-loss testing.
- Package verification and filesystem mutation must be separated: untrusted package bytes cannot self-assert `PROS_APP_TRUST_VERIFIED`.

## Required implementation sequence

1. **Choose the authority boundary.** Prefer a privileged package-manager service using narrowly scoped kernel handles/syscalls, or a kernel package-install service with an explicit, versioned request ABI. Do not link private kernel VFS headers into `user/apps`, expose mount pointers, or trust raw userspace pointers.
2. **Define bounded requests.** Version requests; bound package size, file count, path length, manifest length, dependency count, and total bytes. Copy and validate all userspace data through the established user-copy boundary. Reject absolute paths, dot components, duplicate paths, path traversal, links/reparse-like entries unless explicitly supported, and integer overflow.
3. **Define package verification.** Verify package signature/provenance and manifest-to-payload binding before calling the trusted registry transition. Record the verified digest and signer/policy result; the current in-memory `trust_state` bit is not verification evidence.
4. **Define staging layout and isolation.** Stage only beneath a manager-owned directory on the target mount. Never derive filesystem paths directly from unchecked package IDs. Ensure staged content is not discoverable as installed, and reject collisions with existing files.
5. **Specify publication guarantees per filesystem.** ProsenjitFS rename currently rejects an existing destination and requires both parent directories on the same active mount; its implementation uses journal transactions. Before using rename as the package publication primitive, add an explicit VFS-level sync contract and document its return/error semantics, verify the target block device has a real flush callback, and define how a multi-file package becomes visible atomically. A per-file rename is not an atomic whole-package publish. If the required guarantees are unavailable, fail closed or document weaker semantics explicitly.
6. **Persist registry state.** Use a versioned registry record/journal with checksums and bounded parsing. Recover interrupted transactions on boot; reconcile staged/published packages with registry records before exposing apps.
7. **Commit ordering.** Write and verify staged bytes; flush data and required metadata; atomically publish; durably record registry commit; only then report success. On every failure, preserve the primary error and report rollback failure separately. Recovery must handle crashes between each step.
8. **Enforce uninstall semantics.** Check reverse dependencies and ownership; remove only paths recorded in a verified install manifest; never recursively delete a user-controlled path assembled from an app ID. Make uninstall recoverable.
9. **Tests before enabling.** Add mock-adapter tests for partial writes, short writes, path collision, rename failure, rollback failure, registry-write failure, corrupt journal, simulated restart recovery, and dependency races. Then run host tests with warnings-as-errors, filesystem unit tests, and QEMU boot/runtime tests on supported Linux x64 infrastructure.

## Non-goals until later gates

This integration does not implement a process spawn/exec syscall, native ELF execution, a PE loader, Windows API compatibility, Linux ABI compatibility, desktop launch integration, or arbitrary application support. An installed registry entry must not be treated as permission to execute a binary.

## Acceptance checklist

- [ ] A documented public syscall/service ABI exists and is versioned.
- [ ] Authorization and user-copy validation are implemented and tested.
- [ ] Package signature/provenance verification is implemented by a trusted component.
- [ ] ProsenjitFS create/write/rename semantics and durability guarantees are explicitly specified, including its existing optional block flush and journal transaction boundaries.
- [ ] A VFS-level sync contract is defined before the Apps Handler relies on filesystem durability; whole-package visibility is not incorrectly inferred from per-file rename.
- [ ] Persistent registry/journal and boot-time recovery are implemented.
- [ ] Rollback and power-loss fault injection tests pass.
- [ ] Apps Handler tests pass on the Linux x64 runner; OfficeWindows is never used.
- [ ] No unverified claim of build, test, boot, or runtime success is made.
