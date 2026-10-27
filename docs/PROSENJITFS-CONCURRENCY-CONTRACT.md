# ProsenjitFS concurrency contract and scheduler-mutex gate

Status: **AUDIT COMPLETE; IMPLEMENTATION NOT COMPLETE**  
Audit date: 2026-10-27  
Source head reviewed: `87f013c181e1c6da1e0ecf6068b73147d932296a`

This source-level audit does not claim filesystem-wide concurrency safety and does not authorize enabling additional preemption or SMP.

## Source facts confirmed

- `kernel/include/prosenjitfs.h` has one `volatile uint32_t io_lock` per filesystem instance.
- `kernel/prosenjitfs.c` implements `pfs_io_lock()` / `pfs_io_unlock()` using atomic test-and-set/release and memory barriers. This is an append-write lock, not a filesystem-wide operation lock.
- `kernel/include/thread.h` exposes `thread_block(wait_channel)` and `thread_wake(thread)`, but no mutex/wait-queue object or atomic “check condition + enqueue/block” API.
- `kernel/thread.c` changes state/channel in `thread_block()`; `thread_wake()` separately tests state, marks READY, enqueues, and clears the channel. The public API does not provide a shared lock/IRQ-save contract for making these operations atomic with a mutex condition check.
- Storage I/O while holding a busy-wait spinlock can deadlock if its owner is preempted and another runnable thread spins on the same lock.
- Integrity scans require caller serialization. Existing in-source calls are self-test calls; this does not prove production concurrent mutation/scan safety.

## Required scheduler-aware mutex contract

1. **Atomic acquisition-or-queue:** a waiter either acquires an unlocked mutex or is linked to its wait channel before the owner can release and miss it.
2. **No lost wakeup:** condition check, waiter publication, blocked-state transition, release, and wake selection share a reviewed synchronization discipline.
3. **Valid state transitions:** a blocked thread is not also runnable-queued; repeated/invalid wakeups cannot corrupt queue membership.
4. **Ownership/misuse:** define owner identity, recursion behavior, non-owner unlock, uninitialized mutex errors, and queue insertion failure handling.
5. **Correct handoff:** a blocked caller cannot continue while marked BLOCKED; save/switch/resume semantics must agree with the scheduler.
6. **No blocking under spinlock:** never hold a spinlock across block I/O, flush, potentially blocking allocation, or scheduler handoff.
7. **Lock ordering:** document ordering for filesystem operations, journal state, vnode/namespace state and device I/O; identify callback recursion.
8. **Failure-safe wake:** no waiter may be stranded or mutex ownership left inconsistent after an error.

## Filesystem integration sequence

1. Implement the scheduler atomic wait/wakeup protocol and deterministic tests: wake-before-block, block-before-wake, repeated wake, invalid waiter, and handoff/resumption.
2. Implement the scheduler-aware mutex and test mutual exclusion, handoff, owner errors, and lost-wakeup resistance.
3. Classify every VFS/filesystem callback for recursion and lock order before adding an operation-lock boundary.
4. Add controlled interleaving tests for append/write/truncate; rename/unlink/rmdir versus lookup/open/readdir; and integrity scan versus mutation. Check inode generations, vnode lifetime, transaction consistency, and post-test integrity.
5. Run host tests, exact-head build, ISO QEMU, native BIOS QEMU and fault-injection suites. Physical-device power-loss durability remains a separate hardware gate.

## Decision

**P5 remains OPEN.** No scheduler mutex or filesystem-wide lock was implemented in this audit because current block/wake APIs do not establish the atomic protocol needed to implement one safely. The next code task is scheduler atomic wait/wakeup—not a guessed mutex inside ProsenjitFS. Do not claim native filesystem concurrency completion until all gates have code and exact-head test evidence.


## 2026-10-27 follow-up: IPC wait-path hazard confirmed by source inspection

The current branch's `kernel/ipc.c` and `kernel/thread.c` expose additional reasons not to implement a mutex on top of the current API without first changing the scheduler contract:

- `ipc_channel_wait(channel, thread)` accepts a caller-supplied thread pointer, but calls `thread_block(channel)`, which blocks the scheduler's `current_thread` rather than the supplied `thread`. The function does not verify that `thread == thread_current()`. If they differ, it can block one thread while publishing another as the channel waiter.
- `thread_block()` only sets `wait_channel` and `PROS_THREAD_BLOCKED`; it does not itself perform a context switch. Any blocking primitive must define how the caller stops executing before returning to normal work, and must not leave a function returning to a thread that has been marked BLOCKED.
- The channel lock is a spinlock held while `thread_block()` runs. This currently avoids a simple sender interleave only under assumptions that need an explicit interrupt/preemption contract; it is not a general atomic scheduler wait primitive and must not be reused as a mutex design by analogy.
- `thread_wake()` changes state and enqueues the thread independently. A future atomic wait API needs a documented lock order between scheduler/run-queue state and object wait queues to avoid lock inversion.

### Required regression cases before implementation is accepted

1. `ipc_channel_wait` rejects a thread pointer different from `thread_current()` without changing either thread or the channel waiter list.
2. Wake arriving immediately before wait publication cannot leave a thread asleep with a ready condition.
3. Wait publication and BLOCKED transition are one scheduler-visible operation; the caller cannot continue as RUNNING while also listed as a waiter.
4. Wake-one removes exactly one matching waiter, clears its wait channel only after successful enqueue, and cannot duplicate run-queue membership.
5. Cancellation/termination racing with wake leaves no stale waiter pointer and cannot resurrect a DEAD thread.
6. Tests cover repeated wake, wake of non-waiter, full waiter queue, invalid process state, and enqueue failure rollback.

These are source-review findings, not a claim that a runtime race has been reproduced. No kernel code was changed in this follow-up. Keep scheduler atomic wait/wakeup as the next implementation milestone and do not mark P5 complete.
