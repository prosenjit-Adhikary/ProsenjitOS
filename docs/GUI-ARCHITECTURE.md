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
- a display-driver present operation.

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

1. Implement a real framebuffer/display provider for the boot graphics handoff.
2. Connect display discovery/binding to the driver manager.
3. Add a real input provider, starting with keyboard and mouse/HID.
4. Complete ELF/process startup and init/service management.
5. Build the userspace/session boundary needed by an Xorg-compatible stack.
6. Package and integrate XFCE only after the lower layers are executable.
7. Verify boot -> userspace -> display -> desktop in QEMU before marking GUI
   milestones complete.

The custom window-system/compositor path remains intentionally deferred.
