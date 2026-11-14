# Process Lifecycle and Isolation Contract

## Scope

This document records the current process lifecycle invariants used by ProsenjitOS. It is intentionally independent of filesystem implementation work so the process subsystem can evolve without coupling to NTFS or VFS metadata changes.

## Lifecycle

A process progresses through these states:

- **NEW** — initialized but not yet runnable.
- **READY** — registered and eligible for scheduling.
- **ZOMBIE** — exited; exit status is retained until the parent reaps it.
- **DEAD** — resources have been released and the process is removed from the PID registry.

The current process manager does not permit a DEAD process to transition back to another state.

## Parent/child rules

- A child records both `parent_pid` and a direct `parent` pointer.
- A parent supplied to `process_create()` must be active.
- `process_wait()` accepts only a direct child of the supplied parent.
- A non-zombie child cannot be reaped.
- Reaping first validates thread-stack/TSS/RSP ownership, handle-table integrity and address-space teardown safety before releasing any child resource.
- A child's own handles to its process object may be closed by its reap transaction; external process-object references must be released by their owners first.
- A second wait after successful reaping fails because the child is no longer present in the live PID registry.

## Resource lifetime

Process address spaces are switched with the process context. A process cannot be reaped while one of its threads is still current, and process exit first terminates all non-dead threads and removes them from runnable queues. The kernel CR3 is restored when the current process exits.

Process destruction is deliberately transactional:

1. `process_exit()` disables local interrupts, switches away from the exiting address space when necessary, terminates its threads and only then publishes ZOMBIE plus the exit code.
2. `process_wait()` disables local interrupts and validates the child's process-object reference count, every dead thread's queue/wait state and owned kernel stack, TSS RSP0 and current RSP, the complete handle table, and the address-space page-table/ownership graph.
3. Only after all preflight checks pass does it close the child's handles and destroy its inactive address space. It then marks the child DEAD; `process_unregister()` releases the dead thread stacks and registry records before dropping the final process-object reference and unlinking the PID.
4. The parent receives the exit code only after successful unregister. A failed preflight leaves the ZOMBIE, handles, address space, thread records and caller output unchanged.

The address-space preflight detects invalid/cyclic page-table trees, page-table aliases across branches, duplicate mapped user physical pages, duplicate owned-page records and stale/mismatched ownership metadata before destruction. The handle-table preflight validates entry metadata, object lifetime and per-table reference counts before close-all begins.

The serialization uses local interrupt exclusion under the current single-CPU model. It is not an SMP locking design and must not be described as SMP-safe.

This keeps exit status observable while preventing the PID registry from exposing a partially reaped process.

## Capability and syscall policy

Each process owns an independent handle table, capability policy, and syscall policy. Handle operations require an active process. A limited-security process starts without administrative capability, while a super-security process receives the full capability policy.

Syscall-policy changes are restricted to the super-security control path, and policies are validated against the supported syscall mask before installation.

## Isolation boundary

The process object owns an address-space object and a handle table. Resource release therefore forms an explicit isolation boundary: address-space teardown and handle closure occur before the process can be unregistered.

This contract is a design invariant, not a claim that the complete user-mode/ELF startup subsystem is finished. ELF loading, init/service management, and user-space utilities remain separate roadmap milestones.

## Regression expectations

Any future process-manager change should preserve:

- unique live PIDs;
- direct-parent-only wait semantics;
- zombie exit-status retention;
- exactly-once resource reclamation;
- rejection of double reap/unregister;
- no capability grant beyond the process capability policy;
- no syscall policy bits outside the supported mask;
- address-space teardown before process unregister;
- no owned physical page may be mapped twice in one address space;
- user mappings are restricted to the dedicated PML4[1] user region;
- a parent cannot exit while a direct child remains live or zombie;
- process objects are unregistered only after their final object reference is released.

