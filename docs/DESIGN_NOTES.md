# Design Notes

## Why x86-64?

x86-64 provides a widely documented platform with mature emulators, debuggers, and development tools.

## Why start with BIOS?

A small BIOS boot path makes the first boot milestone easier to understand. UEFI can be added after the core kernel is stable.

## Why a C kernel?

C provides direct memory and hardware access while keeping most kernel code easier to read than assembly. Assembly will be limited to architecture-specific entry, context switching, and other operations that require it.

## Development rule

Every major subsystem should have a documented interface and a small test or diagnostic path before the next subsystem is layered on top.
