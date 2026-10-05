# GUI Phase 8 — Integration Contract

## Kernel foundation

Phase 8 has the device-facing and rendering foundations required by a future
desktop stack:

- display-device lifecycle/present/blank contract
- input-device event queue
- ARGB8888 graphics surface primitives
- memory-backed framebuffer abstraction
- bounded compositor foundation with windows, z-order, opacity and damage
  tracking
- deterministic boot-time self-tests
- hardened user-address mapping boundary
- hardened ELF64 process-image loading policy
- capability-mediated graphics-session ABI in `include/abi/gui.h`

These are **kernel foundations**, not a desktop environment.

## Required userspace path

```
kernel display/input/framebuffer
        |
        v
capability-authorized userspace graphics/session service
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

The kernel must not link Xorg, Wayland, XFCE, a file manager, a shell, or a
desktop policy engine.

## Verification gates

These gates are not complete until there is a real userspace runtime:

1. Load and enter a real userspace executable through the ELF/process path.
2. Start a real user-mode init/service manager.
3. Start the graphics/session service through the capability boundary.
4. Bind a real framebuffer/display provider.
5. Route real keyboard/pointer events to userspace.
6. Integrate an existing X11/Xorg or Wayland stack.
7. Launch an XFCE session.
8. Validate desktop shell, file manager, terminal and settings/security UI.
9. Run QEMU/native-boot regression and fix every source/runtime failure.

No placeholder executable, fake desktop component, skipped test, or weakened
expected value may be used to turn these gates green.

## 2026-10-05 truth table

| Component | Status |
|---|---|
| Kernel graphics/display/input primitives | COMPLETED |
| User address-space boundary | IMPLEMENTED AND HARDENED |
| ELF64 validation/load primitive | IMPLEMENTED; RUNTIME VERIFICATION PENDING |
| Filesystem-backed first userspace image | NOT IMPLEMENTED |
| Init/service manager | NOT IMPLEMENTED |
| Userspace graphics/session service | ABI DEFINED; SERVICE NOT IMPLEMENTED |
| Real framebuffer/input provider | DRIVER-DEPENDENT |
| X11/Xorg or Wayland | BLOCKED BY USERSPACE ABI/LIBC/POSIX DEPENDENCIES |
| XFCE | BLOCKED BY USERSPACE ABI/LIBC/POSIX DEPENDENCIES |

## Dependency rule

Phase 8 userspace integration depends on the Phase 7 process startup and
init/service-manager path. Mature desktop software must remain userspace and
must not be copied into kernel space.
