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


### Superseding correction — descriptor v2 and EDD resolver are now implemented in source

This section supersedes the immediately preceding v1-only handoff description and the earlier statement that the EDD resolver was merely the next task.

- Descriptor v2 is a 40-byte Multiboot2 custom tag with BIOS drive identity, flags, normalized ATA I/O base, control base, and ATA master/slave selector.
- Stage 2 calls EDD INT 13h AH=48 and accepts only a valid EDD 3.0 path for legacy ISA + ATA on standard primary/secondary channels. It validates the EDD signature, path length, reserved fields, path checksum, channel base, and device selector.
- Kernel parsing accepts either a valid BIOS-drive-only descriptor or a descriptor with a validated ATA path. Native ATA storage requires the ATA-path-valid flag; missing/unsupported EDD path fails closed.
- ATA PIO now probes the descriptor-selected standard channel and master/slave device. It no longer silently selects primary master in the native storage boot path. The compatibility no-argument probe/self-test wrappers remain available to other callers.
- The EDD 3.0 path format is based on Phoenix EDD 3.0 section 5.8 and tables 3–5: https://mbldr.sourceforge.net/specsedd30.pdf

This resolves the source-level BIOS-to-legacy-ATA identity gap only for those two supported ISA/ATA channel layouts. It does not resolve PCI IDE/AHCI, ATAPI, USB, SCSI, RAID, or arbitrary firmware mappings.

**No assembler/compiler or QEMU/BIOS runtime verification has been performed.** Do not claim the resolver works on hardware until the exact PR head completes Build/QEMU and its boot log demonstrates the descriptor and selected device. Crash-record production attachment/writes remain blocked pending that runtime evidence and exact-device reservation validation.


### EDD dual-format correction and PCI-IDE test-platform finding — 2026-11-25

A further source cross-check found and corrected two issues in the EDD resolver:

1. EDD 3.0 has two deployed layouts: Phoenix length 36 / 66-byte result and T13 length 44 / 74-byte result. Stage 2 now requests a 74-byte buffer, accepts both formats, checks the extra T13 ATA-path reserved QWORD and reserved byte, and calculates the corresponding checksum length (36 or 44 bytes). The successful branch now returns before the failure-cleanup label; previously the success path could fall through and clear the valid identity. Both issues are corrected in source but remain uncompiled/unexecuted.
2. The current CI native boot command uses QEMU's default PC machine with \`-drive ... if=ide\`. QEMU's default PC provides PCI IDE controllers; SeaBIOS reports PCI host-bus identity for ATA disks attached to a PCI controller. The current resolver accepts only ISA host-bus paths, so it will fail closed under this existing QEMU configuration and the expected native ATA/ProsenjitFS boot milestones will not be reached.

Next dependency is a strict PCI IDE compatibility-mode resolver: validate EDD PCI BDF/channel, verify the exact PCI function's class/subclass, require the selected channel's ProgIF native-mode bit clear and I/O decode enabled, then map to the standard legacy ports. Native-mode PCI IDE/AHCI/SATA and unverified mappings remain rejected. No runtime result is claimed.


### Descriptor v3 — support verified PCI IDE compatibility mode

The earlier PCI-IDE blocker is addressed at source level:

- Stage 2 recognizes both EDD host bus \`ISA \` and \`PCI \` for ATA interfaces. For PCI it extracts bus/device/function/channel from the EDD interface path, validates BDF/channel ranges and reserved bytes, and maps channel 0/1 to the corresponding legacy I/O port pair as a candidate only.
- The Multiboot2 custom tag is now descriptor v3, size 48 bytes, carrying host-bus kind and packed PCI BDF/channel in addition to the normalized legacy I/O/control ports and ATA device selector.
- Kernel parser validates v3 structure and channel-to-port consistency. Parser self-tests include a valid synthetic PCI channel-1 path, mismatched port pair, invalid channel, unsupported descriptor version, malformed size, invalid BIOS drive, no-path descriptor, and duplicate descriptor.
- Before native ATA access, the kernel verifies the referenced PCI function exists, has class 01h/subclass 01h (IDE), selected channel is in compatibility mode (ProgIF bit 0 primary / bit 2 secondary clear), and PCI I/O decode is enabled. Only compatibility-mode legacy ports are used; native-mode PCI IDE and AHCI/SATA are rejected.

PCI IDE ProgIF semantics and legacy compatibility port mapping are based on the PCI IDE controller programming-interface definition; UEFI PI describes checking ProgIF bit 0 to distinguish compatibility from native mode, and the IDE controller references document the primary/secondary mode bits and fixed legacy ports: https://uefi.org/specs/PI/1.8/V5_IDE_Controller.html

**Verification boundary:** this is source-level only. Exact-head build/assembly, parser self-test runtime, SeaBIOS EDD path capture, and native QEMU ATA/ProsenjitFS milestones remain unverified. Do not claim the PCI resolver works on QEMU/hardware until those pass. Crash-record attachment/writer remains blocked.


### EDD AH=48 returned-size semantics correction — 2026-11-25

A review against the EDD 3.0 result-buffer rules and SeaBIOS `fill_generic_edd()` found that stage 2 incorrectly treated the returned size WORD as the total byte count of the extended EDD structure. The caller supplies 74 as the maximum buffer capacity; the BIOS may set the returned WORD to 30 and still populate the device-path extension at offset 30. SeaBIOS keeps the entry capacity in a local, sets the returned WORD to 30, and writes the path/checksum according to whether the original capacity was 66 or 74. The former `>=66` / `>=74` checks would reject the normal SeaBIOS result and prevent PCI IDE handoff.

Corrected stage 2 to require a returned legacy size of at least 26, then validate the signature, path length 36/44, format-specific reserved fields and checksum. The input capacity remains 74 bytes. This is a source-level fix; do not mark it runtime verified until the exact head assembles and the QEMU/SeaBIOS native boot workflow passes.

References: Phoenix EDD 3.0 section 5.8 (https://mbldr.sourceforge.net/specsedd30.pdf); SeaBIOS `fill_generic_edd()` (https://github.com/coreboot/seabios/blob/master/src/block.c).


### Exception-vector input validation — 2026-11-25

The RAM-only capture API now rejects vector values above the architectural exception range (0–31) before claiming the capture guard or modifying pending-record state. Its self-test verifies that an invalid vector is rejected while an already pending valid record remains readable and unchanged.

Source-level change only; exact-head build, exception-path runtime, and QEMU persistence verification remain outstanding.


## 2026-11-26 — startup self-test runtime verification

- The existing `pros_crash_record_selftest()` is called during kernel startup and its success marker is required before boot continues. The exact source head `310045754cb1845a90c62407c8b08ba031f9b52f` completed the Build/boot workflow successfully, including both ISO QEMU and native BIOS QEMU boot tests; therefore the in-memory checksum-corruption, nested-capture, disk-slot fallback, write-failure and flush-failure self-test paths ran successfully on both boot paths.
- This does **not** establish production persistence across a real reset: the audit still finds no production attachment of the native block device to the crash-record service and no production flush call. Durable reboot recovery, device-identity-bound disk reservation, panic-safe raw writing and storage-originated-fault exclusion remain open.
- The documentation update is source metadata only; the new documentation-only commit still requires its own exact-head CI checks before the PR verification record is refreshed.

---

## PR #642 exact-head continuation audit — 2026-11-26

**Reviewed PR head:** `48327359c46dd7824f0926c4da42c71f143b84b0`  
**Branch:** `phase1/transactional-wait-reap-20261114`  
**Review type:** source re-check plus exact-head GitHub Actions verification.

### Previously reported source findings rechecked

The following findings from earlier review comments were checked against the current PR branch source. They are not current blockers:

1. **Context-switch stack-pointer ABI:** `kernel/context_switch.asm` saves and loads `[thread + 88]`. `kernel/include/thread.h` asserts both `offsetof(struct pros_thread, context) == 40` and `offsetof(struct pros_thread, context.rsp) == 88`. The earlier offset-80 mismatch is corrected.
2. **Quarantine restoration:** the scheduler records quarantine-induced parking in `quarantine_parked`; unquarantine restores only marked threads. The security-quarantine self-test retains an independently blocked-thread case and a second-process quarantine case.
3. **VFS open-handle ownership:** file and directory open paths claim caller storage in the active-handle registry before path lookup or CREATE mutation. A repeated claim of the same live pointer returns busy. The VFS self-test checks both ordinary reopen and `O_CREATE` reopen rejection while the original file remains open.
4. **Exception-dispatch re-entry:** `exception_dispatch_active` remains set while CPL3 fault handling performs scheduler selection and post-selection diagnostics. It is cleared only immediately before returning the selected frame. A nested exception during those guarded operations takes the fail-stop path.

These are source-level confirmations; the exact-head boot workflow also completed successfully as recorded below.

### Exact-head CI and runner evidence

At the reviewed head:

- Build and boot-test workflow run `45787742935`: **success**.
- Security architecture workflow run `45787742997`: **success**.
- The build job ran on a GitHub-hosted Linux runner, image `ubuntu-26.04`.
- The build job completed kernel build, ISO QEMU boot, native BIOS QEMU boot, and the selected diagnostic/test steps successfully.

This evidence applies to the exact HEAD above. It does not establish behavior on a later commit until that later SHA has its own required checks.

### Remaining Phase 1 blocker — durable fatal-crash persistence

The current production panic path captures a bounded RAM record and transfers to the assembly-only terminal panic continuation. It does not yet write that record to disk or recover it after reboot.

The existing generic block-device wrappers are not suitable for direct use from the fatal path: their error handling calls health-reporting code. The normal ATA path also uses its ordinary device lock and polling budget. A panic-safe writer must therefore be separately specified and implemented; the ordinary block/VFS/ATA stack must not be called from the terminal panic continuation without a proven bounded, non-reentrant contract.

Before enabling production persistence, the implementation must still prove all of the following:

- persistent ownership and lifetime of the selected device context;
- validated device identity, capacity, sector geometry, and exclusive reservation of the two crash-record sectors against the actual boot image/partition layout;
- a bounded fail-fast raw write and durability barrier that cannot spin on a lock held by the interrupted context and cannot invoke normal health/VFS/filesystem paths;
- storage-originated-fault exclusion and safe behavior when the device is absent, busy, unsupported, or times out;
- torn-write and two-slot fallback behavior, followed by a QEMU reboot test that reads the record back from the disk image rather than RAM.

If those guarantees cannot be proven for a device, the correct behavior remains RAM-only capture followed by halt.

### Review and merge gate

The PR remains open and unmerged. The current GitHub review-submission list contains **zero approving reviews**; the active `Protect main` ruleset requires one approving review. Do not merge or mark Phase 1 complete on the basis of passing CI alone.

**Disposition:** the previously reported source defects above are corrected and exact-head Build/Security checks pass. Durable production crash-record persistence and the required independent approving review remain open. Phase 1 is not complete.

---

## PR #642 continuation audit — 2026-11-26 (HEAD 183bb638)

**Exact reviewed HEAD:** `183bb638862d211b3a4e010ab5f81d57aa09f865`  
**Branch:** `phase1/transactional-wait-reap-20261114`

### Source re-check

The following earlier findings were rechecked against this exact branch source:

- **Context-switch ABI:** `kernel/context_switch.asm` saves and restores RSP at `[thread + 88]`; `kernel/include/thread.h` statically asserts the C field offset is 88.
- **Exception re-entry:** the dispatch guard remains set during scheduler selection and post-selection diagnostics; it is cleared immediately before returning the selected frame. Nested exceptions during guarded work take the fail-stop path.
- **VFS handle ownership:** file/directory handles are claimed in the active-handle registry before path lookup or create mutation; a repeated claim of a live object is rejected.
- **Quarantine release:** scheduler-induced parking is tracked by `quarantine_parked`; unquarantine restores marked threads while preserving independently blocked threads.
- **Crash-record production path:** the fatal exception path still captures the bounded RAM record and transfers to the terminal assembly panic continuation. It does not yet perform durable disk writes or reboot recovery.

These are source inspections, not new independent runtime reproductions.

### Exact-head CI

Both required workflows completed successfully on this exact HEAD:

- Build and boot-test: run `45862177827` — **success**.
- Security architecture checks: run `45862177685` — **success**.
- Build job ran on a GitHub-hosted `ubuntu-26.04` runner.

### Remaining gate

Durable fatal-crash persistence remains unimplemented. Do not add ordinary ATA/block/VFS calls to the terminal panic continuation. The next implementation must first establish persistent device ownership, validated identity/capacity/sector geometry and exclusive reserved-sector ownership, then a bounded fail-fast raw panic writer with storage-fault exclusion, followed by a QEMU reboot/readback test.

The PR is open, non-draft and reported mergeable, but has **zero submitted reviews**. The repository's `Protect main` ruleset requires one approving review. No merge was performed. Phase 1 is **not complete**.

## PR #642 continuation — production storage ownership and panic-writer design audit (2026-11-26)

**Reviewed source head:** `c89e969b440d86b3456619e0e4916dc3466f730c`  
**Scope:** native ATA device lifetime, geometry/identity, reserved-region proof, and panic-write entry constraints. This is a source/design audit; it does not implement or claim durable persistence.

### Exact source findings

1. **The only production ATA object is automatic storage.** In the native-storage block of `kernel_main()`, `struct pros_ata_pio native_ata` is a block-local object. Its embedded `pros_block_device` points back to that object through `private_data`. It is currently used by the partition view and filesystem in that scope, but it is not a persistent crash-service-owned device object. Production crash-record attach/flush call sites remain absent.
2. **Geometry is available after IDENTIFY, but identity is not retained.** `ata_identify()` sets the block device's 512-byte sector size, capacity, LBA28/LBA48 mode and flush-command capability. The 256-word IDENTIFY buffer is local and discarded; model/serial/firmware identity is not retained in `struct pros_ata_pio`. The boot descriptor supplies BIOS drive, legacy I/O ports, ATA selector and (for PCI IDE) BDF/channel, but those fields alone are not a stable disk identity.
3. **Normal ATA and block paths are unsuitable for panic entry.** ATA task-file operations serialize through `io_lock` and fail fast if already owned. The block wrappers call health-reporting code on invalid requests and I/O errors. The panic writer must bypass the generic block wrappers, but it must still refuse to touch the device if the ordinary ATA lock is already held.
4. **The current disk-layout check is incomplete for crash-region ownership.** The native path recognizes the ProsenjitFS MBR partition at LBA 2048 with 512 sectors. That alone does not prove sectors 2046–2047 are exclusively reserved: every valid MBR partition entry must be checked for overlap, malformed/overflowing ranges must be rejected, and GPT/protective-MBR or other unsupported layouts must fail closed. The two-slot default must not be enabled merely because the filesystem partition begins at 2048.
5. **The fatal path remains intentionally RAM-only.** It captures the minimal record and transfers to the terminal assembly panic continuation. The continuation does not return to `kernel_main()`; a deferred main-loop flush cannot persist fatal records.

### Safe implementation contract (design decision)

Before adding a production attach or raw writer:

- Move the native ATA device and its crash-writer state into statically allocated kernel-owned storage with an explicit initialized/validated state. Do not retain pointers to the current automatic `native_ata` object.
- During normal initialization, retain a bounded IDENTIFY-derived identity fingerprint (including validated serial/model words, plus controller BDF/channel or ISA port/drive path), sector geometry, LBA mode and flush capability. Reject absent, malformed, unsupported, or inconsistent identity data. Treat the fingerprint as a consistency guard, not cryptographic authentication.
- Parse and validate the complete on-disk partition scheme. Prove both crash LBAs are within capacity and disjoint from every partition and boot/reserved region. Reject GPT/protective MBR and unknown layouts until they have an explicit reservation contract. Do not assume the LBA 2046 reservation for arbitrary devices.
- Keep normal block-device and panic-writer APIs separate. The panic writer must use fixed-size stack/static buffers, no allocation, no VFS/ProsenjitFS, no scheduler/health callbacks, no blocking lock acquisition, and an independently specified finite ATA poll budget.
- The raw writer must atomically try-claim the same ATA task-file ownership used by normal I/O. If it is already held, fail immediately and halt with the RAM record intact. Do not clear or forcibly reset a lock potentially owned by the interrupted code. A successful write must issue the validated durability command and only then publish the slot as committed according to the two-slot protocol.
- Add an explicit storage-fault exclusion check before raw I/O. If the fatal RIP/vector indicates execution inside ATA/block/partition/storage-critical code, or the device's ownership/identity/lock state is uncertain, skip persistence and preserve RAM-only behavior. The exclusion must be testable and must not depend on health reporting.
- The panic path must never call the ordinary `block_device_read/write/flush`, `pros_crash_record_flush_pending()`, filesystem code, or normal ATA entry points.

### Required test gates before enabling the writer

- Static lifetime and device-context integrity across kernel initialization and panic handoff.
- IDENTIFY identity/geometry negative tests (invalid strings/word validity, zero/overflow capacity, unsupported flush capability, changed device fingerprint).
- Complete MBR overlap/bounds tests, including multiple partitions, malformed ranges, protective MBR/GPT, and exact adjacent-boundary acceptance.
- Panic writer tests for already-held ATA lock, absent/floating device, BSY timeout, ERR/DF, write failure, flush failure, and bounded exit count; every failure must preserve the pending RAM record.
- Storage-originated exception exclusion and nested panic behavior.
- Two-slot torn-write/fallback checks plus a real QEMU reboot/readback test proving recovery from the disk image after reset, not from RAM.
- Both ISO and native BIOS QEMU paths must pass; unsupported/read-only boot must remain operational and explicitly report persistence unavailable.

### Current disposition

No production source change was made in this audit. The findings establish prerequisites, not a completed panic writer. The safe next code slice is to add retained ATA identity/geometry and complete reservation validation with unit/boot regressions; only after those pass should a separately bounded raw writer be implemented. Durable fatal-crash persistence remains **OPEN / NOT IMPLEMENTED / NOT VERIFIED**.

Exact-head required workflows for `c89e969b440d86b3456619e0e4916dc3466f730c` completed successfully: Build and boot-test run `45864641265` — **success**; Security architecture run `45864641309` — **success**. These results validate this documentation/source head's existing test suite; they do not prove the new persistence design or reboot recovery.


## 2026-11-27 — production crash-record persistence call-graph recheck

Audited the current PR #642 branch with repository-wide symbol search and direct source inspection:

- `kernel/kernel.c` calls `pros_crash_record_capture()` from the exception/panic reporting path. It does not call `pros_crash_record_attach()` or `pros_crash_record_flush_pending()`.
- The only definitions/references returned for `pros_crash_record_attach()` and `pros_crash_record_flush_pending()` are their declarations/implementations in the crash-record module and its self-test usage. No production deferred-flush worker, scheduler callback, boot-time attach, or reboot-time persistence integration is currently wired.
- `pros_crash_record_flush_pending()` performs block-device slot writes through `crash_slot_write()`; it must not be invoked from the exception handler, panic handoff, timer/IRQ context, or a path that may be servicing a storage-originated fault.
- The existing in-memory capture is therefore not evidence of durable crash persistence. Persistent attach/flush/reboot-readback remains **NOT INTEGRATED / NOT VERIFIED**.

### Corrected next design slice — reuse the existing EDD resolver

A follow-up trace of `boot/native_stage2.asm` and `kernel/kernel.c` shows that the native BIOS boot-device resolver is already implemented; it must not be duplicated:

- Stage 2 records BIOS DL, queries EDD AH=48, validates the EDD 3.0 device-path signature/format/checksum and supported ATA interface path, then serializes descriptor v3. It distinguishes ISA ATA from PCI IDE compatibility mode and records the normalized legacy I/O ports, ATA device selector, and PCI BDF/channel where applicable.
- The kernel parses and validates that descriptor, verifies the exact PCI IDE function/channel and I/O-decode state when applicable, then probes the ATA PIO device using the resolved port/device tuple. The native boot path proceeds through MBR parsing, the expected ProsenjitFS partition, and LBA-translation/bounds checks.
- This is stronger than treating BIOS DL as ATA primary-master. It is a source-confirmed resolver plus existing native BIOS QEMU boot-path evidence, but it does **not** retain a durable disk serial/model fingerprint or establish exclusive crash-slot ownership. The current local `native_ata` object is automatic storage and is not a persistent crash-service-owned device context.
- The fatal exception route transfers to a terminal panic continuation and does not return to `kernel_main()`. Therefore an ordinary deferred main-loop worker cannot flush a fatal record after that handoff. Do not treat a proposed deferred worker as the solution to fatal-crash persistence.

Remaining prerequisites before attaching crash storage or adding a panic writer:

1. Move the validated ATA device/context into static kernel-owned storage and retain a bounded IDENTIFY-derived identity fingerprint, capacity, sector geometry, LBA mode and flush capability. Cross-check the retained identity against the EDD-resolved controller/channel/device path; reject changed, absent, malformed or ambiguous identity.
2. Prove exclusive ownership of the proposed crash LBAs against the **complete** partition table and boot/reserved regions. The expected ProsenjitFS partition at LBA 2048 does not by itself prove LBAs 2046–2047 are free. Reject malformed/overflowing entries, overlaps, protective MBR/GPT and unsupported layouts unless an explicit reservation contract exists.
3. Only after those gates, design a separate bounded raw panic writer with fixed buffers, finite ATA polling, atomic try-claim of the ordinary ATA task-file lock, storage-originated-fault exclusion, and no allocation, VFS, filesystem, scheduler, health callbacks or blocking locks. If any precondition fails, preserve the RAM record and report persistence unavailable.
4. Test identity/reservation rejection cases, held-lock and ATA BSY/ERR/DF/write/flush failures, pending-record retention, two-slot torn-write fallback, and actual QEMU reboot/readback from the disk image. A module self-test that flushes a RAM-backed fake device is not production persistence evidence.

No production source implementation or runtime persistence pass is claimed by this audit entry. Existing exact-head Build/ISO-QEMU/native-BIOS-QEMU/Security passes verify the existing boot path only; they do not establish crash-record persistence. Phase 1 remains open.


### 2026-11-27 — reserved-LBA guard added and exact-head boot verification

- Source commit `8044fe73008fd3f01104abe0e1d2486e0ee695c0` adds `native_crash_record_reservation_valid()` in `kernel/kernel.c`. The native BIOS storage path now validates the two proposed crash-record sectors `[2046, 2048)` against the parsed primary MBR partition extents and device capacity before continuing with partition selection.
- The guard rejects empty/unsupported or malformed extents, protective-MBR type `0xEE`, out-of-device extents, arithmetic overflow, and any overlap with either reserved sector. Its boot self-test covers a normal PFS partition starting at LBA 2048, an adjacent partition ending at 2046, overlaps at both reserved sectors and across the boundary, a protective MBR, and insufficient device capacity.
- Exact-head Build & Boot-test run #11687 (`45941848122`) completed **SUCCESS** on `8044fe73008fd3f01104abe0e1d2486e0ee695c0`; Security Architecture run #11169 (`45941848133`) also completed **SUCCESS** on that same head. The Build run includes kernel/artifact compilation, ISO QEMU boot and native BIOS QEMU boot, so the new guard's self-test executed on the native boot path.
- This establishes the reservation guard and its tested boot path only. It does not attach the crash-record module, retain an IDENTIFY identity fingerprint, prove GPT/unsupported-layout ownership, or persist/recover crash records across reboot. A bounded raw panic-writer and failure/reboot-readback tests remain unimplemented. Phase 1 is not complete.


## 2026-11-27 — ATA object lifetime and integration-boundary recheck

Reviewed the complete native-storage block in `kernel/kernel.c` together with
`pros_ata_pio_probe_at()`, `ata_identify()`, the block-device callbacks, and
the ProsenjitFS mount/unmount sequence.

- `native_ata`, `native_partition_view`, and `native_fs` are automatic
  objects scoped to the native-storage verification block in `kernel_main()`.
  The current path uses them to probe the disk, validate the MBR reservation,
  mount/write/unmount, remount read-only/read/unmount, and then leaves the
  scope. It does not currently publish the ATA object as a long-lived
  production storage service or attach it to the crash-record component.
- The inspected success path unmounts ProsenjitFS before leaving the block.
  Thus this audit does **not** identify a demonstrated dangling-pointer
  defect in the current storage self-test. The lifetime limitation is that
  the automatic ATA object is not available to a later panic/recovery service.
- `ata_identify()` reads all 256 IDENTIFY words into a local array, derives
  sector capacity, LBA28/LBA48 mode, and flush command support, then discards
  the identity words. `struct pros_ata_pio` retains controller ports, drive
  selector, addressing mode, flush command, lock, trace and block-device
  state, but no validated serial/model/firmware fingerprint or raw IDENTIFY
  identity.
- `pros_ata_pio_probe_at()` initializes the block-device callbacks with
  `private_data = ata`; those callbacks recover the owning ATA object from
  that pointer. Any future persistent attachment must therefore place the
  complete ATA object in static kernel-owned storage before initializing the
  block device, and must not copy/move it after callback registration.
- The current native storage block is a boot-time verification path, not a
  permanent device-manager registration. A crash service must not retain a
  pointer to the block-local object after the block ends.

### Next safe source slice

Before production crash-record attachment, implement and test a persistent
ATA-device context with explicit lifecycle state. Capture only bounded,
validated IDENTIFY identity fields; retain controller path, capacity, sector
size, LBA mode and flush capability; and reject reinitialization or use before
the identity state is committed. Add tests for ATA word-string byte order,
space trimming/termination, malformed or blank identity, capacity overflow,
flush capability validity, and stable fingerprint comparison. Keep the
existing temporary self-test path unchanged until the persistent context
passes both ISO and native BIOS boot tests. Do not add crash-record attach,
raw panic I/O, or claim durable persistence in this slice.

This is a source/lifetime audit and design refinement only. No source patch,
crash-record attachment, durable write, or reboot-readback PASS is claimed.
Phase 1 remains open.


### 2026-11-27 — ATA IDENTIFY identity retention implemented (validation-only)

Implemented a bounded identity decoder in `kernel/ata_pio.c` and retained
its output in `struct pros_ata_pio`:

- Decodes ATA IDENTIFY serial (words 10–19), firmware (23–26), and model
  (27–46), honoring ATA's high-byte/low-byte character ordering.
- Trims surrounding ASCII spaces, rejects blank fields and any non-printable
  or non-ASCII byte, and always terminates accepted strings.
- Sets `identity_valid` only when all three fields pass validation. Probe
  initialization clears prior identity state before issuing IDENTIFY.
- Invalid identity metadata does **not** fail ordinary ATA block-device
  probing or filesystem access; it only means this device is not yet eligible
  for a future identity-bound crash-persistence attachment.
- Added boot-invoked ATA self-test coverage for byte order, trimming and
  termination, retained field values, blank serial rejection, and
  non-printable-byte rejection.

This is metadata capture and validation only. The ATA object is still
block-local in the current native-storage boot path; no static device-service
lifecycle, EDD-path cross-check, capacity/geometry snapshot contract,
identity-change comparison, crash-record attach, raw panic writer, or
reboot-readback integration is implemented. No durable crash persistence is
claimed. Exact-head build/ISO-QEMU/native-BIOS-QEMU and security workflows
must pass before this slice is considered verified.


### 2026-11-27 — kernel-owned ATA context lifetime implemented

The native BIOS storage path no longer keeps its primary ATA PIO object on
the `kernel_main()` stack. `native_ata_context` now has static kernel
storage for the full kernel lifetime, preserving the address captured in
`block.private_data` by `block_device_init()`. The boot path also tracks
uninitialized/probing/ready/failed states and rejects a second initialization.

After probe, before partition parsing or filesystem use, the boot path checks
that the block device points back to the exact static ATA object, has read,
write, and flush callbacks, uses 512-byte sectors, and reports nonzero
capacity. Validated IDENTIFY identity is retained in the same static object;
if identity validation fails, ordinary storage remains available but the
boot path explicitly reports crash persistence as disabled.

This removes the specific block-local ATA object lifetime hazard for the
native boot storage context. It does not yet provide a reusable device-manager
API, durable identity fingerprint policy (including EDD-path cross-check and
identity-change handling), GPT/reserved-region ownership proof, crash-record
attachment, panic-safe raw writes, or reboot/readback verification. The
partition view and ProsenjitFS test objects remain scoped to the current boot
verification block and are not retained by a crash service. Exact-head CI is
required before considering this slice verified.
