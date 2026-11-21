# Crash-Record Persistence Integration Audit

Audit ref: `6a632c98eef806e2cdf79ba819aa097a2d8ee768`
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

## Exception-entry and emergency-stack source audit

Follow-up ref: `6a632c98eef806e2cdf79ba819aa097a2d8ee768`. The audit inspected
`kernel/idt.asm`, `kernel/linker.ld`, `boot/entry.asm`, the IDT setup and
`exception_dispatch()` in `kernel/kernel.c`, plus the ATA and block-device
implementations.

### Current stack topology

The TSS currently has three configured Interrupt Stack Table entries:

- IST1: 8 KiB NMI stack.
- IST2: 8 KiB double-fault stack.
- IST3: 8 KiB page-fault stack.

The IDT assigns vector 2 to IST1, vector 8 to IST2, and vector 14 to IST3.
Other exception gates use IST=0. There is no configured panic IST or separate
panic continuation stack. The bootstrap stack is 128 KiB and is also used by
the user-mode exit trampoline; it is not a dedicated panic stack.

The common exception stub in `kernel/idt.asm` passes the current exception
frame directly to `exception_dispatch()` on the stack selected by the CPU.
It does not copy the frame to a separate panic stack. The assembly stub then
expects the original frame to remain intact for its `iretq` epilogue. Any
handoff design must preserve this distinction: recoverable user-copy page
faults currently edit the original saved RIP and return, while fatal
exceptions halt.

### Capture ordering confirmed

In `exception_dispatch()`, the current order is:

1. Nested-exception guard and active flag.
2. Exception counters/vector/RIP bookkeeping.
3. Serial output for vector, error code, and RIP.
4. ProsenjitFS diagnostic query and serial output.
5. Read CR2.
6. Crash-record capture and health reporting.
7. Later exception-specific handling and terminal halt for fatal faults.

Therefore capture is not yet the first bounded operation after the nested
exception guard. The capture must be moved ahead of serial and filesystem
diagnostics, while keeping any health/reporting callbacks out of the minimal
capture section.

### Why the ordinary ATA/block path is not a panic writer

The ATA driver uses an atomic try-lock (`io_lock`) and returns an error if
the lock is already held. Its normal read/write/flush operations call bounded
poll loops with `ATA_TIMEOUT = 1,000,000` iterations. The generic block
wrapper also records health events on I/O failures. Those properties are
useful in normal operation but do not establish panic-context safety:

- The faulting execution may have interrupted code while the ATA lock was held.
  Re-entering the normal API would then fail fast, and must not spin waiting
  for that lock.
- A one-million-iteration device poll is not yet a measured wall-clock bound
  for a panic path.
- The block wrapper's health reporting is an additional callback path that
  should not be entered by a minimal panic writer.
- A fault in ATA/block/health code must not trigger recursive use of the same
  storage and diagnostic stack.

### Design decision before coding

Do not simply assign another IDT IST value to all exceptions or change the
current common stub to switch stacks without first specifying frame ownership.
IST entry changes the hardware stack frame, and the current recoverable
page-fault path relies on editing and returning through the original frame.
NMI, double-fault, and page-fault stacks are already distinct; a shared panic
stack also needs an explicit nested-fault policy so a second exception cannot
silently overwrite the first panic frame.

The implementation design must specify and test:

1. Which exception classes enter the panic continuation, and which recoverable
   exceptions remain on the current returnable path.
2. How the original hardware frame is copied or referenced, with correct
   handling of error-code and no-error-code vectors and IST-saved RSP/SS.
3. How the panic stack is reserved, aligned, mapped in both ISO and native
   boot paths, and protected from ordinary allocator ownership.
4. How a nested fault on the panic stack is detected without depending on
   ordinary C diagnostics or a potentially damaged stack.
5. How the panic writer proves exclusive access to ATA state, uses a separate
   bounded poll budget, bypasses generic health callbacks, and refuses storage
   re-entry for storage-originated faults.
6. How the reserved LBA is validated against the actual device capacity and
   disk layout before the panic writer is enabled.
7. How a QEMU reboot test proves that the persisted record is read back from
   the disk image, not merely retained in RAM or reported by a component
   self-test.

A safe first implementation slice should be split into independently
reviewable commits: (A) panic-frame/stack contract and assembly-only tests,
(B) minimal early capture and nested-fault behavior, (C) panic-safe device
attachment and fail-fast raw writer, and (D) reboot persistence/recovery tests.
Do not implement (C) until (A) and (B) have exact-head build and boot evidence.

## Updated Phase 1 gate

**OPEN — NOT IMPLEMENTED / NOT VERIFIED.** The source audit confirms that
existing NMI/DF/PF IST stacks are not a panic continuation stack, the common
stub has no frame handoff, and ordinary ATA/block I/O is not a safe panic
writer. No panic-stack or persistence implementation is claimed by this
document.
