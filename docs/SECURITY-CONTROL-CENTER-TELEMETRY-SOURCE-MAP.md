# Security Control Center Telemetry Source Map

Status: source-audit note; no implementation claim. Cross-checked against current branch source during the Security Control Center design review.

## 1. Existing health-event ring

Source:
- `kernel/include/health.h`
- `kernel/health.c`

Verified facts:
- `PROS_HEALTH_EVENT_MAX` is 128.
- `struct pros_health_event` contains 64-bit sequence and timestamp, 32-bit severity/category/source/code, and a 64-bit value. This internal C structure is not the proposed 20-byte userspace wire record.
- `pros_health_snapshot()` exposes `event_sequence`, `event_count`, `dropped_events`, last severity and last category to kernel callers.
- `pros_health_get_event(offset, out)` enumerates retained records from the oldest retained event.
- The timestamp is `timer_ticks()`; `health.h` says `timer_frequency_hz()` defines the unit.
- When the ring is full, `pros_health_record()` increments a static `uint32_t dropped_events` with a plain increment. It does not currently saturate. At integer wrap, a consumer could observe a smaller value after long-running overflow.
- The current in-memory ring is not evidence of persistent audit storage. Its initialization resets counters and records.

Implementation gates:
1. Define whether dropped count is lifetime, boot, or broker-epoch scoped.
2. Change the source counter to saturate or otherwise define wrap semantics, with focused self-tests at `UINT32_MAX`, before claiming monotonic drop telemetry.
3. Expose the count only through a trusted broker after checking snapshot error status. Never synthesize zero on unavailable/error.
4. Define locking/consistent snapshot semantics before concurrent producers/readers or SMP support is claimed; this source inspection alone does not establish race freedom.

## 2. Existing security monitor

Source:
- `kernel/include/security_monitor.h`
- `kernel/security_monitor.c`

Verified facts:
- The monitor keeps a bounded 32-entry process-state table, with score, event count, last timestamp, and current action.
- The event enum includes mass file modification, persistence change, privilege escalation, process injection, security tamper, abnormal network, credential access and policy violation.
- `pros_security_monitor_record()` updates state and calls `pros_health_record_security()` with the action as the health-event code and a packed value containing event type and score.
- That health record is a decision summary, not a one-to-one structured event record containing all original fields. The original PID/type/severity are not all exposed as separate fields in the health event record.
- The monitor state is a kernel C interface, not a public userspace query API. Do not expose raw internal state to an untrusted UI.
- The monitor's `clear` operation is a mutating kernel interface and must not be exposed through the initial read-only protocol.

Implementation gates:
1. Specify the source-to-wire mapping and stable public reason codes.
2. Decide which process identity information is safe to expose and how it is redacted/authorized.
3. Ensure the UI distinguishes a security decision summary from a full audit event.
4. Keep all mutation operations out of protocol v1.

## 3. Existing syscall and IPC boundary

Source:
- `include/abi/syscall.h`
- `kernel/syscall.c`
- `kernel/include/ipc.h`
- `kernel/ipc.c`
- `kernel/include/object.h`
- `kernel/object.c`

Verified facts:
- Public syscall numbers currently run from 1 through 7; no dedicated security telemetry query syscall is declared.
- IPC messages are limited to 64 bytes; channels have eight message slots.
- Syscall IPC send validates the requested length and user address range, checks handle type and write authorization, then copies user input before sending.
- Low-level receive checks message length against capacity before copying/dequeueing. This alone does not establish syscall-level copy-out failure semantics; the full syscall path must be tested.
- Handles use slot/generation values and rights checks, but handle possession alone does not prove a named service's identity.
- A kernel C interface or kernel-side service-manager callback is not proof of an authenticated userspace broker or service-discovery path.

## 4. Wire-format mapping warning

The protocol proposal's 20-byte event record is a deliberately compact wire representation, not a direct cast of `struct pros_health_event`. In particular:
- Internal timestamp is 64-bit while proposed wire timestamp is 32-bit; a conversion, unit, range and overflow policy must be explicitly defined before implementation.
- Internal source and code are 32-bit; proposed source identifier is 16-bit. Define stable mapping and reject/trap unrepresentable values rather than silently truncate.
- Internal `value` is 64-bit and intentionally omitted from v1 to avoid leaking packed state or arbitrary internal values.
- Severity/category are numeric internal enums. Unknown values must be handled without unsafe assumptions; the public protocol must define its own stable enum values.

Do not use structure casts, packed-struct assumptions, or copy internal kernel structures directly into userspace. Build wire messages field-by-field, validate every conversion, initialize all response bytes, and test exact byte lengths.

## 5. Readiness decision

The current source offers useful kernel-side telemetry foundations but is **not yet an end-to-end Security Control Center API**. Before UI integration, the project still needs:
- reliable dropped-event semantics;
- stable, explicit source-to-wire mapping;
- a trusted userspace broker identity and endpoint lifecycle;
- bounded request parsing and negative authorization tests;
- syscall-level copy failure and event-consumption tests;
- exact-head build and QEMU evidence for the eventual runtime implementation.

This document is a design/source map only. It does not change kernel behavior or assert that the telemetry is currently accessible from userspace.
