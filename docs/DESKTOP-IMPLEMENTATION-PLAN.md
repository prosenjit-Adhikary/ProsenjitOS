# ProsenjitOS Desktop Implementation Plan

## Boundary
The desktop environment is userspace. Kernel-core must not contain XFCE,
Xorg/X11, Wayland, a desktop shell, or desktop applications.

## Dependency chain
Display/input device binding
-> userspace graphics ABI/boundary
-> userspace init/service manager
-> Xorg/X11 or Wayland
-> XFCE session
-> desktop applications

## Gates
1. Verify process/address-space/usercopy and ELF execution foundation.
2. Define a stable userspace display/input interface; keep hardware access behind kernel/device boundaries.
3. Add userspace service/session startup infrastructure.
4. Bring up an existing Xorg/X11 or Wayland stack as an external userspace component.
5. Bring up XFCE as the desktop environment.
6. Validate keyboard, mouse, display, session lifecycle and application launch.
7. Add security-policy integration for graphical sessions without privileged desktop shortcuts.
8. Add deterministic QEMU/local regression coverage.

## Verification rule
Every completed gate must be rechecked from source and by the strongest available
local/runtime test. Documentation alone is never treated as implementation.

## Execution constraint
Prefer the local Debian/self-hosted runner. Do not require GitHub-hosted Actions
for desktop development or verification.
