# ProsenjitOS

**ProsenjitOS** is an independent x86-64 operating-system project developed with the assistance of ChatGPT.

The goal is to build a small, understandable operating system from the ground up while exploring architectural ideas commonly associated with modern Windows/NT systems.

## Vision

ProsenjitOS is not a clone of Windows and does not use Microsoft proprietary source code. It is an original learning and engineering project.

## Current status — v0.1 foundation

- Repository initialized
- Project architecture documented
- Development roadmap created
- Kernel C entry point scaffolded
- VGA text-console output implemented
- Build-system skeleton added

## Architecture

```text
Applications / Shell
        |
User-mode services
        |
System-call interface
        |
Kernel executive-style subsystems
        |
Kernel core: scheduler, memory, interrupts
        |
Drivers / hardware abstraction
        |
x86-64 hardware
```

## Planned kernel subsystems

- Boot and CPU initialization
- Physical and virtual memory management
- Interrupts and timers
- Processes and threads
- Scheduler
- Kernel object/handle model
- System calls
- I/O manager and driver model
- Filesystem and storage
- Networking
- Security and access control
- User-mode services
- Command-line shell

## Repository layout

```text
ProsenjitOS/
├── boot/              # Bootloader and early CPU setup
├── kernel/            # Kernel source and headers
├── drivers/           # Hardware drivers
├── user/              # User-mode programs and services
├── docs/              # Architecture and development documentation
├── scripts/            # Build/development helpers
└── Makefile           # Build entry point
```

## First milestone

The first technical milestone is a bootable x86-64 image that reaches the kernel entry point and displays a diagnostic message. Later milestones will add interrupts, memory management, scheduling, drivers, storage, networking, and user mode.

See [docs/ROADMAP.md](docs/ROADMAP.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## License

MIT
