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

No desktop shell, window manager, file manager, terminal emulator, XFCE code,
Xorg server, or Wayland compositor is linked into the kernel image.

## Implemented foundation

- Bounded display-device registration and mode/present/blank lifecycle.
- Bounded keyboard/pointer event queue with device identity and sequence numbers.
- Bounded ARGB8888 graphics surfaces with clipping-safe fill/copy operations.
- Graphics presentation through the display-device contract.
- Deterministic graphics/display/input self-tests.
- Hardened user-address mapping boundary: user mappings cannot target the
  supervisor PML4 slot or silently replace an existing mapping.
- ELF64 process-image validation/load policy is now fail-closed for integer
  overflow, W+X mappings, invalid user addresses, ambiguous PT_LOAD page
  overlaps and unsupported PIE/ET_DYN images.
- A stable capability-mediated graphics-session ABI is defined in
  include/abi/gui.h for future userspace graphics/session services.

These components are kernel interfaces only; they do not constitute a desktop
environment.

- Bounded display-device registration and mode/present/blank lifecycle.
- Bounded keyboard/pointer event queue with device identity and sequence numbers.
- Bounded ARGB8888 graphics surfaces with clipping-safe fill/copy operations.
- Graphics presentation through the display-device contract.
- Deterministic graphics/display/input self-tests.

These components are kernel interfaces only; they do not constitute a desktop
environment.

## Remaining integration gates

1. A real framebuffer/display driver must provide a validated display device.
2. The ELF loader/process foundation must be connected to a real userspace
   filesystem/image and first-process startup path.
3. A real user-mode init/service manager must own the graphics/session service
   lifecycle.
4. Connect the capability-mediated graphics ABI to a userspace graphics/session
   service; desktop policy must remain outside the kernel.
5. Integrate an existing X11/Xorg and/or Wayland stack without adding a new
   window system to the kernel.
6. Bring up an XFCE session as the initial desktop environment.
7. Add desktop services/UI: file manager, terminal, settings/security center
   and permission prompts.
8. Validate capability/device permissions and security policy across the GUI
   boundary.

### Status classification

- **COMPLETED:** kernel graphics/display/input abstractions, deterministic
  abstraction tests, hardened user mapping boundary, and ELF64 validation/load
  policy described above.
- **IMPLEMENTED BUT NOT RUNTIME VERIFIED:** the current branch's ELF loader
  remains integrated as a kernel loading primitive; a real external userspace
  image and service-manager boot chain are not yet present.
- **BLOCKED BY EXTERNAL/PLATFORM DEPENDENCY:** real hardware display/input
  provider coverage and execution of a mature X11/Wayland/XFCE stack require
  the userspace ABI/libc/POSIX/application compatibility layers.
- **NOT IMPLEMENTED:** Xorg/Wayland server, XFCE, desktop shell, file manager,
  terminal, settings/security UI, and real graphical session startup.

A future ProsenjitOS-specific compositor or shell is optional and is not a
prerequisite for the initial desktop target.

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
