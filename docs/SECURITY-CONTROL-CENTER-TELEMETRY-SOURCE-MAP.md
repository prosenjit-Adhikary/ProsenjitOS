# Security Control Center Telemetry Source Map

Status: source-audit note; no implementation claim. Cross-checked against current branch source during the Security Control Center design review.

## 1. Existing health-event ring

Source:
- `kernel/include/health.h`
- `kernel/health.c`

Verified facts:
- `PROS_HEALTH_EVENT_MAX` is 128.
- `struct pros_health_event` contains 64-bit sequence and timestamp, 32-bit severity/category/source/code, and a 64-bit value. This internal C structure is not the proposed 24-byte userspace wire record.
- `pros_health_snapshot()` exposes `event_sequence`, `event_count`, `dropped_events`, last severity and last category to kernel callers.
- `pros_health_get_event(offset, out)` enumerates retained records from the oldest retained event.
- The timestamp is `timer_ticks()`; `health.h` says `timer_frequency_hz()` defines the unit.
- When the ring is full, `pros_health_record()` increments the static `uint32_t dropped_events` counter unless it is already `UINT32_MAX`; the counter therefore saturates instead of wrapping. `pros_health_selftest()` checks the `UINT32_MAX - 1` boundary using two additional records and calls `pros_health_init()` after a successful test to clear synthetic telemetry. `kernel/kernel.c` now invokes this self-test fail-closed before the initial health sample, and the build workflow requires its success marker in both QEMU boot paths. Source inspection confirmed the wiring on 2026-10-26; exact-head compile/runtime verification remains pending CI.
- The current in-memory ring is not evidence of persistent audit storage. Its initialization resets counters and records.

Implementation gates:
1. Define whether dropped count is lifetime, boot, or broker-epoch scoped.
2. Preserve the implemented saturation and boundary self-test with exact-head CI; the source counter remains 32-bit, so wire conversion must not imply greater precision.
3. Expose the count only through a trusted broker after checking snapshot error status. Never synthesize zero on unavailable/error.
4. Define locking/consistent snapshot semantics before concurrent producers/readers or SMP support is claimed; this source inspection alone does not establish race freedom.

### Additional source audit: ring synchronization and sequence reset

A direct read of `kernel/health.c` shows `pros_health_record()`, `pros_health_snapshot()`, and `pros_health_get_event()` access the event ring, head/count, dropped counter and sequence without an evident ring lock or snapshot transaction in these functions. A snapshot can therefore not yet be described as an atomic view relative to concurrent event writers; an event read racing with overwrite/reset also lacks an explicit consistency guarantee from these functions alone. This observation is a source-level correctness gate, not proof of a particular race occurring in the supported current boot model.

`pros_health_init()` resets `event_head`, `event_count`, `dropped_events`, and `next_sequence` to zero. Sequence values are therefore not globally unique across initialization/reboot epochs. The broker must not pretend a cursor from an earlier epoch remains valid. Until an epoch identifier and synchronized ring semantics are designed, service restart or source reset should invalidate existing cursors and return STALE/UNAVAILABLE instead of claiming continuity.

Before concurrent broker reads are implemented, specify the execution/interrupt model and a synchronization strategy for event writes, snapshot reads, ring enumeration and reset. Avoid introducing a lock that can deadlock in interrupt context; review call sites and lock order, and test wraparound, reset, overwrite, concurrent read/write and dropped-counter saturation.

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

The current draft protocol on `security/scc-u64-timestamp-get-event-guard` proposes a 24-byte event wire record. It is deliberately not a direct cast of `struct pros_health_event`, whose layout and field widths are kernel-internal.

- `sequence`: wire `u64`, sourced from the internal 64-bit sequence, but only meaningful within a documented source/broker epoch.
- `timestamp_ticks`: wire `u64`, sourced from the internal 64-bit `timer_ticks()` timestamp. `timer_frequency_hz()` defines units; this is boot-relative monotonic time, not wall-clock time.
- `severity` and `category`: wire `u8` values require explicit stable public enums. Do not silently cast internal `u32` enums; reject or map unknown values according to a reviewed compatibility rule.
- `source_id`: wire `u16` while internal source is `u32`. Define a stable allow-listed mapping and never truncate unknown values.
- `reason_code`: wire `u32`, sourced from internal code only through a reviewed public mapping. Never expose raw kernel addresses, packed internal state, or arbitrary values.
- Internal `value` is omitted from v1 to avoid leaking packed state or arbitrary internal values.

The 24-byte record plus the 24-byte response header permits one event in a 64-byte IPC message (48 bytes total). Two records would require 72 bytes and must be rejected. `GET_EVENT` requests must carry a nonzero sequence; the response record sequence and response cursor must both equal that requested sequence. This request-binding check is framing validation, not peer authentication.

Build wire messages field-by-field, validate every conversion, initialize all response bytes, and test exact byte lengths. No response serializer or stable source/reason registry is currently implemented.

## 4.1 Public enum mapping and source allow-list gate

The v1 protocol document now proposes explicit public severity values 0..4 (DEBUG through CRITICAL) and category values 0..8 (SYSTEM through RECOVERY), with 255 reserved for UNKNOWN. These are protocol values, not an instruction to cast internal `uint32_t` values directly. The serializer must map by explicit switch/table and map unknown internal values to UNKNOWN.

Reason codes are not globally unique: current health recovery codes 1..8 and network codes 1..6 are scoped to their respective categories. The future public registry must key mappings by category plus internal code and assign stable public reason IDs. A source-ID inventory is also still required. Until the registry and inventory are reviewed, arbitrary internal source/code values must not be passed through. Security-monitor health events encode an action plus packed type/score in the internal value; that packed value must remain private and must not be treated as a full original security event.

## 4.2 Source/reason inventory — first-pass audit (not a public registry)

Reviewed against `kernel/health.c`, `kernel/include/health.h`, `kernel/security_monitor.c`, and `kernel/include/security_monitor.h`. This is an inventory of internal producer shapes, not authorization to expose their values.

| Producer path | Internal category | Internal source/code shape | Public mapping decision |
|---|---|---|---|
| `pros_health_sample()` | SYSTEM | source=1, code=1; value is free physical pages | Candidate for a coarse “health sample” reason only after product/privacy review; never publish the raw value in event v1 |
| `pros_health_selftest()` synthetic records | MEMORY / SYSTEM | source=2, code=1; source=2, code=2; ring test source=3 with changing code | Test-only records; must not be treated as production reason registry entries. Self-test reinitializes the ring on success |
| Recovery helpers/watchdog | RECOVERY | recovery codes 1..8; watchdog registration uses code 1 and dynamic service ID as source; timeout uses code 8 and dynamic service ID | Category-scoped internal codes overlap. Dynamic service IDs are not public source IDs; use reviewed generic reason/source mapping until an explicit stable registry exists |
| Network helper | NETWORK | declared network codes 1..6; caller supplies source ID | Neither caller source nor code may pass through by default; inventory all call sites before approving mappings |
| Security monitor | SECURITY | source `0x53444D4E` (“SDMN”); code is action 0..2; value packs event type in high 32 bits and score in low 32 bits | This is an action/score summary, not an original event. Do not publish packed value or infer the original event type from the public reason code |
| Exception capture | RECOVERY | source constants `0x45584350`, `0x45584351`, `0x45584352`; code is truncated exception vector; value contains RIP, error code, or CR2 | Sensitive machine-state data; keep source/code/value internal until a dedicated redacted exception taxonomy is reviewed |
| Filesystem helper | FILESYSTEM | caller supplies source, code and value; no stable enum registry established by this audit | Unmapped by default; inspect all call sites and define a stable, privacy-reviewed taxonomy before publication |

Important limits:
- The health API accepts arbitrary `uint32_t source` and `code`; wrapper names do not enforce stable IDs.
- Current self-test calls are not evidence that a production producer taxonomy exists.
- The security monitor ignores the return value from `pros_health_record_security()`; the monitor decision can succeed while the corresponding health-ring append fails. End-to-end telemetry must therefore not claim one-to-one complete security-event capture.
- `pros_health_record_exception()` places instruction pointer, error code and CR2 in the internal `value`. This reinforces the v1 rule that `value` is never serialized.
- Do not mint final public reason IDs from this first pass. Next prerequisite: repository-wide call-site inventory for `pros_health_record_recovery`, `pros_health_record_network`, `pros_health_record_security`, and `pros_health_record_filesystem`; then security/privacy review and exact byte-level serializer tests.


## 4.2 Audited internal reason-code/source inventory (partial, 2026-10-27)

This is a source-backed inventory of the call sites inspected for the v1 serializer gate, **not** yet the public wire registry. Internal source and code are currently 32-bit values; never truncate/cast them to the proposed 16-bit source_id or 16-bit public reason field without an explicit reviewed mapping. A code is meaningful only in its category and source context unless the public registry explicitly defines otherwise.

| Internal category | Source expression / source value | Internal code | Call-site meaning | Public mapping status |
|---|---|---|---|---|
| STORAGE (3) | 0x424C4B52 ("BLKR") | PROS_BLOCK_HEALTH_INVALID_REQUEST = 1 | Invalid block read request or LBA range | Candidate registry entry; public source/reason ID not assigned |
| STORAGE (3) | 0x424C4B52 ("BLKR") | PROS_BLOCK_HEALTH_READ_FAILURE = 2 | Block read callback failed | Candidate registry entry; public source/reason ID not assigned |
| STORAGE (3) | 0x424C4B57 ("BLKW") | PROS_BLOCK_HEALTH_INVALID_REQUEST = 1 | Invalid block write request or LBA range | Candidate registry entry; public source/reason ID not assigned |
| STORAGE (3) | 0x424C4B57 ("BLKW") | PROS_BLOCK_HEALTH_WRITE_FAILURE = 3 | Block write callback failed | Candidate registry entry; public source/reason ID not assigned |
| STORAGE (3) | 0x424C4B46 ("BLKF") | PROS_BLOCK_HEALTH_INVALID_REQUEST = 1 | Invalid flush setup request | Candidate registry entry; public source/reason ID not assigned |
| STORAGE (3) | 0x424C4B46 ("BLKF") | PROS_BLOCK_HEALTH_FLUSH_FAILURE = 4 | Block flush callback failed | Candidate registry entry; public source/reason ID not assigned |
| DRIVER (6) | 0 | PROS_DRIVER_HEALTH_REGISTER_FAILURE = 1 | Driver registration rejected | Source is currently zero/unspecified; do not expose as a stable identity |
| DRIVER (6) | 0 | PROS_DRIVER_HEALTH_REGISTRY_EXHAUSTED = 2 | Driver/device ID registry exhausted | Source is currently zero/unspecified; do not expose as a stable identity |
| SECURITY (5) | 0x53454350 ("SECP") | PROS_SECURITY_HEALTH_PROVIDER = 1 (header-defined name) | Provider registration/query failure; code used in inspected provider call sites | Candidate registry entry; verify all uses before freezing public reason |
| SECURITY (5) | 0x53444D4E ("SDMN") | current->action = ALLOW 0 / QUARANTINE 1 / BLOCK 2 | Security monitor decision summary | Action is not an original detection reason; packed value contains event type/score and must stay private |
| RECOVERY (8) | 0x4C4F4750 ("LOGP") | 2 | Persistent log sink failed; in-memory ring records degradation | Candidate registry entry; reason is currently a literal, not a named enum |
| SYSTEM (0) | 0x4C4F4701 | 1 | Logging self-test marker event | Test-only/internal event; exclude from stable public reasons unless separately approved |

Additional header-defined code families found but not yet fully inventoried across every call site:
- RECOVERY: PROS_HEALTH_RECOVERY_* values 1..8 (boot begin, storage ready, driver ready, health ready, crash recovered, boot complete, failure, watchdog timeout).
- NETWORK: PROS_HEALTH_NETWORK_* values 1..6 (device discovered/removed, driver bound/unbound, probe failed, stack unavailable). Caller/source assignments still need full call-site audit.
- DRIVER: PROS_DRIVER_HEALTH_* values 1..5; only registration and registry exhaustion were confirmed in the inspected excerpt. Audit remaining device registration, unsupported class and remove failure paths.

**Registry rule:** do not assign public IDs yet to rows marked candidate. Next audit must enumerate every pros_health_record*() call, all code enums and all source constants on this branch, then review category/source/code tuples with a human before implementing the serializer. Unknown tuple => generic UNKNOWN reason/source, never raw passthrough. The inventory is intentionally partial and must not be read as proof that unlisted producers do not exist.

## 5. Readiness decision

The current source offers useful kernel-side telemetry foundations but is **not yet an end-to-end Security Control Center API**. Before UI integration, the project still needs:
- reliable dropped-event semantics across source reset/reboot and broker epoch boundaries;
- stable, explicit source-to-wire mapping;
- a trusted userspace broker identity and endpoint lifecycle;
- bounded request parsing and negative authorization tests;
- syscall-level copy failure and event-consumption tests;
- exact-head build and QEMU evidence for the eventual runtime implementation.

This document is a design/source map only. It does not change kernel behavior or assert that the telemetry is currently accessible from userspace.
