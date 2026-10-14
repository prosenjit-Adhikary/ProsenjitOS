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

**Fix:** apply the device-capacity bound to both primary and backup superblocks during probe. Added a self-test fixture that recomputes checksums for oversized primary/backup copies, asserts probe rejection, and restores the formatted copies before continuing. The fixture has not yet been executed in a build/boot.

### 33. Direct capability close could release an under-counted aliased object
**Severity: High (capability/object lifetime integrity).**

`handle_table_close_all()` already preflighted same-table alias counts, but direct `handle_close()` only validated the selected entry. If corrupted metadata left two slots pointing to one object while its reference count was smaller than the alias count, closing one handle could drop the object to `PROS_OBJECT_NONE` while another live slot still pointed to it. The direct close path also did not reject all malformed rights/generation metadata before mutation.

**Fix:** added a shared same-table alias invariant check to `handle_check()`, `handle_alloc()`, duplicate, copy-transfer, move-transfer, and direct close. Malformed alias generation/rights/reserved metadata or `refcount < aliases` is rejected before capability mutation. Added a `process_selftest()` regression that deliberately undercounts a two-slot alias and verifies check/allocate/duplicate/copy/move/close all reject without changing either slot or the reference count; after restoring the count, both handles close normally. Source-reviewed only; build/runtime verification remains pending.

### 34. Object refcount could not distinguish owner references from cross-table capability references
**Severity: High (cross-process capability lifetime integrity).**

Same-table alias counting cannot detect an under-counted object referenced once in each of multiple process tables. If `refcount` were corrupted below the global number of capability references, releasing one process's handle could set the object type to `NONE` while another process still held a live handle. Likewise, an owner release could consume the last reference even though a handle remained.

**Fix:** add `pros_object.handle_refs` as a global live-capability-reference count. Handle allocation increments it; close decrements it before releasing the associated reference; `object_release()` refuses to decrement when no non-handle reference remains. Validate `handle_refs <= refcount` and that the count covers all aliases visible in each table. Updated process lifecycle, IPC object checks, hardening invariants, and regression fixtures. Source-reviewed only; full build and runtime verification remain pending.

### 35. ProsenjitFS accepted unsupported feature bits
**Severity: Medium (on-disk format compatibility / fail-closed parsing).**

The version-1 superblock validator required all four currently implemented feature bits but also accepted arbitrary additional bits. A checksum-valid disk image could therefore claim feature semantics this implementation does not understand and still be mounted as if it were compatible.

**Fix:** reject every feature bit outside the version-1 supported mask. Added a self-test fixture that sets an unknown high bit in both checksum-valid superblock copies, verifies discovery rejects the image, and restores the original formatted copies before the normal mount test. Source-reviewed only; the self-test has not run in a build/boot.

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


## 2026-10-14 — Self-hosted build runner recovery

- Investigated build/boot run #5448 (job ID 114725676763). The runner was online and checkout succeeded, but the job stopped at the old toolchain-install step because `sudo apt-get update` required an interactive password. The ISO build and both QEMU boot tests were skipped; this was an environment/workflow failure, not a compiler result.
- Updated `.github/workflows/build.yml` on this branch to match the repository's current preinstalled-toolchain verification step, avoiding privileged package installation on the self-hosted runner.
- Workflow fix commit: `3baf5912fb6dc16ede10c253c90d92feaf9e0f28`.
- New build/boot run #5735 (run ID 38530172950) and security architecture run #5237 (run ID 38530172909) were observed queued when this note was written. No successful build, QEMU boot, or runtime self-test is claimed until those jobs finish.
- The capability-table/object lifetime concurrency finding described above remains an open high-priority source-level risk. This workflow correction does not fix or verify that race.


## 2026-10-14 — Build #5735 diagnosis and boot-label fix

- Build/boot run #5735 (run ID 38530172950) did start on the self-hosted Debian runner. Toolchain verification passed; assembly stopped before C compilation at `boot/entry.asm:90` with `symbol `long_mode_entry.security_halt' not defined`.
- Root cause: NASM local labels are scoped to the preceding non-local label. The branch to `.security_halt` was scoped under `long_mode_entry`, but the definition followed the non-local `cpu_security_init` label and therefore belonged to a different scope.
- Fixed in commit `c19c3caf766ea2564676207bbb3a14e975ab0656`: branch now targets the explicit non-local `security_halt` label, and the halt label is defined in that global scope.
- VFS source re-check: the current branch contains the whole-path bound in `vfs_path_lookup()` and the provider-registry self-test snapshots/restores existing provider order. These are source observations only; the full kernel build has not reached C compilation yet, so they do not prove the recurring runtime VFS/provider self-test passes.
- New build/boot run #5740 (run ID 38530679161) and security architecture run #5242 (run ID 38530679146) were queued after the fix. Until the new build passes, ISO generation and both QEMU boot tests remain unverified. ProsenjitFS persistence/remount and crash-recovery are also not yet proven by runtime tests.


## 2026-10-14 — Follow-up build diagnosis: unused process object validator

- Build/boot run #5740 (run ID 38530679161) progressed past NASM and toolchain setup, then failed C compilation under `-Werror`:
  `kernel/process.c:17:12: error: ‘process_object_type_valid’ defined but not used [-Werror=unused-function]`.
- Root cause: the branch introduced an explicit ABI object-type validator but `process_handle_type()` did not call it. This was a real compile failure, not a runner/billing issue.
- Fixed in commit `ae48763aaaaf77ad8f06ae90f8d1160815479fa5`: `process_handle_type()` now rejects undefined object types, reserved handle-entry state, and retired maximum-generation entries before returning the object type; it still delegates full capability/entry validation to `handle_check()`.
- Source re-check confirms the helper is now referenced in the same translation unit. This is not yet a compiler pass; a fresh PR build must verify it and reveal any subsequent diagnostics.
- Security architecture run #5242 (run ID 38530679146) passed its architecture-contract check, but that is not equivalent to runtime security self-tests.
- ISO creation and QEMU boot tests were skipped in #5740 because compilation failed. VFS runtime tests and ProsenjitFS persistence/remount/crash-recovery remain unproven.


## 2026-10-14 — Build #5746 diagnosis: missing syscall ABI include

- Build/boot run #5746 (run ID 38530932047) passed runner setup, checkout, and toolchain verification, then stopped during C compilation in `kernel/intent.c` because `PROSENJIT_SYSCALL_POLICY_ALL` was undeclared in the two authorization self-test fixtures.
- Root cause: `intent.c` used the syscall-policy ABI macro without directly including its defining shared ABI header. This was a genuine compile error, not a runner issue.
- Fixed in commit `1e851346f83c073e6f98ac00d968bd3d5e4fee17` by directly including `../../include/abi/syscall.h` after `process.h`, matching the kernel's shared ABI header layout.
- Security architecture run #5248 passed the architecture-contract check; this does not test compilation or runtime behavior.
- A fresh build/boot run must still confirm this include fix and reveal any further diagnostics. ISO generation, QEMU boot, the startup quarantine self-tests, VFS/provider runtime checks, and ProsenjitFS persistence/recovery remain unverified.


## 2026-10-14 — ELF overlap self-test corrected by source-level diagnosis

- Build/boot run #5748 compiled the kernel and generated the ISO, but the QEMU ISO boot failed at the ELF validation negative self-test before later boot milestones. The previous log did not include the self-test's internal return code.
- Source inspection found a deterministic test-fixture inconsistency: the overlap regression set the second PT_LOAD virtual address to `ELF_TEST_CODE_VA + 0x800` while keeping `p_offset = 0x1000`. The validator correctly enforces `p_vaddr % 4096 == p_offset % 4096`, so this fixture returns `PROS_ELF_EFORMAT` before reaching the intended overlap check; the test incorrectly expected `PROS_ELF_ECONFLICT`.
- Corrected the fixture in commit `b92dc74f0765dd23c9df9424b5d78752aaf8b4b0`: the first segment's test memory range is enlarged to `0x1800`, and the second segment starts at `ELF_TEST_CODE_VA + ELF_PAGE`, preserving page congruence while producing a real overlap. The test now exercises the overlap branch rather than failing earlier for alignment.
- This is a source-level root-cause diagnosis and fix; it is not yet runtime-verified. A fresh self-hosted Build/QEMU run must confirm the fix. Security architecture checks do not substitute for QEMU/runtime evidence.


## 2026-10-14 — Latest verification blocker: self-hosted jobs remain queued

- Latest associated Build and boot-test run #5756 (run ID `38531760665`) and Security architecture run #5258 (run ID `38531760723`) are both queued. Their jobs have no steps and no logs, so they have not compiled or tested the current head.
- Current workflow files explicitly use `runs-on: [self-hosted, linux, x64]`. An earlier statement in `docs/AI_DEBUG_LOG.md` that these workflows had switched to `ubuntu-latest` was inaccurate; it has been corrected. No runner change was made because the user's preference is to avoid GitHub-hosted/paid services when possible.
- This is currently a runner availability/dispatch blocker. It is not evidence that these latest attempts found a new code defect, and it is not evidence of a passing build.
- Latest actual code/runtime evidence remains Build #5748: C compilation and ISO generation completed, then QEMU failed at the ELF negative self-test. The overlap fixture was corrected in `b92dc74f0765dd23c9df9424b5d78752aaf8b4b0`, but that fix has not yet been executed by a successful build/boot run.
- Required next step: restore/confirm the self-hosted runner is online and accepting jobs, then rerun Build/QEMU. Do not close the PR or claim verification complete before the corrected ELF self-test and the remaining startup milestones pass.


## 2026-10-14 — Additional source-level monitor containment defect

**Finding (High: fail-closed behavior monitoring).** `pros_security_monitor_enforce()` previously rejected a process by calling `process_state()` in its initial guard. Since `process_state()` intentionally maps invalid process-object metadata to DEAD, a raw READY/RUNNING/BLOCKED process with corrupted object metadata returned early and never reached `contain_process()`. This contradicted the intended fallback that prevents corrupted metadata from leaving a scheduler-visible process runnable.

**Fix:** commit `5781a215643517cabbd1df51c70ae007e229c7da` moves process-state validation after pointer/PID matching, checks the raw scheduler-visible state, and attempts canonical containment when the validated helper reports DEAD. Added a regression test through the public enforcement API. The code change is not yet compiler- or runtime-verified.

**Current verification blocker:** Build #5760 (run ID `38532057578`) and Security #5262 (run ID `38532058158`) remain queued with no job steps/logs. The new test must run after the self-hosted runner is accepting jobs. No claim of passing build or QEMU boot is made.


## 2026-10-14 — Admission record coherence enforcement

**Finding (High: trust-decision integrity).** The execution admission boundary previously selected behavior from record->security.action without validating that the action agreed with the trust state. A contradictory MALICIOUS + ALLOW record could therefore reach the allow branch. The quarantine table similarly accepted any record whose action was QUARANTINE, even when its state claimed TRUSTED.

**Fix:** introduced pros_file_admission_record_valid() in the admission layer. It rejects out-of-range state/action values, unknown evidence bits, incoherent state/action pairs, TRUSTED without local-trust evidence, VERIFIED without format-valid and signature-valid evidence, and SUSPICIOUS without suspicious-reputation or invalid-signature evidence. Both pros_security_exec_admit() and pros_security_quarantine_add() now enforce this contract. Regression cases cover contradictory MALICIOUS/ALLOW execution input and TRUSTED/QUARANTINE insertion.

**Commits:** 80ce57a37adc68719b26a90e3380a3c48a57b569, d36f01305be223bca53ab127c0e5ab67a7aee253, 379f0f02d331482d436e21704173f188fe9e1d1deb, 3ff8941ae3f5a28afbe9e3d52c9f8fbbcc1ad0b5.

**Verification caveat:** source-level only. No compiler, self-test, or QEMU result is available for these commits yet. Do not mark this issue verified until the self-hosted build and runtime tests execute successfully.
