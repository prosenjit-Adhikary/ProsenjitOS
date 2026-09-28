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

The current kernel API is now exposed to CPL3 through capability-mediated send/receive syscalls. The syscall layer validates user ranges before copying data and checks READ/WRITE authority through the security intent pipeline. Blocking/wakeup and IPC creation from user space are still deferred.

## Next IPC milestones

1. Add user-space IPC channel creation and lifecycle ownership.
2. Expose blocking receive through the syscall scheduler handoff.
3. Add explicit cross-process endpoint transfer syscall semantics.
4. Add stronger fault-safe user-copy primitives and concurrency locking.


## Blocking foundation

The kernel now has a bounded receiver wait queue. A receiver can associate its
thread with a channel and enter `PROS_THREAD_BLOCKED`; a later send wakes one
waiting receiver and returns it to the scheduler run queue. The CPL3 receive syscall now uses an internal block sentinel, switches to a runnable
thread through the interrupt-frame scheduler path, and retries the receive syscall
when a receiver is woken. Until an idle thread exists, receive refuses to block if
there is no runnable successor.


## Concurrency protection

Channel queue indices, message count, and receiver wait-list mutations are protected
by a small atomic lock. The send path releases the queue lock before waking a receiver,
so scheduler/run-queue operations are not performed while the IPC channel lock is held.
This is a foundation for SMP-safe IPC, not a complete kernel-wide locking scheme.
User-buffer copying still depends on the existing address-range validation and does not
yet provide a fault-safe copy-from/to-user primitive.


## Validated user-copy layer

IPC syscall payloads now pass through shared `copy_from_user` and `copy_to_user`
helpers. Each helper validates the complete user range and required permissions before
performing the copy, and IPC queue locks are not held during user-memory access.
This reduces duplicated raw user-pointer dereferences. It is not yet fault-recoverable:
a concurrent unmap or page fault after validation still requires a page-fault fixup path.
