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
waiting receiver and returns it to the scheduler run queue. The actual CPL3
blocking syscall handoff is intentionally separate so an interrupt-return frame
is never resumed incorrectly.
