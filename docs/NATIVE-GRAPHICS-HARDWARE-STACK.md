# ProsenjitOS Native Graphics & Hardware Stack

Status: design/implementation map; source and runtime verification must be checked independently.
Scope: native display, graphics, input, and hardware-facing services for the shared ProsenjitOS desktop. This document does not claim a working graphical session.

## 1. Product boundary

The target desktop has one shared user experience and one shared desktop integration layer:

- Windows applications: `.exe`, `.msi`, and selected Win32 APIs through a future user-space Windows runtime.
- Native applications: ELF executables and native packages.
- Both application families use the same window management, display presentation, keyboard, pointer, clipboard, and audio services when those services are available.
- Native hardware drivers and device policy remain ProsenjitOS-owned. Windows application compatibility must not imply that Windows kernel drivers can be loaded as native drivers.

The graphics/hardware stack is a prerequisite for the shared desktop layer, not a replacement for the process, security, filesystem, package, or service foundations.

## 2. Required dependency direction

```text
Applications (native ELF / Windows PE runtime)
                 |
       Shared Desktop Integration Layer
  window management | input routing | clipboard | audio
                 |
       User-space display/compositor services
                 |
     Versioned user/kernel device ABI + broker
                 |
       Kernel display/input/audio abstractions
                 |
       Native driver model / PCI / hardware
```

Higher layers must use documented lower-layer contracts. Desktop applications and compositors must not access device registers, physical memory, or privileged kernel internals directly. Slow package, network, or reputation operations must not block frame presentation or input dispatch.

## 3. Display and graphics responsibilities

### Kernel/device boundary

The kernel-side display contract is limited to:

- device discovery and native driver binding;
- validation of backend/device capabilities and resource ranges;
- explicit ownership and permission checks for device access;
- a bounded framebuffer or presentation-buffer contract where supported;
- lifecycle handling for device reset, removal, and driver failure;
- safe fallback to a serial/text console when graphics initialization fails.

The existing roadmap records a display abstraction with a framebuffer/backend contract, validation, present path, and lifecycle boundary. That source-level status must not be confused with a complete GPU driver, hardware acceleration, or a usable desktop.

### User-space graphics services

User space owns:

- graphics API/backend selection and rendering policy;
- composition of windows and frame scheduling;
- window surfaces, damage tracking, and presentation policy;
- font/text rendering and scaling;
- display configuration and multi-monitor policy when supported;
- session integration and recovery after compositor failure.

Start with the simplest backend that can be validated on the current boot target. Keep the interface backend-neutral so a software framebuffer backend can precede accelerated GPU backends. Do not assume a specific GPU, graphics API, or hardware acceleration path before the device inventory and current code are audited.

## 4. Input, clipboard, and audio

### Input

Define a versioned event contract for keyboard and pointer devices. Validate event sizes, device identity, key/button state, timestamps, queue bounds, and permissions. Support hot-unplug and lost-device recovery. The desktop service routes events to the active session/window; the kernel does not implement application focus policy.

### Clipboard

Clipboard data is a session-scoped user-space service. Enforce per-session ownership and explicit transfer boundaries. Bound payload sizes and supported formats; do not expose one user's clipboard to another session without an authorized sharing operation.

### Audio

Audio is an optional service until a concrete device/backend is implemented. Keep a stable user-space API for stream lifecycle, format negotiation, and bounded buffering. If audio is unavailable or crashes, graphics and input must continue operating. Never claim audio support based only on PCI class detection.

## 5. Shared service ABI and security

Before desktop integration, define versioned, size-tagged ABI structures and explicit errors for:

- device enumeration and capability queries;
- display mode/backend discovery and buffer/present operations;
- input event delivery and device lifecycle;
- service readiness, shutdown, and health reporting.

Every operation must validate ABI version and structure size, handles/capabilities, user-buffer ranges, integer overflow, resource ownership, and request bounds. Unknown required flags or unsupported operations fail closed with a stable error; optional features are negotiated explicitly. Device grants must be least-privilege and revoked on process exit, session end, device removal, and driver unbind.

Desktop services run in user space under separate identities and bounded restart policies. A crashed compositor must not crash the kernel. Recovery must preserve a text/serial diagnostic route and must not perform destructive repair automatically.

## 6. Staged implementation order

1. **Audit current source**: locate the existing display abstraction, framebuffer/present path, PCI display-class mapping, device permission broker, input code, and related tests. Reconcile roadmap checkboxes against code before changing status.
2. **Freeze ABI contracts**: document versioning, sizes, errors, capabilities, ownership, lifecycle, and compatibility behavior. Add focused host-side tests where supported.
3. **Framebuffer backend**: validate dimensions, pitch, pixel format, buffer length, arithmetic overflow, and present bounds. Add deterministic synthetic tests for malformed modes and buffers.
4. **Input service**: implement/verify keyboard and pointer event contracts, queue bounds, device removal, and session routing.
5. **User-space service lifecycle**: readiness, clean shutdown, crash detection, bounded restart, diagnostics, and resource cleanup.
6. **First desktop integration**: choose one existing window-system path compatible with the current environment; XFCE with an existing X11/Xorg and/or Wayland stack remains the documented candidate, not a guaranteed capability.
7. **Windows GUI bridge**: only after native desktop presentation works, map the selected Windows runtime's supported window/message/drawing/input/clipboard/audio subset into the same desktop services.
8. **Performance and resilience**: record repeatable boot/session, idle CPU/RSS, input latency, frame pacing, IPC, and recovery baselines before optimization.
9. **Hardware expansion**: add accelerated graphics and additional display/input/audio drivers one device class at a time, with documented fallback behavior.

## 7. Verification gates

Use focused checks rather than running every test after every documentation or isolated change.

- **Source/ABI:** compile-time layout checks, version/size rejection, error-contract checks, overflow and invalid-handle tests.
- **Framebuffer:** valid and invalid dimensions/pitch/format/buffer lengths, bounds, and deterministic present markers.
- **Input:** event ordering, queue exhaustion, key/button release, unplug, permission denial, and session isolation.
- **Lifecycle:** service startup failure, crash/restart bounds, shutdown, cleanup, and fallback to text diagnostics.
- **Boot/QEMU:** verify only when an appropriate runner is explicitly available; capture exact commit, command, boot markers, and logs.
- **Hardware:** record device IDs, firmware/VM profile, driver/backend, and unsupported features. QEMU success does not establish physical-hardware compatibility.
- **Regression:** when code touches shared ABI or kernel device boundaries, run the relevant build, boot, driver, security, process, and storage gates before claiming completion.

Track each milestone separately as source-implemented, build-verified, QEMU-verified, hardware-verified, security-tested, or performance-baselined. A missing runner means verification is pending; it is not a pass.

## 8. Current execution constraint

No runner is currently active for this work. Therefore this phase is restricted to repository inspection and reviewable documentation/contract preparation. Do not dispatch jobs, trigger CI intentionally, or claim build, QEMU, or hardware verification. Resume implementation only when a runner is explicitly available, starting with the source audit in step 1 and a focused change plan.


## 10. Initial repository cross-check (source-index evidence; runtime unverified)

The initial GitHub source search found the following existing components and constraints. These are discovery findings, not a complete code audit:

- `kernel/device_broker.c` and `kernel/include/device_broker.h` expose grant, revoke, permission-check, and self-test interfaces. The desktop must consume this broker rather than add direct device access.
- `kernel/kernel.c` invokes the device-broker self-test during kernel initialization, according to indexed source excerpts.
- The roadmap marks the kernel display abstraction as implemented at source level, including framebuffer/backend validation, present path, and lifecycle boundary. This does not establish that a physical or QEMU display backend is fully operational.
- The roadmap still marks the keyboard/input implementation as incomplete. The end-to-end device-to-userspace input event path therefore remains an explicit prerequisite to a usable desktop.
- `user/desktop/desktop_manager.c` and its header provide a bounded, versioned manifest registry/preflight foundation. The repository's desktop implementation log explicitly distinguishes this registry from a session launcher; a registered desktop is not yet proof of a launched graphical session.
- The desktop plan already requires auditing display/input ABI versioning, permissions, errors, and real implementations before integration.

### Audit decisions

1. Reuse the existing device broker, display contract, and DML registry; do not create parallel authorities or duplicate managers.
2. Treat keyboard/pointer event delivery and service lifecycle as prerequisite gaps until source-level contracts and implementations are confirmed.
3. Keep the Windows GUI bridge behind a functioning native desktop session. PE loading, Win32 API coverage, and GUI compatibility are separate milestones.
4. Do not change roadmap completion markers based on search excerpts alone.
5. No build, unit test, CI workflow, QEMU boot, or hardware test was started during this audit because no runner is active.

