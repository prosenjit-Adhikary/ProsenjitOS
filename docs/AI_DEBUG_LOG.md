# ProsenjitOS AI Debug Log

## 2026-10-12 — Security structure audit
Branch: `security/hardening-audit-20261012`

### Findings / changes
- Fixed invalid `security_quarantine_selftest()` fixture: set process object type, reference count, and PID. This directly addresses the startup self-test path associated with error stage `-11501`.
- Hardened live capability entries and direct object-layer APIs: reject unknown object types, nonzero reserved fields, zero rights, generation zero, undefined rights, and rights beyond process policy; enforce checks in allocation, check, duplicate, and transfer paths; reject retired maximum-generation handles, high-bit aliases of 24-bit handles, and corrupted free slots.
- Added hardening self-test cases for malformed capability metadata; isolated the referenced test object so unknown-type tests reach the intended validation branch rather than short-circuiting on process identity.
- Made `security_authorize()` fail closed until kernel hardening is ready.
- Validate intent object types against the defined ABI object set at creation and authorization; reject undefined rights bits.
- Validate process lifecycle and quarantine-state invariants; initialize reserved process bytes and add a process regression check.
- Restrict `process_set_state()` to scheduler-visible states (`NEW` through `BLOCKED`), prevent an active process from resetting to `NEW`, and reserve terminal `ZOMBIE`/`DEAD` transitions for lifecycle code. This prevents fabricated zombies (without thread teardown), zombie resurrection, and lifecycle-state reset; added regression assertions and adjusted the execution-admission test fixture.
- Harden `security_quarantine()` and `security_clear_quarantine()` against inconsistent quarantine flags and corrupted restore-state metadata. Invalid previous-state values now fail without clearing containment or writing an arbitrary process state; added a regression test for a corrupted restore state.
- Reject corrupted reserved metadata in IPC objects.
- Saturate hardening violation/generation counters rather than allowing wraparound; preserve violation counts across reinitialization instead of erasing earlier fail-closed events.
- Preserved explicit local deny decisions across both valid and invalid signature admission paths and made a valid local deny outrank malformed metadata; added regression cases.
- Corrected signed admission so successful cryptographic verification is recorded as VERIFIED (unless separate local-trust policy applies), not relabeled as TRUSTED.
- Invalid file-admission flags and unknown reputation values now fail closed to UNKNOWN/QUARANTINE, with regression coverage.
- Behavior-monitor resource exhaustion, timestamp rollback, and failed process termination now attempt quarantine containment.
- Added a fail-closed monitor containment fallback: if normal quarantine rejects corrupted quarantine metadata on an otherwise live process, force BLOCKED with a canonical quarantine marker and no restore target; added a regression test proving quarantine clear leaves it BLOCKED.
- Audit-ledger exhaustion no longer clears a quarantine decision; a startup regression self-test forces ledger exhaustion and verifies the process remains quarantined and BLOCKED.
- Added regression coverage ensuring high bits cannot alias a valid capability handle across check, lookup, and duplication paths.
- Removed duplicate `ed25519_scalar.h` include.
- Updated audit report: `reports/security-structure-audit-2026-10-12.md`.

### Verification checklist
- [x] Current GitHub main source inspected.
- [x] Source fixes committed to the audit branch.
- [ ] Clean Debian GCC build (GitHub Actions run queued).
- [ ] QEMU boot reaches completion with all security self-tests passing.
- [ ] Verify stage `-11501` no longer occurs.
- [ ] Verify CI and full regression results (build/boot and security-design workflows remain queued on self-hosted runner).
- [ ] Final report and verified log synchronized to main.

### Important
Do not mark this work complete until build, QEMU, and regression checks pass. The GitHub source edits have not yet been built or boot-tested in the Debian VM.
