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

**Fix:** validate object types against the ABI-defined set; reject corrupted object/entry reserved fields, zero-rights live entries, retired maximum-generation handles, non-canonical 64-bit handle aliases, and malformed free slots. Enforce the same invariants at allocation, handle-check, duplicate, copy-transfer, move-transfer, and process-handle-type boundaries rather than relying only on the top-level authorization path. Expanded the hardening self-test for unknown types, reserved-field corruption, generation zero/retirement, zero rights, dead referenced objects, high-bit handle aliases, malformed free slots, and rights outside policy/undefined rights bits.

### 3. Process metadata needed explicit initialization and lifecycle validation
**Severity: High (nondeterministic authorization denial / corrupted-state handling).**

The process structure contains reserved security bytes that were not explicitly initialized by process_init(). Once reserved bytes are validated, uninitialized stack contents could make a valid process fail hardening checks.

**Fix:** zero all reserved process bytes during initialization, reject nonzero object/process reserved fields, and validate process lifecycle plus quarantine previous-state consistency. Added a process self-test for the reserved-byte initialization contract. IPC object validation also now rejects a nonzero object reserved field.

### 4. Authorization was permissive before hardening initialization
**Severity: High (startup ordering / fail-closed).**

`security_authorize()` checked process invariants only when hardening was already ready. If called before initialization, that check was skipped.

**Fix:** authorization now denies while the hardening startup gate is not ready and records a security violation. Once ready, process invariant checks are mandatory.

### 5. Intent object type and rights validation
**Severity: Medium (malformed intent rejection).**

Intent construction only rejected object type NONE, and authorization cast the 32-bit intent object type to 16 bits for handle checking. Invalid values could be truncated before comparison.

**Fix:** accept only ABI-defined object types at construction and authorization boundaries; reject undefined requested-rights bits before authorization.

### 6. File-admission policy could erase an explicit local deny
**Severity: High (policy bypass).**

The signed-admission path cleared `local_denied` on both valid and invalid signature paths. A valid signature could therefore override an administrator's deny decision, while an invalid signature could downgrade an explicit deny to quarantine.

**Fix:** preserve local-deny metadata regardless of signature outcome, make a well-formed local deny the highest-priority admission decision even when other metadata is malformed, and add regression tests for valid/invalid signatures and malformed reputation combined with explicit deny. A valid signature now remains `VERIFIED` rather than being relabeled as locally `TRUSTED`.

### 7. Malformed file-admission metadata could be treated as trusted
**Severity: Medium (malformed policy input).**

Unknown reputation enum values and non-boolean trust flags could pass through truthiness checks and reach the allow policy.

**Fix:** malformed flags/reputation now resolve to UNKNOWN/QUARANTINE; regression tests cover unknown enum and invalid flag encodings.

### 8. Monitor enforcement needed containment fallbacks
**Severity: High (monitor resource exhaustion / teardown failure).**

When the behavior monitor ran out of state slots, received non-monotonic timestamps, or failed to terminate a blocked process, the process could remain runnable without a reliable monitoring decision.

**Fix:** monitor capacity/timestamp failures trigger quarantine, and failed termination attempts fall back to quarantine.

### 9. Audit-ledger exhaustion could undo containment
**Severity: High (fail-closed enforcement).**

When a quarantine decision was made but the reason ledger could not accept another record, the authorization path cleared quarantine before returning DENY. That could leave a process runnable after a privileged request triggered containment.

**Fix:** quarantine remains active when audit storage fails. Added a startup self-test that deliberately exhausts the ledger sequence, attempts a privileged request, and verifies DENY plus persistent BLOCKED/quarantined state before restoring test state.

### 10. Source cleanup
Removed a duplicate `ed25519_scalar.h` include from `kernel/intent.c`.

## Verification status

- [x] Source-level inspection of `kernel/intent.c`, `kernel/security_hardening.c`, `kernel/security_quarantine.c`, `kernel/security_monitor.c`, `kernel/security_admission.c`, `kernel/security_file.c`, `kernel/object.c`, `kernel/process.c`, `kernel/ipc.c`, `kernel/paging.c`, and associated headers.
- [x] Applied fixes on `security/hardening-audit-20261012`.
- [ ] Clean `./scripts/build.sh clean` / full `-Wall -Wextra -Werror` build (GitHub Actions run queued; no Debian commands have been run).
- [ ] QEMU ISO boot and complete startup self-test chain.
- [ ] Confirm the quarantine failure `-11501` is gone.
- [ ] Confirm security hardening startup gate and live process checks pass.
- [ ] Review CI results and run full regression tests (both workflows remain queued on the repository's self-hosted runner).
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
