# ProsenjitOS Security Deep Source Audit — 2026-10-11

## Scope and verification boundary

This audit reviewed the current `main` security implementation and made changes on branch `security/deep-audit-20261011`. The changes below are source-level fixes with regression assertions added where practical. A clean build, complete self-test run, and QEMU runtime verification have **not** been performed in this audit session; those results must not be inferred from this report.

## Findings fixed

### 1. Capability handle high-bit aliasing

**Area:** `kernel/object.c`, `kernel/process.c`

Handle encoding uses an 8-bit slot and 16-bit generation (bits 0–23), but the original lookup paths truncated high bits instead of rejecting non-canonical 64-bit values. Distinct attacker-controlled values could therefore alias a live handle. Added a canonical-encoding check to handle check, close, duplicate, copy-transfer, and move-transfer operations. Added a process self-test that attempts a high-bit forged alias and confirms it cannot check, duplicate, close, or transfer the real handle. A second truncation path was found in `process_handle_type()`: it parsed slot/generation directly without checking canonical encoding, which could make syscall pre-validation accept a forged alias. It now delegates validity checking to `handle_check()` before returning an object type.

### 2. Execution admission could return ALLOW for a quarantined process

**Area:** `kernel/security_exec.c`

The original admission path rejected only DEAD/ZOMBIE processes. An already quarantined process in BLOCKED state could present a later ALLOW record and receive `PROS_SECURITY_EXEC_ALLOW`. Admission now requires READY/RUNNING state and rejects a process whose quarantine flag remains set. Added a regression assertion for the ALLOW-after-quarantine bypass attempt.

### 3. Signature admission could override local deny policy and mislabel trust

**Area:** `kernel/security_admission.c`

The original verified-signature path cleared `local_denied` and converted successful cryptographic verification into local trust, returning TRUSTED rather than preserving the VERIFIED evidence type. The invalid-signature path also cleared a local deny. The signed path now preserves deny policy for both valid and invalid signatures, uses the verified signature evidence directly in the policy evaluator, clears caller-supplied `local_trusted` metadata so it cannot relabel a signed result as TRUSTED, and keeps VERIFIED distinct from TRUSTED. Added regression assertions for caller-trust/signature separation plus valid-signature-plus-deny and invalid-signature-plus-deny cases.

### 4. Authorization did not fail closed before hardening initialization

**Area:** `kernel/intent.c`

The original authorization path skipped process-hardening checks when hardening was not ready. Authorization now denies until the hardening contract is ready and validates process invariants on every authorization request.

### 5. Intent object-type truncation

**Area:** `kernel/intent.c`

Authorization cast the 32-bit intent object type to 16 bits without first rejecting high bits. This could let malformed values alias a real object type. Intent validation now rejects out-of-range operation values and object types greater than `UINT16_MAX` before the cast.

### 6. IPC receive could report a blocked syscall without blocking

**Area:** `kernel/syscall.c`, `kernel/ipc.c`, `kernel/thread.c`

The receive path first observes an empty channel, then registers its waiter. The channel wait routine deliberately returns success without blocking if a sender has published a message in that interval. The syscall path previously treated every successful wait as a real block and returned the internal-block sentinel even when the thread remained RUNNING. This could strand the receive operation or make syscall return handling inconsistent. The syscall now checks the thread state after wait registration: if it did not transition to BLOCKED, it retries receive; it only sets the blocked-syscall continuation after an actual block transition. This is a source-level race fix; it still needs compile and runtime validation.

### 7. Ed25519 negative-test coverage strengthened

**Area:** `kernel/security_signature.c`

Added self-test cases that require rejection of (a) a signature with `S = L`, the group order, which is non-canonical, and (b) the encoded identity point as the public key, which is small-order. These complement the existing RFC 8032 positive vector and mutated-`R` negative case. This is test coverage only; it does not establish complete correctness of the field/scalar/point arithmetic or replace independent fuzzing and broader RFC test vectors.

### 8. SHA-512 API invalid-pointer guards and known-answer coverage

**Area:** `kernel/sha512.c`

The public void-returning SHA-512 helpers previously passed a null digest through to finalization, and a non-zero length with a null input silently hashed as if no input were supplied. The helpers now return without processing when the digest is null, when the prefix is null, or when a non-zero input length is paired with a null input pointer. The SHA-512 self-test now includes the standard empty-message known-answer vector alongside the existing `abc` vector.

### 9. Corrected Ed25519 scalar-order regression vector

During review, the new `S = L` negative-test constant was found to be missing the most significant `0x10` byte. That would not have represented the Ed25519 group order. The test constant has been corrected to the exact 32-byte little-endian value for `L`. The test still requires runtime execution.

### 10. Handle-table/object lifetime concurrency remains an open high-priority finding

**Area:** `kernel/object.c`, `kernel/include/object.h`, `kernel/process.c`, `kernel/syscall.c`, `kernel/ipc.c`

The follow-up source review found that handle-table entries and object reference counts are accessed without a handle-table/object lock. Syscall paths can validate a handle and then later read `entry->object` directly; if another thread in the same process closes or transfers that handle between those operations, validation and use are not one atomic lifetime operation. The current `refcount` increment/decrement is also non-atomic. IPC channel validity/count fields are read before acquiring the channel lock.

This is a **source-level concurrency risk**, not a reproduced exploit in this session. It needs a coordinated design: table locking with non-recursive internal helpers, an object-acquire/pin API that retains the object while a syscall uses it, and audited lock ordering for transfer operations. Simply adding a lock to individual public handle functions would not fix direct-entry syscall use and could deadlock nested duplicate/transfer calls, so no superficial partial lock patch was applied. This item remains a blocker for calling the capability/object lifetime layer deeply verified.

## Verification still required

- Run the repository build with the configured `-Wall -Wextra -Werror -O2` flags.
- Run all security, process, capability, VFS, ELF, and kernel self-tests.
- Verify the self-hosted build/boot workflow result; GitHub-hosted runner assumptions must not be substituted for the repository's self-hosted runner.
- On Debian, run the ISO under QEMU and inspect serial output and exception logs.
- Add deterministic regression coverage for the IPC empty→publish→wait race, then run the full build and self-test suite.
- Execute the expanded Ed25519 negative tests and compare field/scalar/point arithmetic against independent known-answer vectors.\n- Execute SHA-512 empty-message, `abc`, and 111/112/127/128/129-byte repeated-`a` known-answer tests; these boundary vectors are now in the self-test but have not yet been run in a build/runtime.\n- Continue auditing handle/object lifetime and concurrency rules, cryptographic implementation against independent test vectors, syscall authorization call paths, and cross-subsystem integration.

## Status

Source-level fixes and additional regression tests are committed on this branch. The handle-table/object lifetime concurrency finding remains open and high priority; build, self-test, and runtime verification are still pending. This is not a claim that all security bugs are eliminated or that the security subsystem is fully verified.

### Concurrency remediation plan (source audit follow-up)

The direct-use paths were checked in the current branch. In `kernel/syscall.c`, IPC send dereferences `process->handles.entries[slot].object` at the send path and passes it to `ipc_channel_send()`; IPC receive repeats the same lookup and keeps the entry pointer across receive/wait/retry. The initial `process_handle_type()` validation does not pin that object, so the later lookup is not a lifetime guarantee. The IPC receive path may block while holding this raw entry pointer, making close/reuse/object teardown during the wait especially important to address.

The IPC channel's own spinlock protects queue and waiter mutations, but `ipc_channel_valid()` currently reads `object.type`, `object.refcount`, `head`, `tail`, and `count` before the channel lock is acquired; `ipc_channel_count()` reads `count` without taking that lock. Moving queue validation under the lock is necessary, but by itself is not sufficient until callers hold a stable object reference before attempting to acquire the channel lock.

Required implementation sequence before this finding can be closed:

1. Add a per-handle-table lock and split object-layer operations into public locking wrappers and private `*_locked` helpers. Never call a public wrapper recursively while already holding the same non-recursive lock.
2. Add an object pin/acquire API that, under the table lock, validates canonical handle, generation, rights, and type, then safely increments a reference before releasing the table lock. Add a matching release path on every syscall exit, including error and blocked-continuation paths.
3. Rewrite IPC syscalls to use the pinned object pointer, not a raw handle-table entry pointer. For a blocked receive, define how the pin is retained or re-acquired on continuation and how process teardown cancels the waiter; do not retain a stack-local raw entry pointer across scheduling.
4. Make copy/move transfers atomic across source and target tables with a documented lock order (for example, stable address order), and ensure move consumes the source only after target allocation succeeds while both tables remain protected.
5. Make object reference updates race-safe under a single documented synchronization rule. Do not mix atomic and non-atomic accesses to the same refcount.
6. Move IPC mutable-field checks into a locked helper and lock `ipc_channel_count()`. Ensure channel lifetime is pinned before taking its channel lock, and document lock ordering between handle-table/object and channel locks.
7. Add deterministic tests for close-versus-use, duplicate-versus-close, transfer-versus-close, object final release versus IPC send/receive, and empty→publish→wait scheduling. Run these repeatedly under the native self-test harness and the normal warning-as-error build.

No locking change is recorded as complete in this follow-up: this sequence spans handle allocation, close-all teardown, duplication, cross-process transfer, syscall continuations, and IPC object lifetime. Applying only a lock to individual functions would leave a use-after-release window or introduce nested-lock deadlocks. This remains a high-priority release blocker until implemented and tested.
