# Current PR #270 implementation status — 2026-11-26

> This is the live handoff record for PR #270. The repository-wide mandatory format is defined in [PULL-REQUEST-IMPLEMENTATION-STATUS-STANDARD.md](PULL-REQUEST-IMPLEMENTATION-STATUS-STANDARD.md). Update this section whenever implementation, blockers, or verification changes.

- PR: [#270 — Security Control Center: Bounded Telemetry Dispatcher Core](https://github.com/prosenjit-Adhikary/ProsenjitOS/pull/270)
- State: OPEN / DRAFT (must remain so until all applicable gates pass)
- Head branch: `security/scc-service-dispatch-core`
- Base branch: `security/scc-u64-timestamp-get-event-guard`
- Documentation update commit: `c29601a1b5341158d07e199c4e1a456f8307b491`
- Previous implementation/CI commits: `966baa00a25e28c8429bc8d3a98961eee050bd3b`, `0337d7f5bc92aa2c7820d313623ccf792061e38b`
- Current exact HEAD at time of this record: `c29601a1b5341158d07e199c4e1a456f8307b491`
- Verification: GitHub Actions for the latest HEAD has not yet been observed to complete. Earlier run #9683 failed at ISO QEMU boot; later run #11616 was pending on HEAD `0337d7f5bc92aa2c7820d313623ccf792061e38b`. Those results do not verify this documentation commit.

## Requirement and implementation matrix

| Requirement | Status | Current evidence / limitation |
|---|---|---|
| Bounded telemetry dispatcher core | IMPLEMENTED (core only) | `user/security/telemetry_service.c/.h`; framing, bounded response construction, deterministic initialization, backend validation and caller-provided authorization decision. This is a host-testable core, not a trusted production broker. |
| Parser/dispatcher host tests | IMPLEMENTED; latest exact-HEAD execution PENDING | `tests/security_control_center_service_test.c`, `scripts/test_security_control_center_service.sh`, Makefile target. Earlier run passed the dispatcher and sanitizer steps, but that evidence is for an earlier SHA. |
| ISO/native BIOS QEMU timeout allowance | IMPLEMENTED in workflow | `.github/workflows/build.yml`: QEMU timeout increased from 10s to 90s and job timeout from 6m to 12m. This is not proof that boot succeeds. |
| GitHub-hosted build runner | IMPLEMENTED in workflow | Workflow now selects `ubuntu-latest` and installs NASM, GRUB, xorriso, mtools and QEMU. Exact-HEAD workflow result is still required. |
| Native kernel IPC broker integration | NOT IMPLEMENTED | Existing IPC primitives exist, but no verified SCC broker endpoint/request path is wired to them. |
| OS-authenticated trusted service identity | NOT IMPLEMENTED | Caller-supplied authorization boolean is not peer authentication. No verified kernel-provisioned broker identity binding or per-request identity enforcement exists. |
| Native userspace broker spawn/lifecycle | NOT IMPLEMENTED | Existing process creation primitives do not establish a complete native spawn/exec, broker publication, restart and cleanup lifecycle. |
| Real kernel telemetry provider connection | NOT IMPLEMENTED | Kernel health/security-monitor interfaces exist, but dispatcher backend callbacks are not wired to them as a production service. Test backend values must never be presented as live telemetry. |
| SCC UI integration | NOT IMPLEMENTED | No verified UI client integration or truthful unavailable/unsupported/error state mapping is connected to this dispatcher. |
| Full concurrency, timeout, cancellation and crash recovery | PARTIAL / NOT IMPLEMENTED | Contract is documented; production broker transport/lifecycle and runtime regression evidence are missing. |
| Security and boot regression on latest HEAD | PENDING | Requires the latest-head Actions result and QEMU ISO + native BIOS boot evidence. |

## QEMU investigation status

- Earlier workflow run #9683 (run ID `41599917785`) built the ISO successfully; host tests, parser/dispatcher tests and sanitizer tests passed in that run. The ISO QEMU boot step failed.
- The log showed storage volume discovery/self-test progress and then no expected ProsenjitFS self-test completion marker before the 10-second QEMU timeout. The available log did not identify a precise failing filesystem operation or prove a CPU exception.
- Therefore the exact root cause remains **UNCONFIRMED**. A 10-second timeout may have been too short for the current boot self-test workload; this is a hypothesis, not a proven diagnosis.
- Workflow changes now allow 90 seconds for both ISO and native BIOS QEMU invocations. The next run must determine whether boot completes or expose a reproducible failure. Do not weaken or skip the self-test, remove required markers, or treat a timeout increase as a root-cause fix.
- Do not claim BIOS verification until the native BIOS QEMU step actually passes.

## Current completion gates

- [x] Record bounded dispatcher implementation and its actual limitations.
- [x] Increase boot-test execution allowance without removing test assertions.
- [x] Move workflow to a GitHub-hosted runner and declare required build/QEMU packages.
- [ ] Observe passing exact-HEAD build and host security/parser/dispatcher tests.
- [ ] Reproduce and resolve the QEMU boot failure; pass ISO boot.
- [ ] Pass native BIOS boot.
- [ ] Implement native IPC integration and bounded request handling.
- [ ] Implement OS-provisioned service identity and per-request authorization.
- [ ] Implement native broker spawn, registration, availability, shutdown, restart and cleanup.
- [ ] Connect real supported telemetry providers with strict field allow-lists and truthful unsupported/unavailable states.
- [ ] Integrate the existing SCC UI without duplicate components.
- [ ] Add and execute applicable negative, concurrency, timeout/cancellation, crash/restart and resource-cleanup tests.
- [ ] Verify all applicable required checks on the exact latest PR HEAD.
- [ ] Confirm no unrelated merged functionality regressed.

**Current decision: NOT COMPLETE / NOT MERGE-READY.** The implemented dispatcher core and workflow adjustments are partial milestones only. Keep PR #270 open and draft. No automatic merge.

## Next safe actions

1. Inspect the latest workflow run on the current HEAD and capture the full QEMU log.
2. If QEMU still fails, add narrowly scoped progress/diagnostic markers around the ProsenjitFS self-test stages and reproduce; do not infer the failing operation from a missing final marker.
3. Fix only a proven root cause and add a regression test.
4. Continue with the existing native process/IPC/security architecture contracts; do not add a caller-controlled identity flag or fake broker.
5. Update this record in every implementation milestone, including exact SHA and test evidence.

---

# Security Control Center Broker Lifecycle Contract (Proposal)

Status: design-only. This contract does not claim a broker executable, process spawn API, service registry, or endpoint authentication exists.

## Purpose and trust boundary

The Security Control Center must query security telemetry through a trusted, least-privilege userspace broker. The UI is an unprivileged client and is never itself the authority for security decisions. Kernel-private health/security-monitor functions must not be called directly from UI code or exposed by serializing internal kernel structures.

Related prerequisites:
- `docs/NATIVE-SPAWN-CONTRACT.md` — native process construction, publication and rollback.
- `docs/SECURITY-CONTROL-CENTER-IPC-PROTOCOL.md` — bounded read-only wire proposal.
- `kernel/include/ipc.h`, `kernel/ipc.c`, `kernel/syscall.c`, `kernel/usercopy.c` — existing transport and copy boundaries.

## Implementation update (2026-10-27)

A host-testable dispatcher core now exists in `user/security/telemetry_service.c` and `.h`, with a sanitizer-backed host test runner at `scripts/test_security_control_center_service.sh` and the `security-service-test` Makefile target. The dispatcher performs bounded framing/response construction, enforces a caller-supplied authorization decision, and rejects malformed backend event lengths/sequences.

This is **not** the trusted userspace service described by this lifecycle contract. The authorization input is a boolean supplied by its caller; the dispatcher does not authenticate a peer. Backend callbacks are not yet wired to `pros_health_snapshot()` / `pros_health_get_event()`, and no broker process is launched or registered. Kernel IPC, OS-controlled endpoint identity, native process spawn, lifecycle rollback/restart, and UI client integration remain unimplemented. The CI workflow has been updated to run the host test, but a passing run for the exact commit must be observed before treating that gate as satisfied.

Do not describe this dispatcher as active protection or live telemetry. The UI must remain unavailable/error until the trusted launch and endpoint identity contract is implemented and verified.

## Current source status

The current repository has kernel-side health/security-monitor C interfaces and bounded IPC primitives, but no dedicated public security telemetry query syscall. The public syscall ABI does not establish a general spawn/exec route. The inspected boot path's CPL3/IPC smoke test is not proof of a production userspace broker launch. Kernel service-manager callbacks are not equivalent to an authenticated userspace service manager. Treat these as blockers, not assumptions.

## Required lifecycle states

Use an explicit state machine; names are proposed and must be mapped to existing project conventions before implementation:

`ABSENT -> STARTING -> READY`
`STARTING -> FAILED`
`READY -> STOPPING -> ABSENT`
`READY -> FAILED`
`FAILED -> STARTING` only under an explicit bounded restart policy.

- **ABSENT:** no broker endpoint is advertised.
- **STARTING:** executable admission, process construction, initial CPL3 thread and endpoint setup are in progress; clients cannot query yet.
- **READY:** broker has completed self-checks, endpoint identity and rights are provisioned, and a versioned status query succeeds.
- **STOPPING:** endpoint is withdrawn before resources are torn down; new requests fail explicitly.
- **FAILED:** endpoint is unavailable; failure reason is safely recorded and clients show unavailable/error, never an empty healthy result.

A transition to READY is the commit point. It must happen only after process publication, endpoint provisioning, and a successful protocol handshake. Never advertise a half-created broker.

## Startup transaction

1. Resolve the broker executable through VFS and retain a stable file reference.
2. Apply existing executable-admission/security policy before loading.
3. Construct the process using the approved native spawn lifecycle. Do not expose it as runnable until ELF, user stack, owned kernel stack, initial CPL3 entry frame, capability policy and cleanup ownership are valid.
4. Provision a fresh IPC endpoint/channel and minimum rights. Do not reuse a UI-owned handle as proof of broker identity.
5. Bind the endpoint to an OS-controlled service identity or equivalent unforgeable process/service reference. A numeric PID, process name, or possession of a copied handle alone is insufficient.
6. Perform a versioned bounded handshake. Validate response framing and expected service identity.
7. Publish READY atomically only after all prior steps succeed.

Every step needs a rollback action. Any failure before READY withdraws the endpoint, closes created handles, releases VFS/image references, removes unpublished process/thread state, and leaves no runnable orphan. Rollback must respect the native spawn contract's ownership/refcount invariants.

## Request authorization

- Authenticate the peer at the service boundary using OS-provisioned identity, not a field supplied by the caller.
- Authorize each operation independently. Version 1 is read-only; reject unknown opcodes and any mutation request.
- Keep UI/client rights read-only. Do not grant ADMIN or unrestricted handle transfer to the broker or UI without a separate threat model.
- Bound request count, payload size, work per request and event batch size. Avoid blocking indefinitely on a full queue or absent peer.
- Return explicit DENIED, UNAVAILABLE, STALE, INVALID_ARGUMENT or INTERNAL_ERROR outcomes without leaking protected process/resource existence.
- Rate-limit or otherwise bound repeated malformed requests so a client cannot starve the broker.

## Shutdown, restart and failure

1. Withdraw endpoint publication before destroying the channel or process.
2. Stop accepting new requests and define whether in-flight requests drain or fail; never leave this implicit.
3. Terminate/reap the broker using the implemented process lifecycle; do not bypass thread/address-space/object reference cleanup.
4. Record a bounded failure event where the health-event mechanism permits it.
5. Restart only under a bounded policy with backoff and a retry ceiling. Repeated failures must not create an unbounded process/channel leak loop.
6. Reset event cursors safely across broker epochs. Until epoch identity is part of a reviewed protocol, clients must treat restart cursors as STALE and restart pagination.
7. On missing, failed, or unresponsive broker, UI state is Unavailable/Stale/Error—not “No threats” and not “Protected.”

Broker failure must never weaken enforcement or convert a denied action into an allow. Telemetry is diagnostic; security policy remains authoritative in the kernel or its approved policy service.

## Trusted endpoint identity and publication contract

The current IPC object handle is a capability to a channel, not proof that the peer is the approved Security Control Center broker. Do not treat a channel name, PID/TID, request ID, self-reported service name, or possession of a copied handle as sufficient identity.

Before a userspace broker can be considered trusted, the implementation must define and test all of the following:

1. **Kernel-controlled identity:** broker identity is assigned by a trusted launch/provisioning path, not supplied by the broker itself. Until an authenticated launch path exists, the endpoint remains untrusted and telemetry must be unavailable to privileged consumers.
2. **Atomic endpoint publication:** create the endpoint privately; establish owner identity and least-privilege rights; validate protocol version and required capabilities; only then publish it to authorized clients. There must be no discoverable half-initialized endpoint.
3. **Least privilege:** the broker receives only the telemetry-read capability and endpoint rights it needs. It must not gain process creation, filesystem mutation, device control, or policy-change authority merely because it serves the UI.
4. **Caller authorization:** each request is checked against the caller's kernel-authenticated process identity and endpoint rights. A request's request_id, payload fields, process name, or claimed role is never an authorization token.
5. **Restart and stale references:** endpoint generation/epoch changes on restart; old handles and cursors fail closed as stale. Do not silently bind an old client to a new broker instance.
6. **Failure cleanup:** failed startup withdraws any unpublished/published endpoint, closes handles, releases references, and leaves the UI in Unavailable/Error. Shutdown first stops new requests, then drains/cancels outstanding work, withdraws the endpoint, and releases resources in a defined order.
7. **Auditable denial:** unauthorized and malformed requests receive bounded generic errors. Do not reveal hidden process existence, privileged object identity, or event content in error details.

No current source path has yet been verified to provide all these guarantees. In particular, the kernel service-manager callback/dependency facility is not itself a userspace launcher or authenticated service registry. Do not implement an identity check by comparing a self-reported PID/name, and do not add a public syscall or relax the current ABI as a shortcut. A future endpoint API requires a separately reviewed kernel ABI, launch-path integration, handle-rights policy, and negative tests.

## Concurrency and ownership requirements

- Define synchronization for endpoint publication/withdrawal, request handling, restart and process exit before SMP or concurrent clients are supported.
- Audit current IPC channel lock/waiter behavior and user-copy fault-recovery state; existing single-CPU assumptions are not proof of SMP safety.
- Assign one explicit owner to each endpoint, channel reference, process/thread object, VFS reference and queued request. Document transfer of ownership and cleanup order.
- Define behavior for client exit during a request, broker exit during send/receive, queue saturation, stale handles and failed copy-out.
- Do not claim reliable delivery/retry until syscall-level copy semantics and dequeue timing are covered by tests.

## Verification gates

Before implementation is accepted:
1. Unit-test state transitions and every startup rollback point.
2. Prove no endpoint is discoverable before READY and none remains after STOPPING/FAILED.
3. Negative tests for forged identity, copied/stale handle, wrong object type, attenuated rights, unauthorized opcode, malformed frame, queue saturation and client/broker crash.
4. Test restart/epoch cursor behavior and prove failure cannot appear as an empty successful event list.
5. Add host parser tests, then QEMU tests proving an authorized client reads a bounded snapshot and an unauthorized process is denied.
6. Test failure cleanup for process, thread/kernel stack, address space, handle and VFS references using the native spawn contract.
7. Require exact-commit build, ISO QEMU boot, native BIOS QEMU boot, security checks and review before merge.

## Non-goals

This contract does not implement a broker, general spawn syscall, service registry, UI, persistent event store, or enforcement controls. Those require separate reviewed implementation changes. Documentation is not runtime security.


## Telemetry source readiness gate
The health ring's dropped counter is a saturating `uint32_t`: when the ring is full, `pros_health_record()` stops incrementing at `UINT32_MAX`. `pros_health_selftest()` exercises the `UINT32_MAX - 1` boundary with two additional records and reinitializes telemetry after a successful boundary test. `kernel/kernel.c` now invokes it fail-closed before the initial health sample, and the build workflow requires the success marker in both QEMU boot paths. This wiring is source-inspected but still requires exact-head compile/runtime verification. The wire protocol's `u64` drop-count field must be zero-extended from the current 32-bit source and must not imply more precision. Define reset/reboot and broker-epoch semantics before promising lifetime-monotonic counts. Separately, the security monitor records a decision summary rather than a one-to-one original security event; the broker must not claim full event fidelity until the field mapping is tested. The proposed 20-byte wire event is not a direct cast of the internal health-event C structure.
