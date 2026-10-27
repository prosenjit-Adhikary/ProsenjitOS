# Security Control Center Protocol Test Matrix

Status: test matrix and host parser/test sources exist. Host parser tests, AddressSanitizer/UndefinedBehaviorSanitizer tests, ISO build, ISO QEMU boot, and native BIOS QEMU boot passed on exact head commit `f95206a0897264054e90d1e996f6e82cd4808f13` in [Build and boot-test run #40548193855](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/40548193855). The wire protocol is still only a proposal; trusted broker, authenticated endpoint provisioning, authorization integration, and UI remain unimplemented.

Related design:
- [IPC telemetry protocol](SECURITY-CONTROL-CENTER-IPC-PROTOCOL.md)
- [Telemetry source map](SECURITY-CONTROL-CENTER-TELEMETRY-SOURCE-MAP.md)
- [Service lifecycle contract](SECURITY-CONTROL-CENTER-SERVICE-LIFECYCLE.md)

## 1. Pure parser boundary

The first pure host-testable C framing parser is implemented in user/security/telemetry_protocol.c with its interface in user/security/telemetry_protocol.h. It is free of kernel pointers, global state, allocation, and I/O. It accepts (const uint8_t *bytes, size_t length) and returns a typed parse result plus a view into the caller-owned byte buffer only after complete validation. Null input is rejected for all lengths. The caller must retain the buffer lifetime; the parser must never cast untrusted bytes to a packed C struct.

The parser must:
- Reject a null pointer for any length, including (NULL, 0); the implementation treats a null input as SCC_PARSE_INVALID_ARGUMENT and has a regression test for it.
- Validate minimum header size before reading any field.
- Decode little-endian integers byte-by-byte or through checked helpers; never depend on host endianness or alignment.
- Check header + payload using subtraction-based bounds checks (for example, payload length must equal length - header_size after proving length >= header_size) rather than unchecked addition.
- Require exact total length, supported version/opcode, zero v1 flags/reserved fields, and opcode-specific payload lengths.
- Enforce total length <= 64 bytes, response payload <= 40 bytes, and event record count <= 2.
- Keep syntax validation separate from authorization. A valid frame is not an authorized request.
- Never interpret a malformed/error response as an empty event list or as evidence of “no threats”.

## 2. Request framing vectors

| Case | Input condition | Expected result |
|---|---|---|
| RQ-01 | Length 0 | Reject; no field reads |
| RQ-02 | Length 1..15 | Reject short request header |
| RQ-03 | Length 16, version 1, known opcode with empty payload | Accept only for GET_STATUS |
| RQ-04 | Length 16, LIST_EVENTS opcode | Reject wrong opcode payload size |
| RQ-05 | Length 24, GET_EVENT with 8-byte payload | Accept framing |
| RQ-06 | Declared payload length differs from actual bytes | Reject |
| RQ-07 | Total length 65 or greater | Reject before payload access |
| RQ-08 | Version 0 or unsupported future version | Reject with bounded version error |
| RQ-09 | Unknown opcode | Reject with unsupported-opcode result |
| RQ-10 | Non-zero flags or reserved bits | Reject v1 frame |
| RQ-11 | category_mask arbitrary 32-bit value | Parse framing; separately mask/validate defined categories without overflow |
| RQ-12 | max_records 0 or 3..65535 | Reject |
| RQ-13 | max_records 1 or 2 and reserved zero | Accept framing |
| RQ-14 | Byte buffer deliberately unaligned | Same result as aligned buffer |
| RQ-15 | Fuzzed lengths and random bytes | No out-of-bounds access, UB, crash, or unbounded work |

For GET_STATUS the request payload is exactly 0 bytes; LIST_EVENTS is exactly 16 bytes; GET_EVENT is exactly 8 bytes. A 16-byte request with LIST_EVENTS or GET_EVENT is not valid merely because the header is complete.

## 3. Response framing and payload vectors

| Case | Input condition | Expected result |
|---|---|---|
| RS-01 | Length 0..23 | Reject short response header |
| RS-02 | Length 24, zero payload, valid header | Accept only for an error or an operation whose schema allows empty payload |
| RS-03 | Declared payload differs from actual payload | Reject |
| RS-04 | Length exceeds 64 | Reject |
| RS-05 | Unknown status enum | Treat as protocol error; never silently map to OK |
| RS-06 | Non-zero v1 flags | Reject |
| RS-07 | GET_STATUS success with payload length other than 24 | Reject |
| RS-08 | GET_STATUS response with non-zero event_cursor | Reject; v1 requires the header cursor to be zero and carries latest sequence in the status payload |
| RS-09 | LIST_EVENTS success with 0, 1, or 2 records | Accept only if payload length is exactly count × 20 and count <= requested max |
| RS-10 | LIST_EVENTS payload 41..65535 bytes | Reject; never truncate silently |
| RS-11 | GET_EVENT success with payload length other than 20 | Reject |
| RS-12 | Any error status with success-shaped event data | Reject or ignore payload per final normative rule; never present as successful events |
| RS-13 | Response request_id/opcode mismatch | Reject as unmatched response |
| RS-14 | Output buffer filled with non-zero poison before builder call | Every transmitted byte is initialized deterministically |
| RS-15 | Fuzzed responses and malformed record fields | No out-of-bounds access, UB, crash, or unbounded work |

Version 1 now specifies zero payload and zero event_cursor for every non-OK response; the parser enforces this rule. Any future change requires a protocol version/review and updated tests.

## 4. Size arithmetic invariants

- Request header: 16 bytes; response header: 24 bytes; event record: 20 bytes.
- GET_STATUS success payload: 24 bytes (component_id u16, state u16, capabilities u32, dropped_event_count u64, latest_event_sequence u64); total response 48 bytes.
- LIST_EVENTS query: 16 bytes; total request 32 bytes.
- LIST_EVENTS response with one event: 44 bytes; with two events: 64 bytes exactly.
- GET_EVENT request: 24 bytes total; successful response with one event: 44 bytes.
- No operation may exceed the 64-byte IPC message limit.
- Compile-time assertions should cover only local, fixed ABI constants; they must not depend on native struct packing.

## 5. Cursor and telemetry invariants

- LIST_EVENTS cursor advances only to the last event actually included in a successful response.
- Empty page, error response, malformed reply, or denied request never advances the cursor.
- Event sequences are strictly increasing within a documented broker epoch; duplicates or regressions are rejected or surfaced as STALE.
- Counter values are monotonic only within the source's defined epoch. Current source drop count is uint32_t and saturates at UINT32_MAX; a u64 wire representation is zero-extended, not additional precision.
- A saturated drop counter must be displayed as “at least UINT32_MAX dropped” or equivalent, not as an exact lifetime total.
- If the broker cannot determine the drop count or sequence epoch, it returns DEGRADED/UNAVAILABLE rather than zero or an apparently complete history.
- The timestamp clock domain, tick frequency, wrap policy, and epoch must be defined before timestamps are rendered as wall-clock times.

## 6. Authorization and lifecycle negative tests

These tests cannot be proven by the pure parser; they require a kernel-enforced endpoint and identity mechanism:
- Untrusted process with guessed endpoint identifier.
- Client with copied/duplicated handle but missing required rights.
- Wrong kernel object type, closed handle, stale generation, or endpoint restarted during request.
- Broker claims another PID/name/role in payload.
- Caller changes identity between validation and use (TOCTOU).
- Endpoint visible before owner, rights, protocol version, and capabilities are finalized.
- Broker crashes after receive but before response; client timeout and restart behavior.
- Unauthorized query must not reveal hidden process/event existence through payload or differentiated detailed errors.
- Request replay or duplicate request IDs; request IDs are correlation only and must not grant authority.
- Handle close, endpoint withdrawal, and outstanding request cleanup on partial startup failure.

All authorization tests must fail closed. Do not simulate trusted identity by trusting a PID/name string in a host-only test.

## 7. IPC copy-out and delivery semantics

Current source audit indicates the syscall receive path can dequeue into a kernel buffer before attempting user-space copy-out. A fault during copy-out can therefore consume a message. Before claiming retry-safe or exactly-once delivery:
1. Define whether receive is destructive or supports peek/commit.
2. If introducing peek/commit, bind the commit to the exact channel/message generation so concurrent receivers cannot consume a different message.
3. Preserve the message on copy-out failure, or explicitly document destructive semantics if the architecture cannot safely preserve it.
4. Test invalid user pointer, read-only destination, page-boundary crossing, undersized destination, concurrent receivers, closed channel, and queue-full sender.
5. Verify all error paths preserve queue counts/head/tail and reference counts.

Do not modify the kernel receive path until the delivery contract and concurrency behavior are reviewed.

## 8. Acceptance gates

1. Implement pure parser + serializer and host tests before kernel integration.
2. Run tests with warnings-as-errors, address/undefined-behavior sanitizers where supported, and a fuzz target with a bounded time/memory budget.
3. Review the normative error-payload rule, status semantics, timestamp domain, and broker epoch semantics.
4. Add authorization/lifecycle tests only after kernel-controlled endpoint identity exists.
5. Run both ISO/QEMU and native BIOS/QEMU boot tests plus Security architecture checks on the exact tested commit.
6. Update the work log with commit SHA, exact commands, outputs, and failures. Source inspection is not a substitute for test results.
