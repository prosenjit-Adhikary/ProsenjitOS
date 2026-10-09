
## 2026-10-09 — VFS vnode API self-test diagnostics

- Clean build completed successfully using ./scripts/build.sh clean.
- QEMU boot passes the earlier memory, paging, heap, address-space,
  crash-record, ELF validation, storage-health, process, thread and IPC tests.
- Security manager initialization still fails in VFS security self-tests.
- Instrumented vfs_vnode_api_selftest() failure paths to report source lines.
- Changed vfs_security_selftest() to propagate the detailed vnode test error.
- Latest QEMU stage: 0xFFFFFFFFFFFFE401 (signed -7167).
- With the current diagnostic offsets, the failing assertion maps to
  kernel/vfs.c line 1167.
- Root cause is not yet fixed. Diagnostic instrumentation is temporary
  and must be removed after the failing assertion is understood.
- Local ELF self-test fixture fixes and other existing kernel/filesystem
  changes must be preserved.
