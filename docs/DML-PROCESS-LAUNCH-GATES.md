# DML process-launch implementation gates

## Status

This document is a source-audit record and implementation gate, not a claim
that process launching is implemented. The DML runtime currently coordinates
a backend through synchronous `start` and `stop` callbacks. The repository
does not yet provide a production adapter for those callbacks.

## Source-audit findings

- `include/abi/syscall.h` exposes ABI version, test completion, handle close,
  handle duplication, IPC send/receive, and handle transfer. It does not expose
  process spawn/exec or wait.
- `kernel/process.c` implements process-object creation/registration and
  lifecycle operations. `process_create()` initializes and registers a
  process in READY state; it does not itself load an executable or start a
  user-mode instruction stream.
- `kernel/elf_loader.c` validates and loads an ELF image into a supplied
  process address space and returns entry/image/stack metadata. Loading alone
  does not prove that the CPU has entered the process in user mode.
- `kernel/thread.c` has thread creation and scheduling primitives. Its
  `thread_init()` builds a kernel context-switch stack around a kernel entry
  address. That interface must not be assumed to be a safe CPL3 ELF entry
  trampoline.
- `kernel/init.c` has a filesystem-backed `/init` load helper with executable
  admission checks. This is a loader helper, not a general userspace spawn,
  wait, or service-manager API.
- The public syscall policy is an explicit bitmask. Any new syscall requires
  deliberate ABI numbering, policy-mask integration, argument validation,
  user-memory validation, authorization, and regression coverage.

## Required implementation sequence

1. **Trace the existing ring-3 entry path.** Identify the exact interrupt/IRET
   frame construction, user code and stack mapping permissions, segment
   selectors, entry trampoline, scheduler handoff, and cleanup path. Reuse a
   verified path instead of treating a kernel thread entry as a user entry.
2. **Define process ownership and lifetime.** Decide how process/thread objects
   and kernel stacks are allocated, who owns their memory, how PIDs/TIDs are
   assigned, and how failed ELF loading rolls back all resources.
3. **Define a narrow versioned spawn contract.** Use a validated executable
   reference/handle rather than trusting arbitrary user pointers or a path
   supplied without VFS permission checks. Bound argument/environment counts
   and total bytes. Specify output PID/handle semantics and all error cases.
4. **Preserve executable security admission.** Filesystem-backed execution must
   pass the existing admission/quarantine/block decision before ELF loading.
   Enforce user/kernel mapping separation and least-privilege syscall and
   capability policies.
5. **Define wait/reap and stop semantics.** Parent/child rules, zombie status,
   direct-child wait, handle closure, address-space destruction, thread
   termination, and exactly-once resource release must remain consistent with
   `docs/PROCESS-LIFECYCLE.md`.
6. **Add focused tests before integration.** Cover invalid/unmapped user
   pointers, malformed ELF, unsupported ELF features, denied execution,
   duplicate/invalid handles, PID/TID exhaustion, partial-allocation rollback,
   failed thread creation, child exit/wait/reap, double wait, and policy denial.
7. **Implement the DML backend only after the kernel contract is verified.**
   Its start callback must return a nonzero stable process handle only after a
   child has been successfully created and made runnable. Its stop callback
   must terminate/wait/reap the child and report success only after it is no
   longer running. A failed callback must not silently orphan a process.
8. **Verify on the designated OfficeWindows runner only.** Check the assigned
   runner name before accepting results. Run focused host tests, the full
   source build, and the existing boot checks. Keep QEMU/kernel boot failures
   separate from DML unit-test results; do not call GUI support complete until
   real rendering and input delivery work.

## Non-goals for this milestone

- Do not put the DML, a desktop shell, XFCE, Xorg, Wayland, or desktop
  applications inside kernel-core.
- Do not add a guessed syscall ABI merely to make the DML mock tests pass.
- Do not wire the mock backend into boot or claim it launches a real session.
- Do not mark a desktop environment complete based solely on manifest parsing,
  dependency ordering, ELF loading, or a successful ISO build.

## Exit criteria

The production-launch milestone is complete only when the source contains a
reviewed process creation + user-mode entry + wait/reap path, the DML adapter
uses that real path, focused regression tests pass, and the required build and
boot verification passes on the designated runner. Real desktop completion
additionally requires a supported display/window-system stack and verified
keyboard/pointer event delivery.
