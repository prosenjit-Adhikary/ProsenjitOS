# Scheduler-Aware Mutex: Implementation Prerequisites

**Status:** Design/audit checkpoint only — no scheduler-aware mutex implementation is claimed.
**Audited baseline:** `947aca8692b9b59c637cca82f92a6320afda042a` (main after FS-11 software-gate reconciliation).
**Related work:** Native ProsenjitFS fail-fast I/O and VFS lifetime gates; PR #265 must not be merged as-is.

## Why a scheduler-aware mutex is required

The current filesystem lock is a spinlock. It can hold the CPU while synchronous storage operations execute, and the existing scheduler wait/wakeup interfaces do not yet provide a proven atomic condition-check, waiter-registration, and block transition. A naive mutex built on `thread_block()` and `thread_wake()` can lose wakeups; replacing the spinlock with that naive design could deadlock.

The current `thread_block(void *wait_channel)` implementation marks the current thread blocked, but does not atomically coordinate that transition with a mutex wait queue or scheduler handoff. `thread_wake()` transitions a blocked thread to READY and enqueues it, but there is no general mutex wait-queue contract or scheduler-wide guard in these interfaces. The current kernel is a single-CPU foundation; this is not an SMP synchronization guarantee.


## IPC wait-path source correction — 2026-11-14

The earlier local-timer-preemption hazard described for `ipc_channel_wait()` was inaccurate for the current source and is superseded by this correction.

- `ipc_lock()` in `kernel/ipc.c` saves RFLAGS and executes `cli` before acquiring the channel's atomic test-and-set lock.
- `ipc_unlock()` releases the lock and executes `sti` only if IF was set on entry. Callers that entered with interrupts already disabled retain that state.
- `ipc_channel_wait()` calls `thread_block(channel)`, appends the waiter, and releases the channel lock. `thread_block()` marks the current thread BLOCKED but does not itself switch context. On the current single-CPU execution path, the local timer cannot preempt the waiter inside this critical section because IF remains clear until after the waiter is registered and the lock is released.
- Therefore the specific scenario “the local timer switches away while a BLOCKED waiter owns the channel lock” is **not supported by the inspected current code path**. Do not report it as a confirmed race.
- This does **not** establish an SMP-safe scheduler protocol. Local `cli` affects only the current CPU. The channel lock serializes channel data across CPUs, but scheduler run-queue/state operations outside the channel lock still require their own globally correct synchronization if SMP is introduced.
- The generic `thread_block()/thread_wake()` API does not encode a universal scheduler/wait-queue guard contract. A new sleeping mutex must explicitly coordinate ownership, waiter registration, BLOCKED transition, wakeup, cancellation, queue removal, and scheduler handoff for the supported CPU model.

The scheduler-aware mutex remains unimplemented. Do not copy the IPC pattern blindly or replace the filesystem's fail-fast gates with a sleeping lock until the full ownership/guard/teardown contract is implemented and tested.

Required IPC/scheduler regressions before any broader claim:

- Verify `ipc_channel_wait()` keeps IF clear from before lock acquisition through waiter publication and lock release, and restores the caller's prior IF state on every return path.
- Verify a message already queued wins over waiter registration (`EALREADY`) and does not leave a BLOCKED thread or stale waiter entry.
- Verify sender-vs-waiter registration, wake-vs-cancel, duplicate waiter rejection, and early wake before a fresh interrupt frame is published without lost wakeups or double enqueue.
- Verify no spinlock is held across a scheduler context switch.
- If SMP is later supported, add multi-CPU tests for channel ownership, scheduler queue/state mutation, waiter registration, wakeup, and cancellation. Local `cli` is not sufficient evidence for SMP.

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

The audited main baseline is `947aca8692b9b59c637cca82f92a6320afda042a`. Build/boot-test run `43598789929` and Security architecture run `43598789954` passed on the immediately preceding documentation-only candidate. Those checks validate the repository's build/boot and security contracts; they do not validate a scheduler-aware mutex, SMP, or general blocking filesystem concurrency. The IPC correction above is source-reviewed against current `kernel/ipc.c`; it is not a runtime interleaving test.
