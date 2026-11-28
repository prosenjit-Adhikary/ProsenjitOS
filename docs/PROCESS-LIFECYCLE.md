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
- Reaping releases the child's handle table and address space before unregistering it.
- A second wait after successful reaping fails because the child is no longer present in the live PID registry.

## Resource lifetime

Process address spaces are switched with the process context. A process cannot be reaped while one of its threads is still current, and process exit first terminates all non-dead threads and removes them from runnable queues. The kernel CR3 is restored when the current process exits.

The current wait/reap sequence is:

1. `process_exit()` changes a live process to ZOMBIE, terminates its threads, and preserves its exit code.
2. `process_wait()` disables local interrupts and validates the process registry, direct parent/child ownership, parent state, kernel output buffer, process object, complete handle table, self-handle count, unexpected external references, and child ownership.
3. Before the first resource mutation, wait runs the complete thread-reap preflight (registry and run-queue integrity, dead/unblocked state, stack-slot and guard/stack mappings, physical-page ownership, active TSS RSP0, and current RSP exclusion) and address-space destroy preflight (inactive CR3, bounded page-table traversal, and an exact bijection between user leaf mappings and the owned-page list).
4. Only after all preflights pass does the commit close handles, release the validated address-space user/table page sets, unmap/free the thread-stack batch, unlink thread/process registry records, and release the final process-object reference. The exit code is copied to the validated kernel output buffer only after successful unregister.

The design relies on local interrupt exclusion and the current single-CPU execution model; it does not claim SMP safety. Each batch allocator/page-map commit repeats its own validation before changing state. The source regression suite now includes rejection/retry cases for an unowned user mapping and an active TSS stack, with snapshots of mappings, allocated pages, handles, process state, and exit-output sentinel. Existing PID-registry and object-header corruption tests remain in place. Full runtime execution, the wider fault-injection matrix, and exact-head Build/QEMU/Security verification are still pending; Work Item 1 is not complete until those gates pass.

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
