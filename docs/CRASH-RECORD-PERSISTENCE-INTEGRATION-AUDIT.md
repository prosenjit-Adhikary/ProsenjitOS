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


---

## Authoritative source-state correction — 2026-11-25

**Reviewed PR #642 head:** `86feffd117dc0758e8343e0046418d547bed2ef9`.

The earlier sections above are historical audit snapshots. Their statements that production panic routing is absent and that capture occurs after optional diagnostics are superseded by the 2026-11-24 implementation and the current source inspected at this head. The current state is:

### Implemented in the current source

- `exception_dispatch()` preserves the recoverable user-copy #PF path and the explicitly classified CPL3 process-fault handoff before entering the fatal path.
- For fatal non-syscall exceptions, the dispatcher reads CR2 when applicable, captures the bounded RAM-only crash record, normalizes the raw exception frame, and enters the production panic handoff before serial, health, filesystem, block-device, or ATA diagnostics.
- The production assembly handoff validates the versioned 128-byte frame, atomically claims first ownership, copies the complete frame into static storage, switches to the linker-reserved 16 KiB panic stack, and enters a non-returning assembly `cli; hlt` continuation. The continuation performs no C calls or device I/O.
- The assembly ISR checks the published owner state before re-entering C, so a nested exception after ownership is claimed fail-stops in assembly.
- The panic stack and frame storage are linker-reserved in BSS; current mapping checks enforce supervisor RW+NX mappings, alignment, size, containment, and endpoint translation. The panic stack has no unmapped guard page or canary; it is currently a terminal handoff stack and must not be described as protected against arbitrary stack growth.
- Crash-record capture now uses atomic first-owner publication and validates the pending record before peek, consume, or persistence. The startup self-test includes checksum corruption, nested-capture, sequence exhaustion, attach lifecycle, geometry, and write/flush retry cases.

These are source-level findings. The startup self-test and panic-stack probe still require exact-head Build/boot execution; this audit does not claim runtime PASS.

### Still not implemented: durable production persistence

A call-site scan of the current `kernel/kernel.c` found no production call to either `pros_crash_record_attach()` or `pros_crash_record_flush_pending()`. The attach/flush calls in `kernel/crash_record.c` are confined to its synthetic-device self-test. The native storage device is therefore not yet attached to the crash-record service, and a fatal panic currently ends in the RAM-only terminal continuation.

Consequently:

1. A captured fatal record is not yet durable across reset or power loss.
2. Do not add a normal main-loop flush and claim that it persists fatal records: the fatal continuation does not return to that loop.
3. Do not call the ordinary block/VFS/ATA path from the panic continuation. The current ATA lock and polling behavior are not a demonstrated panic-safe contract.
4. Do not silently choose the reserved LBA for arbitrary devices. Device identity, capacity, sector size, partition/layout overlap, and lifetime must be validated before attachment.
5. A storage-originated fault must not re-enter the same storage path.

### Correct next implementation order

1. **Phase C design audit:** identify the exact production storage-device object and lifetime on each boot path; define an explicit reserved-region ownership contract and prove it against the actual image/partition layout.
2. **Panic-safe writer contract:** specify a separate bounded, fail-fast raw-device operation (or a rigorously proven equivalent) that does not allocate, take a possibly-held lock, invoke health callbacks, or re-enter a faulting storage stack. If exclusive device ownership cannot be proven, halt with the RAM record only.
3. **Attachment and ownership:** attach only after device geometry and reservation validation; make initialization failure non-fatal to ordinary boot and preserve the no-writable-device path.
4. **Persistence regression:** inject lock-busy/device-absent/timeout/write/flush failures; prove bounded termination and pending-record retention; then reboot the same disk image and verify the record is recovered from disk, including torn/newest-slot fallback.
5. **Exact-head verification:** run Build/boot and Security workflows on the resulting exact PR head, verify both ISO and native BIOS paths, and retain the distinction between bounded QEMU observation timeout and a natural exit.

**Disposition:** panic frame normalization, early RAM capture, first-owner assembly handoff, and the bounded terminal panic stack are present in source. Durable crash-record persistence and its production device attachment remain OPEN. Phase 1 remains incomplete; PR #642 must remain unmerged until the full Phase 1 exit criteria and exact-head review/CI gates are satisfied.


---

## PR #642 exact-head re-audit — 2026-11-25

**Reviewed source head:** `db8deae084842f82ab4c098d072ec84f1a78d55c`.

This section supersedes any earlier head identifier in this document. The source review was performed against the PR branch, not the repository default branch.

### Production storage-path findings

- The native boot path creates a stack-owned `struct pros_ata_pio native_ata`, probes it, parses the MBR, locates the expected ProsenjitFS partition (type `0xDA`, first LBA 2048, 512 sectors), and creates a partition-device view.
- The ATA block device is initialized by `pros_ata_pio_probe()`; IDENTIFY populates device geometry and selects a flush command where supported. The ATA operations are serialized by a fail-fast per-device atomic lock.
- The generic `block_device_read/write/flush()` wrappers are not panic-safe: failures call `pros_health_record()`. The ATA callbacks also use the normal ATA lock and the normal million-iteration polling budget.
- The crash-record store itself accepts any 512-byte block device plus a caller-supplied LBA. It checks only sector-size and range at attach time; it does not prove device identity, reservation ownership, partition non-overlap, callback safety, or device-object lifetime.
- Its persistent read/write paths call the generic block wrappers. Its write path then calls the generic flush wrapper. Thus attaching the current native ATA object would still not make the store safe to call from the fatal continuation.
- The native ATA object is a local variable in the boot routine. Any production attachment must not outlive that object's scope. A persistent global or explicitly owned storage context is required if later safe-context use is intended.
- No production call to `pros_crash_record_attach()` or `pros_crash_record_flush_pending()` was found in `kernel/kernel.c` at this head. The crash-record service remains unattached in production; fatal captures remain RAM-only.

### Reservation finding — do not infer safety from the partition start

The expected ProsenjitFS partition begins at LBA 2048, while the crash-record header defines a default LBA of 2046 for two sectors. That adjacency is not proof that LBAs 2046–2047 are exclusively reserved: the MBR/GPT layout, other partitions, boot metadata, disk-image provisioning, and actual device capacity must all be checked. The current partition lookup alone does not establish ownership of those two sectors. Do not wire the default LBA into production based only on the partition's start LBA.

### Safe implementation boundary

The existing ATA lock is fail-fast, which is preferable to spinning on a lock held by the interrupted context, but that fact alone is insufficient. The generic block wrappers invoke health callbacks, the ATA poll budget is not calibrated as a panic-time bound, and no policy currently distinguishes storage-originated faults from unrelated fatal faults. A panic writer must have its own bounded raw operation and explicit eligibility state; it must fail closed if the device lock is busy, the fault originated in storage, geometry/layout ownership is unproven, or the flush command is unsupported.

A further sequencing constraint follows from the current implementation: because the production panic continuation is assembly-only and terminal, normal safe-context `flush_pending()` cannot persist a fatal record after capture. The writer must be integrated into a carefully specified pre-terminal handoff before the final halt, or through a separately proven reset/recovery mechanism. Do not weaken the assembly-only terminal contract by casually adding ordinary C calls.

### Disposition

No production attachment or panic-time write is safe to add in this slice. The next source task is to establish a stable storage-device lifetime and an image-level reservation contract, then design a standalone fail-fast raw writer and its storage-originated-fault exclusion. Only after those contracts are implemented should the persistence path and reboot regression be added. This is a source audit and design correction, not a runtime verification claim. Exact-head Build/QEMU, Security, and independent review remain required; no merge is authorized.


### Lifetime clarification for the native ATA object

The native path's `native_ata` object is a local in `kernel_main()`, but `kernel_main()` does not return: it eventually transfers control through `thread_enter_interrupt_frame()`. Therefore the local object's storage remains live for the lifetime of the running kernel under the current control flow. The concern is not an already-proven dangling pointer. Any future crash-store attachment must nevertheless document and preserve this non-returning lifetime assumption, and must not move the attachment into a helper whose stack frame returns while the store retains the device pointer.


### Native disk-layout reservation cross-check — 2026-11-25

This addendum corrects the reservation uncertainty in the preceding audit note.

**Confirmed native test-image reservation contract:**

- `scripts/build.sh` defines the native BIOS disk image as stage-1 at LBA 0, stage-2 at LBAs 1–24, the build-sized kernel beginning at LBA 25, a two-sector crash-record reservation at LBAs 2046–2047, and the disposable ProsenjitFS partition at LBAs 2048–2559.
- The build rejects a kernel extent that reaches the crash reservation and rejects a crash extent that overlaps the ProsenjitFS partition.
- Its readback verifier checks the complete image geometry, all occupied-extent overlaps, exact crash extent, initial zero contents, MBR partition type/start/length, and image bounds.
- The build then formats only the ProsenjitFS partition with the project formatter and verifies that the reserved crash sectors remain byte-for-byte zero.
- `docs/BOOT.md` documents this same native BIOS layout.
- The FAT32/exFAT USB image helper scripts are separate test media and are not the native BIOS disk image. Their partition gaps must not be confused with the native image ABI.

Therefore, **the native BIOS build image does explicitly reserve LBA 2046–2047**. The earlier statement that the repository had no image-level reservation evidence was too broad. This reservation is currently proven for the generated disposable native BIOS test image and formatter path; it is not a blanket reservation on arbitrary installed disks or removable media.

### Remaining critical blocker — BIOS boot-device identity is not bound to ATA

The exact next source dependency is now clearer than the generic reservation audit:

- `boot/native_stage1.asm` saves BIOS drive number `DL` and uses it for the stage-2 read.
- `boot/native_stage2.asm` saves and reuses `DL` for subsequent INT 13h kernel reads.
- At protected-mode handoff, stage 2 currently passes only `EAX = NATIVE_MAGIC` and `EBX = MB2_BUFFER`; it does not pass the saved BIOS drive identity or a versioned boot-device descriptor.
- `boot/entry.asm` preserves only those two values across BSS clearing and passes them to `kernel_main()`.
- `pros_ata_pio_probe()` unconditionally selects primary ATA channel ports `0x1F0/0x3F6` and device `drive = 0` (primary master).
- No source-level identity mapping proves that BIOS `DL` refers to that primary-master ATA device. The native boot layout's reserved LBAs must not be written through the independently selected ATA device until this identity is carried through a validated boot protocol and resolved to the exact block-device instance.

This is consistent with the explicit warning already present in `docs/BOOT-ARCHITECTURE.md`. The local `native_ata` object in `kernel_main()` remains live under the current non-returning control flow; object lifetime is not the current blocker. Device identity is.

### Required next implementation slice — versioned native boot-device handoff

Before crash persistence attachment, define and implement a small versioned boot handoff descriptor with:
1. magic, version, structure size, and explicit source/transport kind;
2. preserved BIOS drive number from stage 1/2, plus flags indicating whether it is valid;
3. strict address/range/alignment validation in the 32-bit entry before C trusts it;
4. preservation across BSS clearing without relying on BSS-resident globals;
5. an explicit kernel-side resolver that maps the reported boot device to a supported block-device instance, or returns unavailable;
6. fail-closed behavior when the BIOS drive cannot be proven to map to the ATA device (do not assume `DL=0x80` means primary-master in every topology);
7. source assertions and QEMU coverage for descriptor version/size/invalid values and for the matching/mismatching drive identity cases.

Do not simply pass the BIOS drive byte and equate it to the ATA `drive` bit: BIOS drive numbers, ATA channel/device selection, USB emulation, and other firmware mappings are different namespaces. A real resolver or an explicitly constrained, verified platform contract is required.

Only after this handoff is validated may the native disk-image reservation be bound to that exact device. Panic-safe raw I/O, bounded lock/poll behavior, storage-originated-fault exclusion, and reboot persistence remain separate gates. This is source-level audit/design evidence; no runtime or CI PASS is claimed.


### PR #642 continuation — versioned native BIOS drive metadata

Implemented on the PR branch:

- `boot/native_stage2.asm` now emits a ProsenjitOS-specific Multiboot2 tag type `0x1000` carrying version 1, fixed descriptor size, BIOS INT 13h transport, preserved BIOS `DL`, validity flag, and zero reserved field.
- `kernel/kernel.c` now validates the descriptor only for native boot, bounds the boot-info address/size to the current 1 GiB identity-mapped region, validates tag sizes/alignment/end marker, rejects duplicates and unsupported fields, and runs a parser self-test against valid and malformed fixtures.
- The kernel reports the BIOS drive number but explicitly states that BIOS drive identity is not yet resolved to ATA. It does not translate the BIOS drive byte into the ATA device selector.

This is an incremental boot-protocol transport implementation, **not** a storage-device identity resolver. The current ATA probe still chooses primary master independently; native storage use therefore remains constrained to the existing test platform assumption and must not be used as proof for crash-record reservation ownership. Next: inspect EDD AH=48 device-path information and determine whether the firmware reports a verifiable ISA/ATA port and device path; fail closed when unavailable or unsupported.

Source changes are not compiler/runtime verified. Exact-head Build/QEMU and Security checks plus independent review remain required. No merge is authorized.


### EDD device-path resolver implementation — 2026-11-25

The previous addendum's "EDD resolver next" item has been implemented at source level:

- Stage 2 issues INT 13h AH=48 with a 66-byte EDD 3.0 result buffer.
- It validates the 0xBEDD signature, 36-byte device-path length, reserved bytes, checksum, ISA host bus, ATA interface, standard primary/secondary base I/O address, and master/slave device path.
- It normalizes only supported legacy ISA/ATA paths into boot descriptor v2. Unsupported or malformed EDD information leaves the ATA-path-valid flag clear; the loader continues booting without claiming a mapping.
- The kernel parser accepts descriptor v2 and validates the normalized fields. Native storage initialization now fails closed when the ATA path flag is absent.
- ATA PIO probe APIs accept an explicit channel/device and reject non-standard base/control-port pairs. The native ATA self-test and actual block-device probe use the resolved descriptor values.

The BIOS drive byte is still not treated as an ATA selector. The mapping comes only from validated EDD device-path metadata. PCI ATA controllers and all non-ATA EDD interfaces remain unsupported by this direct PIO path.

**Verification boundary:** no assembler/compiler/QEMU/BIOS runtime execution has been performed on this exact head. EDD firmware may omit or vary optional v3 path data; such systems will now fail closed for native ATA storage while retaining the alternative GRUB boot path. Exact-head Build/QEMU, Security and independent review remain mandatory. Crash-record attachment/writer work remains blocked until this storage identity path passes runtime verification and disk reservation is bound to the resolved device.
