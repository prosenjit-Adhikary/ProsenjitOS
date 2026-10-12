# ProsenjitOS Security Structure Audit — 2026-10-12

## Scope and method
Reviewed the current `main` source for the security startup chain, process quarantine, intent authorization, capability-table invariants, paging protections, and behavior-monitor enforcement. Changes are isolated on branch `security/hardening-audit-20261012`; `main` has not been modified directly.

## Findings and fixes in this branch

### 1. Quarantine self-test used an invalid process fixture
**Severity: High (startup-blocking).**

`security_quarantine_selftest()` initialized only `process.state`, but `process_state()` returns `PROS_PROCESS_DEAD` unless the object is a live `PROS_OBJECT_PROCESS` with a nonzero reference count. The first call to `security_quarantine()` therefore failed before exercising quarantine transitions. This is consistent with the previously observed `security_manager_init` failure stage `-11501`.

**Fix:** initialize the fixture's object type, reference count, and PID before exercising the state transitions.

### 2. Capability-table validation accepted malformed object metadata
**Severity: High (defense-in-depth / corrupted kernel state).**

The hardening check rejected `PROS_OBJECT_NONE` but did not reject unknown object type values, nonzero reserved fields, or zero-rights live entries.

**Fix:** validate object types against the ABI-defined set; reject corrupted object/entry reserved fields and zero-rights live entries. Expanded the hardening self-test for unknown types, reserved-field corruption, generation zero, rights outside policy, and undefined rights bits.

### 3. Authorization was permissive before hardening initialization
**Severity: High (startup ordering / fail-closed).**

`security_authorize()` checked process invariants only when hardening was already ready. If called before initialization, that check was skipped.

**Fix:** authorization now denies while the hardening startup gate is not ready and records a security violation. Once ready, process invariant checks are mandatory.

### 4. Intent object type and rights validation
**Severity: Medium (malformed intent rejection).**

Intent construction only rejected object type NONE, and authorization cast the 32-bit intent object type to 16 bits for handle checking. Invalid values could be truncated before comparison.

**Fix:** accept only ABI-defined object types at construction and authorization boundaries; reject undefined requested-rights bits before authorization.

### 5. Source cleanup
Removed a duplicate `ed25519_scalar.h` include from `kernel/intent.c`.

## Verification status

- [x] Source-level inspection of `kernel/intent.c`, `kernel/security_hardening.c`, `kernel/security_quarantine.c`, `kernel/security_monitor.c`, `kernel/object.c`, `kernel/process.c`, `kernel/paging.c`, and associated headers.
- [x] Applied fixes on `security/hardening-audit-20261012`.
- [ ] Clean `./scripts/build.sh clean` / full `-Wall -Wextra -Werror` build.
- [ ] QEMU ISO boot and complete startup self-test chain.
- [ ] Confirm the quarantine failure `-11501` is gone.
- [ ] Confirm security hardening startup gate and live process checks pass.
- [ ] Review CI results and run full regression tests.
- [ ] Sync the final verified log/report to `main`.

## Status
**Not complete yet.** Source fixes are committed to the audit branch, but this environment cannot run the user's Debian VM/QEMU commands directly. Do not call the hardening complete until the build and runtime/CI checks above pass. Any CI failure must be diagnosed and fixed before final sign-off.

## Reproduction commands (Debian VM)
```sh
cd /root/Projects/ProsenjitOS
git fetch origin
git checkout security/hardening-audit-20261012
./scripts/build.sh clean
timeout 30s qemu-system-x86_64 -cdrom build/prosenjitOS.iso -serial stdio -display none -no-reboot -no-shutdown
```
