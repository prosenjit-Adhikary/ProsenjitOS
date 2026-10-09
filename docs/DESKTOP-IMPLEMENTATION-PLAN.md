# ProsenjitOS Desktop Implementation Plan

## Boundary
The desktop environment is userspace. Kernel-core must not contain XFCE,
Xorg/X11, Wayland, a desktop shell, or desktop applications.

## Dependency chain
Kernel process/address-space/syscall foundation and ELF loading
-> filesystem-backed userspace execution
-> userspace PID 1 and service/session manager
-> real display backend + userspace display API and keyboard/pointer event path
-> existing Xorg/X11 or Wayland
-> XFCE session
-> desktop applications

## Source-inspected status (2026-10-09)
- Kernel ELF64 loader implementation and self-tests exist.
- Kernel-side service dependency/lifecycle manager and self-tests exist; this is not a userspace PID 1.
- A filesystem-backed `/init` loading API exists, but its existence alone does not prove normal boot integration.
- Display/input ABI headers exist. The display binding self-test uses a synthetic backend; the inspected tree did not show a separate real display backend or input event implementation.
- The boot path constructs a test ELF from the embedded user-test section. This is a CPL3 smoke path, not a general filesystem-backed userspace distribution.
- Existing Xorg/Wayland and XFCE runtimes are not integrated into the current boot path.
- The public `include/abi/syscall.h` currently declares version/test-completion, handle operations and IPC only; it does not define a filesystem open/read/write, process spawn/exec, display-present or input-event wait interface.
- The kernel display contract and public display ABI use the same struct tag name `pros_display_info` with different layouts. A deliberate translation/API boundary is required before exposing display data to userspace; do not pass one structure across that boundary as-is.

## Gates
1. Verify process/address-space/usercopy and ELF execution foundation.
2. Connect filesystem-backed executable loading to a normal process/exec path and test failure/cleanup cases.
3. Implement userspace PID 1 and service/session startup infrastructure.
4. Implement a real display backend and safe userspace presentation contract; keep hardware access behind kernel/device boundaries.
5. Implement keyboard/pointer event delivery to userspace.
6. Bring up an existing Xorg/X11 or Wayland stack as an external userspace component.
7. Bring up XFCE as the desktop environment.
8. Validate keyboard, mouse, display, session lifecycle and application launch.
9. Add security-policy integration for graphical sessions without privileged desktop shortcuts.
10. Add deterministic QEMU/local regression coverage for the full graphical boot.

## Verification rule
Every completed gate must be rechecked from source and by the strongest available
local/runtime test. Documentation alone is never treated as implementation. A QEMU
timeout is acceptable only when the serial trace reaches the expected live checkpoint;
it is not proof of graphical desktop startup.

## Execution constraint
Prefer the local Debian/self-hosted runner. Do not require GitHub-hosted Actions
for desktop development or verification.
