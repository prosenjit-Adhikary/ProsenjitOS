# Crash-Record Persistence Integration Audit

Audit ref: `04ae369645adbbe8aa21798e6c6e1feb6b185890`
Scope: Phase 1 panic/crash observability and persistence integration.
Evidence: source inspection of `kernel/kernel.c`, `kernel/crash_record.c`,
`kernel/include/crash_record.h`, `kernel/ata_pio.c`, and
`kernel/include/ata_pio.h`. This is a source audit; it is not runtime evidence.

## Finding

The crash-record component has a bounded two-slot on-disk format, checksum,
sequence selection, capture-only API, persistence API, and synthetic block
device tests for write/flush failure and retry. However, at the audited ref,
the production integration is incomplete:

1. `exception_dispatch()` calls `pros_crash_record_capture()`, but the
   repository has no production call to `pros_crash_record_attach()`.
2. The repository has no production call to
   `pros_crash_record_flush_pending()`. Its only call sites are the
   crash-record self-tests.
3. The native ATA block device is probed in the native-BIOS storage path, but
   it is not attached to the crash-record component. The ATA object is local
   to that storage initialization scope.
4. The exception path captures the record and then enters a terminal
   `cli; hlt` loop. It does not return to a normal safe-context worker.
   Consequently, simply adding a later main-loop flush call would not persist
   fatal exception records: the normal execution path never resumes.
5. Capture currently occurs after serial output and a ProsenjitFS diagnostic
   read in `exception_dispatch()`. A fault in this earlier reporting path can
   prevent the record from being captured; the nested-exception guard then
   halts immediately.

## Existing guarantees

- `pros_crash_record_capture()` only constructs a RAM record and computes
  its checksum; it does not perform block I/O.
- `pros_crash_record_flush_pending()` writes one slot and requires the block
  device flush operation to succeed before clearing the pending RAM record.
- Failed writes and failed durability flushes retain the pending record for
  retry in the self-test.
- The default reservation is LBA 2046 for two 512-byte sectors. The native
  disk image layout reserves sectors 2046-2047 immediately before the
  ProsenjitFS partition beginning at LBA 2048.
- The exception handler explicitly documents that block I/O must not run in
  its ordinary fault context.

## Required architecture before production integration

Do not call the ordinary block/VFS/ATA stack directly from the current
exception handler. A correct fatal-fault persistence path needs an explicit
panic handoff:

1. Capture the minimal record as the first bounded action after the nested
   exception guard, before serial, health, filesystem, or diagnostic work.
2. Transfer control to a dedicated panic continuation on a preallocated,
   known-good emergency stack. Do not return to the faulting instruction.
3. Define and document the panic writer's safety contract. It must not depend
   on heap allocation, VFS, ProsenjitFS, scheduler locks, normal thread stacks,
   or a lock that may have been held by the faulting CPU.
4. Use a bounded, fail-fast storage path with a prevalidated reserved region.
   If the storage path cannot be proven safe (for example, ATA state/lock is
   unavailable or the fault may have originated in the storage driver), retain
   the RAM record for diagnostics and halt without retrying indefinitely.
5. Attach and validate the reserved crash-record region during normal device
   initialization, before enabling the relevant storage-backed crash writer.
   Verify that the region does not overlap the partition table, bootloader,
   ProsenjitFS partition, or another device's data.
6. Keep the existing two-slot commit/checksum protocol. A failed write or
   flush must not clear the pending record.
7. On the next boot, read and validate both slots before any clear/consume
   operation; expose the recovered record through a bounded diagnostic path.

A normal-context deferred flush may still be useful for recoverable diagnostic
events, but it is not a substitute for the fatal-panic handoff.

## Required regression matrix

- Exception capture occurs before any optional reporting callback.
- Nested exception does not recurse into storage or overwrite the first record.
- Panic handoff switches to the dedicated emergency stack and never resumes
  the damaged context.
- ATA lock-busy, device-absent, timeout, write failure, and flush failure all
  terminate within a fixed bound.
- A storage-originated fault does not re-enter the failing storage path.
- Successful persistence survives reboot and validates sequence/checksum.
- Torn/corrupt newest slot falls back to the older valid slot.
- Reserved LBA boundary and partition-overlap rejection are tested.
- ISO/no-writable-disk boot remains functional and reports persistence
  unavailable without claiming a durable record.
- Both ISO QEMU and native BIOS QEMU paths emit explicit pass/fail markers for
  the supported portions of this matrix.

## Phase 1 status

**OPEN — NOT IMPLEMENTED / NOT VERIFIED.** The component-level self-test is
not evidence of production crash-record persistence. Do not mark the
panic/crash Phase 1 gate complete until a safe panic handoff, real device
attachment, durable write/reboot recovery, and exact-head required CI tests
are present.
