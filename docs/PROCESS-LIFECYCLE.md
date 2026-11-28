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

The current implementation's exit/wait sequence is:

1. `process_exit()` requires a registered process, rejects registered children, and performs a read-only process-thread/run-queue/IPC-waiter preflight under local interrupt exclusion before terminating threads. A current process switches to the kernel CR3 before its current thread is retired. The exit code is published only after thread termination succeeds.
2. `process_wait()` disables local interrupts, validates the complete process registry and direct-parent relationship, validates a supervisor-writable kernel output pointer, validates the process object and complete handle table, and checks that the process object has no references beyond its base reference and self-handles.
3. Before the first resource mutation, wait performs the complete thread registry/run-queue/kernel-stack/TSS/RSP preflight and address-space page-table/owned-page/active-CR3 preflight. It then closes the handle table and destroys the address space.
4. After resource release, wait marks the process DEAD and calls `process_unregister()`. Unregister repeats thread reap preflight, rejects any still-registered children, atomically releases the complete stack-page batch, and only then removes thread and process registry identities and releases the final process-object reference.

The dedicated Work Item 1 branch now validates process registry nodes through bounded trusted identities, checks unique PIDs and parent-pointer ancestry, and rejects dangling, self-referential, or cyclic parent relationships. Thread registry identities are tied to stack slots; the run queue is bounded and structurally validated; IPC wait-channel storage and waiter membership are checked before process exit. Address-space page tables use the PMM's typed page-table allocator and teardown validates the complete owned user/table-page set before the atomic bitmap release. Thread stacks use the checked batch unmap/free transaction.

Boot self-tests now cover invalid IPC wait-channel pointers, corrupted thread-stack ownership, active-child-CR3 rejection, corrupt user page-table links, invalid output pointers, and repaired retry paths. The process self-test also checks that failed wait attempts preserve the ZOMBIE state, handle references, address-space root, and exit result. These checks are source-added only: the exact branch head still requires a warnings-as-errors build, QEMU execution, and Security verification before this lifecycle work can be marked complete.

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
- a parent cannot exit or unregister while any direct child remains registered;
- process objects are unregistered only after their final object reference is released.
