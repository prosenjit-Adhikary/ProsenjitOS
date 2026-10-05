# ProsenjitOS GUI Architecture

## Boundary

The desktop environment is **not part of the kernel**. The intended architecture is:

```
Firmware
  -> native bootloader
  -> kernel
  -> memory/address-space/security
  -> process/thread/syscall
  -> ELF loader
  -> init/service manager
  -> userspace graphics/session service
  -> X11/Xorg and/or Wayland
  -> XFCE
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
- Hardened user-address mapping: user mappings cannot target the supervisor
  PML4 slot or silently replace an existing mapping.
- Hardened ELF64 process-image validation/loading policy: arithmetic/range
  checks, W^X enforcement, supported user-address range, conservative
  PT_LOAD page-overlap rejection, BSS zero-fill and rollback.
- Stable capability-mediated graphics-session ABI in `include/abi/gui.h`.

These are kernel interfaces/foundations only; they do not constitute a desktop
environment.

## Status classification

### COMPLETED

- Kernel graphics/display/input abstractions.
- Deterministic graphics/display/input self-tests.
- User-address mapping security boundary.
- ELF64 validation/loading primitive and fail-closed ET_DYN/PIE policy.
- Capability-mediated GUI protocol definition.

### IMPLEMENTED BUT NOT RUNTIME VERIFIED

- End-to-end execution of an ELF image loaded from a real userspace image.
- Userspace graphics/session service implementation.

The loader is integrated into the kernel build, but there is not yet a
filesystem-backed first-process image and service-manager startup path.

### BLOCKED BY EXTERNAL/PLATFORM DEPENDENCY

- Real display/input hardware providers beyond the current abstraction layer.
- X11/Xorg or Wayland execution.
- XFCE execution.

Those require a usable userspace ABI/libc/POSIX/application runtime and real
device providers.

### NOT IMPLEMENTED

- Xorg/Wayland server integration.
- Init/service manager.
- Desktop shell.
- File manager.
- Terminal emulator.
- Settings/security UI.
- Permission prompts.
- Full graphical session startup.

A future ProsenjitOS-specific compositor or shell is optional and is not a
prerequisite for the initial mature-desktop target.

## Remaining integration gates

1. Provide a real userspace filesystem/image mechanism.
2. Start a real user-mode init/service manager through the ELF/process path.
3. Start a userspace graphics/session service using capability-authorized IPC.
4. Bind a real framebuffer/display provider and route real input events.
5. Integrate an existing X11/Xorg and/or Wayland stack.
6. Launch and validate an XFCE session.
7. Validate desktop permission/security policy.
8. Run the full QEMU/native-boot regression suite with no masked failures.
