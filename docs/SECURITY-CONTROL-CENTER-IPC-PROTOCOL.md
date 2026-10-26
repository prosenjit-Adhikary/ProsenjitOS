# Security Control Center IPC Telemetry Protocol (Proposal)

Status: design proposal only. No public ABI structs, syscall numbers, broker process, or UI client are implemented by this document.

## 1. Constraints and trust boundary

- Current kernel IPC message maximum is 64 bytes; every request and response must fit unless a separately reviewed transport is implemented.
- The public syscall ABI has generic IPC send/receive and handle operations, but no dedicated security-event query operation.
- This wire format is not authentication. A peer is trusted only if the OS provisions its endpoint to an approved service identity with least-privilege rights and validates the caller/endpoint relationship.
- The UI is an unprivileged client. The broker translates kernel-private state into explicit ABI fields; never serialize kernel pointers, native kernel structs, credentials, or unbounded strings.
- Version 1 is read-only: status and event queries only. No mutation opcode.

## 2. Framing proposal

All fields are little-endian fixed-width integers. No compiler bitfields, pointers, implicit padding, or variable-length strings.

### Request header — 16 bytes

| Offset | Field | Type | Rule |
|---:|---|---|---|
| 0 | version | u16 | Must equal 1 |
| 2 | opcode | u16 | Unknown values rejected |
| 4 | request_id | u32 | Correlation ID, not authorization |
| 8 | payload_len | u16 | Exact payload bytes after header |
| 10 | flags | u16 | Must be zero in v1 |
| 12 | reserved | u32 | Must be zero |

Request total length must equal 16 + payload_len and be <= 64 bytes. Reject undersized headers, length mismatch, nonzero reserved/unknown flags, unsupported versions, unknown opcodes, and arithmetic overflow.

### Response header — 24 bytes

| Offset | Field | Type | Rule |
|---:|---|---|---|
| 0 | version | u16 | Supported version |
| 2 | opcode | u16 | Echo accepted operation |
| 4 | request_id | u32 | Echo correlation ID |
| 8 | status | u32 | Explicit protocol result |
| 12 | payload_len | u16 | Exact payload bytes after header |
| 14 | flags | u16 | Must be zero in v1 |
| 16 | event_cursor | u64 | Last returned event sequence; zero if inapplicable |

Response total length must equal 24 + payload_len and be <= 64 bytes. The largest v1 response payload is 40 bytes. Initialize every output byte before populating fields.

### Status values

| Value | Name | Meaning |
|---:|---|---|
| 0 | OK | Operation completed; payload has documented shape |
| 1 | UNSUPPORTED_VERSION | Version not supported |
| 2 | UNSUPPORTED_OPCODE | Operation not implemented |
| 3 | INVALID_ARGUMENT | Length, enum, cursor, or field invalid |
| 4 | DENIED | Caller or capability not authorized |
| 5 | UNAVAILABLE | Broker or telemetry source unavailable |
| 6 | STALE | Requested snapshot/cursor no longer valid |
| 7 | BUFFER_TOO_SMALL | Required output cannot fit; no partial success |
| 8 | INTERNAL_ERROR | Generic failure; do not disclose sensitive internals |

An error must not be represented as an empty successful event list. Existing IPC receive may dequeue before copy-out succeeds; do not assume retry/delivery guarantees.

## 3. Version 1 operations

| Opcode | Operation | Request payload | Response payload |
|---:|---|---|---|
| 1 | GET_STATUS | Empty | 24-byte status snapshot |
| 2 | LIST_EVENTS | 16-byte query | Up to two fixed-size event records |
| 3 | GET_EVENT | u64 sequence | One event record, if it fits |

No permission changes, quarantine actions, device grants, network policy mutations, or update actions are permitted in this version.

### GET_STATUS payload — 24 bytes

- component_id: u16
- state: u16 (UNKNOWN, READY, DEGRADED, UNAVAILABLE)
- capabilities: u32 (known feature bits only; zero means no optional feature advertised)
- dropped_event_count: u64 (zero-extended source count; saturating count for the current boot/source epoch; zero is valid only if the source can establish that no events were dropped)
- latest_event_sequence: u64 (latest source sequence observed in the same snapshot)

The response header's `event_cursor` is reserved for event pagination and must be zero/inapplicable for `GET_STATUS`; the latest status-snapshot sequence is carried only by `latest_event_sequence`. Source status (2026-10-26): `kernel/health.c` stores `dropped_events` as `uint32_t`, saturates it at `UINT32_MAX`, and `pros_health_selftest()` checks the `UINT32_MAX - 1` boundary with two additional records and reinitializes health telemetry after a successful test. `kernel/kernel.c` now invokes this self-test fail-closed during boot, and `.github/workflows/build.yml` requires its success marker in both ISO/QEMU and native BIOS/QEMU logs. This boot integration has been source-inspected but not yet compiled or boot-tested; exact-head CI remains pending. A `u64` wire field is only a zero-extended representation of the current 32-bit source count; it does not imply more source precision. A broker that cannot reliably count drops must return an explicit degraded/unavailable status rather than fabricate zero. This is broker-reported telemetry, not a blanket claim that the OS is secure. UI must label the source and age of the last successful query.

### LIST_EVENTS query — 16 bytes

- after_sequence: u64 (exclusive cursor; zero means earliest retained event)
- category_mask: u32 (zero means all categories authorized to this caller)
- max_records: u16 (1..2)
- reserved: u16 (must be zero)

The two-record cap is deliberate: each event record is 20 bytes, so two records plus the 24-byte response header fit exactly in 64 bytes. If the event schema grows, lower the record count or define a separately reviewed transport/version; never silently exceed 64 bytes.

### Event record — 20 bytes

- sequence: u64 — monotonically increasing within one broker epoch
- timestamp_ticks: u32 — clock domain/units must be defined before implementation
- severity: u8 — fixed enum; unknown values preserved as UNKNOWN
- category: u8 — fixed enum; unknown values preserved as UNKNOWN
- source_id: u16 — stable non-pointer identifier
- reason_code: u32 — stable public reason code, not a raw kernel address

Event details, paths, command lines, credentials, and arbitrary evidence blobs are excluded from v1. A bounded, redacted detail operation requires separate review.

## 4. Cursor and failure semantics

- For `LIST_EVENTS`, response `event_cursor` is the last event sequence actually included, not merely the latest sequence observed. For `GET_STATUS`, it is zero/inapplicable; use `latest_event_sequence` in the status payload.
- If `LIST_EVENTS` returns no records, the response cursor must not advance.
- Sequence numbers must be monotonic within a broker epoch. Restart/epoch semantics must be specified before persistent cursors are promised; invalid cursors return STALE.
- `GET_STATUS.dropped_event_count` reports the source's bounded loss counter. Never imply complete history if the source cannot prove it; zero is not a valid fallback for an unknown count.
- Failed query, missing broker, stale cursor, or malformed reply maps to UI Unavailable/Stale/Error, never No threats.
- Authorization failure must not leak whether another process, hidden event, or protected resource exists.
- request_id is correlation only, not replay protection or identity.

## 5. Required tests before implementation is accepted

### Parser and framing
- Test every message length 0 through 65 and boundaries at 16, 24, and 64 bytes.
- Test version mismatch, unknown opcode/status/enum, nonzero flags/reserved, payload-length mismatch, and integer-overflow attempts.
- Test zero, one, two, and invalid record counts; prove response payload never exceeds 40 bytes.
- Use an uninitialized-output test to prove all response bytes are initialized.

### Authorization and lifecycle
- Test authorized broker/client, untrusted process with guessed/copied handle, wrong object type, stale/closed handle, attenuated rights, and unauthorized endpoint.
- Test broker absent, startup failure/restart, stale cursor, empty/full event ring, overflow, monotonic sequence, and dropped-event accounting.
- Test copy-in/copy-out failure, especially receive consumption before copy-out; document actual behavior.
- Fuzz the parser and run QEMU integration tests before exposing the protocol to UI.

## 6. Implementation gates

1. Review this proposal with docs/NATIVE-SPAWN-CONTRACT.md and existing IPC/user-copy code.
2. Define service identity, endpoint provisioning, and rollback before allowing UI access.
3. Add shared public ABI declarations only after naming, integer types, versioning, and tests are approved.
4. Add host parser tests and negative authorization tests before kernel/QEMU integration.
5. Build the read-only UI adapter only after an end-to-end trusted broker query exists.

Until these gates are satisfied, this is a proposal, not an implementation or security guarantee.