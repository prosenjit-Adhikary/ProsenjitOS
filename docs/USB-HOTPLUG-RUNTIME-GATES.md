# USB Hotplug Runtime — Implementation Gates

Status: **not implemented**. This document records a source audit of `main` at
`97b52742fa8647bca3d359515ac89cca2715322f` (2026-11-13). It is a design
and verification boundary, not a completion claim.

## Verified current state

- `pros_xhci_poll_port_events()` is a bounded, fail-fast event-ring consumer.
  It takes the shared event lock, peeks before dequeue, consumes only Port
  Status Change Events, records coalesced port IDs, and leaves command/transfer
  completion TRBs queued for their transaction owners.
- `pros_xhci_take_pending_port_change()` atomically consumes one coalesced
  port-change bit. The current source search found no runtime caller.
- `pros_xhci_program_interrupter_disabled()` deliberately leaves
  `IMAN.IE` clear. The No-op command bootstrap explicitly uses polling mode
  and leaves `USBCMD.INTE` clear.
- The kernel PIC setup unmasks only IRQ0. The IDT currently installs the PIT
  timer gate at vector 32 and the syscall gate at vector 128; there is no
  xHCI MSI/MSI-X, IOAPIC, or local-APIC interrupt delivery implementation in
  the current source tree.
- The boot-time xHCI path enumerates a connected device and publishes a
  persistent MSC owner. It does not start a long-lived event worker or call
  the port-event polling primitive after boot enumeration.
- `pros_usb_msc_detach()` exists, but no xHCI port-disconnect event consumer
  invokes it. A safe detach must not free DMA-visible memory merely because a
  port-change bit was observed.

## Required implementation order

1. **Interrupt delivery foundation**
   - Establish and validate the platform interrupt-controller model (APIC
     discovery/enablement, vector allocation, EOI, masking, and routing).
   - Add PCI MSI/MSI-X capability parsing and safe device interrupt
     configuration, including rollback and readback.
   - Add a dedicated xHCI interrupt entry path that only records bounded
     notification state and acknowledges the interrupt correctly. It must
     not perform enumeration, block-device operations, or teardown in IRQ
     context.
2. **Worker notification and scheduling**
   - Define a race-free event/wait primitive with interrupt-safe waiter
     registration and wakeup; prove no lost wakeup across the check/block
     boundary.
   - Start a supervisor-only worker with a bounded stack and explicit
     shutdown/lifetime ownership. The worker must sleep when idle and must
     not busy-loop or poll from the timer IRQ.
   - Verify timer preemption, blocked-worker wakeup, failed-switch rollback,
     worker exit, and safe reaping under QEMU.
3. **Runtime event consumption**
   - Serialize the single event-ring consumer with command/transfer
     transaction owners. A port event must never cause a command or transfer
     completion TRB to be discarded.
   - Resolve root-port, slot, endpoint, and MSC ownership before changing
     device state. Coalesced notifications must be drained without losing a
     concurrent producer update.
4. **Disconnect and DMA quiescence**
   - Stop new MSC I/O admission before teardown and wait for all in-flight
     requests/references to drain.
   - Stop/disable endpoint and slot activity using bounded xHCI commands;
     prove controller ownership of every posted TD has ended before releasing
     any transfer ring, bounce buffer, context, event ring, or other DMA object.
   - If completion or halt state is uncertain, poison the path and retain DMA
     allocations. Never guess that DMA has stopped.
   - Unregister the block device and release mounts/VFS references only after
     transport I/O and outstanding filesystem users are quiescent.
5. **Reconnect and regression evidence**
   - Re-enumerate the same port with fresh slot/endpoint state and fresh
     generation ownership; reject stale completions from the prior device.
   - Add deterministic tests for connect/disconnect/reconnect, duplicate and
     coalesced port events, event-ring contention, in-flight I/O, failed stop,
     timeout, stale completion, and every teardown rollback boundary.
   - Require exact-head Build, ISO QEMU boot, native BIOS QEMU boot, and
     Security architecture checks. QEMU/mock tests do not replace physical
     controller hotplug and DMA stress evidence.

## Explicit non-claims

The current implementation is **not runtime USB hotplug**. PR #591/#595
provide bounded polling and event-lock regression coverage only. Kernel-owned
thread support (#592–#594) is a prerequisite, not a persistent worker or
interrupt-notification implementation. No disconnect/reconnect, controller
DMA-quiescence, physical-device hotplug, or SMP-safety completion is claimed.

## Next safe engineering milestone

Implement and test the platform interrupt-controller and PCI MSI/MSI-X
foundation independently before enabling xHCI interrupts. Do not set
`USBCMD.INTE` or `IMAN.IE` until the complete vector, handler, EOI, and
worker-notification path is live and verified.
