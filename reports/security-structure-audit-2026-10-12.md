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

### 16. Process handle type lookup relied on numeric enum bounds
**Severity: Medium (object identity validation).**

`process_handle_type()` accepted object types using a numeric range check rather than the explicit ABI-defined set. If the enum ever contains reserved/gap values, an unknown type could pass this final lookup boundary.

**Fix:** validate object types using an explicit switch over supported types and add a regression assertion for an unknown type value.

### 17. IPC syscalls derived entry pointers before slot validation
**Severity: Medium (memory-safety robustness).**

IPC send/receive decoded the low handle byte and formed `&entries[slot]` before checking whether the slot was within the handle-table bound. Although preceding handle validation usually rejected invalid values, the ordering relied on out-of-range pointer formation and did not explicitly recheck object metadata at the final dereference site.

**Fix:** validate slot and generation first, then form the entry pointer; recheck live IPC object type, reserved fields, and refcount before using it.

### 18. Self-service syscall policy setter could broaden policy
**Severity: Medium (least-privilege invariant).**

`process_set_syscall_policy()` was documented as a tightening operation but accepted any valid syscall mask for a Super process, including re-enabling bits previously removed.

**Fix:** require each new mask to be a subset of the current mask and add a regression assertion proving a removed bit cannot be restored through this API.

### 19. Process unregister mutated PID lookup before validating object integrity
**Severity: Medium (lifecycle consistency).**

`process_unregister()` unlinked the process and cleared monitor state before `object_release()` could reject corrupted type/reserved metadata, potentially leaving a failed unregister no longer discoverable by PID.

**Fix:** validate process object type, reserved metadata, and nonzero PID before unlinking; add a regression assertion that corrupt metadata is rejected while the process remains registered.

### 20. Monitor fallback could miss a live process with a corrupt object header
**Severity: High (containment bypass under metadata corruption).**

The fallback `contain_process()` used `process_state()`, which intentionally maps invalid process-object metadata to `DEAD`. If a READY/RUNNING process had a corrupt object header, fallback containment returned without blocking it.

**Fix:** containment now inspects the raw scheduler-visible state after a null check, attempts the normal validated quarantine transition, and force-canonicalizes containment if metadata validation fails. Added regression coverage for a READY process with corrupt object-reserved metadata.

### 21. Bulk handle cleanup could partially mutate a corrupt table
**Severity: High (resource cleanup consistency).**

`handle_table_close_all()` previously iterated and closed entries one at a time. If a later slot had malformed generation/rights/object metadata, or multiple slots aliased an object whose refcount was smaller than the number of references, cleanup could release earlier handles before failing.

**Fix:** preflight every slot before closing any handle, validate the permanently reserved slot and all free/live metadata, and verify each referenced object's refcount covers its aliases in the table. Added a regression test confirming a malformed later slot leaves earlier valid entries and refcounts untouched.

### 22. Process reap could become unrecoverable after unregister validation failed
**Severity: High (process lifecycle integrity).**

`process_wait()` destroyed handles/address space before checking all `process_unregister()` invariants. If the process object metadata was corrupt, unregister could fail after teardown, while the code restored `ZOMBIE`; a later wait would then fail because the address space was already destroyed. The exit code was also published before a successful reap.

**Fix:** preflight process object type/reserved metadata, refcount, PID, address-space presence, and process-list identity before destructive teardown. Require no owned pages at unregister, and publish `exit_code` only after successful reap. Added a regression case showing corrupt object metadata is rejected without destroying the zombie's address space.

### 23. VFS path lookup lacked a whole-path length bound
**Severity: Medium (input validation / resource control).**

`vfs_path_lookup()` bounded each component and counted components, but did not enforce the declared `PROS_VFS_PATH_MAX` over the complete path before walking it.

**Fix:** reject paths longer than `PROS_VFS_PATH_MAX` before acquiring vnode references; added an overlong-path regression case.

### 24. Unused ProsenjitFS helper could fail warnings-as-errors builds
The source contained an unreferenced static `pfs_data_start()` helper. With the project's warnings-as-errors policy, this can stop a clean build.

**Fix:** removed the unused helper and confirmed there are no remaining references in the branch source.

### 25. Authorization denied invariant violations without containing the process
**Severity: High (security enforcement).**

When `security_authorize()` detected that hardening was not ready or a live process failed its invariant check, it denied the current operation but did not necessarily stop the process from continuing user execution. Inconsistent quarantine metadata also made the normal quarantine API reject containment.

**Fix:** add a fail-closed containment fallback for scheduler-visible process states. On a hardening/invariant failure, canonicalize the quarantine marker, clear any untrusted restore target, and force `BLOCKED` before returning the denial. Added startup regression tests for corrupted security-reserved bytes and invalid object-reserved metadata; clearing the repaired quarantine does not resume the process. The fixture runs under a narrowly scoped self-test mode and asserts the global violation counter is unchanged.

### 26. Process hardening did not verify aliased capability reference counts
**Severity: High (object lifetime integrity).**

The process invariant checker verified that each live handle pointed to a live object, but did not ensure that an object referenced by multiple slots held at least that many references. An under-counted object could reach zero and become `PROS_OBJECT_NONE` while another live handle still referenced it.

**Fix:** count each object's aliases in the process handle table and reject an invariant violation when its refcount is smaller than the number of live slots. Added a regression fixture with two aliased entries and an insufficient refcount.

### 27. Process hardening skipped malformed free capability slots
**Severity: Medium (capability-table integrity).**

The runtime process invariant checker skipped every empty handle slot, allowing malformed free-slot metadata to evade the hardening gate.

**Fix:** validate empty slots for nonzero generation, zero reserved metadata, and zero rights. Preserve `UINT16_MAX` as a valid retired empty slot and validate slot zero's canonical empty state.

### 28. Kernel data hardening boundary could be unaligned
**Severity: High (boot hardening gate).**

`pros_security_hardening_init()` requires every linker-provided protection range to have page-aligned start and end addresses. `__kernel_data_end` was set to the raw end of `.data`, which is not guaranteed to be page-aligned.

**Fix:** define `__kernel_data_end = ALIGN(4K)` so the data protection range covers complete pages and satisfies the range validator.

### 29. Execution admission could bypass an existing process quarantine
**Severity: High (execution boundary).**

`pros_security_exec_admit()` returned ALLOW based only on the file's admission record. A process already marked quarantined could therefore receive an ALLOW result for a trusted file, and invalid process-object metadata caused an early denial without containing an otherwise READY process.

**Fix:** execution admission rejects non-scheduler-visible states, returns BLOCK for a pre-existing quarantine regardless of file trust, and canonicalizes containment for a scheduler-visible process whose object metadata is invalid. Added regression cases for both. It also canonicalizes malformed quarantine flags/restore-state metadata to BLOCKED, with a regression test ensuring a READY process cannot remain runnable with corrupt quarantine state.

### 30. ELF loader trusted raw lifecycle state without validating process identity
**Severity: High (user-image mapping boundary).**

`pros_elf_load()` checked raw `state` for ZOMBIE/DEAD but did not verify that the process object itself was valid. A READY/NEW process with corrupt object type/reserved metadata or PID zero could reach image validation and mapping logic.

**Fix:** reject PID zero and require `process_state()` to recognize a live process object before mapping; add a regression case proving corrupt object metadata is rejected without creating the code mapping.

### 31. Source cleanup
Removed a duplicate `ed25519_scalar.h` include from `kernel/intent.c`.

### 32. ProsenjitFS probe accepted superblocks larger than the device
**Severity: Medium (filesystem discovery / storage boundary).**

`prosenjitfs_mount()` rejected a superblock whose declared `total_blocks` exceeded `device->sector_count`, but `prosenjitfs_probe()` only checked the internal superblock geometry. This could make discovery identify an image as ProsenjitFS even though its declared capacity extends beyond the actual block device, creating inconsistent probe-versus-mount behavior.

**Fix:** apply the device-capacity bound to both primary and backup superblocks during probe. This is defense-in-depth and aligns discovery with mount admission. No build or runtime test has yet verified the change.

### CI runner availability
Both required workflows were configured for an offline self-hosted runner and repeatedly remained queued. They now target `ubuntu-latest`, with the build workflow installing its NASM/GRUB/xorriso/mtools/GCC/binutils/QEMU dependencies. However, the latest hosted attempts failed before any steps were reported and the job-log endpoint returned no logs. This does not establish a code/build failure or a successful run; the runner/job-startup issue remains unresolved, and no commands were run on the user's Debian VM.

## Verification status

- [x] Source-level inspection of `kernel/intent.c`, `kernel/security_hardening.c`, `kernel/security_quarantine.c`, `kernel/security_monitor.c`, `kernel/security_admission.c`, `kernel/security_file.c`, `kernel/security_exec.c`, `kernel/object.c`, `kernel/process.c`, `kernel/ipc.c`, `kernel/syscall.c`, `kernel/address_space.c`, `kernel/usercopy.c`, `kernel/elf_loader.c`, `kernel/vfs.c`, `kernel/prosenjitfs.c`, `kernel/linker.ld`, and associated headers.
- [x] Applied fixes on `security/hardening-audit-20261012`.
- [x] Manually evaluated all 14 document-marker assertions from the security-design workflow against the branch's `SECURITY-MALWARE.md` and `ROADMAP.md`; every marker is present. This is a source-content check, not a substitute for a successful CI job.
- [ ] Clean `./scripts/build.sh clean` / full `-Wall -Wextra -Werror` build (hosted job failed before steps; no compiler output available).
- [ ] QEMU ISO boot and complete startup self-test chain (not executed by CI yet).
- [ ] Confirm the quarantine failure `-11501` is gone.
- [ ] Confirm security hardening startup gate and live process checks pass.
- [ ] Resolve hosted workflow job-startup failure and review actual build/boot/security-design results.
- [ ] Sync the final verified log/report to `main`.

## Status
**Implementation advanced; verification pending.** Source fixes and regression self-tests are committed to the audit branch. Debian/QEMU commands have intentionally not been run, as requested; the latest GitHub-hosted build/boot and security-design jobs failed before reporting steps; no build or boot test has been evidenced. Do not call the hardening fully verified until those checks pass. Any CI failure must be diagnosed and fixed before final sign-off.

## Reproduction commands (Debian VM)
```sh
cd /root/Projects/ProsenjitOS
git fetch origin
git checkout security/hardening-audit-20261012
./scripts/build.sh clean
timeout 30s qemu-system-x86_64 -cdrom build/prosenjitOS.iso -serial stdio -display none -no-reboot -no-shutdown
```
