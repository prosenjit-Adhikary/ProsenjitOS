# ProsenjitOS Desktop Implementation Plan

## 1. Goal

Build a **ProsenjitOS Desktop Management Layer (DML)**: a replaceable, independently testable userspace subsystem that coordinates desktop environments and their supporting services. Users should eventually be able to install, remove, select, update, and launch supported desktop environments without requiring a kernel change for each desktop.

The DML must first be developed and verified outside kernel-core. Existing working foundations are to be reused wherever possible; this plan does **not** authorize deleting or rewriting them wholesale.

## 2. Non-negotiable architecture boundary

Desktop environments and their management/orchestration logic are userspace software. Kernel-core must not contain XFCE, KDE Plasma, GNOME, Xorg/X11, Wayland, a desktop shell, a compositor, a graphical session manager, or desktop applications.

The DML communicates with the kernel only through explicit, versioned, validated interfaces: syscalls, process/ELF facilities, IPC, device interfaces, and the display/input ABI. “Direct connection to the kernel” means a documented kernel-userspace contract—not linking desktop code into the kernel or granting desktop components unrestricted hardware access.

**Long-term integration rule:** after the userspace DML is stable, review whether any missing low-level primitive or ABI needs to be added to the kernel. Integrate only such minimal kernel primitives when justified. Keep the DML and desktop environments in userspace even in a mature system; placing the full manager in kernel-core would increase kernel attack surface and make desktop failures more likely to affect system stability.

## 3. Target architecture

```text
Firmware / bootloader
        |
Kernel core
  - process and address-space isolation
  - syscalls, scheduler, memory management
  - device discovery, drivers, permissions
  - minimal display/input/device interfaces
        |
Stable kernel-userspace contracts
  - ELF/process execution and IPC
  - versioned display and input ABI
  - VFS and service communication
        |
Userspace init + service manager
        |
ProsenjitOS Desktop Management Layer (DML)
  - desktop/package registry and selection
  - dependency and compatibility checks
  - session orchestration and lifecycle
  - display/window-system integration
  - input routing and device/service coordination
  - permissions, health checks and recovery
        |
Supported window system / graphics stack
  - existing Xorg/X11 or Wayland components, where compatible
        |
Installable desktop environment
  - initial target: XFCE
  - future candidates: KDE Plasma, GNOME, other compatible DEs
        |
Desktop applications
```

This is a target architecture, not a statement that all listed components already exist or currently run. Each component must pass its implementation and verification gates before being marked complete.

## 4. Responsibilities of the DML

### 4.1 Desktop and package management
- Maintain a registry of installed desktop environments, versions, required components, and supported session entry points.
- Provide install, remove, update, select-default, and launch operations through a future package-management interface.
- Validate package origin/integrity, dependency availability, version constraints, and compatibility before activation.
- Keep desktop-specific files and dependencies outside the kernel image.
- Fail safely when a package is incomplete or incompatible; do not silently substitute fake implementations or stubs.

### 4.2 Session and service lifecycle
- Start only after the userspace process/ELF path and init/service manager are operational.
- Resolve declared service dependencies and start services in a deterministic order.
- Launch a selected desktop session with a defined user identity, environment, working directory, and inherited capabilities.
- Track startup, running, shutdown, and failure states; collect diagnostic logs.
- Shut down or restart failed user-space components without panicking or corrupting the kernel.
- Support clean logout, session restart, and switching desktops only after the necessary isolation and cleanup behavior is implemented.

### 4.3 Display, graphics, and window-system integration
- Reuse the kernel display/framebuffer/backend foundation and versioned display ABI where valid.
- Keep hardware discovery, privileged register access, memory mapping decisions, and device permissions behind kernel/device-service boundaries.
- Define the userspace graphics/display service contract before integrating a window system.
- Prefer mature existing Xorg/X11 or Wayland components rather than inventing a new windowing system or compositor at this stage.
- Do not claim a window system is supported until its dependencies, required kernel interfaces, startup, rendering/presentation, and failure behavior have been verified on this OS.

### 4.4 Input
- Reuse and audit the existing input ABI and driver/device foundations.
- Implement the complete event path from device/driver boundary to the userspace input service and active session.
- Define event ownership, permissions, focus/routing, device removal, and error handling.
- Verify keyboard and pointer input in the real session; the existence of an ABI header alone is not input support.

### 4.5 Security and isolation
- Run desktop components with the least privilege they require.
- Use existing process isolation, handles/capabilities, IPC, VFS, and security admission boundaries where available.
- Do not grant a desktop environment unrestricted kernel or device access for convenience.
- Validate executable/package inputs and session configuration; keep privileged operations in narrow, auditable services.
- Ensure a desktop crash or malformed request cannot bypass kernel validation or compromise other users/sessions.

### 4.6 Health, diagnostics, and recovery
- Record explicit startup milestones and meaningful failure reasons.
- Apply bounded restart policies to prevent crash loops.
- Preserve logs and distinguish “component missing,” “unsupported dependency,” “service failed,” and “runtime not verified.”
- Recovery must follow **Detect -> Verify -> Repair -> Verify -> Rollback** where applicable; destructive repair is disabled by default.
- Never report a desktop as ready solely because a script found matching filenames, symbols, or documentation.

## 5. Existing implementation: preserve and migrate incrementally

No component is to be deleted merely because the DML architecture changes. Audit its actual role, dependencies, and implementation first.

| Existing area | Planned treatment |
| --- | --- |
| Process, address-space, scheduler, syscall, IPC foundations | Preserve; verify that userspace services can safely use them. |
| ELF64 loader and user-entry foundation | Preserve and reuse; check the current source and runtime evidence. Dynamic relocation or other unsupported ELF features remain explicitly unsupported until implemented. |
| Kernel graphics/display abstraction and framebuffer/backend contracts | Keep low-level device and presentation primitives in the kernel where appropriate; expose only a stable, validated userspace boundary. |
| Versioned display/input ABI headers | Audit compatibility, versioning, error handling, permissions, and actual implementations. A header is not a working service. |
| PCI/device/driver model and device permission broker | Extend existing infrastructure; do not replace it with desktop-specific hardware access. |
| Existing init/service-manager work | Inspect and reuse if suitable; refactor or complete the userspace lifecycle path instead of duplicating it. |
| Desktop-specific logic accidentally placed in kernel-core | Identify exact dependencies, create a userspace replacement, migrate callers in small steps, then remove the kernel copy only after equivalent behavior and regression coverage are demonstrated. |
| Existing tests, build scripts, security checks, Native ProsenjitFS, bootloader, and unrelated drivers | Preserve; adjust only where the DML integration requires it and keep their regression gates. |

Before moving code, record its current behavior and callers. For every migration, use a focused commit, build/test the affected boundary, and retain a clean rollback path. Avoid a large, all-at-once rewrite.

## 6. Dependency chain and implementation phases

### Phase 0 — Source audit and baseline
- [ ] Inspect current source, ABI headers, build rules, init/process paths, driver boundaries, and desktop-related scripts.
- [ ] Reconcile documentation status with source; treat conflicting or stale status text as unresolved until checked.
- [ ] Record existing tests, known failures, and the last verified commit.
- [ ] Establish a component matrix: implemented, partial, missing, blocked, and runtime-verified.
- **Exit gate:** no planned deletion/move without a source-backed reason and identified replacement.

### Phase 1 — Stable kernel/userspace contract
- [ ] Verify process/address-space isolation, user-copy validation, IPC, ELF loading, and safe user entry.
- [ ] Audit/version display and input ABI contracts and define errors, permissions, ownership, and lifecycle.
- [ ] Keep privileged hardware access in kernel/device boundaries.
- **Exit gate:** small userspace test services can use the supported contracts without bypassing kernel validation.

### Phase 2 — Userspace startup and shared services
- [ ] Implement or complete init/service-manager startup and service dependency handling.
- [ ] Define configuration, logging, service status, and orderly shutdown/restart.
- [ ] Bring up minimal display and input services behind the established contracts.
- **Exit gate:** services start deterministically, report useful failures, and stop/restart safely.

### Phase 3 — DML core
- [ ] Create the DML as a separate userspace component with documented internal interfaces.
- [ ] Implement desktop registry/selection, dependency validation, session launch, lifecycle tracking, logs, and bounded recovery.
- [ ] Add package-management integration only when the underlying package/install trust and rollback mechanisms are ready.
- [ ] Ensure adding a desktop is configuration/package integration, not a kernel source change, unless a genuinely missing low-level ABI is demonstrated.
- **Exit gate:** DML can validate a session definition, reject invalid/incompatible definitions, and manage a test userspace service without destabilizing the kernel.

### Phase 4 — First real desktop integration
- [ ] Select a compatible existing Xorg/X11 or Wayland route based on actual dependency and ABI feasibility.
- [ ] Integrate the chosen window system as an external userspace component; do not implement a custom window system as a shortcut.
- [ ] Bring up XFCE as the initial target only after the required window-system and service prerequisites work.
- [ ] Verify real display presentation, keyboard/pointer events, terminal/application launch, logout, and session shutdown.
- **Exit gate:** boot-to-session works in a reproducible environment with logs and no kernel-boundary violation.

### Phase 5 — Additional desktop environments
- [ ] Document supported versions and dependencies for each candidate desktop.
- [ ] Add KDE Plasma, GNOME, or other environments only after their runtime, graphics stack, services, and dependencies are supported.
- [ ] Test install/remove/update, default-session switching, missing dependencies, failed startup, and recovery.
- **Exit gate:** each advertised desktop has independent, repeatable acceptance evidence; unsupported environments are clearly reported as unsupported.

### Phase 6 — Hardening and release
- [ ] Add malformed configuration/package tests, permission checks, failure injection, and crash-loop tests.
- [ ] Run warnings-as-errors builds and all affected regression/security tests.
- [ ] Run QEMU and local hardware/runtime tests when a suitable runner is available.
- [ ] Review whether any minimal kernel ABI/primitive is missing; do not move the DML itself into kernel-core.
- [ ] Update roadmap and status documents only from observed source and test evidence.
- **Exit gate:** every release claim is backed by reproducible test evidence; known limitations are documented.

## 7. Verification and reporting rules

For each gate, report these separately:
1. **Implemented:** source exists and is wired into the intended path.
2. **Build-verified:** relevant build completes with the required warning/error policy.
3. **Runtime-verified:** the actual behavior was observed in QEMU or suitable hardware.
4. **Regression-verified:** existing affected tests still pass.
5. **Security/boundary-verified:** desktop code remains outside kernel-core and requests are validated.

A passing text-search script, a header, a design document, or a successful compile alone does not prove a working desktop. Do not mark a gate complete until its stated acceptance evidence exists.

## 8. Test strategy and execution constraints

- Prefer the local Debian/self-hosted runner when available; do not make GitHub-hosted Actions a mandatory dependency for desktop development.
- If the local Debian system is unavailable, continue source audit, documentation, and changes that can be validated safely, but mark QEMU/hardware runtime verification as pending. Never imply it was run.
- Keep test logs and reports as artifacts where possible; report exact command/result and the commit tested.
- Run focused tests after each migration, then the full relevant build/boot/security regression suite.
- Do not bypass failing tests, fabricate success markers, weaken security checks, or change unrelated components to make a gate appear green.

## 9. Initial dependency chain

Display/input device binding
-> stable kernel-userspace display/input contracts
-> process/ELF execution
-> userspace init/service manager
-> shared userspace display/input services
-> DML session orchestration
-> existing Xorg/X11 or Wayland (compatible route)
-> XFCE initial target
-> desktop applications

The chain may be developed in parallel only where dependencies permit; downstream desktop integration must not be declared complete while an upstream gate is missing.

## 10. Current status

This document defines the target and work plan; it is not proof of a complete desktop implementation. The host-testable DML registry/preflight source exists under `user/desktop/`, with a dedicated unit-test source and `make desktop-unit-test` target. It validates bounded manifests and dependency graphs and selects a default entry; it does not install packages, spawn processes, launch a window system, or run a desktop session. A source audit found and fixed a missing error return in `pros_dml_select()` for an uninitialized/corrupt manager; regression cases for an uninitialized manager and an out-of-range registry count were added to `tests/desktop_manager_test.c` (commits `195f2b7` and `d4d0c47`). The `scripts/desktop_complete.sh` prerequisite gate now runs the DML host unit-test target, treats QEMU timeout as expected only when required boot milestones are present, and explicitly reports that a passing prerequisite gate does not prove GUI runtime. The host unit test and gate have not yet been executed after these changes, so build/runtime verification remains pending. The complete userspace init/service path, end-to-end input route, real window-system runtime, XFCE session, and boot-to-desktop verification still require source and runtime evidence. Treat conflicting status documentation as a reason to audit, not as grounds to assume completion.

## 11. Related documentation

- [User-mode architecture](../user/README.md)
- [Project roadmap](ROADMAP.md)

## 12. Stability, compatibility, and service contracts

These are release requirements, not optional future polish.

### 12.1 Versioned ABI and capability negotiation
- Every kernel-userspace ABI and shared-service protocol must define a version, structure sizes, supported feature bits, error codes, ownership rules, and lifecycle semantics.
- Prefer additive, size-tagged structures and explicit capability negotiation. Reject unknown required features and malformed sizes safely; never guess a caller's structure layout.
- Preserve backward compatibility within a declared compatibility policy. When a breaking change is unavoidable, version it explicitly and test both supported versions during migration.
- Validate every user pointer, length, integer conversion, handle, permission, state transition, and cross-process message at the trust boundary. Do not trust userspace validation as a substitute for kernel validation.

### 12.2 Service ownership and failure domains
- Define one authoritative owner for each resource (display device, input device, seat/session, audio device, package database, and desktop-session state). Use explicit leases/handles and deterministic hand-off rather than competing independent owners.
- Keep policy/orchestration separate from mechanism: DML chooses and supervises sessions; dedicated services perform narrowly scoped device or system operations.
- Services must declare dependencies, readiness checks, restart policy, resource limits, and shutdown behavior. Avoid circular dependencies; detect cycles and fail with an actionable diagnostic.
- A crashed or wedged optional service must not panic the kernel or silently corrupt global state. Use bounded timeouts, cancellation, queue limits, and restart budgets. Do not automatically restart a service forever.
- Separate per-user/session state from system-wide state. One user's desktop must not be able to inspect or control another user's session without explicit authorization.

### 12.3 Desktop manifests and compatibility
- Each installable desktop must provide a validated manifest containing package identity/version, entry point, required and optional services, dependency/version constraints, supported ABI/features, session environment, permissions/capabilities, resource expectations, and health-check/shutdown hooks.
- Parse manifests with bounded input sizes, strict schema validation, safe path handling, and rejection of unknown security-critical fields.
- Resolve dependencies deterministically; detect cycles, conflicts, missing capabilities, and incompatible versions before changing the active session.
- Maintain a tested compatibility matrix per desktop version, window system, graphics backend, input backend, ABI version, and supported hardware/virtual-machine profile. “Installable” must not be presented as “runtime supported” without acceptance evidence.

## 13. Security, updates, and recovery requirements

- **Threat model:** assume desktop applications, downloaded packages, user configuration, IPC messages, and optional services can be malicious or malformed. Kernel and privileged brokers remain the final enforcement points.
- **Least privilege:** use per-service identities and capability/handle grants scoped to the required operation and lifetime. Avoid ambient authority and unrestricted device access. Revoke grants on process exit, session end, device removal, and package retirement.
- **Package trust:** verify package signatures against an explicit trust policy where signatures are available; verify content hashes and package identity; reject path traversal, symlink escapes, duplicate/conflicting files, and unsafe archive entries. A hash alone proves identity, not publisher authenticity.
- **Transactional installation/update:** stage files separately, validate dependencies and permissions, preserve the last known-good manifest/configuration, then activate atomically where the filesystem supports it. On any failure, restore the previous known-good state. Never overwrite a running component in place unless its update protocol explicitly supports that operation.
- **Rollback safety:** bind rollback metadata to package identity/version and integrity checks; prevent downgrade to a known-vulnerable version when policy forbids it. Do not delete the last known-good desktop or recovery path until the new session passes its health gate.
- **Configuration safety:** validate configuration before activation, write changes atomically where supported, retain a bounded last-known-good configuration, and recover without destructive repair by default.
- **Secrets:** never place credentials or long-lived tokens in desktop manifests, command-line arguments, public logs, or world-readable configuration.
- **Auditability:** log security-relevant install, permission, session, failure, and rollback events with bounded storage, clear reason codes, and privacy-conscious redaction. Logging failure must not deadlock a privileged path.
- **No false assurance:** signing, sandboxing, isolation, or secure update must remain marked incomplete until implemented and tested end-to-end.

## 14. Performance and resource-efficiency requirements

Performance must be measured against a recorded baseline on a named build and machine/VM profile; do not invent universal millisecond targets before the first baseline exists.

- Keep the DML event-driven where practical. Avoid busy loops, polling without a bounded interval, and repeated full scans of installed desktops or dependencies on every session event.
- Cache validated manifests/dependency results with explicit invalidation on install, remove, update, or configuration changes. Never let stale cache entries bypass security checks.
- Keep kernel crossings and IPC messages coarse enough to avoid per-frame/per-input-event management overhead where a safe batching mechanism is appropriate. Input latency and frame presentation must be measured end-to-end.
- Bound memory, message queues, log buffers, restart attempts, manifest sizes, package sizes, dependency traversal, and recovery work. Handle exhaustion predictably rather than allocating without limits.
- Keep desktop orchestration out of latency-sensitive kernel paths. Use asynchronous work for slow package, disk, and diagnostic operations; do not block input/render paths on network access or reputation lookups.
- Establish repeatable measurements for boot-to-login, session startup, idle CPU, resident memory per service, input-to-visible-response latency, frame pacing/dropped frames, IPC throughput/latency, package install/update time, and crash recovery time.
- Compare against a documented baseline after changes. Investigate meaningful regressions; optimize measured bottlenecks rather than adding speculative complexity.
- Provide graceful degradation when optional audio, network, or reputation services are unavailable. The core desktop session must not wait indefinitely for optional network services.

## 15. Acceptance gates and release checklist

Do not call the design “perfect” or release-ready merely because its documents are complete. The goal is a defensible, testable architecture with known limits.

### Required automated and manual evidence
- [ ] ABI compatibility tests: old/new supported clients, malformed versions/sizes, unknown feature flags, invalid handles, and invalid user buffers.
- [ ] Service lifecycle tests: dependency ordering/cycle rejection, readiness timeout, clean shutdown, crash, hang, bounded restart, and resource cleanup.
- [ ] Isolation tests: denied cross-user/session access, least-privilege device access, capability revocation, and malformed IPC.
- [ ] Package tests: signature/trust failures, tampered hashes, archive/path traversal, conflicting files, missing dependencies, interrupted installation, failed activation, rollback, and forbidden downgrade.
- [ ] Desktop manifest tests: invalid schema, oversized input, unsafe paths, dependency cycles, missing capabilities, and unsupported ABI.
- [ ] Functional desktop tests: boot to login, render/present, keyboard and pointer input, focus/routing, launch native terminal/application, logout, switch supported sessions, and recover after a session crash.
- [ ] Performance baseline: boot/session latency, idle CPU/RSS, input latency, frame pacing, IPC, install/update, and recovery under a documented test profile.
- [ ] Stress/fault tests: repeated session switching, service restarts, memory/queue exhaustion, device removal, storage errors, and interrupted updates.
- [ ] Regression evidence for existing boot, process/ELF, VFS/filesystem, driver, and security paths touched by the work.
- [ ] QEMU and suitable hardware tests, with exact commands, commit IDs, logs, and limitations recorded.

### Release decision
A release can be described as **verified for a specific configuration** only when the required gates pass for that configuration. Maintain separate status labels for source-implemented, build-verified, QEMU-verified, hardware-verified, security-tested, and performance-baselined. Missing test infrastructure is a blocker to a verification claim, not evidence of success.

## 16. Implementation order and change discipline

1. Audit source and reconcile status documentation; do not assume checkboxes reflect current source behavior.
2. Freeze and document the smallest usable ABI contracts before integrating a desktop stack.
3. Complete process startup, init/service management, permissions, and shared display/input service prerequisites.
4. Implement DML manifests, dependency resolution, lifecycle supervision, diagnostics, and safe failure behavior.
5. Add package trust and transactional update/rollback before exposing general install/update operations.
6. Integrate one compatible window-system route and one desktop (XFCE is the initial candidate) end-to-end.
7. Establish security, stability, and performance baselines; only then add additional desktop environments.
8. Make small, reviewable commits. For every migration, preserve unrelated functionality, run focused checks, run relevant regressions, and keep a rollback path.
9. If the Debian/QEMU environment is unavailable, continue source/documentation work but leave runtime, hardware, and performance gates explicitly pending.

