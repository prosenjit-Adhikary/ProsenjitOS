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
