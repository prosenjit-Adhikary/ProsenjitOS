# ProsenjitOS — System Health Monitoring

Status: implementation batch hardened 2026-10-01; monotonic time, typed kernel telemetry, expanded VFS counters, persistent-sink lifecycle integration, bounded rotation/retention, non-blocking emergency crash capture, and deferred crash persistence are implemented. Full end-to-end crash reproduction and fresh CI/QEMU verification remain dependent on available build/runner execution.

## Purpose

System health monitoring is a first-class observability subsystem. It must provide low-overhead, structured, persistent and actionable health information without becoming a boot-critical dependency or causing the failure it is trying to report.

The design is informed by established OS patterns:
- Linux exposes kernel/process/resource state through procfs/sysfs and provides Pressure Stall Information (PSI) for CPU, memory and I/O contention.
- FreeBSD combines periodic subsystem/security checks with persistent/rotated output and has event-driven device notification facilities.
- OpenBSD provides continuously refreshed system statistics covering load, memory, scheduling, interrupts and I/O.
ProsenjitOS uses these as design references, not copied interfaces.

## 1. Health domains

### CPU
Collect online CPU count, scheduler tick/uptime, runnable/blocked thread counts, user/kernel/idle time where available, interrupt/exception counters, scheduler latency/overrun indicators, and CPU pressure/contention metrics when supported.

### Memory
Collect total physical memory, free/available pages, allocated/reserved pages, kernel heap usage, user-space memory accounting when available, allocation failures, page-fault statistics, memory pressure, and OOM/low-memory events.

The monitor must distinguish capacity, current usage, pressure and failures; a single percentage is never the complete health signal.

### Storage
Collect discovered block devices, device online/offline state, read/write operation counts, I/O errors/timeouts, flush failures, queue/latency indicators when available, filesystem mount state, and filesystem integrity/recovery events.

### Filesystem
Collect mount/unmount events, read/write/truncate errors, journal transaction failures, replay/recovery results, corruption/integrity failures, free-space and inode/block exhaustion indicators, and read-only/degraded transitions.

Current VFS telemetry records mount/unmount, successful file read/write/truncate operations, successful unlink/mkdir/rmdir/rename and directory open/read operations, plus classified lookup/I/O/permission/corruption error counters. Generic overflow/path-size failures are not treated as corruption.

### Kernel
Collect warnings, exceptions/faults, failed assertions/self-tests, lockup/deadlock indicators when implemented, panic/crash events, boot health markers, and subsystem health transitions.

Current implementation records exception/page-fault counters and the last exception vector/RIP. Security authorization decisions and driver device lifecycle transitions also emit bounded structured health events.

### Thermal/power/hardware
The interface must support temperature sensors, fan/thermal state, battery/AC state where applicable, and throttling/power-state events. Missing sensors must report unsupported/unavailable, never a false healthy value.

## 2. Health state model

Every monitored resource uses explicit states:

UNKNOWN
HEALTHY
DEGRADED
CRITICAL
FAILED

UNKNOWN is distinct from HEALTHY. Missing telemetry must not be interpreted as success.

A state transition produces a structured event. Repeated identical samples are coalesced/rate-limited so monitoring cannot flood the logging system.

## 3. Telemetry pipeline

Kernel subsystem
 -> typed health counter/snapshot
 -> lightweight event/telemetry ring
 -> kernel observability interface
 -> system-health collector
 -> structured log/event backend
 -> persistent diagnostic store
 -> CLI/GUI/API consumers

The kernel exposes stable semantic telemetry contracts rather than forcing user space to parse private kernel structures.

## 4. Sampling and event strategy

Use a hybrid model:
- counters for cheap cumulative facts
- periodic snapshots for trends
- event-driven notifications for faults/state transitions
- threshold triggers for critical conditions
- on-demand detailed diagnostics

Suggested sampling schedules remain design-only and are NOT implemented by the current batch. No interval-based health sampling is performed:
- 1s: lightweight health counters/scheduler/memory pressure summary
- 5s: storage/filesystem/device summary
- 30s: hardware/thermal inventory where supported
- immediate: panic, crash, I/O failure, corruption, OOM, watchdog and critical state transitions

Any future interval scheduler must be separately designed, bounded, and explicitly introduced; it is outside this batch.

## 5. Resource-pressure model

Do not rely only on utilization percentages. The health subsystem should eventually track CPU contention/latency, memory reclamation/pressure, I/O contention/latency, allocation failure trends, and sustained versus transient pressure.

Linux PSI demonstrates the value of distinguishing partial and full resource stall states rather than treating all high utilization as equivalent.

## 6. Failure safety

Monitoring must be fail-safe:
- no dynamic allocation in panic-path telemetry
- bounded/rate-limited event queues
- no blocking filesystem operation from interrupt context
- no recursive logging loops
- no dependency on a healthy user-space logger for kernel correctness
- dropped telemetry increments a loss counter
- critical crash/panic metadata uses a reserved emergency path
- monitoring failure never automatically triggers destructive repair

## 7. Persistent health history

The future diagnostic store should retain boot health summary, previous shutdown/crash marker, critical health transitions, resource-pressure summaries, storage/filesystem failures, watchdog actions, and kernel panic/crash metadata.

Retention is bounded and protected by the logging subsystem's rotation and retention policy.

## 8. Interfaces

Planned semantic interfaces:

pros_health_snapshot
- timestamp (monotonic timer ticks; unit defined by `timer_frequency_hz()`)
- boot/session identifier
- CPU summary
- memory summary
- storage summary
- filesystem summary
- kernel fault summary
- thermal/power summary
- overall state

pros_health_event
- timestamp
- severity
- domain
- source/component
- event type
- state transition
- numeric/context fields
- correlation identifier
- persistence priority

pros_health_counter
- monotonically increasing identifier
- current value
- optional delta/rate
- reset semantics

These are design-level contracts; ABI details are frozen only after kernel/user ABI review.

## 9. CLI/API requirements

A future native health utility should support current health summary, per-domain details, recent transitions, resource-pressure view, storage/filesystem health, kernel fault summary, machine-readable structured output, and diagnostic snapshot/export.

A GUI consumer may present the same semantic data but must not become the source of truth.

## 10. Tests and CI gates

The implementation is not called production-complete until the remaining environment-dependent gates are verified. Current deterministic tests cover:
- healthy baseline
- missing/unsupported sensor
- counter overflow/rollover
- sampling jitter
- event burst/rate limiting
- telemetry queue overflow
- dropped-event accounting
- low-memory behavior
- storage I/O error reporting
- filesystem corruption/recovery event reporting
- panic/crash emergency telemetry
- reboot/persistence of critical diagnostics
- malformed user-space requests
- concurrent readers
- SMP-safe counter/event access when SMP is enabled

QEMU regression should verify deterministic boot-health markers and at least one synthetic degraded-health transition before the observability gate is considered verified.

Network telemetry remains event-hook capable but is not fabricated: the current repository does not yet provide a real network-device/link subsystem to instrument. The health API therefore exposes a network category without pretending unsupported network metrics exist.

## 11. Implementation order

1. Kernel monotonic time/uptime source contract — implemented: `timer_ticks()` is the monotonic tick source, `timer_frequency_hz()` exposes the PIT-derived rate, `timer_uptime_ms()` provides a derived millisecond uptime view, and `timer_selftest()` validates the contract.
2. Typed health counters and snapshot structures.
3. CPU/scheduler telemetry.
4. Physical-memory/allocator telemetry.
5. Kernel fault/exception telemetry.
6. Block-device/storage telemetry.
7. Filesystem/VFS health events.
8. Bounded telemetry ring/event transport.
9. User-space system-health collector.
10. Structured persistent diagnostic store foundation (VFS-backed bounded sink is implemented and attached opportunistically after native writable storage is ready).
11. Rotation/retention integration — implemented with bounded file count/size and deterministic rotation.
12. CLI/API.
13. Fault-injection and QEMU regression suite.
14. SMP/per-platform sensor extensions.

## 12. Completion rule

System Health Monitoring is complete only when the implementation order above is covered by code, persistent diagnostics, failure-safe behavior and automated regression tests. Documentation alone must never mark the implementation as complete.

## Architectural references

Linux kernel procfs and PSI documentation:
- /proc exports load average, memory information, kernel messages and other kernel statistics.
- PSI provides CPU, memory and I/O pressure measurements and threshold notification mechanisms.

FreeBSD documentation:
- periodic performs routine subsystem/security checks and can write output to logs that are rotated by newsyslog.
- devd provides event-driven device state notifications.

OpenBSD documentation:
- systat/vmstat provide continuously refreshed views of load, memory, scheduling, interrupts and I/O statistics.
