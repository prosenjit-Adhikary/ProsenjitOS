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

## 4.3 Expanded repository call-site inventory

A repository code search and branch-source inspection found additional production callers beyond the first-pass wrapper inventory. This section supersedes any implication that the wrappers have only one producer.

| Call site | Category | Source/code behavior observed | Privacy/mapping gate |
|---|---|---|---|
| `kernel/driver.c` | NETWORK | Source is device ID; codes include DEVICE_DISCOVERED, DEVICE_REMOVED, PROBE_FAILED, DRIVER_BOUND, DRIVER_UNBOUND and STACK_UNAVAILABLE. Discovery value packs vendor/device IDs; failure values may encode driver errors | Device IDs and hardware identifiers are internal identifiers, not stable public source IDs. Map each approved reason explicitly; do not serialize raw values |
| `kernel/security_provider.c` | SECURITY | Source `0x53454350`; code `PROS_SECURITY_HEALTH_PROVIDER`; value may be provider index | Provider index is internal inventory detail. Use a coarse reason only after reviewing the provider enum and public semantics |
| `kernel/security_hardening.c` | SECURITY | Source `0x53454348`; initialization uses code 1 and generation as value; violation path accepts caller-supplied code/value | Caller-supplied code/value require tracing to all call sites; generation and violation metadata must not pass through raw |
| `kernel/security_admission.c` | SECURITY | Source `0x53454341`; uses admission/signature-failure constants; one value packs security action/state | Must map by explicit reviewed reason; never expose packed action/state |
| `kernel/intent.c` | SECURITY | Source `0x53454349` ("SECI"); code packs decision in upper bits and reason in lower bits; value is intent ID | The source-ID collision with file admission has been fixed in the current PR branch. Packed code and intent ID remain private and require explicit allow-listed mapping |
| `kernel/kernel.c` | RECOVERY | Crash-capture failure uses `PROS_CRASH_HEALTH_SOURCE` and `PROS_CRASH_HEALTH_CAPTURE_FAILED`; recovered user-copy fault uses source `0x55504346`, code 1, and RIP-like frame value | Trace constants to their defining headers; never expose addresses or raw failure values |
| `kernel/vfs.c` | FILESYSTEM | Source `0x56465348`; code comes from VFS error/reporting path; value is a negated return code | Error code domain and call sites must be reviewed; no arbitrary VFS code pass-through |

The current wrapper call-site audit found no production caller of `pros_health_record_filesystem()` outside `kernel/vfs.c` in the repository search results; the helper itself and self-test also exist. Repository search results are a source-discovery aid, not a proof of completeness—repeat the search on the exact branch/head before implementing the registry.

**Updated implementation gate:** do not finalize public source IDs or reason IDs yet. The internal collision between file admission (`0x53454341`, `SECA`) and intent audit (`0x53454349`, `SECI`) is resolved on this branch. Remaining work is to complete the call-site/enum review, define the public allow-list, and test unknown source/code pairs fail closed to a documented generic representation. The raw internal `value` remains excluded from v1.


## 4.3 Additional producer families found in follow-up search (partial)

A repository search for the four health-record helper families found additional producers that must be included before the registry can be frozen:

| Category | Source | Code | Observed semantics and privacy gate |
|---|---|---|---|
| SECURITY (5) | 0x53454341 ("SECA") | PROS_SECURITY_HEALTH_ADMISSION = 1 | Admission result summary; internal value packs action/state. Do not expose packed value. |
| SECURITY (5) | 0x53454341 ("SECA") | PROS_SECURITY_HEALTH_SIGNATURE_FAILURE = 2 | Signature verification failure; currently value 0 in inspected call site. |
| SECURITY (5) | 0x53454341 ("SECA") | PROS_SECURITY_HEALTH_PROVIDER = 3 (header enum; no admission call site confirmed in this audit) | Do not assume this code is emitted by file admission; provider implementation uses source `0x53454350`. Trace all call sites before public mapping. |
| SECURITY (5) | 0x53454348 ("SECH") | 1 and caller-supplied code | Security-hardening ready/violation events; dynamic codes require a full call-site and enum audit. |
| FILESYSTEM (7) | 0x56465348 ("VFSH") | caller-supplied code | VFS error helper records a code and the negated internal result in value. Do not expose raw error/value or path/resource details. |
| NETWORK (4) | dynamic device ID | PROS_HEALTH_NETWORK_DEVICE_DISCOVERED = 1 / DEVICE_REMOVED = 2 | Driver registry emits network-device lifecycle events; value may pack vendor/device IDs. Public source must not be a raw internal device ID without privacy and stability review. |
| RECOVERY (8) | 0x45584350 ("EXCP") | exception vector truncated to u32 | Critical exception record; value includes RIP. Sensitive machine-state details must remain private/redacted. |
| RECOVERY (8) | 0x45584351 ("EXC1") | exception vector truncated to u32 | Critical exception record; value contains error code. Treat as sensitive internal diagnostic. |
| RECOVERY (8) | 0x45584352 ("EXC2") | exception vector truncated to u32 | Critical exception record; value contains CR2/page-fault address. Do not serialize raw address. |
| RECOVERY (8) | 0x43525348 ("CRSH") | crash-record codes 1..3 plus capture-failure code | Crash/recovery reporting uses a separate header-defined source and code family; complete constants/call sites need audit. |
| RECOVERY (8) | watchdog service ID (dynamic) | PROS_HEALTH_RECOVERY_WATCHDOG_TIMEOUT = 8 | Watchdog timeout event; value is elapsed ticks. Service ID needs a stable allow-list and must not be accepted from an untrusted client as identity. |
| RECOVERY (8) | 0x55504346 ("UPCF") | 1 | Recovered user-copy fault event; value contains a frame field and needs review before any public mapping. |

This table records search-discovered tuples, not a finished public registry. Some search excerpts were from the repository's default branch; before implementing a serializer, re-verify each producer and enum against the exact PR head and enumerate all helper call sites on that branch. In particular, source IDs may be dynamic (device/watchdog IDs), codes may be caller-supplied, and internal value fields can contain pointers/addresses, hardware IDs, scores or packed decisions. Default behavior for an unreviewed tuple remains UNKNOWN/UNAVAILABLE, with value omitted.

## 5. Readiness decision

The current source offers useful kernel-side telemetry foundations but is **not yet an end-to-end Security Control Center API**. Before UI integration, the project still needs:
- reliable dropped-event semantics across source reset/reboot and broker epoch boundaries;
- stable, explicit source-to-wire mapping;
- a trusted userspace broker identity and endpoint lifecycle;
- bounded request parsing and negative authorization tests;
- syscall-level copy failure and event-consumption tests;
- exact-head build and QEMU evidence for the eventual runtime implementation.

This document is a design/source map only. It does not change kernel behavior or assert that the telemetry is currently accessible from userspace.


## 4.2 Source and reason-code inventory (partial, source-verified 2026-10-27)

This is an inventory of identifiers found in the inspected headers/call sites, not yet the approved public registry. Internal numeric source IDs are shown only to help maintainers find call sites; the broker must not expose them directly until each mapping is reviewed.

| Producer / area | Internal category | Internal source ID | Internal reason/code values verified | Mapping status |
|---|---:|---:|---|---|
| Block I/O read path | STORAGE (3) | `0x424C4B52` ("BLKR") | 1 invalid request; 2 read failure | Source and codes observed in `kernel/block.c` / `kernel/include/block.h`; public reason IDs not assigned |
| Block I/O write path | STORAGE (3) | `0x424C4B57` ("BLKW") | 1 invalid request; 3 write failure | Same internal code enum; value 1 is reused for invalid request |
| Block flush path | STORAGE (3) | `0x424C4B46` ("BLKF") | 1 invalid request; 4 flush failure | Call-site inventory still needs complete flush-path cross-check |
| Driver registry | DRIVER (6) | `0` in observed calls | 1 register failure; 2 registry exhausted; 3 device registration failure; 4 unsupported class; 5 remove failure | Header registry verified; source ID zero is not yet approved as a public source |
| VFS error reporting | FILESYSTEM (7) | `0x56465348` ("VFSH") | 1 I/O error; 2 permission error | `kernel/vfs.c` currently emits these two codes from `vfs_health_error()`; lookup counters may increment without an event |
| File admission / signature | SECURITY (5) | `0x53454341` ("SECA") | 1 admission; 2 signature failure | Stable admission/signature codes; provider events use separate `0x53454350` source |
| Security provider | SECURITY (5) | `0x53454350` ("SECP") | 3 provider in inspected calls | Provider implementation should be fully enumerated before public mapping |
| Security hardening | SECURITY (5) | `0x53454348` ("SECH") | 1 ready; dynamic violation code in other call | Not a stable public registry yet |
| Intent decision audit | SECURITY (5) | `0x53454349` ("SECI") | `(decision << 16) | reason` internal packed code; value is intent ID | Source collision resolved in kernel source; packed code and intent ID remain private and must map through a reviewed allow-list |
| Security monitor | SECURITY (5) | `0x53444D4E` ("SDMN") | current action in code; internal value packs event type and score | Never expose packed value; define a reviewed event/action mapping first |
| Logging persistence degradation | RECOVERY (8) | `0x4C4F4750` ("LOGP") | 2 persistence emit failure | This is a call-site literal, not yet a public stable code contract |

| Network device lifecycle | NETWORK (4) | dynamic device ID | 1 discovered; 2 removed (from `kernel/driver.c`) | Dynamic device IDs are internal; do not expose directly |
| Exception capture | RECOVERY (8) | `0x45584350` family observed in `kernel/health.c` | exception vector used as code; error code/RIP/CR2-related data in private fields | Sensitive fault addresses/register context must not be sent raw |
| Crash-record recovery | RECOVERY (8) | `0x43525348` ("CRSH") | 1 recovered; 2 vector; 3 invalid; capture-failed code also used by kernel | Verify the capture-failed constant and all callers before freezing public mapping |
| Recovered usercopy page fault | RECOVERY (8) | `0x55504346` ("UPCF") | 1 recovered usercopy fault | Call-site literal; public source/reason IDs not assigned |
| Boot/recovery logging | RECOVERY (8) | `0x52454356` ("RECV") | boot-begin event; driver-ready logging uses source `0x44525648` | Distinguish boot milestones from dynamic watchdog events; don't infer registry completeness |
| Watchdog timeout | RECOVERY (8) | dynamic watchdog service ID | `PROS_HEALTH_RECOVERY_WATCHDOG_TIMEOUT` (8) | Service ID is dynamic/private; map to reviewed service class or UNKNOWN |

Additional verified definitions in `kernel/include/health.h`: recovery codes 1..8 (boot begin, storage ready, driver ready, health ready, crash recovered, boot complete, failure, watchdog timeout); network codes 1..6 (device discovered/removed, driver bound/unbound, probe failed, stack unavailable). Their complete source-ID/call-site pairing was not established in this pass, so do not infer a public source mapping from the code enums alone.

### Required registry review before serializer code

1. Give each public source a unique stable identifier and document its producer/category; do not use source ID zero as a catch-all without an explicit reviewed rule.
2. Define public reason IDs by a stable key that disambiguates category, source, and internal code where necessary. The former `0x53454341` collision was resolved by assigning intent audit the distinct internal source `0x53454349` (`SECI`). Keep this internal packed code private; public reason mapping still requires a finite allow-list.
3. Treat dynamic/internal codes as UNKNOWN unless a finite allow-list entry explicitly maps them. Do not serialize raw intent decision/reason bit packing, PID/TID, handles, LBA, score, or the internal `value` field.
4. Add compile-time or host regression tests for every approved mapping and unknown-value fallback before connecting the serializer to the broker.
5. Extend this inventory with all `pros_health_record*` call sites and verify the list against a repository-wide search; this table is intentionally partial, not exhaustive.
