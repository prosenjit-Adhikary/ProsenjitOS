# Security Control Center Broker — Implementation Plan

Status: planning artifact only. No broker executable, trusted service registry, endpoint authentication, serializer, or kernel IPC integration is implemented by this document. Keep the Security Control Center UI in an explicit Unavailable/Not implemented state until the gates below are met.

Last reviewed: 2026-10-26
Related documents:
- `SECURITY-CONTROL-CENTER-IPC-PROTOCOL.md`
- `SECURITY-CONTROL-CENTER-SERVICE-LIFECYCLE.md`
- `SECURITY-CONTROL-CENTER-TELEMETRY-SOURCE-MAP.md`
- `SECURITY-CONTROL-CENTER-TEST-MATRIX.md`
- `NATIVE-SPAWN-CONTRACT.md`

## Architecture decision

Use a least-privilege userspace broker as the only planned bridge between the UI and kernel-private telemetry. The UI is an untrusted read-only client. The broker must not become an authority for enforcement decisions; kernel or approved policy services remain authoritative. Do not add a dedicated syscall merely to make the UI display something.

The protocol parser in `user/security/telemetry_protocol.c` is a pure host-side framing/shape validator. It is not a transport, serializer, authentication mechanism, service registry, or proof of trustworthy telemetry.

Current source constraints that block implementation:
- `include/abi/syscall.h` exposes syscall IDs 1–7 (version, test completion, handle operations, IPC send/receive/transfer); no public spawn/exec or security telemetry query syscall is declared.
- `kernel/include/ipc.h` defines a 64-byte message maximum and eight queue slots.
- `kernel/include/object.h` defines generation-tagged handles and rights, but channel-handle possession is not proof of the approved peer identity.
- `kernel/include/health.h` exposes kernel C APIs and an internal event record; it is not a public userspace endpoint.
- `docs/NATIVE-SPAWN-CONTRACT.md` documents missing/general native process-launch requirements; do not assume `process_create()` or init loading is a production userspace spawn route.

## Non-negotiable trust invariants

1. **Fail closed:** if identity, endpoint rights, source snapshot, protocol validation, or required dependencies cannot be verified, report Unavailable/Degraded/Error. Never translate failure into “no threats” or “protected”.
2. **OS-established identity:** never trust a PID, process name, request field, endpoint name, or self-asserted service ID as authentication. Bind the endpoint to an unforgeable kernel-owned process/service reference established by the launch path.
3. **Atomic publication:** create and configure the endpoint privately; establish identity and least-privilege rights; validate protocol handshake; only then publish it to clients. No partially initialized endpoint may be discoverable.
4. **Least privilege:** UI receives query-only access. No mutation opcodes in protocol v1, no implicit privileged-handle inheritance, and no unrestricted handle transfer.
5. **Bounded work:** fixed 64-byte transport, bounded queues, bounded per-request processing, explicit malformed-request policy, and bounded restart/backoff. Avoid unbounded allocation and blocking.
6. **No unsafe serialization:** build fields byte-by-byte in little-endian order. Never cast or copy a kernel struct to the wire. Explicitly map internal enums and 32-bit source IDs to reviewed public values.
7. **Consistent snapshot semantics:** define locking/sequence/counter epoch rules before exposing concurrent telemetry. A 64-bit wire drop counter currently zero-extends the kernel's saturating 32-bit counter; it does not add precision.
8. **Auditable failure:** authorization denial, unavailable source, stale cursor, overflow, startup failure and shutdown are distinguishable internally without leaking protected resource existence to an unauthorized client.

## Source-audit finding: IPC receive is not retry-safe on copy-out failure

Source inspected on this branch:
- `kernel/syscall.c`, `PROSENJIT_SYS_IPC_RECEIVE` case
- `kernel/ipc.c`, `ipc_channel_receive()`
- `kernel/usercopy.c`, `copy_to_user()`

Observed ordering:
1. The syscall validates the destination ranges and IPC handle/READ authorization.
2. `ipc_channel_receive()` copies the queued message into a kernel-local buffer, sets `length_out`, advances `head`, decrements `count`, and unlocks the channel.
3. Only after dequeue, the syscall copies the message bytes to the user buffer, then copies the length to `length_out`.
4. If either `copy_to_user()` fails, the syscall returns an error but the queued message has already been consumed. Range prevalidation does not guarantee the later copy cannot fault.

Impact: a client must not assume a failed receive is safe to retry or that the message remains queued. For a future broker, this can lose a request/response and create ambiguous timeout behavior. This is a documented existing limitation, not a claim of exploitable memory corruption.

Do not patch this by casually splitting receive into peek and commit. A safe change needs a reviewed delivery contract and concurrency tests, including:
- failed message-buffer copy-out does not consume the message, if that is the chosen contract;
- failed length-out copy does not consume the message, if that is the chosen contract;
- queue head/count remain consistent under concurrent sender/receiver and waiter wake/cancel paths;
- too-small capacity rejects without dequeue (currently the lower-level function checks size before copying/dequeueing);
- exactly one successful receive consumes exactly one message;
- fault-injection exercises both copy-out failure points.

Preferred design to review: reserve/peek a message under a defined channel transaction, copy to userspace without holding a spinlock across faultable memory access, and commit dequeue only after both output copies succeed; add a reservation token/generation or serialize receivers so a second receiver cannot consume the same head during copy-out. The current channel implementation has no such transaction token. Do not implement this design until locking, blocking, waiter and interrupt interactions are audited.

## Source-audit finding: handles attenuate rights but do not authenticate service identity

Source inspected on this branch:
- `kernel/object.c`: `handle_duplicate()`, `handle_transfer_copy()`, and `handle_transfer_move()`
- `kernel/syscall.c`: `PROSENJIT_SYS_HANDLE_TRANSFER` case
- `kernel/include/object.h`: generation-tagged handle entries with a rights mask

Observed safeguards:
- Handle duplication requires the source's DUPLICATE right and rejects requested rights not contained in the source rights.
- Copy/move transfer requires TRANSFER rights and similarly rejects rights amplification.
- The syscall resolves a target process by PID and authorizes the transfer intent before copying/moving the handle.

Security implication: these are useful capability attenuation checks, but they do not prove that the target PID is the approved Security Control Center service, nor that a transferred IPC endpoint remains bound to a kernel-established service identity. The future provisioning path must separately authorize the destination service/process and bind identity to the endpoint object. Do not infer authentication from a valid generation-tagged handle or from successful rights reduction.

Required tests: rights amplification is rejected; duplication without DUPLICATE is rejected; transfer without TRANSFER is rejected; stale generation is rejected; authorized transfer to an untrusted target does not gain broker identity; only the approved launcher can provision/publish the trusted endpoint.

## Dependency-ordered milestones

### M0 — Freeze contracts and prove existing boundaries (current)

- [x] Document bounded protocol parser and tests; exact-head CI still needs a result.
- [x] Document native spawn and service lifecycle as contracts rather than implemented capabilities.
- [x] Define 24-byte event wire record, 64-bit boot-relative tick timestamp, one-event pagination, and GET_EVENT request/response sequence binding on the current draft branch.
- [x] Audit the syscall receive path and document copy-out/dequeue failure semantics: the current implementation consumes the message before both user copies complete; retry is not guaranteed.
- [x] Audit handle duplication/transfer: rights are attenuated, but target PID/handle possession does not establish trusted broker identity.
- [ ] Audit IPC queue ownership and lock/waiter concurrency from source and tests.
- [ ] Reconcile all docs against the current 24-byte record; no stale 20-byte descriptions.
- Exit gate: source paths and tests are recorded; no unsupported end-to-end claim; exact-head CI green.

### M1 — Specify identity, capability and publication primitives (design/review only)

- Define the trusted launcher identity token as a kernel-owned object/reference, not a user-provided number.
- Define endpoint creation, private provisioning, atomic publication/unpublication, and revocation behavior.
- Define caller authorization for GET_STATUS, LIST_EVENTS and GET_EVENT; explicitly deny all other opcodes.
- Define the process/endpoint lifecycle and rollback ownership table, including partially created process, thread, address space, channel, and handles.
- Define how service identity survives handle duplication/transfer; either prevent unauthorized transfer or preserve verifiable endpoint identity and rights.
- Exit gate: negative threat-model review covers forged identity, copied handles, stale handles/generation wrap, restart races, unauthorized client, and partial startup rollback. No syscall numbers are allocated yet.

### M2 — Establish a viable native launch and IPC service route

- Complete the prerequisite portions of `NATIVE-SPAWN-CONTRACT.md`: non-runnable construction state, validated ELF and bounded user stack, owned kernel stack, correct CPL3 entry frame, least-privilege syscall/handle policy, publication as the final commit point, and complete rollback.
- Do not treat init-image loading, a kernel service-manager callback, or a boot smoke test as general userspace process spawning.
- Prove syscall copy-in/copy-out failure semantics and whether receive dequeues before copy-out succeeds. Add regression tests before relying on retry or delivery guarantees.
- Add/extend public ABI only after review establishes a real need and compatibility/error semantics. Avoid adding a security-specific telemetry syscall if capability-protected generic IPC can safely meet the requirement.
- Exit gate: a minimal unprivileged test process can be launched, gets only intended handles, exchanges bounded messages, and can fail/exit without leaked runnable threads, handles, channels or address spaces.

### M3 — Implement broker source adapter and wire serializer

- Start with a read-only health snapshot/event adapter. Check every kernel API return value; never fabricate a zero snapshot on failure.
- Map internal severity/category to explicit public enums and source/code to a stable allow-list. Unknown/unrepresentable mappings must be handled explicitly, never truncated.
- Serialize into a zero-initialized 64-byte buffer; use checked little-endian writes and fixed lengths. For event responses, return one 24-byte record and ensure GET_EVENT's record sequence and response cursor equal the requested nonzero sequence.
- Define epoch/reset behavior for event sequences and drop counters. Until epochs are represented and reviewed in the protocol, restart invalidates old cursors and returns STALE/UNAVAILABLE rather than implying continuity.
- Define concurrency/locking for snapshots and ring retrieval. Avoid returning a record from a different source epoch than the cursor.
- Exit gate: serializer unit tests cover every byte offset, all public enum values, unknown internal values, integer boundaries, empty/full ring, stale cursor, source failure, and deterministic initialization. Sanitizer tests pass.

### M4 — Authenticate endpoint and integrate the read-only service

- Provision endpoint identity and query-only rights through the reviewed OS launch path; UI must not discover/trust an endpoint based solely on its name or PID.
- Publish READY only after identity/rights setup and a bounded protocol handshake; unpublish before shutdown/teardown.
- Rate-limit malformed/repeated requests and bound in-flight work. Define queue-full, timeout, peer-death, restart, stale cursor and in-flight shutdown behavior.
- Ensure UI can only display data returned by the authenticated service and clearly labels source and freshness. No mutation buttons are enabled by protocol v1.
- Exit gate: integration tests prove unauthorized callers are denied, copied/forged identity does not work, service restart invalidates cursors, source failure is visible, and UI never reports “No threats” for an error path.

### M5 — Release gate

- Run host unit tests and ASan/UBSan where supported.
- Run exact-head security architecture checks, build, and QEMU/boot workflows; retain run IDs and commit SHA in the work log.
- Test malformed frames, zero sequence, response binding, max lengths, queue saturation, copy-out failure, startup rollback, process exit, endpoint revocation and repeated restart.
- Review documentation and threat model independently. Keep PR draft until required checks pass and reviewers approve; never merge automatically.

## Explicit non-goals for v1

- No quarantine/release/restore, device grant/revoke, firewall/network-policy mutation, update/install, or privilege-changing operation.
- No process command lines, credentials, arbitrary file paths, raw kernel pointers, or packed internal values in event details.
- No claim of persistent audit storage: the current health ring is bounded in-memory telemetry and initialization resets it.
- No claim that telemetry proves the OS is secure or that an empty event page proves absence of threats.

## AI/human continuation protocol

Before each continuation:
1. Read this plan and the work log.
2. Fetch the current PR head SHA and status; never assume a previous SHA remains current.
3. Inspect current source and exact diff before writing. Do not duplicate an existing contract.
4. Make one reviewable, dependency-ordered change; preserve the draft/unmerged state.
5. Record changed files, exact commit SHA, CI run IDs/statuses, tests actually run, and one next action. Distinguish planned, implemented, tested, and runtime-verified.
