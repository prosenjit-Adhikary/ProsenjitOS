# Native Spawn Contract

## Status and scope

This document records the native ELF process-launch contract and its current implementation status. The kernel-facing `process_spawn()` transaction and scheduler-created CPL3 launch path are implemented and covered by the current boot regressions. This is not a public spawn/exec syscall, dynamic ELF loader, Linux/POSIX ABI, or Windows compatibility implementation. Historical requirements below remain design constraints; completed items are identified in the dated implementation notes.

Architecture boundary: native ELF applications use the Native Subsystem; Windows executables use the separate Windows Subsystem. Both may later integrate with the Shared Desktop Integration Layer. This contract does not put desktop/session logic in kernel-core.

## Current source facts

- `struct pros_process` owns an address space, process state, user entry/stack fields, handle table, parent relationship, and security policies (`kernel/include/process.h`).
- `process_create()` initializes an address space, sets parent and security defaults, publishes the process as READY, and registers it. It does not itself load an ELF image or create a runnable thread.
- `pros_init_load()` reads and admits `/init`, checks execution security, validates ELF, and calls `pros_elf_load()`. This is an init-image loading path, not a general public spawn/exec ABI.
- `thread_init()` allocates a supervisor kernel stack from the kernel heap, records ownership on the thread, and builds a kernel bootstrap context. The cooperative and interrupt-frame schedulers validate the selected thread's owned stack and publish its top as TSS `RSP0`; failed address-space switches restore the previous `RSP0`, and reaping refuses to free a stack still published there. The `INT 0x80` gate uses IST=0, so CPL3 syscall entry lands on the selected thread's TSS `RSP0` stack. The interrupt-frame ABI includes the CPL3-only user RSP/SS tail (20 qwords total). `thread_schedule_interrupt()` copies user-originated frames into per-thread storage and validates selectors, user RSP and restricted RFLAGS before restoring them. CPL3 timer preemption remains disabled. `enter_user_mode()` in `boot/entry.asm` remains a one-off boot handoff, not the general process launcher. The current native path uses `process_spawn()` to build the first CPL3 interrupt-return frame, publish the process/thread only after construction, and launch through the scheduler. The boot regression verifies the spawned user's actual argv/envp vector and strings, SYS_EXIT, and parent wait/reap. This is a kernel-facing implementation/test path, not a public spawn syscall. The cooperative `thread_schedule()` path uses kernel-context switching and must never invoke a user entry as a kernel C function. The thread object itself remains caller-owned and must outlive reaping; kernel bootstrap entry is not a user ELF entry mechanism.
- The public syscall ABI currently exposes version, test completion, handle close/duplicate, IPC send/receive, and handle transfer. It has no spawn/exec syscall.
- The process/thread lifecycle has explicit cleanup constraints: a process cannot be finally unregistered while its address-space root remains, its object has extra references, or it owns live/current threads. Process exit publishes its exit code only after thread termination succeeds.

These facts and the dated implementation evidence establish a working, scheduler-created CPL3 launch regression for the supported static ELF64 test profile. They do not establish a general user-facing application launcher, public spawn/exec syscall, dynamic ELF linking, timer-preemptive user scheduling, or SMP support.

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
8. Allocate and own a distinct kernel stack for the initial thread. The scheduler publishes the selected owned stack top as TSS `RSP0`, and the CPL3 `INT 0x80` gate uses IST=0 so syscall entry lands on that per-thread stack. The scheduler-created first-user-entry frame and CPL3 IRET handoff are implemented: `process_spawn()` prepares the initial frame, the interrupt-frame scheduler validates/restores it, and the boot regression verifies actual CPL3 execution, argv/envp contents, `SYS_EXIT`, and parent wait/reap. The syscall-entry regression also validates active stack ownership and the hardware five-word CPL3 IRET tail (`RIP/CS/RFLAGS/RSP/SS`). **Still not implemented:** CPL3 timer preemption and SMP-safe scheduler/lifecycle concurrency. This is a kernel-facing launch/test path, not a general user-facing spawn/exec syscall or production application launcher.
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

## Wait/reap hardening status at this PR head

The implementation in this PR has moved the complete read-only thread-stack
preflight ahead of irreversible process-resource teardown:

- `thread_process_reapable()` now rejects a current/live process thread and
  validates every matching dead thread's registry/queue/wait state, owned
  stack slot, guard and stack mappings, physical-page identity/allocation,
  checked stack-range arithmetic, active TSS `RSP0`, and current kernel
  `RSP` exclusion.
- `process_wait()` calls that preflight before `process_release_resources()`.
  It also validates the child's self-handle reference count before teardown.
- `process_release_resources()` preflights both the complete handle table and
  address-space destruction before closing handles or freeing the address
  space. Address-space preflight rejects the active CR3. The destructive
  operations repeat their own preflight while the current single-CPU path
  keeps interrupts disabled.
- `handle_table_close_all()` validates the complete table and object
  reference invariants before its first close. It returns immediately on an
  unexpected close failure rather than continuing to drain entries.
- `process_unregister()` still performs final thread-registry unlink and
  process-object release only after the address-space root is gone and the
  process is no longer current/live.

This closes the previously documented ordering gap at the source level. It
does **not** establish SMP safety: the lifecycle and allocator paths still
depend on single-CPU execution and local interrupt exclusion.

### Remaining verification required

The current implementation has useful positive-path and active-TSS-stack
regressions, but the complete fault-injection matrix is not yet demonstrated
at this PR head. Add deterministic negative-path tests proving that each
rejected preflight leaves the process retryable as ZOMBIE and preserves all
handles/references, address-space mappings, thread registry entries, owned
kernel stacks, TSS state, and exit status. Cover at least:

1. Corrupt stack slot, guard mapping, stack-page identity, stack-range
   overflow, active TSS `RSP0`, and current `RSP` overlap.
2. Active-CR3 address-space destruction rejection before any handle close.
3. Invalid handle generation, object type/reserved bits, refcount underflow,
   and duplicate references in the handle table.
4. A forced failure at each destructive boundary, including address-space
   destruction and final thread/process unregister, with retry behavior and
   no double-free or lost reference.
5. Parent/child ownership, live-thread refusal, repeated wait/reap, and
   failed user copy-out before destructive teardown.

Do not claim the wait/reap transaction is fully verified until those tests
and the exact-head required Build, boot/QEMU, and Security checks pass.

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

Historical note: at the time this paragraph was first written, the scheduler-created first-user-entry and parent wait/reap path was awaiting exact-head workflow evidence. That review condition is superseded by the 2026-11-27 Phase 1 process-execution gate audit in `docs/FOUNDATION-AUDIT.md`, which records the required source, regression, QEMU and workflow evidence. CPL3 timer preemption and SMP remain explicitly out of scope for this Phase 1 gate; a general user-facing spawn/wait syscall and production application launcher are still not provided.


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


## 2026-11-16 — Thread-stack transaction dependency gate

The process-spawn and wait/reap paths depend on a reliable owned-kernel-stack lifecycle. The current helpers do not yet provide an all-or-nothing release contract:

- Stack allocation rollback and normal stack release call void page-unmap and physical-page-free helpers. Those helpers can silently decline an operation, so the caller cannot prove that all mappings and pages were released before clearing the slot/ownership metadata.
- The page-unmap and translation walkers check PRESENT bits but do not authenticate intermediate page-table pages as aligned, allocator-owned page-table storage before dereferencing them. A corruption-safe stack preflight must not reuse this unchecked walk.
- Registry scans used by thread construction, live/reapability checks, unpublished abort, process reap, and kernel-thread reap need one bounded acyclic-registry contract. A cycle can otherwise hang before later stack validation runs.
- Stack ownership must be unique across the complete registry, not just within the process being reaped. Reject duplicate thread nodes, slots, virtual ranges, and any of the four physical pages shared with another kernel/process thread.
- Slot reservation, page allocation/mapping, thread-field initialization, CPL3 frame construction, registry publication, validation, and release must share an enforced quiescent single-CPU/interrupt-exclusion contract. A public API must not rely on an undocumented caller-side `cli`.
- Thread-manager initialization must not erase a non-empty run queue, current thread, registry, or stack-slot bitmap.

### Required implementation contract

1. Add checked page-table-walk validation and expected-physical-address unmap preflight/commit. Authenticate every intermediate table page before dereferencing it; reject the whole batch if any VA does not map its recorded PA.
2. Add non-contiguous physical-page batch-free preflight/commit. Validate alignment, managed range, allocated state, and uniqueness for the entire set before changing any bitmap or free-page count.
3. Build a complete stack-release plan for all affected threads before the first mutation. The commit path must not discover a late per-stack failure after earlier stacks have been released.
4. Apply the same checked release/rollback path to stack-allocation failure, unpublished-thread abort, process-thread reap, and kernel-thread reap. Retain the slot and ownership metadata until every release is proven complete; quarantine rather than recycle uncertain resources.
5. Validate the complete registry with a documented bound (at most 64 stack-owning slots), cycle/repeated-node rejection, and cross-owner stack-page uniqueness. The validator must run before any unbounded registry traversal.
6. Make thread construction publish only after all required fields, including the initial CPL3 frame, are complete; enforce and restore the caller's original interrupt-enable state on success and failure.
7. Make manager initialization fail closed when any queue, current-thread, registry, or slot ownership remains.

### Required regression matrix

- Two-node registry cycle, repeated node, over-limit registry, and target node behind a cycle: bounded rejection, no mutation.
- Same slot/base alias, duplicate physical page under another slot, and alias against both a kernel thread and a different process.
- Slot bitmap/registry mismatch and orphaned reservation.
- Missing PTE, mismatched expected PA, corrupt intermediate page-table reference, and already-free physical page.
- Failure on the last page/last target: no earlier PTE, allocator bit, slot bit, thread field, or registry link may have changed.
- Allocation/map failure at each of four pages: exact restoration of PTEs, allocator bitmap/free-page count, and slot bitmap; an unprovable cleanup must retain/quarantine the slot.
- Successful multi-thread release: each owned page and stack is released exactly once; unrelated threads and queue links remain unchanged.
- `thread_manager_init()` with live state must reject without clearing any state.
- `thread_init_user()` must not become schedulable/registry-visible with an incomplete user frame.

This is a Phase 1 prerequisite. No arbitrary-corruption safety, SMP safety, or successful runtime behavior is claimed until implementation and exact-head regression evidence exist.


## 2026-11-16 — Checked stack page-release contract implemented

The kernel now has bounded all-or-nothing primitives for non-contiguous physical-page release and expected-PA virtual unmapping. Both validate the entire batch before mutation. The page-table walk used by these primitives authenticates the kernel root and intermediate tables as allocator-owned pages before dereferencing them.

The thread-stack allocator rollback and release paths now use these primitives. Guard and stack-page checks use the checked translation path. Failed cleanup retains the slot reservation rather than returning an uncertain virtual slot to the allocator.

Required verification still pending: exact-head warnings-as-errors build, allocator/paging self-tests, ISO and native BIOS QEMU regression, and Security architecture workflow. The change is not yet runtime-verified and does not close the global thread-registry or all-process transactional teardown gates.


## 2026-11-16 — Implemented checked stack release and bounded registry validation

The following implementation gates are now present:

1. A bounded, all-or-nothing non-contiguous physical-page batch-free API.
2. A bounded expected-physical-address page-unmap API that validates the entire batch and authenticates allocator ownership of intermediate page-table pages before dereference.
3. A checked kernel translation helper used for stack guard and data-page validation.
4. Stack allocation rollback and stack release integrated with the checked batch operations. Uncertain cleanup quarantines the slot instead of recycling it.
5. A bounded global thread-registry validator (maximum 64 nodes) that rejects cycles/repeated nodes, duplicate stack slots, physical-page aliases, invalid ownership, and slot bitmap/registry disagreement.
6. Fail-closed thread-manager initialization and self-tests proving it does not erase live ownership.

The exact tested source head `1f4d2c9ed249855878234a436741dfee926af171` passed the Build/boot workflow (run `43865301602`) and Security architecture workflow (run `43865301636`). The Build run includes warnings-as-errors compilation, physical-memory/paging/thread self-tests, ISO QEMU boot, and native BIOS QEMU boot. Both workflows selected GitHub-hosted fallback.

## 2026-11-28 — Exact-head lifecycle audit

Reconciled the rollback coverage note with PR #642 head
`cb1f4e7e4b23307445d49bc31ceeb0e0e64ee583`. The exact-head Build/boot workflow
(`46109468005`) and Security architecture workflow (`46109468028`)
completed successfully. ISO QEMU, Native BIOS QEMU, and the production
crash-record two-boot persistence test all passed.

The wait/reap source audit confirms that `process_wait()` performs the
process-registry, child-count, complete handle-table, address-space,
thread/address-space-disjointness, and thread-reapability checks before
`process_release_resources()`. Its existing boot self-test exercises
corrupt registry links, child presence, reserved handle slot, address-space
root, handle generation/reserved/rights, duplicate-reference underflow,
stack mapping/ownership, run-queue corruption, live RSP, and TSS.RSP0; each
rejected case checks that the zombie and its resources remain available for
a valid retry.

The destructive sequence is also guarded by interrupt exclusion and repeats
the relevant handle/address-space preflights in the teardown routines.
`process_unregister()` validates the final thread-reapability and object
header/refcount before unlinking. No source-confirmed partial-teardown bug
was found in this pass. However, the suite does not yet have a dedicated
fault-injection test that forces a failure *after* handle close has committed
or during final unregister; that boundary remains explicitly unclaimed as
covered. Do not add a synthetic failure after irreversible teardown without
a safe transaction/rollback design.

PR #642 remains open and unmerged. This audit does not declare the broader
wait/reap failure-injection matrix or all Phase 1 exit criteria complete.

## 2026-11-28 — Rollback preflight rejection/retry coverage

At PR #642 head `9f560de0f61facccf19cabc8e14c4682d181ffeb`, the boot-time
spawn rollback self-test now exercises aggregate cleanup rejection and retry
for both publication states:

- Unregistered rollback: corrupt reserved metadata in an otherwise empty
  handle slot; verify rejection preserves the initialized child resources,
  restore metadata, then retry cleanup successfully.
- Unregistered rollback: corrupt an owned-page physical-address record; verify
  address-space preflight rejects without teardown, restore the authoritative
  mapped physical address, then retry successfully.
- Registered/queued rollback: repeat both cases and verify the process remains
  PID-visible, READY, and queued with its resources retained until metadata is
  restored and the same cleanup operation succeeds.
- Existing registered-descendant rejection remains covered independently;
  the child is exited/reaped before rollback is retried.

The exact-head Build/boot workflow (run `46109015613`) and Security
architecture workflow (run `46109015605`) passed. The Build workflow includes
warnings-as-errors compilation, ISO QEMU, Native BIOS QEMU, and the production
crash-record two-boot persistence test. The kernel's process lifecycle
self-test passed, which gates these new cases. This adds spawn rollback
preflight coverage; it does **not** close the separate wait/reap destructive
failure-injection matrix or all Phase 1 exit criteria. PR #642 remains open
and unmerged.

Status reconciled against PR head `646388874996d60dc6bfc88457d5d5f26354273a` (2026-11-18): public thread constructors now hold interrupt exclusion through complete frame construction and registry publication; `thread_init_user()` supplies the validated CPL3 frame before the registry link is published. `pros_elf_selftest()` and the scheduler-created child boot regression verify argc, argv/envp NULL terminators, pointer bounds, 16-byte stack alignment, and exact NUL-terminated argument/environment bytes. Spawn rollback failpoints 1–5 and aggregate preflight-rejection/retry cases 6–7 are present. Exact-head Build/ISO QEMU/Native BIOS QEMU and Security workflows passed (runs `44292223796` and `44292223802`). These facts close the spawn/initial-stack work package. They do **not** close the broader wait/reap failure-injection matrix, panic/fault handoff, or all Phase 1 exit criteria. SMP remains unsupported. PR #642 remains open and unmerged.
