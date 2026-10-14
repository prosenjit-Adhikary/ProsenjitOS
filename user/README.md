# ProsenjitOS User Mode

User-mode programs and services are outside the kernel image.

## Desktop path

ELF loader -> process creation -> init/service manager -> display/input service boundary -> existing Xorg/X11 or Wayland -> XFCE session -> desktop applications.

The versioned display/input contracts live in `include/abi/display.h` and `include/abi/input.h`. They do not grant userspace direct hardware access.

The kernel currently exposes `PROSENJIT_SYS_INPUT_POLL` (syscall 8) for bounded legacy PS/2 keyboard polling. Pass a writable `struct pros_input_event *` in the first syscall argument (`RSI` in the current `int 0x80` ABI). Return value `1` means one event was copied, `0` means no event is pending, and `PROSENJIT_SYSCALL_EBADF` means the output pointer is invalid. `value0` is the set-1 scan code (extended keys add `0x100`); `flags` distinguishes press and release. Mouse events, IRQ-driven input, key-to-text layout, and a userspace input service are not implemented yet.

## Current status

The repository currently has kernel-side process/address-space/syscall foundations, but it does not yet contain a working ELF userspace loader, userspace init/service manager, Xorg/Wayland runtime, or XFCE runtime. Desktop Gate G therefore remains incomplete until these are actually implemented and boot-tested.

Documentation and ABI files are never treated as completion. Every milestone requires source recheck, warnings-as-errors build, QEMU runtime verification, regression coverage, and a recheck that no desktop stack entered `kernel/`.
