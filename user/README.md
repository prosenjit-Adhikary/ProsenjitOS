# ProsenjitOS User Mode

User-mode programs and services are outside the kernel image.

## Desktop path

ELF loader -> process creation -> init/service manager -> display/input service boundary -> existing Xorg/X11 or Wayland -> XFCE session -> desktop applications.

The versioned display/input contracts live in `include/abi/display.h` and `include/abi/input.h`. They do not grant userspace direct hardware access.

## Current status

The repository currently has kernel-side process/address-space/syscall foundations, but it does not yet contain a working ELF userspace loader, userspace init/service manager, Xorg/Wayland runtime, or XFCE runtime. Desktop Gate G therefore remains incomplete until these are actually implemented and boot-tested.

Documentation and ABI files are never treated as completion. Every milestone requires source recheck, warnings-as-errors build, QEMU runtime verification, regression coverage, and a recheck that no desktop stack entered `kernel/`.
