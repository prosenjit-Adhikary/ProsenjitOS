# ProsenjitOS Architecture

ProsenjitOS is designed as an independent x86-64 operating-system project. Its architecture takes conceptual inspiration from Windows/NT while using original code and interfaces.

## Layers

```text
+-------------------------------+
| Applications / Shell          |
+-------------------------------+
| User-mode Services            |
+-------------------------------+
| System Call Interface         |
+-------------------------------+
| Executive-style Subsystems    |
| Process | Object | I/O | VMM  |
+-------------------------------+
| Kernel Core                   |
| Scheduler | Interrupts | MM   |
+-------------------------------+
| Hardware Abstraction / Drivers|
+-------------------------------+
| x86-64 Hardware               |
+-------------------------------+
```

## Design principles

1. Keep kernel interfaces explicit and documented.
2. Separate policy from low-level mechanisms where practical.
3. Keep hardware-specific code behind driver/architecture boundaries.
4. Prefer small testable subsystems over one large kernel component.
5. Never copy proprietary Microsoft implementation code.

## Initial target

The first target is a simple x86-64 BIOS boot path that reaches a C kernel entry point and writes a diagnostic message. Later milestones can add UEFI and modern hardware support.
