# Desktop / GUI Architecture

## Status

This document records the first implemented GUI foundation. It does **not**
claim a graphical desktop session is bootable yet.

## Current boundary

The kernel now exposes a small display handoff contract:

- a validated framebuffer address;
- width and height;
- pitch;
- bits per pixel;
- a display-driver present operation;
- a real Multiboot2 framebuffer provider registered through the kernel driver model;
- physical framebuffer reservation before the page allocator is used.

The contract lives below any window system. It intentionally does not define
X11, Wayland, a compositor, a window manager, or an application toolkit.

### Intended stack

```
Bootloader
    |
Kernel / display driver
    |
Validated graphics surface
    |
Userspace display/session layer
    |
Xorg-compatible display path (target)
    |
XFCE session (target)
    |
Applications
```

## Safety rules

A display surface is rejected unless:

- the framebuffer address is non-zero;
- dimensions are non-zero and bounded;
- bits-per-pixel is byte-aligned and within the supported range;
- pitch can contain one complete row.

No physical framebuffer is claimed until a real display driver supplies it.
The generic contract therefore cannot turn an unsupported display device into
a falsely "working" GUI.

## Remaining implementation

1. Add hardware input providers (keyboard and mouse/HID) feeding the input-event queue contract now implemented.
2. Complete ELF/process startup and init/service management.
3. Build the userspace/session boundary needed by an Xorg-compatible stack.
4. Package and integrate XFCE only after the lower layers are executable.
5. Verify boot -> userspace -> display -> desktop in QEMU before marking GUI
   milestones complete.

## Implemented foundation additions

- A bounded input-event queue provides deterministic kernel-to-userspace event
  buffering semantics and has a boot-time self-test.
- Framebuffer validation rejects arithmetic overflow in the minimum row-size
  calculation.

These are contracts/foundations only; no hardware keyboard/mouse driver or
bootable Xorg/XFCE session is claimed yet.

The custom window-system/compositor path remains intentionally deferred.
