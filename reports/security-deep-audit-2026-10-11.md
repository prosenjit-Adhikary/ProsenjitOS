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

## Verification still required

- Run the repository build with the configured `-Wall -Wextra -Werror -O2` flags.
- Run all security, process, capability, VFS, ELF, and kernel self-tests.
- Verify the self-hosted build/boot workflow result; GitHub-hosted runner assumptions must not be substituted for the repository's self-hosted runner.
- On Debian, run the ISO under QEMU and inspect serial output and exception logs.
- Continue auditing concurrency/lifetime rules, cryptographic implementation against independent test vectors, syscall authorization call paths, and cross-subsystem integration.

## Status

The five source-level issues above have been patched on this branch. This is not a claim that all security bugs are eliminated or that the security subsystem is fully verified.