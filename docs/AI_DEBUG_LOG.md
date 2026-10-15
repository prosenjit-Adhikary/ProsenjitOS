# ProsenjitOS AI Debug Log

## 2026-10-12 — Security structure audit
Branch: `security/hardening-audit-20261012`

### Findings / changes
- Fixed invalid `security_quarantine_selftest()` fixture: set process object type, reference count, and PID. This directly addresses the startup self-test path associated with error stage `-11501`.
- Hardened live capability entries and direct object-layer APIs: reject unknown object types, nonzero reserved fields, zero rights, generation zero, undefined rights, and rights beyond process policy; enforce checks in allocation, check, duplicate, and transfer paths; reject retired maximum-generation handles, high-bit aliases of 24-bit handles, and corrupted free slots.
- Added hardening self-test cases for malformed capability metadata; isolated the referenced test object so unknown-type tests reach the intended validation branch rather than short-circuiting on process identity.
- Made `security_authorize()` fail closed until kernel hardening is ready.
- Validate intent object types against the defined ABI object set at creation and authorization; reject undefined rights bits.
- Validate process lifecycle and quarantine-state invariants; initialize reserved process bytes and add a process regression check. Reject PID zero directly in `process_init()` and cover it with a regression assertion, rather than creating a partially initialized process that cannot be registered.
- Restrict `process_set_state()` to scheduler-visible states (`NEW` through `BLOCKED`), prevent an active process from resetting to `NEW`, and reserve terminal `ZOMBIE`/`DEAD` transitions for lifecycle code. This prevents fabricated zombies (without thread teardown), zombie resurrection, and lifecycle-state reset; added regression assertions and adjusted the execution-admission test fixture.
- Harden `security_quarantine()` and `security_clear_quarantine()` against inconsistent quarantine flags and corrupted restore-state metadata. Invalid previous-state values now fail without clearing containment or writing an arbitrary process state; added a regression test for a corrupted restore state.
- Reject corrupted reserved metadata in IPC objects.
- Saturate hardening violation/generation counters rather than allowing wraparound; preserve violation counts across reinitialization instead of erasing earlier fail-closed events.
- Preserved explicit local deny decisions across both valid and invalid signature admission paths and made a valid local deny outrank malformed metadata; added regression cases.
- Corrected signed admission so successful cryptographic verification is recorded as VERIFIED (unless separate local-trust policy applies), not relabeled as TRUSTED.
- Invalid file-admission flags and unknown reputation values now fail closed to UNKNOWN/QUARANTINE, with regression coverage.
- Behavior-monitor resource exhaustion, timestamp rollback, and failed process termination now attempt quarantine containment.
- Added a fail-closed monitor containment fallback: if normal quarantine rejects corrupted quarantine metadata on an otherwise live process, force BLOCKED with a canonical quarantine marker and no restore target; added a regression test proving quarantine clear leaves it BLOCKED. The fallback now inspects the raw scheduler-visible state when object metadata is corrupt (since `process_state()` intentionally maps invalid objects to DEAD), with a regression case proving a corrupt object header cannot leave a READY process runnable.
- Audit-ledger exhaustion no longer clears a quarantine decision; a startup regression self-test forces ledger exhaustion and verifies the process remains quarantined and BLOCKED.
- Added regression coverage ensuring high bits cannot alias a valid capability handle across check, lookup, and duplication paths.
- Removed duplicate `ed25519_scalar.h` include.
- Updated audit report: `reports/security-structure-audit-2026-10-12.md`.

- ProsenjitFS superblock validation now rejects unknown feature bits instead of accepting unsupported on-disk semantics. Added a checksum-valid fixture that sets an unknown high feature bit in both primary/backup copies, expects probe rejection, and restores the formatted copies before mount tests. Source-reviewed only; build/boot execution remains pending.
- ProsenjitFS discovery now rejects a structurally valid/checksummed superblock whose `total_blocks` exceeds the probed block device's `sector_count`, for both primary and backup copies. Added a regression fixture that rewrites both superblock copies with an oversized capacity and recomputed valid checksums, verifies probe rejection, then restores the original copies before normal mount tests. This fixture is source-reviewed only; the self-test has not been executed in a build/boot.
- Added `pros_object.handle_refs` to track live capability references globally across process tables, separate from owner references. `handle_alloc` increments both object `refcount` and `handle_refs`; close decrements the handle count before releasing its reference. `object_release` refuses to consume the last non-handle reference while handles remain. The shared alias invariant is enforced by check/allocate/duplicate/copy/move/close and hardening checks; malformed same-table aliases and `handle_refs > refcount` fail closed. Regression coverage checks under-counted two-slot aliases across all entry points, preserves entries/counts on rejection, verifies valid close cleanup, and proves owner release cannot invalidate a live handle.

### Verification checklist
- [x] Current GitHub main source inspected.
- [x] Source fixes committed to the audit branch.
- [x] Re-ran a lexical delimiter/comment/string balance scan on all 14 changed C translation units after the ProsenjitFS probe regression addition; no imbalance or unterminated comment/string was detected. This is not a compiler or semantic test.
- [x] Re-ran the lexical scan on all 14 changed C translation units after adding global `pros_object.handle_refs`, updating process/IPC/hardening validation, expanding cross-table lifetime regressions, and simplifying close preflight; no delimiter imbalance or unterminated comment/string was found.
- [x] After rejecting unknown ProsenjitFS feature bits and adding a checksum-valid unknown-feature regression fixture, re-ran the lexical scan across all 14 changed C translation units; all 14 passed. The scan caught a missing closing parenthesis in the new validation condition, which was corrected before this final scan. This remains source-level checking only, not compilation or runtime verification. GitHub Actions still reports failure with no steps/job logs exposed, so this source scan does not establish build or runtime correctness.
- [x] Manually checked all security-design workflow document markers against the current branch (`SECURITY-MALWARE.md` and `ROADMAP.md`); all 14 checks pass in source. CI execution itself remains unverified because the job failed before steps.
- [ ] Clean GCC build (hosted GitHub Actions job failed before any steps were reported; no compiler run is evidenced).
- [ ] QEMU boot reaches completion with all security self-tests passing (boot step has not executed).
- [ ] Verify stage `-11501` no longer occurs in a real boot log.
- [ ] Restore actionable CI results: build/boot and security-design hosted jobs currently fail before reporting steps; job logs are unavailable, so root cause remains unresolved.
- [ ] Final report and verified log synchronized to main.

### Important
Do not mark this work complete until build, QEMU, and regression checks pass. The GitHub source edits have not yet been built or boot-tested. The user's Debian VM has not been touched; current hosted CI attempts failed before producing step logs.

- Harden object reference-count operations and handle close against corrupt object metadata; add regression coverage that preserves both refcounts and live handle entries on rejection. Execution admission now refuses a trusted-file ALLOW result for an already quarantined process, requires a scheduler-visible state, and forces containment when object metadata is invalid; added regression tests for both cases. `pros_elf_load()` now also rejects PID-zero or invalid process-object metadata before mapping any user page; the ELF self-test verifies a corrupt object header causes no mapping. It also canonicalizes malformed quarantine flags/restore-state metadata to BLOCKED and tests that a READY process cannot remain runnable with a corrupt quarantine marker.
- Fixed IPC send/receive syscall paths to validate the decoded slot range before deriving an entry pointer; they also recheck live IPC object metadata before dereferencing it. Added a monotonicity guard to `process_set_syscall_policy()` so self-service policy changes cannot re-enable syscall bits, with a regression assertion. Hardened `process_unregister()` to reject invalid process-object identity/reserved metadata before unlinking PID lookup state, with a regression assertion. `process_wait()` now preflights unregister invariants before destroying child resources and only publishes `exit_code` after successful reap; corrupted object metadata is rejected without destroying the zombie's address space. `process_unregister()` also requires `owned_pages == NULL`. `process_handle_type()` now validates object types against the explicit ABI-defined set rather than relying on an enum numeric range; the hardening self-test rejects an unknown type value.
- Removed the unused `pfs_data_start()` static helper from `kernel/prosenjitfs.c` after source cross-check found it had no call sites (a `-Werror=unused-function` build blocker). Bounded `vfs_path_lookup()` to `PROS_VFS_PATH_MAX` and added an overlong-path regression case. Page-aligned `__kernel_data_end` in `kernel/linker.ld`; the hardening range validator rejects non-page-aligned boundaries, while the previous linker symbol used the raw (potentially unaligned) end of `.data`.
- Authorization now canonicalizes containment to BLOCKED when a scheduler-visible process fails the hardening-ready or process-invariant gate, rather than only denying the current request. Added startup regression tests for corrupt security-reserved bytes and invalid object-reserved metadata; both force containment, and clearing quarantine without a trusted restore target leaves the process BLOCKED. The test uses a narrowly scoped self-test mode so its intentionally malformed fixture does not increment the runtime hardening-violation counter that the boot gate requires to remain zero.
- Extended `pros_security_hardening_check_process()` to validate free capability-slot metadata as well as live entries: generation zero, nonzero reserved bytes, and stale rights on empty slots are rejected; a `UINT16_MAX` empty slot remains accepted as a legitimate retired slot. Added regression coverage for both malformed and retired free slots. Also validate permanently reserved slot zero remains empty with generation 1, zero rights, and zero reserved metadata. `handle_table_close_all()` now validates the entire table before releasing any references, including reserved slot zero, empty-slot rights, live object metadata, valid live generations, and per-object reference counts for duplicate entries. This prevents malformed late entries or an under-counted aliased object from causing partial bulk cleanup. Added a regression test that corrupts a later slot and confirms the earlier live handle and refcount remain unchanged before cleanup is retried successfully. The live-entry check now also counts aliases per object and rejects a table whose live handles outnumber the object's reference count; a regression fixture covers under-counted duplicate references.
- Correction (2026-10-14): the current `.github/workflows/build.yml` and `.github/workflows/security-design.yml` still specify `runs-on: [self-hosted, linux, x64]`; they were NOT switched to `ubuntu-latest`. The earlier statement claiming a switch to hosted runners was inaccurate and is superseded by this correction. Latest runs #5756 (ID `38531760665`) and #5258 (ID `38531760723`) remain queued, with jobs showing no steps/logs. This points to runner availability/dispatch as the immediate verification blocker, not a confirmed source failure. Keep the user's self-hosted-runner preference; do not change to hosted runners without explicit approval.


## 2026-10-14 — Latest runner queue check

- Queried PR #128 head `bffd4b951bcd6fb5f30addbe33f464a639518b63`: latest associated build/boot run #5756 (ID `38531760665`) and security architecture run #5258 (ID `38531760723`) are both `queued`.
- Both jobs still use `[self-hosted, linux, x64]`; each job has no steps and no log URL yet. Therefore these attempts have not produced new compiler or QEMU results. Do not label them as code failures or passes.
- The prior Build #5748 result remains the latest actual runtime evidence: kernel compiled and ISO was generated, but QEMU boot stopped at the ELF negative self-test. The overlap fixture correction is still awaiting execution.
- Next verification needs the configured self-hosted runner to be online and accepting jobs, or an explicitly approved alternative runner. No commands have been run on the user's Debian VM in this session.


## 2026-10-14 — Monitor enforcement guard bypassed corrupt-state containment

- Source cross-check found a real fail-closed gap in `pros_security_monitor_enforce()`: its initial guard called `process_state()` and returned immediately when invalid object metadata made that helper report `DEAD`. This prevented the already-implemented raw-state containment fallback from running for an otherwise scheduler-visible READY/RUNNING/BLOCKED process.
- Fixed in commit `5781a215643517cabbd1df51c70ae007e229c7da`: first verify the process/event pointers and matching PID, then inspect the raw scheduler-visible state; if `process_state()` reports DEAD for that live raw state, attempt containment and reject the event. A mismatched PID still returns without touching the process.
- Added a regression case to `pros_security_monitor_selftest()` that exercises the public enforcement entry point with a READY process whose object reserved field is corrupt, and requires BLOCKED + canonical quarantine metadata.
- This is a source-level fix only. Build #5760 / Security #5262 are queued without steps or logs, so compilation and runtime execution of the new regression remain unverified.


## 2026-10-14 — Admission-record consistency bypass at execution/quarantine boundary

- Source cross-check found that pros_security_exec_admit() trusted the action field alone: a caller-supplied record marked MALICIOUS but paired with ALLOW could pass the execution boundary. The quarantine insertion API likewise checked only QUARANTINE, allowing contradictory trust-state/action metadata into the quarantine table.
- Added pros_file_admission_record_valid() to validate trust-state/action pairings, reject unknown evidence bits, require local-trust evidence for TRUSTED, require format+signature evidence for VERIFIED, and require suspicious/signature-invalid evidence for SUSPICIOUS.
- Both execution admission and quarantine insertion now reject inconsistent records. Added regressions for a MALICIOUS/ALLOW record at the exec boundary and a TRUSTED/QUARANTINE record at quarantine insertion.
- Commits: 80ce57a37adc68719b26a90e3380a3c48a57b569, d36f01305be223bca53ab127c0e5ab67a7aee253, 379f0f02d331482d436e21704173f188fe1d1deb, 3ff8941ae3f5a28afbe9e3d52c9f8fbbcc1ad0b5.
- Verification status: source edits committed; compiler, security self-tests, and QEMU have NOT been run for this change. The self-hosted workflow queue must be checked separately; queued/no-step jobs are not a pass or a source failure.


## 2026-10-14 — Provider enable-flag canonicalization

- Source review found provider registration copied the caller's enabled field without checking its boolean encoding; reputation lookup treats any nonzero value as enabled. Registration now rejects enabled values greater than 1, and the provider self-test verifies rejection does not change provider_count.
- Commit: ad5507a3af7419929ec8f4d26d7434eb7422f3f2. Source-only change; build and self-test are still pending because the latest self-hosted jobs are queued without steps/logs.


## 2026-10-14 — Conflicting admission evidence combinations

- Follow-up cross-check tightened the admission-record validator: ALLOW decisions now reject explicit local-deny/malicious evidence; mutually exclusive reputation outcomes and simultaneous valid/invalid signature evidence are rejected; UNKNOWN/SUSPICIOUS/MALICIOUS states must agree with action and evidence. A malformed-format MALICIOUS/BLOCK decision remains valid even though no dedicated format-invalid evidence bit exists.
- Added regression cases for VERIFIED+LOCAL_DENY, TRUSTED+malicious reputation, signature-invalid with clean reputation (allowed as independent evidence dimensions), malformed-format block, and unknown evidence bits.
- Commits: 395f1fd5cbf3866c514b92c211345ced8d352a0f, ebbf8450a10895c3daa9fd5440575ce2c647b282, 4a0e78b1cbde3733bd4d2f746dec2fa2aa999f72.
- Verification remains source-only; no compile or runtime test has executed for these changes.


## 2026-10-14 — Strict Ed25519 subgroup validation

- Cryptographic source audit found the verifier rejected small-order points via [8]P = identity but did not prove R and A belong to the prime-order subgroup. Mixed-order points are not excluded by an order-8 test alone.
- Verification now additionally checks [L]R and [L]A are identity, where L is the Ed25519 subgroup order. The RFC 8032 empty-message vector self-test explicitly checks both encoded points satisfy the subgroup predicate.
- Commit: 42c7d6b20c5adb82a399d17e62d47c4c2535caf1.
- Verification status: source-only. This adds scalar multiplications to signature verification and needs timing/performance review plus build/runtime self-test before being considered complete.


## 2026-10-14 — Ed25519 rejection-path regression coverage

- Added signature self-test cases requiring rejection of a small-order identity public key, a small-order identity R encoding, and a non-canonical S scalar equal to subgroup order L. These supplement the RFC 8032 positive vector and the altered-signature negative case.
- Commit: f24cd1d000f5d183afb2a5a3ff1a52d35a66a473.
- Verification remains pending: no compile, self-test execution, timing review, or QEMU boot has run for this latest crypto change.


## 2026-10-14 — Preserve independent malicious reputation evidence

- Follow-up review found `pros_file_admission_evaluate_signed()` discarded all reputation metadata when signature verification failed, even though its policy comment says independent malicious reputation may still require a block. The failure path now preserves only `PROS_FILE_REPUTATION_MALICIOUS`; clean/suspicious reputation and provenance are still not accepted as trust signals after failed verification.
- Added a regression case asserting invalid signature + malicious reputation yields `MALICIOUS/BLOCK` without relying on local-deny metadata.
- Commit: `9f52d37b17d268e4c3ed94e01cbfae40169cdcb1`.
- Source-only change; compile and runtime tests remain pending.


## 2026-10-14 — Ed25519 point-encoding negative tests

- Expanded `pros_ed25519_point_selftest()` to reject the non-canonical field encoding `y = p` and the invalid negative-zero encoding (sign bit set while recovered x is zero). This specifically covers strict decoder branches rather than only the RFC positive vector.
- Commit: `ded8273e90ef31091aec9399caf619a8e6fbf633`.
- Runtime execution is pending; the test code has been committed but is not yet proven by a build or boot.


## 2026-10-14 — QEMU boot fault localization after storage discovery

- Reproduced from Build #5901 (run ID `38569112345`, job ID `115869312895`): toolchain verification and ISO build passed; QEMU stopped before the required CPL3/IPC milestone. Serial output reached `storage volume discovery self-test passed`, then repeated CPU exceptions with `RIP=0` and `CR2=0`.
- The failure output does not yet prove a specific faulty statement. The exception storm obscures the original execution point; artifact upload also reported repository artifact storage quota exhausted, so it is not a reliable diagnostic path.
- Added temporary early `pfs_selftest_progress` checkpoints (100–113) around ProsenjitFS block-device setup, LBA checks, format, superblock validation, mount, and initial VFS operations. Added a serial marker and checkpoint 99 at the call site in `kernel/kernel.c`. The exception handler already prints this progress value when nonzero.
- Purpose: the next QEMU run should identify the first stage active when the exception occurs. These changes are diagnostic only; they are not a claimed root-cause fix. Once the failing operation is identified, fix it, remove or formalize temporary instrumentation, and verify ISO + native BIOS boot and the CPL3/IPC milestone.
- Verification status for this diagnostic commit: source edits committed; compiler/build/QEMU results pending. Do not mark the boot issue resolved until a new run demonstrates the failing stage and a subsequent fixed run passes.


### Follow-up from Build #5908 (2026-10-14)

- Build #5908 (run ID `38571307558`, job ID `115876927285`) compiled and generated the ISO successfully, then failed during QEMU boot. The new serial marker confirmed execution entered `prosenjitfs_selftest()`; the reported progress value was `0x64` (decimal 100).
- The code sets progress 100 immediately before `block_device_init()` and 101 immediately after it returns successfully. Thus the fault is narrowed to the call/return window around this initializer, but the existing evidence does not yet distinguish a fault in the initializer from stack/return-address corruption at its call boundary.
- Added temporary checkpoints inside `block_device_init()`: 1001 at entry, 1002 after input validation, and 1003 after populating the device structure. This intentionally temporary cross-layer instrumentation is diagnostic only and should be removed after root cause is proven.
- Build #5908 did NOT resolve the boot failure. Next run must show whether the last progress is 1001, 1002, 1003, or 101; use that to isolate the exact instruction/call boundary before changing functional behavior.


### 2026-10-15 — VFS security self-test root cause after Build #6010

- Build #6010 compiled the ISO and reached `security_manager_init()`, then failed with stage `0xFFFFFFFFFFFFF036` (`-4042`).
- The stage mapping in `kernel/intent.c` is `-4000 + rc` for `vfs_security_selftest()`; `kernel/vfs.c` maps `vfs_vnode_api_selftest()` failure to `-42`. This isolates the failure to the VFS vnode API self-test, before the later `prosenjitfs_selftest()` call.
- Root cause: the self-test provider did not model the parent-reference ownership used by the native ProsenjitFS provider. Consequently, the test could deactivate the mount while a child file handle remained open; `vfs_file_close()` then correctly rejected the inactive mount, and the self-test failed with `-42`.
- Fix: the self-test now acquires a parent vnode reference during fixture creation and releases it from the fixture vnode's `release` callback. It drops fixture-owned child references while keeping the open file reference, asserts deactivation returns `PROS_VFS_EBUSY`, confirms the file remains usable, closes it, then successfully deactivates the mount and rejects use of the closed handle.
- Source commit: `8b398ec13db69dc768ac03e5317bafab7832a781` (`test: model vnode parent ownership in VFS self-test`).
- Verification: the code is committed, but clean build/QEMU verification is pending. The self-hosted runner has not been assigned to the latest workflow job, so no runtime pass is claimed.
- Artifact upload quota failure in Build #6010 is separate from the boot failure.
- The earlier ProsenjitFS/block-device crash remains unverified because Build #6010 stopped during security-manager initialization before reaching that self-test.
