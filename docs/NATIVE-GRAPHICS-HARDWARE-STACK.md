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
## 13. Device permission lifecycle review

Source inspected: kernel/device_broker.c, kernel/include/device_broker.h, kernel/driver.c, kernel/process.c, kernel/include/process.h, include/abi/syscall.h, and kernel/syscall.c. This was source review only.

### Findings

- The device broker stores READ/WRITE/CONTROL/MAP permissions using a device ID and a process ID. It supports grant checks, per-device cleanup, and explicit revocation.
- Device unregister calls the broker cleanup routine to remove grants for that device. The inspected public syscall list and dispatcher do not expose broker grant/revoke operations to user processes.
- Process exit and reap close process handles and destroy its address space. In the inspected process lifecycle, there is no call to remove all device-broker grants for that process ID.
- The process manager uses numeric PIDs and can allocate future PIDs after a previous process has been reaped. Since broker grants are keyed by PID, process-lifetime cleanup or a non-reusable lifetime identifier should be reviewed before device grants are used by desktop services.
- This is a source-level lifecycle concern; it has not been reproduced at runtime. The broker registry alone does not prove that display/input services safely map device resources or revoke existing mappings.

### Required design work before user-space display/input access

1. Define idempotent cleanup for every process exit and final reap, or use a process-lifetime identity that cannot be confused with a later process.
2. Keep grant/revoke authority behind a trusted policy boundary; user processes must not be able to authorize their own device access.
3. Specify how device removal affects existing mappings, handles, and service notifications.
4. Validate memory ranges, pixel formats, mapping lifetime, and permissions before any display resource is exposed to user space.
5. Add focused host tests for grant cleanup, PID reuse, device removal, repeated revocation, and denial of unauthorized access before runtime testing.

### Scope and next step

No source code was changed, and no test, build, CI, runner, or QEMU job was started. The next implementation candidate is process-scoped broker cleanup, but only after reviewing all process termination and PID allocation paths. Public syscall numbers and roadmap checkboxes remain unchanged.
## 14. PID-width and teardown-path refinement

Follow-up source review compared the process structure and lifecycle with the device broker's key type. This remains a static review; no code was executed.

- `struct pros_process.pid` is `uint64_t`, while `struct pros_device_grant.process_id` and the broker grant/check/revoke APIs use `uint32_t`. The current broker cannot uniquely represent every valid process ID. Before integrating device services, define one canonical process identity and reject out-of-range IDs at the boundary, or widen the broker key consistently.
- `process_create()` receives an explicit PID and `process_register()` advances `next_pid` based on registered IDs; this inspected path does not itself allocate a PID for callers. The lifecycle risk is therefore about possible reuse by the caller after reap, plus width truncation—not proof that an automatic allocator currently reuses IDs.
- `process_exit()` transitions a process to ZOMBIE after thread termination. `process_wait()` closes handles/destroys the address space, marks it DEAD, then unregisters it. Neither path calls a device-broker per-process cleanup routine in the inspected source.
- Device unbind and device unregister both drop grants for the device. That handles device lifecycle, but it does not replace cleanup for an individual process exiting while the device remains bound.

### Revised safe implementation order

1. Decide the canonical identity type for broker grants; do not silently cast a 64-bit PID to 32 bits.
2. Add a broker cleanup operation scoped to one process identity, with idempotent semantics and a clear result contract.
3. Call it at the correct process-lifecycle point, after verifying cleanup ordering and failure handling. Consider both exit-to-zombie and final reap so no authority remains during a long zombie interval.
4. Add focused tests for PID values above UINT32_MAX, process exit without immediate reap, PID reuse after reap, device unbind/removal, repeated cleanup, and unauthorized grants.
5. Only after those contracts are reviewed should display/input service work consume device grants.

No code change is made in this pass because the runner is unavailable and the cleanup/identity contract needs to be decided before changing security-sensitive lifecycle code.
## 15. Public syscall and handle-boundary review

Reviewed `include/abi/syscall.h`, `kernel/syscall.c`, `kernel/include/object.h`, and `kernel/object.c` against the proposed desktop/device-service boundary. Source inspection only; no runner or runtime verification was used.

- The public syscall enum currently exposes version query, user-test completion, handle close/duplicate, IPC send/receive, and handle transfer. It has no process-create/exec, display, input-event, or device-broker grant/revoke syscall in this inspected ABI.
- The existing handle table is a capability-oriented object reference mechanism: entries hold an object pointer, generation, and rights; close-all releases handles. That is useful infrastructure, but it does not itself create a device grant or prove that a device's mapped resources are revoked when a handle closes.
- `PROS_OBJECT_DEVICE` exists as an object type constant. Its presence alone does not demonstrate a complete userspace device-object implementation, a safe device-resource mapping API, or a policy-authorized path from a handle to broker grants.
- The syscall ABI is explicitly versioned and documents published syscall numbers as stable. Do not insert new public syscall numbers or widen public structures casually; first document versioning, error values, capability checks, and compatibility behavior.
- IPC handles can be transferred under capability checks, so any future display/input service design must specify which service handles and rights are provisioned at startup. It must not let an untrusted client self-issue device grants or infer authority from a numeric PID.

### Boundary decision

Keep the next implementation step at contract/design level until the process identity mismatch and grant cleanup semantics are resolved. Prefer provisioning a narrow trusted broker/service authority through existing object/IPC mechanisms where feasible; add a public syscall only if the audited design shows it is necessary. Before implementation, trace the actual device object creation and mapping path end-to-end rather than relying on the object-type constant.

No syscall, object model, or process lifecycle code was changed in this pass. No build, test, CI, or QEMU job was started.
## 16. Driver and broker lifecycle edge cases

Reviewed the concrete driver bind/unbind, device register/unregister, and broker grant/check/revoke paths. This was static source inspection only.

- Device IDs are allocated monotonically by `pros_device_register()` and are not normally recycled: allocation advances `next_device_id` and refuses exhaustion rather than wrapping. This reduces device-ID aliasing risk, but the process-ID width mismatch remains independent and unresolved.
- `pros_device_grant()` accepts only a currently registered device in `BOUND` state and rejects zero IDs/invalid permissions. The broker API itself does not authenticate the caller; it is safe only while kept behind a trusted kernel policy boundary. Do not expose it as a self-service userspace operation.
- `pros_device_grant()` merges additional permissions into an existing `(device_id, process_id)` grant using bitwise OR. A later request cannot narrow an existing grant; `pros_device_revoke()` removes the entire grant. The policy layer must compute the final least-privilege permission set and explicitly revoke/reissue if a reduction is needed.
- `pros_device_check()` confirms the device still exists and that the grant contains the requested single permission bit, but it does not independently require the device state to be `BOUND`. The reviewed unbind path drops device grants after a successful driver `remove()` callback; unregister also drops grants. The transition and callback ordering must be preserved or tightened when asynchronous service access is introduced.
- If the driver's `remove()` callback fails, `pros_driver_unbind()` returns `EBUSY` before dropping grants or clearing the driver binding. That is a fail-safe against pretending teardown succeeded, but callers need a visible failure/retry state and must not assume resources were removed.
- The inspected driver/device structures contain kernel-side callbacks and `driver_data`/`bus_data` pointers. They are not a userspace device-resource mapping ABI. Do not expose these pointers or callback structures across the process boundary.
- The grant registry is a fixed 64-entry array and its operations show no locking in the inspected implementation. Before concurrent driver, process, and desktop service access is enabled, establish the kernel's serialization/locking rule and test grant/check/revoke against lifecycle transitions.

### Follow-up implementation gates

1. Resolve the 64-bit process PID versus 32-bit broker key mismatch before using grants for real processes.
2. Document who may grant/revoke, how rights are reduced, and what state `check` must require during bind/unbind transitions.
3. Confirm a single serialization rule for the grant table and device lifecycle; add focused tests for merge-versus-revoke, callback failure, unbind/check ordering, and registry exhaustion.
4. Keep the native display/input user ABI separate from driver callback structs and raw kernel pointers.

No code was changed. No runner, build, test, CI, or QEMU job was started.
## 17. Display ABI, PCI discovery, and memory-mapping boundary

Reviewed `kernel/display.c`, `kernel/include/display.h`, `include/abi/display.h`, `kernel/pci.c`, `kernel/include/pci.h`, `kernel/memory.c`, and `kernel/include/memory.h`. This is a source-level contract audit, not a runtime verification.

- The public display ABI declares `PROSENJIT_DISPLAY_ABI_VERSION`, an info structure with a `version` field and `framebuffer_bytes`, and a public format enum. The kernel backend info structure is a separate type: it has no `version` member and names the size field `framebuffer_size`. The current kernel display functions do not establish a validated conversion/negotiation path between these two structures. Do not pass one structure as the other or assume the public ABI is wired into syscalls.
- `pros_display_get_info()` checks nonzero dimensions, maximum dimensions, `pitch >= width`, and nonzero framebuffer metadata. It does not validate supported `format`, compute the minimum byte pitch from format/bits-per-pixel, or check overflow-safe `pitch * height` against framebuffer size. These checks are needed before exposing a stable display contract.
- `pros_display_present()` rejects null/zero input and compares submitted byte count with backend framebuffer size, but it does not itself validate the public ABI's pixel format, row layout, or userspace buffer range. The reviewed API is a kernel-facing backend callback path, not evidence of a safe syscall or mapped userspace presentation interface.
- The in-file display self-test backend advertises a fixed synthetic framebuffer address and validates callback plumbing. This is useful as a self-test fixture, not evidence that a real PCI GPU/framebuffer backend, physical mapping, or desktop presentation is operational.
- `pci_scan()` reads PCI identity, class/subclass/programming-interface, and header type, but the inspected scan structure does not include Base Address Registers (BARs) or resource sizes. `pros_driver_register_pci_device()` maps a discovered class and explicitly discards the supplied slot/function values; the reviewed device structure therefore does not preserve a complete PCI location/resource description for later safe binding.
- `physical_page_is_user_mappable()` only checks pages managed by the allocator and explicitly marked as user-mappable. It is not a general MMIO/framebuffer mapping API, and must not be used as proof that device BARs or reserved physical ranges are safe to map into user address spaces.
- No reviewed public syscall provides display info/present, input event delivery, or device-resource mapping. The versioned ABI header alone is not proof of a reachable userspace service.

### Required gates before userspace display

1. Define one explicit, versioned conversion contract between kernel backend metadata and the public display ABI; validate version, structure size, pixel format, pitch units, and all size calculations with overflow-safe arithmetic.
2. Keep device physical addresses and BARs kernel-owned until PCI resource sizing, reserved-range validation, mapping lifetime, and teardown behavior are designed and implemented.
3. Preserve PCI bus/slot/function identity and validated resource metadata in the device model before attempting a hardware-backed display driver.
4. Add a narrow userspace display service/API with handle-based authorization and buffer validation only after the process identity, broker authority, and grant-cleanup gates in Sections 15–16 are resolved.
5. Keep the synthetic display self-test status separate from real hardware, QEMU, or end-to-end desktop readiness.

No source code, syscall ABI, PCI behavior, memory manager, or roadmap checkbox was changed. No runner, build, test, CI, or QEMU job was started.
## 18. PCI identity preservation and input delivery readiness

Reviewed `kernel/driver.c`, `kernel/include/driver.h`, `kernel/pci.c`, `kernel/include/pci.h`, `kernel/include/driver_class.h`, `include/abi/input.h`, `kernel/device_broker.c`, and `kernel/include/device_broker.h`. Findings are source-level only.

- `pci_scan()` discovers bus, slot, function, vendor/device IDs, class/subclass, programming interface, and header type. However, `struct pros_pci_device` does not retain PCI BAR values or resource sizes, so it cannot yet describe validated MMIO/I/O resources for a hardware driver.
- `pros_driver_register_pci_device()` explicitly discards `bus`, `slot`, and `function` after accepting them. The device model retains hardware ID/class and an opaque `bus_data` pointer, but not a durable PCI BDF. This weakens exact device identity for diagnostics, rebinding, resource ownership, and safe hot-remove. Preserve the BDF as data, not as a substitute for resource validation.
- PCI base-class mapping is conservative, but mapping a device to `PROS_DEVICE_CLASS_DISPLAY` or `PROS_DEVICE_CLASS_INPUT` is classification only. It does not establish that a matching hardware driver is registered, bound, or delivering events/frames.
- `kernel/include/driver_class.h` defines a kernel-side input event (`type`, `code`, `value`) and a `read` callback, with a maximum event count constant. `include/abi/input.h` defines a different public event shape with version, type, timestamp, two values, and flags. No conversion/queue contract connecting these event types was found in the inspected paths.
- The public input header declares key/pointer event types and press/release/relative flags, but the reviewed public syscall ABI does not expose an input event read/wait operation. The declarations therefore do not establish a user session input stream, key/button release guarantees, or device-lost notification delivery.
- `kernel/device_broker.c` keeps a fixed-size grant table and updates it directly; no locking or other serialization is visible in that implementation. The driver registry also mutates shared device state. Before concurrent driver, process, or service access is introduced, define a locking/serialization policy and safe lock ordering for grant checks, bind/unbind, and device removal.

### Required gates before hardware-backed desktop input/display

1. Extend the internal PCI discovery/device representation to retain BDF and validated BAR/resource metadata; keep register access and mapping kernel-owned.
2. Define and implement a versioned kernel-to-public input event conversion, bounded queue semantics, event overflow behavior, timestamp policy, and guaranteed key/button release or device-lost handling.
3. Specify how input/display handles are provisioned to authorized user sessions and how permissions are revoked on process exit, device removal, and failed driver removal.
4. Choose and document synchronization for device registry and broker state before concurrent paths are added; include callback failure and check-vs-unbind races in the future targeted tests.
5. Keep class detection, driver registration, driver binding, and actual event/frame delivery as separate readiness milestones.

No code, public ABI, kernel, or roadmap status was changed. No build, test, CI, QEMU, or runner job was started.