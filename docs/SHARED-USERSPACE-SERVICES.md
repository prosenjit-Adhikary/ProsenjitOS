# Shared Userspace Services (SUS) — Architecture Contract

Status: design contract and source-work checklist; not a claim that the desktop stack is operational.
Last reviewed: 2026-10-17.

## 1. Goal

Provide a stable userspace layer that lets multiple independently installed desktop environments share the same OS-facing services. Examples of future desktop choices include XFCE and other environments that can run on a supported X11 or Wayland stack. The layer must not require desktop-specific code in the kernel.

The design prioritizes security boundaries, failure isolation, maintainability, and measurable performance. Do not promise a universally "perfect" system; each milestone must pass its stated verification gates.

## 2. Non-negotiable boundaries

- Kernel responsibilities: memory/address spaces, process/thread scheduling, syscalls, VFS/storage, device ownership and permission-checked device interfaces, and security enforcement.
- Shared Userspace Services responsibilities: service lifecycle, session lifecycle, capability-checked access to display/input/audio/network-facing APIs as those APIs become available, configuration, logging, crash/restart policy, and desktop-independent policy glue.
- Desktop environment responsibilities: shell/panel, window manager/compositor integration, settings UI, file manager, terminal, and desktop applications.
- Existing external components: use an existing supported Xorg/X11 or Wayland implementation; do not build a custom X server as part of this plan.
- Drivers: low-level hardware drivers remain behind kernel/device boundaries. A future userspace driver-management service may discover, select, configure, and supervise permitted driver providers, but must not bypass kernel validation or gain unrestricted device access.
- Do not place XFCE, Xorg, Wayland, application code, or desktop policy inside the kernel.

## 3. Intended dependency graph

Firmware -> bootloader -> kernel -> VFS and process/syscall/security foundations
-> root-volume discovery and mount -> security-admitted userspace init
-> Shared Userspace Services supervisor
-> device/display/input service APIs
-> supported display server/compositor and session manager
-> selected desktop environment -> applications.

This is a target graph, not proof that each edge is currently implemented. The present kernel service-manager API is a lifecycle helper; it is not itself a userspace PID 1, process spawner, or desktop session manager.

## 4. Service contract

Every service registration must have:
- A bounded, validated service identifier and bounded dependency list.
- Explicit start/stop callbacks and a documented context lifetime. The current kernel manager copies service/dependency names but borrows the callback context pointer; the caller must keep that context alive until no callback can run and the registry is reset.
- Declared dependencies, startup order, shutdown order, and a defined failure policy.
- A least-privilege capability set; services must not inherit unrestricted device or filesystem authority by default.
- Structured status/error reporting and bounded retry/backoff policy.
- Deterministic cleanup on partial startup failure and repeated stop/restart.
- Versioned API/ABI contracts before independently updated services are supported.

Supervisor rules:
1. Reject duplicate names, invalid names, duplicate/self dependencies, missing required dependencies, and dependency cycles.
2. Start dependencies before dependants; stop dependants before dependencies.
3. Never silently report RUNNING after a failed callback.
4. Bound retry attempts and prevent tight, unbounded restart loops. Backoff needs a clock/timer facility before timed retry is implemented.
5. Preserve or explicitly document dependant state during restart; propagate failures when recovery is incomplete.
6. Avoid invoking untrusted or blocking service work inside interrupt context or under global kernel locks.
7. Make registry ownership, callback/context lifetime, reentrancy, and concurrency rules explicit. Add synchronization before concurrent registration/lifecycle calls are permitted.
8. Treat service names and metadata as untrusted inputs, and avoid unbounded reads or arithmetic.

The existing kernel service manager may be used for early kernel-owned lifecycle hooks only. A separate userspace supervisor should be introduced when process creation, IPC, capabilities, and executable admission are adequate; do not move desktop service policy into kernel merely to reuse this implementation.

## 5. Display and input contracts

- Keep hardware framebuffer/input access behind kernel-owned device interfaces.
- Validate dimensions, pitch, pixel format, buffer length, pointer/range overflow, and caller permissions on every boundary crossing.
- Make display discovery/query distinct from buffer submission/presentation.
- Define ownership and lifetime for framebuffers, input queues, file descriptors/handles, and session teardown.
- Input events must be attributed to a session/client according to policy; one desktop must not read another session's input by default.
- A passing kernel primitive self-test does not establish physical display output, working keyboard/mouse input, a display server, or a desktop session.

## 6. Session and desktop selection

The userspace session manager should:
- Enumerate installed desktop/session manifests from trusted package metadata.
- Validate executable paths and security admission before launch.
- Launch the selected session as an unprivileged user with a narrow capability set.
- Track session start, readiness, exit status, restart count, and clean logout.
- Avoid restart loops; preserve diagnostic logs and report a clear failure.
- Permit switching desktops by ending or isolating the previous session and releasing its resources.
- Keep per-user preferences separate from privileged system configuration.

A package manager and signed/trusted package metadata are prerequisites for secure installation; do not treat a downloaded desktop manifest as trusted merely because it parses.

## 7. Implementation gates

### Gate A — source contract
- Keep this document aligned with code reality.
- Inventory existing service manager, VFS, ELF loader, process, IPC/capability, display, compositor, and input code.
- Mark each item IMPLEMENTED, SOURCE-REVIEWED, RUNTIME-VERIFIED, or BLOCKED.

### Gate B — service manager correctness
- Compile with the repository's warnings-as-errors flags.
- Exercise registration bounds, caller-owned string mutation, duplicate/missing/self/cyclic dependencies, start/stop order, retry limits, callback failures, restart of a dependency with running dependants, and registry reset.
- Test partial-start failure and cleanup semantics.
- Add synchronization or explicitly assert single-threaded lifecycle operation until synchronization is implemented.

### Gate C — userspace init path
- Discover and mount the real root filesystem.
- Load /init through VFS and security admission, not an embedded test ELF.
- Create a user process/address space and transfer control through the established process/ELF interfaces.
- Verify missing, malformed, rejected, and valid init images fail safely or start correctly.
- Do not mark this gate complete until source wiring and runtime boot evidence both exist.

### Gate D — userspace supervisor
- Implement service manifests, process spawn/wait, IPC, capability assignment, lifecycle tracking, and bounded recovery in userspace.
- Keep kernel code limited to primitive enforcement and interfaces.
- Verify a crash is isolated and does not leave stale handles, device ownership, or orphaned sessions.

### Gate E — display/input/session
- Stabilize versioned display and input APIs.
- Connect one existing display server/compositor as a userspace component.
- Bring up a minimal session, then a supported desktop environment.
- Test display modes, keyboard/mouse input, resize/teardown, multiple sessions where supported, and failure recovery.

### Gate F — package/driver management
- Only after service/process/security foundations are stable, add signed package metadata and desktop install/remove/rollback.
- Later, define high-level driver provider discovery/configuration/supervision without bypassing low-level kernel driver boundaries.

## 8. Verification policy while Debian is offline

Source review and documentation may continue. Do not claim compilation, host tests, ISO generation, QEMU boot, or hardware validation unless the relevant command actually ran and its output was checked. Keep local/self-hosted verification as the preferred path; do not silently switch to GitHub-hosted runners because of billing/quota constraints.

## 9. Current known gaps

- A real root-filesystem -> security-admitted /init -> userspace process handoff is not established as the normal boot path.
- The kernel service manager does not yet replace a userspace PID 1/supervisor.
- Graphics/display/input primitives do not prove a working display server or desktop session.
- Desktop selection, trusted install/update/rollback, and user-session isolation remain future integration work.
- Build and runtime evidence is pending while the local Debian environment is unavailable.
