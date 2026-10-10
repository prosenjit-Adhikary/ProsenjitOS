# ProsenjitOS User Mode

User-mode programs and services belong outside the kernel image. The repository
currently has useful kernel-side foundations, but it does **not** yet provide a
bootable general-purpose userspace or a working graphical desktop.

## Existing foundations (source presence is not desktop completion)

- `kernel/elf_loader.c`: kernel ELF64 validation/loading implementation and self-tests.
- `kernel/init.c`: a filesystem-backed `/init` loading API with execution-security admission.
- `kernel/service_manager.c`: kernel-side service dependency/lifecycle registry and self-tests.
- `include/abi/display.h` and `include/abi/input.h`: versioned ABI declarations.
- `kernel/display.c`: display-device binding abstraction with a synthetic test backend.
- `kernel/framebuffer_device.c`: a Multiboot2 RGB framebuffer provider with bounded geometry, address-range validation, MMIO mapping and driver-manager binding. It is a real provider path only when GRUB supplies a compatible framebuffer tag; native BIOS boot and unsupported modes remain without it until verified.
- `kernel/graphics.c` and `kernel/compositor.c`: bounded ARGB8888 surface operations, clipping, alpha composition, window ordering and deterministic self-tests.
- `kernel/input.c`: bounded event-queue/device registry foundation with deterministic self-test; it does not yet read physical keyboard/mouse hardware.

The current boot path builds a small ELF image from the kernel's embedded user-test
section and enters CPL3 for a kernel/user-mode test. That is **not** equivalent to
loading a normal userspace distribution, launching PID 1 from the filesystem, or
running a desktop session. The `pros_init_load()` API exists, but source presence
alone does not prove that the normal boot path invokes it.

## Missing desktop gates

1. A general userspace process/exec and syscall runtime that can launch filesystem-backed programs.
2. A userspace PID 1/init and userspace service/session manager (the existing service registry is kernel-side).
3. A real display backend with a safe userspace presentation/mapping interface; the current display self-test uses a synthetic backend.
4. A real keyboard/pointer input event path; `include/abi/input.h` is only an ABI declaration.
5. Build/package integration for an existing Xorg/X11 or Wayland stack.
6. XFCE session startup, session lifecycle, keyboard/mouse interaction and application launch.
7. End-to-end QEMU/local regression tests that prove the graphical session actually starts.

## Required architecture

ELF loader/process runtime -> userspace PID 1/service manager -> device/display/input
services -> existing Xorg/X11 or Wayland -> XFCE session -> desktop applications.

Do not put XFCE, Xorg/X11, Wayland, a desktop shell, or applications into kernel-core.
Prefer the local Debian/self-hosted runner; do not require GitHub-hosted Actions.
The desktop gate must remain **NOT COMPLETE** until the missing runtime paths are
implemented and the strongest available local/runtime tests pass.
