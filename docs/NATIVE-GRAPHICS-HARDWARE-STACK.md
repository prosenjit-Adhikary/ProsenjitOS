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



## 9. Out of scope for this phase

- Replacing the existing desktop/windowing stack with a new compositor.
- Writing a GPU driver without a verified target and documented device contract.
- Direct loading of Windows `.sys` drivers.
- Claiming universal `.exe`/GUI compatibility.
- Marking the desktop complete based on architecture documents alone.
- Unrelated kernel, ProsenjitFS, bootloader, or security changes.

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

## 11. Focused source audit — display, input, and launch boundary

This pass inspected the current branch's full contents for `kernel/display.c`,
`include/abi/display.h`, `include/abi/input.h`,
`kernel/include/driver_class.h`, `include/abi/syscall.h`,
`kernel/syscall.c`, `user/README.md`, and the desktop implementation log.
It was source inspection only; no compiler, tests, CI, runner, or emulator was invoked.

### Display: present, but not yet a desktop-facing service

- `kernel/display.c` has a backend-binding path, a mode/info query, a present call,
  and a synthetic self-test backend. The self-test verifies the contract against
  an in-memory fake backend; it does not establish real hardware output.
- The checked mode path bounds width and height and checks minimum pitch and
  nonzero framebuffer metadata. It does not, in the inspected function, validate
  pixel-format semantics or prove that pitch × height fits the reported buffer
  using overflow-safe arithmetic.
- The present path rejects a zero buffer and sizes greater than the backend's
  reported framebuffer size, then delegates to the backend. This is not yet a
  complete userspace presentation ABI: user-buffer validation, ownership,
  format/layout agreement, and a user-accessible service path still need to be
  specified and verified.
- `include/abi/display.h` defines a version constant and metadata/event
  structures, but the inspected public syscall ABI does not expose display
  operations. The relationship between this userspace ABI and the kernel
  backend structures needs an explicit, reviewed adapter/contract rather than
  assuming the similarly named structures are interchangeable.

### Input: data shapes exist; delivery path remains missing/unverified

- `include/abi/input.h` defines versioned event fields and key/pointer event
  types. `kernel/include/driver_class.h` defines a driver read callback and
  a separate kernel-side event structure.
- The inspected public syscall list contains no input-read/event-delivery syscall.
  The inspected code does not establish a concrete keyboard/pointer driver feeding
  an event queue and delivering validated events to a userspace session.
- Before implementing this path, reconcile the two event representations, define
  event-size/version checks, queue ownership and bounds, device removal, key/button
  release semantics, and access control. Do not add an ABI syscall until the
  existing IPC/handle and process contracts have been checked for a suitable path.

### Process and service startup: explicit blocker

- `user/README.md` states that the kernel has process/address-space/syscall
  foundations and ELF/init-image loading primitives, but not a complete
  userspace exec/service-launch path. The inspected public syscall set contains
  handle and IPC calls but no process spawn/exec operation.
- The DML code is a manifest registry and dependency preflight/selection layer.
  Its manifest entry point is metadata; it does not open or execute the entry.
  It must not be extended into a pretend launcher before a real process creation,
  identity/capability, IPC, and service lifecycle contract is identified.

### Ordered next work (still no runner)

1. Trace existing process creation, ELF loading, init image admission, handles,
   and IPC callers from source; identify which pieces are callable from userspace
   and which remain kernel-internal.
2. Audit the existing driver/device lifecycle and permission revocation path before
   wiring display/input services to it.
3. Write a small ABI decision note for display and input: structure sizes,
   version handling, pixel formats, safe buffer validation, event queue bounds,
   device ownership, and error values. Do not alter public syscall numbers yet.
4. Only after the source contracts are understood, propose one narrow implementation
   change with focused host tests. Runtime verification remains blocked until a
   runner is explicitly available.

No source-code behavior, roadmap checkbox, syscall number, kernel component, or
ProsenjitFS/bootloader code was changed in this pass.

## 12. Process, IPC, and device-permission source trace

This pass inspected `kernel/process.c`, `kernel/include/process.h`,
`kernel/init.c`, `kernel/elf_loader.c`, `kernel/ipc.c`,
`kernel/include/ipc.h`, `kernel/device_broker.c`, and the IPC cases in
`kernel/syscall.c`. It was source-only inspection; no execution was started.

### Confirmed reusable foundations

- `process_create()` creates/registers a process object and address space, assigns
  parent linkage and default policies, and sets the process READY. It does not
  itself load an executable or establish a complete user thread/exec transaction.
- `pros_init_load()` reads `/init` from a mounted VFS, admits it through the
  executable-security path, validates it as ELF, and calls `pros_elf_load()`.
  This is a kernel-mediated init-image loading primitive, not a general userspace
  spawn/exec API.
- ELF validation rejects unsupported dynamic/interpreter/TLS program headers and
  checks segment bounds and permissions. Compatibility must be based on the
  loader's actually supported ELF subset, not on the ELF file extension alone.
- IPC is already exposed through public send/receive syscalls and process handles.
  The syscall path validates user ranges and handle type, authorizes read/write
  intents, copies data across the user/kernel boundary, and uses a bounded channel.
  The current message maximum is 64 bytes and channel capacity is 8 messages.
  IPC is a potential control/event transport, but its small message size and
  lifecycle/ownership semantics must be considered before choosing it for graphics.
- The device broker offers process/device grants and revoke/drop operations. It
  is not, by itself, a userspace display/input API. Any future adapter must define
  who is allowed to grant/revoke, ensure teardown removes grants, and reconcile
  the broker's 32-bit process ID parameter with the process model's 64-bit PID
  without truncation or ambiguous identity.

### Still not established by source inspection

- A user-callable transaction that creates a process, admits and loads an image,
  creates its initial thread, applies least-privilege policy, and safely publishes
  it as runnable.
- A userspace init/service manager that launches and supervises services.
- A display/input service with a defined IPC protocol, device grants, queue
  ownership, and shutdown/removal behavior.
- A complete real-device keyboard/pointer event path or physical display output.

### Next safe step

Before implementing new public ABI or changing kernel behavior, map the existing
thread creation and scheduler handoff to the ELF load result, and inspect service
manager definitions/callers to establish whether any existing lifecycle primitive
can be reused. Then draft a narrow launch-transaction proposal with rollback rules
for every failure point. Keep the native desktop ahead of the Windows GUI bridge.
No source code or roadmap checkbox was changed in this pass.
## 12. Follow-up source trace — process, init, IPC, and service manager

This follow-up inspected kernel/process.c, kernel/include/process.h, kernel/elf_loader.c, kernel/init.c, kernel/service_manager.c, kernel/include/service_manager.h, kernel/ipc.c, and the IPC cases in kernel/syscall.c. This is a source trace, not runtime evidence.

### What exists

- process_create() allocates and registers a process with an address space, security level, syscall policy, and handle table. process_exit() and process_wait() implement lifecycle transitions and reaping at the kernel API level. These are useful primitives, but the inspected public syscall list does not let a user process request a new process or perform exec.
- pros_elf_load() validates an ELF64 image, maps its loadable segments, creates a dedicated initial user stack page, and records the entry and stack addresses. pros_elf_load_with_args() is a kernel API for image loading/argument setup, not evidence of a user-callable process-launch service.
- pros_init_load() reads /init from an active VFS mount, applies file admission and execution-security checks, validates the ELF, and loads it into a supplied process. The function loads an image; by itself it does not establish a general init/service supervisor that launches arbitrary user services.
- The kernel service manager has dependency ordering, start/stop callbacks, bounded retries, and state reporting. Its callback types run functions supplied by registered kernel code; this is not a userspace service manager and does not create isolated processes for registered services.
- IPC has bounded message/channel primitives and public send/receive syscalls mediated by handles, user-range validation, and security authorization. This is a possible foundation for future service communication, but a complete userspace service bootstrap and safe channel/handle provisioning path has not been established by this audit.

### Architecture consequence

Do not create a second service manager or pretend that the kernel callback manager is a desktop supervisor. The next design should bridge existing process/ELF and IPC primitives through an explicit, security-reviewed launch contract. That contract must define who may create a process, how executable bytes are admitted, how PID/parent relationships are established, how initial handles/capabilities are passed, how startup failure and exit are reported, and how service restart and shutdown are bounded. Any new syscall or ABI must wait until these ownership and authorization rules are reviewed.

### Current status after this trace

- **Source foundation found:** process lifecycle APIs, ELF loading, /init admission, kernel callback service manager, and handle-mediated IPC.
- **Still missing or not proven:** general user-callable spawn/exec; a userspace init/service supervisor; safe initial IPC/handle provisioning for user services; concrete keyboard/pointer event delivery; userspace display service and actual desktop session.
- **No runtime claims:** no build, test, CI, QEMU, or hardware run was started. No source code, syscall number, roadmap checkbox, ProsenjitFS, or bootloader was changed in this pass.