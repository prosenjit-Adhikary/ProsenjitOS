# ProsenjitOS IPC foundation

The current IPC layer provides a kernel-owned message/channel object with a
bounded, non-blocking ring buffer.

## Channel object

The IPC channel is a first-class kernel object of type PROS_OBJECT_IPC.
Each channel has eight message slots, and each message is limited to 64 bytes.

- ipc_channel_send() enqueues one message.
- ipc_channel_receive() dequeues the oldest message.
- PROSENJIT_IPC_EFULL reports a full queue.
- PROSENJIT_IPC_EEMPTY reports an empty queue.
- A receive with an undersized buffer does not consume the queued message.

## Capability model

An IPC channel can already use the existing capability-handle system.

- READ authorizes receiving.
- WRITE authorizes sending.
- DUPLICATE allows attenuated capability duplication.
- TRANSFER allows the existing copy/move capability-transfer primitives.

The current kernel API is exposed to CPL3 through capability-mediated send/receive syscalls. The syscall layer validates user ranges before copying data and checks READ/WRITE authority through the security intent pipeline. Blocking receive, scheduler handoff/wakeup, concurrency locking, fault-recoverable user-copy, and per-CPU fault state are implemented. User-space channel creation/lifecycle ownership remains outside this milestone.

## Next IPC milestones

1. Add user-space IPC channel creation and lifecycle ownership.
2. Add broader multi-process endpoint-transfer integration tests and production lifecycle handling.
3. Continue hardening SMP/process-lifecycle interactions around IPC.


## Blocking foundation

The kernel now has a bounded receiver wait queue. A receiver can associate its
thread with a channel and enter `PROS_THREAD_BLOCKED`; a later send wakes one
waiting receiver and returns it to the scheduler run queue. The CPL3 receive syscall now uses an internal block sentinel, switches to a runnable
thread through the interrupt-frame scheduler path, and retries the receive syscall
when a receiver is woken. Until an idle thread exists, receive refuses to block if
there is no runnable successor.


## Concurrency protection

Channel queue indices, message count, and receiver wait-list mutations are protected
by a small atomic lock. The current send and explicit wake paths call `thread_wake()`
while holding the channel lock; this means scheduler/run-queue mutation occurs inside
the channel critical section. Lock ordering and the saved interrupt-frame publication
protocol are not proven safe for SMP. Treat this as a single-CPU foundation, not as
an SMP-safe IPC implementation.
User-buffer copying still depends on the existing address-range validation and does not
yet provide a fault-safe copy-from/to-user primitive.


## Validated user-copy layer

IPC syscall payloads now pass through shared `copy_from_user` and `copy_to_user`
helpers. Each helper validates the complete user range and required permissions before
performing the copy, and IPC queue locks are not held during user-memory access.
This reduces duplicated raw user-pointer dereferences. It is not yet fault-recoverable:
a concurrent unmap or page fault after validation still requires a page-fault fixup path.


## Fault-recoverable user copy

The shared user-copy layer now uses a small assembly backend with a page-fault fixup
address. While a copy is active, a kernel page fault redirects the saved RIP to the
copy's recovery path, which returns an error instead of halting the kernel. A regression
probe exercises an intentionally unmapped kernel-mode source address. The design is
currently single-copy-at-a-time and should be extended with per-CPU fault state before
SMP support.


CI verification target: build workflow must execute both ISO and native BIOS QEMU boot tests for this milestone.

## Scheduler eligibility and no-successor handling

A queued thread whose owner becomes blocked or quarantined is retired from the
runnable queue as `PROS_THREAD_BLOCKED`, not misclassified as dead. The interrupt
scheduler also checks the currently interrupted thread before returning its saved
frame: it must not resume a thread whose process is no longer scheduler-eligible.
Because a dedicated idle thread is not implemented yet, this exceptional no-successor
case parks in an interruptible halt loop with `current_thread` detached; a later
interrupt can dispatch a newly runnable thread without overwriting the blocked
thread's saved interrupt frame. This is a single-CPU fallback, not a substitute for
a proper idle thread and synchronized wakeup semantics.
