# Scheduler-Aware Mutex: Implementation Prerequisites

**Status:** Design/audit checkpoint only — no mutex implementation is claimed.
**Audited baseline:** `0970c48ad194bd31a0ca68632e7e4ed8e23977ca` (the earlier CI-verified snapshot; not the current PR base). **Current PR base:** `edbdfeb32e1edd512658b37d20d43f5610434d18`.
**Related work:** Native ProsenjitFS I/O serialization; PR #265 must not be merged as-is.

## Why a scheduler-aware mutex is required

The current filesystem lock is a spinlock. It can hold the CPU while synchronous storage operations execute, and the existing scheduler wait/wakeup interfaces do not yet provide a proven atomic condition-check, waiter-registration, and block transition. A naive mutex built on `thread_block()` and `thread_wake()` can lose wakeups; replacing the spinlock with that naive design could deadlock.

The current `thread_block(void *wait_channel)` implementation marks the current thread blocked, but does not atomically coordinate that transition with a mutex wait queue or scheduler handoff. `thread_wake()` transitions a blocked thread to READY and enqueues it, but there is no general mutex wait-queue contract or scheduler-wide guard in these interfaces. The current kernel is a single-CPU foundation; this is not an SMP synchronization guarantee.

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
- Stress test repeated acquire/release while timer preemption is enabled.
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
