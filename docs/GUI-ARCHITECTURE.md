# ProsenjitOS GUI Architecture

## Boundary

The desktop environment is **not part of the kernel**. The kernel exposes only the
minimum device-facing contracts needed by user-space graphical services:

```
Hardware
  -> native driver/device layer
  -> display + input kernel abstractions
  -> user-space graphics/window service
  -> X11/Xorg and/or Wayland integration
  -> XFCE desktop session
  -> applications
```

No desktop shell, window manager, compositor, file manager, terminal emulator,
or XFCE code is linked into the kernel image.

## Implemented foundation

- Bounded display-device registration and mode/present/blank lifecycle.
- Bounded keyboard/pointer event queue with device identity and sequence numbers.
- Bounded ARGB8888 graphics surfaces with clipping-safe fill/copy operations.
- Graphics presentation through the display-device contract.
- Deterministic graphics/display/input self-tests.

These components are kernel interfaces only; they do not constitute a desktop
environment.

## Remaining integration gates

1. A real framebuffer/display driver must provide a validated display device.
2. User-mode process startup and ELF loading must be stable enough to host
   graphical services.
3. A user-space graphics/window service must own windows, composition and
   damage policy.
4. Integrate an existing X11/Xorg and/or Wayland stack without adding a new
   window system to the kernel.
5. Bring up an XFCE session as the initial desktop environment.
6. Add desktop services/UI: file manager, terminal, settings/security center
   and permission prompts.
7. Validate capability/device permissions and security policy across the GUI
   boundary.

A future ProsenjitOS-specific compositor or shell is optional and is not a
prerequisite for the initial desktop target.
