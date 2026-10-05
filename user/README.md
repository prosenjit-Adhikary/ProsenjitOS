# ProsenjitOS userspace

The current branch contains a minimal real x86-64 ET_EXEC bootstrap program in
`user/init.asm`.

Build flow:

```
user/init.asm
  -> user/init.elf
  -> binary linker object
  -> kernel image
  -> kernel ELF loader
  -> dedicated process address space
  -> scheduler population
  -> CPL3 entry
```

The bootstrap executable is intentionally not a desktop or fake shell. It only
exercises the published syscall ABI and exits through the real process-exit
boundary.

This embedded bootstrap is the temporary first-process mechanism until the
filesystem-backed userspace image and init/service-manager path exist.
