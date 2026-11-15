# Native Spawn Contract

## Status and scope

This document freezes the proposed contract for launching a native ELF process without claiming that a public spawn/exec syscall or complete userspace launch path already exists. It is based on the current process, thread, address-space, ELF/init, syscall, and security interfaces. Treat every item marked **required** as a gate for implementation.

Architecture boundary: native ELF applications use the Native Subsystem; Windows executables use the separate Windows Subsystem. Both may later integrate with the Shared Desktop Integration Layer. This contract does not put desktop/session logic in kernel-core.

## Current source facts

- `struct pros_process` owns an address space, process state, user entry/stack fields, handle table, parent relationship, and security policies (`kernel/include/process.h`).
- `process_create()` initializes an address space, sets parent and security defaults, publishes the process as READY, and registers it. It does not itself load an ELF image or create a runnable thread.
- `pros_init_load()` reads and admits `/init`, checks execution security, validates ELF, and calls `pros_elf_load()`. This is an init-image loading path, not a general public spawn/exec ABI.
- `thread_init()` allocates a supervisor kernel stack from the kernel heap, records ownership on the thread, and builds a kernel bootstrap context. The cooperative and interrupt-frame schedulers validate the selected thread's owned stack and publish its top as TSS `RSP0`; failed address-space switches restore the previous `RSP0`, and reaping refuses to free a stack still published there. The `INT 0x80` gate uses IST=0, so CPL3 syscall entry lands on the selected thread's TSS `RSP0` stack. The interrupt-frame ABI includes the CPL3-only user RSP/SS tail (20 qwords total). `thread_schedule_interrupt()` copies user-originated frames into per-thread storage and validates selectors, user RSP and restricted RFLAGS before restoring them. CPL3 timer preemption remains disabled. `enter_user_mode()` in `boot/entry.asm` is a one-off boot handoff with interrupts disabled, not a general process launcher. **Still missing for native spawn:** construction of the first CPL3 frame for a newly created thread, a production launch/publication transaction, and an end-to-end scheduler-created user-thread launch test. The cooperative `thread_schedule()` path uses kernel-context switching and must never invoke a user entry as a kernel C function; new user threads must be routed exclusively through a validated CPL3 IRET frame. The thread object itself remains caller-owned and must outlive reaping; kernel bootstrap entry is not a user ELF entry mechanism.
- The public syscall ABI currently exposes version, test completion, handle close/duplicate, IPC send/receive, and handle transfer. It has no spawn/exec syscall.
- The process/thread lifecycle has explicit cleanup constraints: a process cannot be finally unregistered while its address-space root remains, its object has extra references, or it owns live/current threads. Process exit publishes its exit code only after thread termination succeeds.

These facts are prerequisites and constraints, not proof of a working native application launcher.

## Required spawn contract

### Inputs

A future versioned spawn interface must accept only bounded, validated user inputs:
- executable path or an already-authorized executable handle;
- bounded argument and environment vectors with explicit total byte/count limits;
- requested working directory only if VFS support and permissions are defined;
- a documented inheritance policy for handles/capabilities;
- explicit launch flags with unknown bits rejected.

Do not accept kernel pointers from userspace. Copy and validate all user memory through the existing user-copy boundary before using it; reject invalid pointers, unterminated strings, overflow, excessive counts, and aggregate-size overflow.

### Admission and construction order

1. Validate syscall version/flags, caller state, argument bounds, and authorization.
2. Resolve the executable through VFS; require a regular file and retain a stable reference while reading.
3. Run the existing executable-admission/security policy before loading. A filesystem read alone must never authorize execution.
4. Validate ELF format, machine/class, segment ranges, alignment, permissions, entry point, and unsupported features using the existing loader's supported contract. Do not claim dynamic linking/relocations unless implemented.
5. Create a child process in a non-runnable construction state. Do not expose it as READY before its image, user stack, and initial thread are fully valid.
6. Build the address space and map only validated loadable segments with appropriate permissions. Reject overflow, overlap/invalid ranges, W+X policy violations where enforced, and user/kernel address-boundary violations.
7. Build a bounded user stack containing argument/environment data and the ABI-defined initial stack layout; record the validated user entry and stack pointer.
8. Allocate and own a distinct kernel stack for the initial thread. The scheduler publishes the selected owned stack top as TSS `RSP0`, and the CPL3 `INT 0x80` gate now uses IST=0 so syscall entry lands on that per-thread stack. The syscall entry regression validates the active stack ownership and the hardware five-word CPL3 IRET tail (`RIP/CS/RFLAGS/RSP/SS`). **Still not implemented for native user launch:** a scheduler-owned first-user-entry frame, an explicit CPL3-aware saved-frame ABI for all scheduler paths, and safe timer preemption. The current boot-time CPL3 test is not a general user-thread launch path.
9. Apply the child syscall/capability policy and a documented handle-inheritance policy. Default to least privilege; never inherit privileged handles implicitly.
10. Register/publish the child and enqueue its initial thread only after every previous step succeeds. Publication must be the final commit point; no partially initialized process may become runnable.

### Success result

Return a child PID/handle only after successful publication. The ABI must define whether the parent receives a PID, a process handle, or both; raw kernel pointers are never returned. The initial thread must begin at the validated ELF entry in user mode, not by calling the entry as a kernel function.

### Failure and rollback invariants

Any failure before publication must leave no externally visible child and no runnable thread. Rollback must be idempotent or guarded by explicit ownership flags and must release, in reverse dependency order:
- queued/runnable state (if publication was attempted);
- registered thread objects and their owned kernel stacks;
- mapped ELF pages and page-table/address-space resources;
- inherited/created handles and references;
- executable file/VFS references and temporary image/argument buffers;
- process registry membership, parent-child linkage, and process object ownership.

Do not call final process unregister while the address-space root exists, a thread is live/current, or object references remain. Do not publish a PID/exit result before the corresponding lifecycle transition succeeds. A rollback error must be surfaced and diagnosed; it must not be hidden by returning a generic success.

## Additional current-main lifecycle hardening gate

`process_wait()` currently performs a read-only `thread_process_reapable()` check, releases the process handles/address space, then reaches `thread_reap_process()` through `process_unregister()`. The latter performs additional kernel-stack/TSS/RSP validation not included in the earlier read-only check. Move the complete stack/TSS validation into a non-mutating preflight before irreversible address-space teardown, and test that a failed preflight leaves a retryable ZOMBIE with all resources intact.

### Expanded wait/reap preflight and partial-release hazards

The current ordering has more than one destructive boundary that must be covered by a single read-only preflight or an explicitly retryable staged teardown:

- `thread_process_reapable()` checks that the process is not current, has no live threads, and has no matching non-dead, waiting, or queued thread. It does **not** validate each owned kernel stack's slot/mapping/physical-page ownership, stack-range arithmetic, the active TSS `RSP0`, or whether the current kernel `RSP` lies inside a stack scheduled for release.
- `thread_reap_process()` performs those stack/TSS/RSP checks only after `process_wait()` has already called `process_release_resources()`. A late failure therefore can leave a visible ZOMBIE whose handles and address space have already been destroyed.
- `process_release_resources()` calls `handle_table_close_all()` before `address_space_destroy()`. The close-all loop continues after an individual close error and can leave a partially drained handle table. In addition, address-space destruction can reject the active CR3 after handles have already been closed. Neither path currently supplies rollback of the earlier releases.

Required correction before relying on wait/reap as a transactional primitive:

1. Add a non-mutating, complete reap preflight that validates every matching thread record and owned stack, including checked base+size arithmetic, stack-slot ownership, all guard/stack mappings, physical-page allocation/identity, active TSS `RSP0`, and current `RSP` exclusion.
2. Preflight address-space destruction, including proving that the target root is not the active CR3, before closing any handle or freeing any resource.
3. Validate every handle-table entry and referenced object's type/refcount/generation invariants before the first close. Define how corrupted entries are quarantined or repaired; do not silently drop a reference.
4. Make the destructive commit phase non-failing after preflight under the current single-CPU execution model, or introduce explicit per-resource teardown progress so every failure remains safely retryable. Do not claim SMP safety; add synchronization before concurrent lifecycle operations are enabled.
5. Add fault-injection tests at each preflight and teardown boundary. For every rejected preflight, assert the process remains ZOMBIE and all handles, address-space mappings, thread registry entries, kernel stacks, TSS state, and exit status remain intact. For staged teardown, assert a retry resumes from recorded progress without double-free, lost references, or stale PID visibility.

These are source-confirmed ordering hazards, not evidence that a particular normal execution currently fails. The implementation PR must reproduce the rejection paths with deterministic tests before changing the lifecycle code.

## Required lifecycle semantics

- Parent exit policy must be explicit: reject exit while live children exist, or implement reparenting before permitting exit. Do not leave dangling parent pointers.
- Wait/reap must validate parent-child ownership and user output pointers before mutating state. A failed copy-out must not silently reap a child or lose its exit status.
- Exit, wait, and spawn must define behavior for duplicate requests, PID exhaustion/reuse, quarantined callers, and concurrent lifecycle operations.
- Scheduler/run-queue and process registry operations need a synchronization strategy before SMP or concurrent spawn/exit is supported. Existing single-path tests do not prove race freedom.
- The public syscall number, register convention, errors, and ABI version change must be added deliberately with compatibility tests; do not reuse existing syscall numbers.

## Focused verification gates

Before marking spawn implemented:
1. Source-level tests for each failure point and rollback ownership.
2. Tests proving failed spawn leaves no PID-visible child, runnable thread, leaked address space, kernel stack, handle, or VFS reference.
3. Tests for malformed ELF, invalid user buffers, oversized arguments, permission/admission denial, allocation failure, and PID collision/exhaustion.
4. A real user-mode launch test proving CPL3 entry and correct initial stack/argument ABI.
5. Exit/wait/reap tests including live-thread refusal, child ownership, failed user copy-out, and repeated cleanup.
6. Warnings-as-errors build and focused kernel regressions.
7. QEMU boot/serial evidence for the exact tested commit, plus security checks. Do not count queued or still-running jobs as passes.

Do not run broad smoke-test matrices for every documentation change. Run only the checks justified by changed source; run full build/QEMU and security gates when executable kernel code or ABI behavior changes.

## Explicit non-goals

This contract does not implement a Windows PE/.exe runtime, MSI installation, dynamic linker, display server, input service, compositor, XFCE session, or general package manager. Those depend on the native launch contract and separate subsystem interfaces.


## 2026-11-15 — Native spawn transaction implementation slice (PR #642)

- Added a kernel-facing `process_spawn()` transaction for the supported static ELF64 ET_EXEC profile. It initializes a child address space, loads PT_LOAD segments and argv/envp, prepares the first user thread, registers the process, and only then publishes the thread to the run queue.
- Added `thread_init_user()` to validate an executable user entry, writable user stack, lower-half user RSP, 16-byte stack alignment, and the exact CPL3 selectors/IRET frame.
- Added `thread_abort_unpublished()` so a NEW, not-yet-queued thread can release its owned kernel stack and registry node during spawn rollback.
- The spawn operation serializes its publication transaction with local interrupt exclusion under the current single-CPU model. This is not an SMP synchronization design.
- Added a malformed-image QEMU regression requiring failed spawn to leave no registered PID, address space, process-object reference, or allocated thread stack.
- **Not yet complete:** this slice does not yet prove the scheduler's first CPL3 handoff from a spawned thread, user process exit through the scheduler-owned thread, or end-to-end parent wait/reap. Those remain Phase 1 gates; do not describe Phase 1 as complete.


## 2026-11-15 — Scheduler-created native child launch and exit/reap path

- Replaced the boot-only direct `enter_user_mode()` path with `process_spawn()`, which loads a two-segment static ELF64 image, builds argv/envp, prepares the initial CPL3 IRET frame, registers the process and publishes its first thread.
- Scheduler self-test threads are terminated and reaped before the real launch. The scheduler then selects the spawned child, switches address spaces, publishes its owned kernel stack as TSS RSP0, and returns the initial CPL3 frame.
- Added an assembly first-entry helper to restore the common 15-GPR frame and execute IRETQ through the CPL3 RIP/CS/RFLAGS/RSP/SS tail.
- The spawned user test receives its IPC capability in RDI, exercises the CPL3 syscall/IPC path, exits through the existing test-completion trampoline, and is reaped by its parent with exit status 42. The test checks PID, address-space and kernel-stack release.
- CPL3 timer preemption and SMP remain disabled/unimplemented. This verifies first launch and syscall-driven execution, not timer-preemptive scheduling or SMP.
- Exact-head Build/ISO QEMU/Native BIOS QEMU and Security verification is required after this update.


## Phase 1 status note — exact-head gate

The scheduler-created first-user-entry and parent wait/reap path is now wired into the boot regression. This is still **in review** until the updated required workflow asserts the new serial milestones and passes on the exact commit. CPL3 timer preemption and SMP remain explicitly out of scope for this Phase 1 gate.


The required workflow now checks the new native spawn milestones for both ISO and Native BIOS QEMU boots. The previous direct-loader-only success marker has been removed from the required gate; it no longer represents the new scheduler-created launch path.


The first-run IRET helper explicitly loads the CPL3 data selector into DS/ES before restoring the user frame, matching the established boot transition contract. Initial user RFLAGS keeps IF clear (`0x2`); CPL3 timer interrupts/preemption remain disabled until a separate preemption/idle/scheduler exit gate is implemented and tested.


## Native process exit ABI

- Added `PROSENJIT_SYS_EXIT` (syscall 8). It is accepted only for a CPL3 caller whose current thread belongs to the current process.
- The syscall invokes the kernel process-exit transaction with the signed 32-bit status from RDI, terminates the process-owned threads, and returns the private exit sentinel consumed by the INT 0x80 assembly exit path.
- `PROSENJIT_SYS_USER_TEST_COMPLETE` now only validates the user test-data mapping and returns normally; it no longer doubles as process termination.
- The booted native test now calls SYS_EXIT with status 42, then the kernel-side parent waits/reaps and verifies the child address space and kernel stack were released.


## Spawn rollback failpoint matrix

The QEMU regression now injects failures at five transaction boundaries: after child process/address-space initialization; after ELF mapping and argv/envp construction; after first user-thread/kernel-stack creation; after PID/process registration; and after run-queue publication. Each injected failure must leave no registered PID, address space, process-object reference, thread registry node, kernel stack, or run-queue link. This is deterministic stage-failure coverage; allocator exhaustion and arbitrary hardware faults are not claimed as exhaustively injected.


## Scheduler handoff rollback regression

Before the successful first CPL3 entry, the QEMU test temporarily invalidates the queued child's address-space root and invokes the interrupt-frame scheduler. It requires the scheduler to reject the CR3 transition, restore the bootstrap TSS RSP0, leave the parent/current process authoritative, and requeue the child exactly once. The root is then restored and the next scheduling attempt performs the real handoff.


A QEMU run exposed a bootstrap-stack ownership edge case: scheduler self-tests could leave `current_thread == NULL` while TSS RSP0 still referenced a now-dead process-owned thread stack. `thread_terminate_process()` now restores bootstrap RSP0 whenever no current thread exists, not only when it directly detached the current process thread. This preserves the reap preflight invariant.
