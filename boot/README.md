# Boot subsystem

This directory contains the early boot path. The initial implementation targets x86-64 BIOS boot for simplicity and educational value. UEFI support is planned later.

The boot path will eventually:

1. Start in 16-bit real mode.
2. Load the kernel from disk.
3. Install descriptor tables and paging structures.
4. Enter 64-bit long mode.
5. Transfer control to the kernel entry point.
