# GUI Phase 8 — Integration Contract

## What is complete in the kernel foundation

Phase 8 now has the device-facing and rendering foundations needed for a desktop stack:

- display-device lifecycle/present/blank contract
- input-device event queue
- ARGB8888 graphics surface primitives
- memory-backed framebuffer abstraction
- bounded compositor foundation with windows, z-order, opacity and damage tracking
- deterministic boot-time self-tests for all of the above

These are **kernel foundations**, not a desktop environment.

## Required user-space path

The supported architecture is:

```
kernel display/input
        |
        v
user-mode graphics/session service
        |
        +--> X11/Xorg and/or Wayland
        |
        v
XFCE session
        |
        +--> desktop shell
        +--> file manager
        +--> terminal
        +--> settings/security center
        +--> permission UI
```

The kernel must not link Xorg, Wayland, XFCE, a file manager, or a shell.

## Verification gates

The following cannot be honestly marked complete until the prerequisite user-mode
runtime exists:

1. Start a real user-space graphics/session service through the ELF/process startup path.
2. Bind a real framebuffer/display driver to the display contract.
3. Route real keyboard/pointer events into the session service.
4. Integrate an existing X11/Xorg or Wayland stack.
5. Launch an XFCE session.
6. Validate desktop shell, file manager, terminal and settings/security UI.
7. Validate permission prompts through the capability/security broker.
8. Run QEMU/native-boot regression and fix every source/runtime failure before
   declaring the phase production-complete.

No placeholder executable or fake desktop component should be used to turn these
gates green.

## Dependency rule

Phase 8 user-space integration depends on the Phase 7 ELF/process startup and
init/service-manager path. Completing those prerequisites is required before
real XFCE/X11/Wayland integration can be verified.


## 2026-10-05 user-mode hardening status

The current branch hardens the prerequisite process/address-space layer without
claiming a desktop runtime:

- ELF64 validation rejects unsupported ET_DYN/PIE images until a load-bias
  contract exists.
- PT_LOAD file/memory arithmetic is checked before dereference or mapping.
- W+X and ambiguous page-overlapping PT_LOAD mappings fail closed.
- User mappings are restricted to the supported user PML4 range and cannot
  replace an existing PTE.
- The loader zero-fills allocated pages and rolls back every page it mapped on
  allocation/mapping failure.
- The graphics-session ABI is defined in include/abi/gui.h and is intended to
  travel over capability-authorized IPC rather than expose hardware directly.

This is **implemented foundation**, not a claim that init, a userspace graphics
service, X11/Wayland, or XFCE can already execute.

### Phase-8 truth table

| Component | Status |
|---|---|
| Kernel graphics/display/input primitives | COMPLETED |
| User-address-space boundary | IMPLEMENTED AND HARDENED |
| ELF64 validation/load primitive | IMPLEMENTED BUT RUNTIME VERIFICATION PENDING |
| Real first userspace process from filesystem/image | NOT IMPLEMENTED |
| Init/service manager | NOT IMPLEMENTED |
| Userspace graphics/session service | ABI DEFINED; SERVICE NOT IMPLEMENTED |
| Real framebuffer/input provider | DRIVER-DEPENDENT |
| X11/Xorg or Wayland | BLOCKED BY USERSPACE ABI/LIBC/POSIX DEPENDENCIES |
| XFCE | BLOCKED BY USERSPACE ABI/LIBC/POSIX DEPENDENCIES |
