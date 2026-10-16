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

This document defines the target and work plan; it is not proof of implementation. Based on the current documented repository status, process/address-space and ELF64-loader foundations and a kernel display abstraction exist, while the complete userspace init/service path, end-to-end input route, real window-system runtime, XFCE session, and boot-to-desktop verification must still be checked against source and runtime evidence. Treat any conflicting status documentation as a reason to audit, not as grounds to assume completion.

## 11. Related documentation

- [User-mode architecture](../user/README.md)
- [Project roadmap](ROADMAP.md)
