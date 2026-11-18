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



## Cross-thread stack-alias wait/reap rejection — zero-mutation evidence

The process wait/reap corruption fixture now snapshots the complete fixed 64-slot stack allocator bitmap and quarantine bitmap, the physical allocator free-page count, each of the eight stack-page ownership records across the two process threads, every corresponding VA-to-PA mapping, and each physical page's allocated/free state before invoking the real `process_wait()` and `thread_reap_process()` paths against a deliberate cross-thread physical-page alias.

Both operations must reject the corrupted ownership graph. The test then requires all snapshots and mappings to remain unchanged, confirms the deliberately freed displaced page remains free, restores a unique physical page, and proves the process becomes reapable again. The snapshot helper is deliberately read-only and does not call the global registry validator, because this regression must observe allocator state while the registry is intentionally invalid.

Exact tested source head: `0168b8bed009efdb824762f5b8b21b8ea97a8397`. Build/boot workflow run [44303434840](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44303434840) and Security architecture run [44303434802](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44303434802) both succeeded. The Build job verified its PR merge checkout included this exact head and passed warnings-as-errors compilation, ISO QEMU, and Native BIOS QEMU. Native BIOS QEMU returned the configured bounded timeout 124 after required markers; this is not a natural shutdown claim.

This closes the **specific cross-thread alias zero-mutation snapshot gap**. It does not close the full wait/reap failure-injection matrix, prove arbitrary memory-corruption safety, or imply SMP safety.


## Late-target thread-stack corruption — zero-mutation regression

The multi-thread process wait/reap fixture now snapshots the stack allocation bitmap, quarantine bitmap, physical allocator free-page count, all eight physical stack-page records and their allocated/free states, and every corresponding stack VA-to-PA mapping **before** corrupting the second thread's stack-page metadata. It then requires both `process_wait()` and direct `thread_reap_process()` to reject the late-target corruption while preserving all snapshots, the first and second stack records, TSS RSP0, process ZOMBIE state, PID registration, address-space root, handle, and caller exit-code sentinel. The corrupted metadata is restored before checking the normal retry path.

This specifically demonstrates that a malformed later stack cannot cause an earlier valid stack to be released before the aggregate preflight rejects the process reap. It complements the separate cross-thread physical-page alias fixture; it does not establish the complete wait/reap matrix or SMP safety.

Exact tested source head: `cf7a567fe0eb37ea9fee524e82236e9b6a9a58c3`. Build/boot run [44305026983](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44305026983) and Security architecture run [44305026995](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44305026995) both succeeded. Build merge checkout `7488b105` matched the exact head; warnings-as-errors compilation, ISO QEMU, and Native BIOS QEMU passed. Native BIOS QEMU returned the configured bounded timeout 124 after required markers.


## Process-manager reinitialization guard

`process_manager_init()` is an initialization operation, not a registry-reset API. It now rejects reinitialization while any process remains registered or a current-process reference exists. The rejection preserves the process list, current-process pointer, and PID allocation state. Reinitialization is permitted only after the registry is empty and no process is current.

The process lifecycle boot self-test runs inside the already initialized kernel process manager. It must not reset the manager and silently discard PID 1. The regression attempts manager reinitialization while its parent and child test processes are registered, requires rejection, and verifies both PIDs remain discoverable before continuing the wait/reap tests.

Source commit: `196bedd27a094c16e84e3ac957f2e89c868fb35b`. Its exact-head Build/QEMU and Security workflows both passed (runs [44370517124](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44370517124) and [44370517066](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44370517066)). This is a lifecycle hardening slice only; Phase 1 remains open.


## Thread-stack allocator type integrity

A thread kernel stack may own only ordinary physical data pages. Page-table-typed pages are reserved for paging structures and cannot be returned through the normal non-contiguous stack-page free batch. The thread-stack validator and global registry validator now reject a page-table-typed physical page before any process or kernel-thread reap begins.

The registry integrity self-test temporarily substitutes a page-table-typed page into a non-current thread stack mapping, requires the read-only registry validation to reject it, restores the original mapping and ownership record, and verifies the registry validates again. This prevents a late stack-release rejection from occurring after an earlier stack in a multi-thread reap has already been released.

Exact tested head: `78ccca5380a1e959e3077c0f7a4f53d161e61c8d`. Build/QEMU and Security workflows both passed (runs [44371510064](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44371510064) and [44371509844](https://github.com/prosenjit-Adhikary/ProsenjitOS/actions/runs/44371509844)). This regression covers allocator page-type integrity; it does not imply arbitrary memory-corruption safety or SMP support.
