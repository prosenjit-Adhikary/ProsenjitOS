# Scheduler-Aware Mutex: Implementation Prerequisites

**Status:** Design/audit checkpoint only — no mutex implementation is claimed.
**Audited baseline for this follow-up:** `fc8604aca130561182e0e1b50077654569ba5e80` (`main` snapshot reviewed for the VFS concurrency checkpoint). Recheck against the exact PR head before implementation; this document is not a substitute for build/runtime verification.
**Related work:** Native ProsenjitFS I/O serialization; PR #265 must not be merged as-is.

## Why a scheduler-aware mutex is required

The current filesystem lock is a spinlock. It can hold the CPU while synchronous storage operations execute, and the existing scheduler wait/wakeup interfaces do not yet provide a proven atomic condition-check, waiter-registration, and block transition. A naive mutex built on `thread_block()` and `thread_wake()` can lose wakeups; replacing the spinlock with that naive design could deadlock.

The current `thread_block(void *wait_channel)` implementation marks the current thread blocked, but does not atomically coordinate that transition with a mutex wait queue or scheduler handoff. `thread_wake()` transitions a blocked thread to READY and enqueues it, but there is no general mutex wait-queue contract or scheduler-wide guard in these interfaces. The current kernel is a single-CPU foundation; this is not an SMP synchronization guarantee.


## Concrete call-site findings from source audit

In the inspected `kernel/ipc.c`, `ipc_lock()` saves RFLAGS and executes `cli` before acquiring the channel's atomic test-and-set lock; `ipc_unlock()` releases the lock and restores IF only when it was set on entry. Consequently, on the current single-CPU execution path, the specific scenario “local timer preempts a waiter while it owns the channel lock” is **not supported by the inspected code**: local maskable interrupts are disabled throughout that critical section. The earlier claim that this channel lock is not IRQ-safe for local timer preemption would be inaccurate for this implementation.

That does **not** establish a general scheduler guard or an SMP-safe protocol. Disabling interrupts affects only the current CPU; the channel lock does not by itself protect scheduler run-queue/state mutations against another CPU. Also, `thread_block()` marks the current thread BLOCKED but does not itself perform the context switch, and the API does not encode a shared scheduler/wait-queue guard contract. This remains a source-level design limitation, not a reproduced runtime failure.

A scheduler-aware mutex must not simply copy the IPC lock pattern. The scheduler/wait-queue protocol must define waiter registration, BLOCKED transition, lock release, wakeup, cancellation, and scheduler handoff under one documented guard for the explicitly supported CPU model. Any implementation must audit and migrate the existing IPC path as part of the same change, not only filesystem call sites.

Add these regression cases before accepting the protocol:

- On the currently inspected single-CPU path, verify that local interrupts remain disabled throughout IPC waiter registration and are restored exactly to the prior IF state on every return path; do not describe local timer preemption inside that critical section as an observed defect.
- Sender racing with receiver wait must result in either a visible queued message or a registered/woken waiter; no lost wakeup under the explicitly supported CPU model.
- Race waiter cancellation/termination against sender wakeup and assert that a thread is not enqueued twice, stale waiter entries are not retained, and the queue remains consistent.
- If SMP is introduced or claimed, add multi-CPU tests for channel lock ownership, scheduler queue/state mutation, waiter registration, and wakeup; local `cli` is not sufficient for those tests.
- Prove that no spinlock is held across a blocking scheduler handoff.

## Required protocol before implementation

1. **Define ownership:** mutex stores an owner thread pointer (or stable thread ID) and a FIFO wait queue. Reject recursive acquisition unless recursive semantics are explicitly designed.
2. **Define scheduler synchronization:** provide a short IRQ-safe scheduler/wait-queue guard. It must protect the combined sequence that checks mutex ownership, registers a waiter, marks the thread BLOCKED, releases the guard, and schedules away. Wakeup must use the same guard and only wake a thread still registered on that mutex.
3. **Prevent lost wakeups:** unlock must transfer ownership or reserve it for one selected waiter while holding the guard; it must not merely clear an owner field and wake an arbitrary thread. Acquisition after wake must validate ownership/reservation.
4. **Define interrupt rules:** mutex acquire is forbidden in interrupt context and while holding locks that may be required by the scheduler or storage completion path. Unlock must preserve the caller's prior interrupt state. No sleeping while holding the scheduler guard.
5. **Define cancellation/teardown:** thread termination must remove a thread from any mutex wait queue before its wait metadata or stack can be reclaimed. Mutex destruction is forbidden while owned or while waiters remain.
6. **Define lock order:** document VFS-handle lock, filesystem lock, scheduler guard, block-device and allocator interactions. No path may acquire these in reverse order. Avoid holding a spinlock across disk I/O or allocation that may block.
7. **Define lifecycle gate:** filesystem unmount must prevent new operations, wait for active operations to drain, then flush and release mount state. A per-file lock alone does not solve unmount-vs-operation races.

## Required tests

- Lock/unlock by one thread; uncontended and contended paths.
- Two or more threads contend; ownership is never simultaneous and each successful acquisition is paired with one release.
- Unlock racing with waiter registration does not lose a wakeup.
- Spurious/stale wakeups do not grant ownership.
- A terminated waiter is detached; other waiters remain intact.
- Recursive acquisition returns a defined error or follows documented recursive semantics.
- Interrupt state is preserved for both IF=0 and IF=1 entry conditions.
- Stress test repeated acquire/release while timer preemption is enabled on the supported uniprocessor configuration; add separate SMP stress only if SMP is implemented and supported.
- Filesystem integration test covering concurrent read/write/truncate/append, followed by namespace and unmount lifecycle tests.

Tests must be deterministic where possible, assert queue/state invariants, and run on the exact commit SHA. A QEMU boot success alone is not sufficient proof of mutex correctness.

## Acceptance gate

- [ ] Scheduler guard and atomic block/wakeup protocol implemented and reviewed.
- [ ] Mutex API and owner/wait-queue invariants documented.
- [ ] All tests above implemented; no test is described as passing unless it ran.
- [ ] Build/boot-test and Security architecture checks pass on the exact PR head.
- [ ] Filesystem call sites audited for nested locking, blocking, allocation, and lock order.
- [ ] Active-operation/unmount gate tested before claiming concurrent filesystem safety.
- [ ] PR #265 is rebased/reimplemented as a narrow change on current `main`; do not merge its diverged 88-commit history wholesale.

## Current verification boundary

The current main baseline `0970c48ad194bd31a0ca68632e7e4ed8e23977ca` passed Build/boot-test and Security architecture checks after SCC dispatcher PR #275 was merged. Those checks validate that baseline only; they do not validate a scheduler-aware mutex or general filesystem concurrency. No local runner result is claimed in this document.

## Deterministic scheduler failure-path test plan (2026-10-29 follow-up)

The current boot regression covers the interrupted-frame eligibility predicate and the path where a blocked thread has a runnable successor. It does **not** execute the no-successor idle loop or force an address-space-switch failure while the interrupted thread is BLOCKED: those production paths intentionally park with `sti; hlt`, so calling them directly from a self-test would hang the boot test.

Before claiming those paths tested, use a narrowly scoped scheduler test seam rather than invoking the non-returning idle loop in the test harness:

1. Factor the no-successor decision into a side-effect-free helper that reports either “resume current frame” or “park until a runnable thread exists,” based on the interrupted thread's state and process eligibility. Production scheduling must consume this same decision.
2. Factor the post-address-space-switch-failure decision similarly. The test must model a BLOCKED previous thread and an eligible candidate whose address-space switch fails, then assert that the previous frame is not selected, the blocked thread is not requeued, and the failed candidate is restored to a consistent queue/state.
3. Keep actual `sti; hlt` parking in the production-only wrapper; do not add a test mode that silently returns to an unsafe frame.
4. Add regression cases for RUNNING + no successor, BLOCKED + no successor, BLOCKED + failed switch, and READY/DEAD stale frame owners. Assert queue head/tail/link consistency and that no thread is both current RUNNING and linked as READY.
5. Run the boot regression under QEMU and require explicit serial milestones for each branch. Report results only from the exact tested commit SHA.

This is a test-design requirement, not an implementation or passing-test claim. Until the seam and cases are implemented and run, the two non-returning failure paths remain uncovered by deterministic runtime tests.
