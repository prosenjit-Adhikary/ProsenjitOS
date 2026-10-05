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
