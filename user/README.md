# ProsenjitOS User Mode

User-mode programs and services are outside the kernel image.

## Desktop path

ELF loader -> process creation -> init/service manager -> display/input service boundary -> existing Xorg/X11 or Wayland -> XFCE session -> desktop applications.

The versioned display/input contracts live in `include/abi/display.h` and `include/abi/input.h`. They do not grant userspace direct hardware access.

## Current status

The repository has kernel-side process/address-space/syscall foundations, a kernel ELF loader (pros_elf_load*), and a kernel /init image admission/loading path. These are low-level primitives, not a complete userspace exec/service-launch path. The current public syscall ABI does not expose process spawn/exec, and the existing service manager is kernel-resident rather than the planned userspace init/service manager. The repository does not yet provide a verified userspace display/input service, Xorg/Wayland runtime, or XFCE runtime. Desktop Gate G therefore remains incomplete until the userspace lifecycle and real desktop path are implemented and boot-tested.

Documentation and ABI files are never treated as completion. Every milestone requires source recheck, warnings-as-errors build, QEMU runtime verification, regression coverage, and a recheck that no desktop stack entered `kernel/`.

## Desktop Management Layer (DML) — initial code

The first host-testable DML component is in `user/desktop/desktop_manager.c` and
`user/desktop/desktop_manager.h`. It currently provides a bounded desktop
registry, a size-tagged, versioned manifest contract, safe relative entry-point
path validation, required-feature checks, dependency preflight (including
missing dependency and cycle detection), deterministic dependency-first
resolution with shared dependencies emitted once, default-desktop selection,
and validated session-state bookkeeping.

A backend-driven session orchestrator now lives in `user/desktop/dml_runtime.c`.
It starts the resolved dependency closure in dependency-first order, tracks
opaque process handles, rolls back a partial startup, stops in reverse order,
and retains failed shutdowns for retry. Its backend callbacks are a contract,
not a kernel process implementation. The public syscall ABI still has no
process spawn/exec operation and there is no production backend wired to a real
userspace process API. See [DML runtime contract](../docs/DML-RUNTIME.md).

A first host-testable userspace compositor/window core is present in
compositor.c and compositor.h. It renders into a caller-owned 32-bit pixel
surface and supports bounded windows, z-order, focus, close, and pointer
hit-testing. It is not connected to a kernel framebuffer mapping, real input
drivers, a process launcher, or a graphical session.

The DML therefore remains **not yet a working OS desktop launcher**: it does
not install packages, create real processes on its own, connect to a display
server, route input, or claim XFCE/KDE/GNOME support. Those operations must be
connected to real userspace process, package, permission, display, and input
services before a desktop can be launched.

When a suitable development environment is available, run the isolated host
unit tests with:

```sh
make desktop-unit-test
```

The test target compiles only the DML registry and its unit tests; it does not
build the kernel, create an ISO, or run QEMU. Until it has actually been run,
its results remain unverified.
