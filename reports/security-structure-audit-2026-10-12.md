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

### 10. Hardening initialization could erase earlier violations
**Severity: Medium (security telemetry integrity).**

The hardening initializer reset its violation counter. A fail-closed authorization denial before initialization or during a later reinitialization could disappear from the runtime verification counter.

**Fix:** preserve the counter across initialization and saturate it at `UINT64_MAX`; static zero initialization provides the clean boot state.

### 11. Process lifecycle setter could revive a zombie
**Severity: High (process lifecycle integrity).**

The generic state setter accepted transitions both into `PROS_PROCESS_ZOMBIE` and from `ZOMBIE` back to active states. This could fabricate a zombie without terminating its threads or revive a terminated process, undermining the `process_wait()` reap path.

**Fix:** restrict `process_set_state()` to scheduler-visible states (`NEW` through `BLOCKED`), reject resetting an active process to `NEW`, and keep terminal `ZOMBIE`/`DEAD` transitions inside process lifecycle code. Added regression assertions for fabricated terminal transitions, active-to-NEW reset, and zombie resurrection; adjusted the execution-admission test to construct its terminal-state fixture directly.

### 12. Quarantine restore trusted corrupted metadata
**Severity: High (containment integrity).**

`security_clear_quarantine()` subtracted one from `quarantine_previous_state` without first constraining it to the valid saved READY/RUNNING encodings. Corrupted metadata could therefore restore an invalid process state while clearing quarantine. The quarantine entry path also treated any nonzero quarantine flag as a valid already-contained state.

**Fix:** require an exact quarantine flag, BLOCKED state, and a valid previous-state encoding before either treating quarantine as idempotent or clearing it. Invalid metadata is rejected without mutating containment state. Added a regression test for an invalid saved state.

### 13. Monitor fallback could silently fail on corrupted quarantine metadata
**Severity: High (containment reliability).**

The monitor's resource-exhaustion, timestamp-rollback, and failed-termination paths attempted `security_quarantine()` but ignored failure. Once quarantine APIs correctly reject inconsistent metadata, these paths could return a monitoring failure while leaving an otherwise live process runnable.

**Fix:** add a fail-closed containment helper. If the process is still live but normal quarantine rejects its corrupted metadata, force `BLOCKED` with a canonical quarantine flag and no restore target. Clearing that repaired quarantine therefore leaves the process blocked rather than guessing its previous state. Added a regression test for corrupted quarantine metadata.

### 14. Process initialization identity invariant
**Severity: Medium (identity invariant).**

`process_init()` could construct a process with identifier zero even though registration and hardening validation reject that identity later, leaving callers with a partially initialized but unusable process.

**Fix:** reject identifier zero before allocating an address space and add a regression assertion.

### 15. Object reference operations trusted corrupt metadata
**Severity: High (capability/object integrity).**

`object_retain()` and `object_release()` previously changed reference counts without validating object type or reserved metadata. `handle_close()` cleared the handle slot before discovering a corrupted object could not be safely released, potentially destroying recovery information and desynchronizing references.

**Fix:** validate object type, reserved fields, and refcount before reference-count changes; validate the handle entry and object before mutating the slot. Added a regression test that corrupts object reserved metadata and verifies retain/release/close all reject it without changing the count or live entry.

### 16. IPC syscalls derived entry pointers before slot validation
**Severity: Medium (memory-safety robustness).**

IPC send/receive decoded the low handle byte and formed `&entries[slot]` before checking whether the slot was within the handle-table bound. Although preceding handle validation usually rejected invalid values, the ordering relied on out-of-range pointer formation and did not explicitly recheck object metadata at the final dereference site.

**Fix:** validate slot and generation first, then form the entry pointer; recheck live IPC object type, reserved fields, and refcount before using it.

### 17. Self-service syscall policy setter could broaden policy
**Severity: Medium (least-privilege invariant).**

`process_set_syscall_policy()` was documented as a tightening operation but accepted any valid syscall mask for a Super process, including re-enabling bits previously removed.

**Fix:** require each new mask to be a subset of the current mask and add a regression assertion proving a removed bit cannot be restored through this API.

### 18. Process unregister mutated PID lookup before validating object integrity
**Severity: Medium (lifecycle consistency).**

`process_unregister()` unlinked the process and cleared monitor state before `object_release()` could reject corrupted type/reserved metadata, potentially leaving a failed unregister no longer discoverable by PID.

**Fix:** validate process object type, reserved metadata, and nonzero PID before unlinking; add a regression assertion that corrupt metadata is rejected while the process remains registered.

### 19. Source cleanup
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
**Implementation advanced; verification pending.** Source fixes and regression self-tests are committed to the audit branch. Debian/QEMU commands have intentionally not been run, as requested; the repository's self-hosted GitHub Actions build/boot and security-design workflows are queued. Do not call the hardening fully verified until those checks pass. Any CI failure must be diagnosed and fixed before final sign-off.

## Reproduction commands (Debian VM)
```sh
cd /root/Projects/ProsenjitOS
git fetch origin
git checkout security/hardening-audit-20261012
./scripts/build.sh clean
timeout 30s qemu-system-x86_64 -cdrom build/prosenjitOS.iso -serial stdio -display none -no-reboot -no-shutdown
```
