# Crash-record QEMU two-boot persistence test plan

Status: **DESIGN SPECIFICATION — NOT IMPLEMENTED / NOT PASSING**

Related work: PR #642, `kernel/crash_record.c`, native BIOS ATA panic writer,
`docs/CRASH-RECORD-PERSISTENCE-INTEGRATION-AUDIT.md`.

## Purpose and acceptance boundary

Prove that a controlled CPU exception enters the real IDT and production
`exception_dispatch()`, captures a record, invokes the production panic-only
ATA write/flush callback, and leaves a valid committed record on the disk image.
A second, independent QEMU process must boot the same retained image and recover
that record through the ordinary crash-record attach/reader path.

The current ISO/native-BIOS QEMU boot tests and fake-I/O self-tests do not prove
this. A normal native boot currently logs
`crash record panic writer attached (no record written)`.

## Test isolation

- Build a dedicated **test-only native BIOS image** with an explicit compile-time
  test configuration. Do not enable the trigger in normal/release builds.
- The test configuration must be visible in the build manifest and serial log;
  the normal build must compile the trigger out.
- Do not reuse a physical disk or mutate a developer disk. Use a disposable,
  CI-created raw image with the same geometry/partition layout required by the
  native boot protocol.
- Keep the raw image unchanged between QEMU process 1 and process 2. Do not
  recreate, truncate, copy-on-write reset, or regenerate it between boots.
- Use an external host-side controller (prefer QEMU GDB stub + batch GDB) to
  release the armed trigger only after the kernel reports that the validated
  panic writer is attached. Avoid keyboard timing or arbitrary delay triggers.

## Boot 1 — real exception and persistent write

1. Launch QEMU with the disposable raw image attached through the same IDE path
   used by the native BIOS test, serial output captured, and a local GDB stub.
2. Wait for an explicit test-armed serial marker. The marker must only be emitted
   after EDD/IDENTIFY geometry, ATA identity, flush capability, partition
   reservation validation, and crash-record attach all succeed.
3. Host controller invokes a dedicated test-only trigger function that executes
   `UD2`. It must enter vector 6 through the real IDT and production
   `exception_dispatch()`; do not call the crash-record writer directly.
4. Production fatal path captures the frame and calls
   `pros_crash_record_panic_flush_pending()`. The native callback must call
   `pros_ata_pio_panic_write_flush()`, which must complete both sector write
   and device cache flush.
5. Emit a test-only success marker only after the callback reports successful
   write+flush. The fatal path then remains fail-stop; QEMU is terminated by the
   host controller after capturing the marker.
6. Require the marker and the expected vector/RIP metadata in serial output.
   Missing marker, failed write, failed flush, unexpected vector, or a normal
   boot completion without the armed trigger is a test failure.

## Boot 2 — independent recovery

1. Start a **new QEMU process** with the exact same raw image path and geometry.
2. Do not arm or trigger a second exception.
3. During ordinary native crash-record attach, the normal slot scanner must
   recover the committed record.
4. A test-only readback observer may print the recovered record after attach,
   but must use the same normal reader/validation implementation. It must not
   parse raw sectors independently or bypass checksum/commit validation.
5. Verify magic, version, size, checksum, committed marker, sequence, vector,
   error code, RIP, and CR2/timestamp fields against Boot 1's expected values.
6. Verify that the record is consumed only after successful validation; test
   cleanup must not clear the raw image before assertions finish.

## Failure and recovery matrix

Run each case on a fresh disposable image unless the case explicitly requires
sequential writes:

| Case | Injection | Required result |
|---|---|---|
| Successful commit | No fault | Boot 2 recovers exact Boot 1 record |
| Write failure | Test-only ATA callback failpoint before sector completion | No success marker; no false committed record |
| Flush failure | Sector transfer completes, cache flush reports failure | No success marker; record is not reported durable |
| Torn newest slot | Controlled partial sector write | Boot 2 selects older valid committed slot |
| Both slots invalid | Corrupt both reserved slots in disposable image | Attach fails closed with ECORRUPT |
| No trigger | Normal build/image or test image without host release | No panic write; ordinary boot remains unchanged |

Failpoints must be confined to the test build and must not weaken or alter the
production ATA command path. Never inject corruption into a physical device.

## Required host-side safeguards

- Bound every QEMU/GDB operation with explicit timeouts.
- Confirm the QEMU PID/process has exited before launching Boot 2.
- Record the raw image SHA-256 after Boot 1 and before Boot 2; the image is
  expected to differ from its pre-test hash due to the crash-record write, but
  must remain byte-for-byte unchanged during Boot 2.
- Capture separate serial, debugcon, QEMU debug, and GDB-controller logs.
- Fail if the host accidentally launches Boot 2 with a different image path.
- Preserve the failing image and logs as CI artifacts for diagnosis.
- Keep this test independent from the ISO boot test; ISO's RAM-backed/fake
  storage path is not a substitute for native BIOS ATA persistence.

## Implementation sequence

1. Add a test-only build configuration and compile-time armed trigger/readback
   hooks; prove normal build has no trigger path.
2. Add a bounded host controller for QEMU GDB stub and a dedicated disposable
   native BIOS image target.
3. Add Boot 1 assertions for real vector 6, production dispatcher entry, ATA
   write completion, and flush completion.
4. Add Boot 2 assertions using the ordinary reader and exact field comparison.
5. Add isolated write/flush/torn/both-invalid cases and artifact retention.
6. Wire the dedicated test into Build/Boot CI and require exact-head success.

## Current completion state

- Production panic dispatcher and ATA write+flush callback exist in source.
- Fake-I/O tests cover callback write/flush failures, torn newest slot, older
  valid fallback, and both-invalid fail-closed behavior.
- ISO and native BIOS boot/self-test CI passed on PR head
  `5c941535a1847c0ac825e730eff4dd6b2bf1581f`.
- The successful native BIOS log explicitly says
  `crash record panic writer attached (no record written)`.
- No controlled real CPU exception, production ATA record write, second-process
  same-image readback, or real-media failure matrix has yet been demonstrated.

Therefore this specification does **not** change the acceptance status:
production crash persistence remains **NOT TESTED**, PR #642 remains
OPEN/UNMERGED, and Phase 1 remains IN PROGRESS.
