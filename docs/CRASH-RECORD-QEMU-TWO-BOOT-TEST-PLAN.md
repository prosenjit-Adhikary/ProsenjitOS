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


## 2026-11-27 — exact-head CI verification of the test-plan specification

**Tested PR head:** `64f450915d490e13570cd284dafc24b478b96ef8`.

- Build/Boot PR run `45987555198`: **SUCCESS**; job `140466925931` verified the PR merge checkout's second parent matched the tested head and completed kernel build, Phase 1 diagnostics, ISO QEMU, and native BIOS QEMU.
- Security Architecture PR run `45987555321`: **SUCCESS**.
- Native BIOS serial output includes the existing fake-I/O `persistent crash record self-test passed` marker and `crash record panic writer attached (no record written)`.
- Native BIOS debugcon reaches `PANIC_PRODUCTION_STACK_SWITCH` and `PANIC_NESTED_FAILSTOP`; this is the existing duplicate panic-owner fail-stop regression, not the new controlled vector-6 crash-record persistence test.
- The current workflow still performs a single native BIOS QEMU invocation with `-no-reboot`, and requires the existing attachment marker explicitly stating no record was written. It does not preserve/reopen the disk image in a second QEMU process.

**Interpretation:** exact-head build, security, ISO/native boot and current regressions pass. The two-boot test in this document remains **NOT IMPLEMENTED / NOT PASSING**. No controlled production fatal exception, confirmed production ATA record write+flush, or same-image second-process readback is established. The integration failure matrix and physical-media/power-loss behavior remain untested.


## 2026-11-27 — source/workflow integration audit (implementation preparation)

The exact-head successful run was inspected beyond its green conclusion. Relevant source and workflow facts:

1. `.github/workflows/build.yml` currently runs one native BIOS QEMU process with a 30-second timeout, `-no-reboot`, and `-no-shutdown`. It has no GDB stub/controller stage and does not reopen the same image in a second QEMU process.
2. The workflow explicitly requires `crash record panic writer attached (no record written)`. This is intentionally an attach-only assertion; it must not be weakened or relabelled as a persistence pass.
3. `scripts/build.sh` currently writes build products under the single `$ROOT/build` directory and compiles the kernel from a fixed CFLAGS array. There is no isolated test-kernel build mode yet. The two-boot harness must use a separate build/output directory and an explicit test-only macro; it must not overwrite the ordinary native BIOS image.
4. `kernel/kernel.c` validates EDD/ATA geometry, ATA IDENTIFY identity, flush support and the reserved MBR extent before calling `pros_crash_record_attach_panic_io()`. The attachment scan reads both slots and publishes the panic callback only after successful inspection. The safe injection point is after this attach has completed and while no block operation is active.
5. The production fatal exception path in `exception_dispatch()` captures the record in RAM, normalizes the fatal frame, and then calls `pros_crash_record_panic_flush_pending()` before production panic handoff. Vector 6 (invalid opcode) is suitable for the test-only controlled trigger; NMI, double fault and machine check must remain excluded.
6. The production callback in `kernel/kernel.c` delegates to `pros_ata_pio_panic_write_flush()`; that function enforces identity/flush prerequisites, rejects active block I/O and held ATA lock, performs one-sector PIO write and waits for cache flush completion. The test success marker must be emitted only after this callback returns success, never merely when the exception is entered.
7. `kernel/crash_record.c` provides `pros_crash_record_peek()`, which reads via the production slot reader and validates the record. A second-boot test-only observer can call this production reader and assert sequence/vector/RIP/error-code without changing the on-disk format.

**Next implementation slice (not yet committed):**
- Add an explicit build-directory override and test-only compile define to the build path, with normal defaults unchanged.
- Add a test-only, GDB-released trigger wait point after panic-writer attachment; release it into `ud2` only in the dedicated crash-test kernel.
- Add a narrowly scoped post-write+flush debugcon marker in the test build, emitted only after the production ATA callback returns success.
- Launch Boot 1 against a disposable native image with QEMU GDB stub; assert trigger, vector 6, write+flush marker and fail-stop.
- Launch Boot 2 as a new QEMU process against the exact same retained image, using the normal attach/slot reader plus a test-only observer; assert record contents and sequence.
- Keep ordinary ISO/native boot jobs on their existing non-triggering builds. Add bounded timeouts and failure logs; no physical media is used.

**Current implementation status:** this section is an integration audit and implementation checklist only. No test-only trigger, GDB controller, two-boot harness, second-boot observer, or real production write/readback has been implemented or passed.


## 2026-11-27 — first implementation slice added (CI pending)

**Source changes now present on the PR branch:**
- `kernel/kernel.c`: under `PROS_CRASH_RECORD_QEMU_TEST` only, added exported GDB trigger variable `pros_crash_record_qemu_test_trigger` and `pros_crash_record_qemu_test_gate()`.
- The test gate runs only after the production panic writer successfully attaches. On an empty record area it waits for a GDB release and executes `ud2`, entering the real IDT vector-6 fatal exception path. If a valid record already exists on the next boot, it validates sequence 1, vector 6, zero error code and nonzero RIP through `pros_crash_record_peek()`.
- The test-only ATA callback emits `CRASHREC_WRITE_FLUSH_OK` only after `pros_ata_pio_panic_write_flush()` returns success.
- `scripts/build.sh`: added a tightly allow-listed `build-crash-test` output directory and opt-in `PROSENJITOS_CRASH_RECORD_QEMU_TEST=1`; normal defaults remain `build` with no test macro.
- `.github/workflows/build.yml`: added an isolated test-kernel/image build and symbol checks proving the test gate/trigger exist in the test ELF and are absent from the ordinary ELF.

**Not yet implemented:** GDB controller and QEMU Boot-1/Boot-2 orchestration. The test-only image is not yet executed; the second-boot observer has not been exercised. No production persistence pass is claimed.

**Current commit under validation:** `dfd70a455714efd24f0806462526bffeb1c3f8ba`. Security Architecture check is SUCCESS; Build/Boot run `45989632708` is pending at the time of this update. Reassess after the exact-head run completes.


## 2026-11-27 — two-boot QEMU controller and CI wiring added (validation pending)

**Implementation now present on the PR branch:**
- Added `scripts/test_crash_record_two_boot.sh`. It starts the isolated test image with a paused QEMU GDB stub, breaks at the test gate that is called only after production crash-writer attachment, sets the test-only trigger, and detaches so the kernel executes the real `ud2` / IDT vector-6 path.
- Boot 1 requires `CRASHREC_WAITING_FOR_GDB`, `CRASHREC_TRIGGER_RELEASED`, and `CRASHREC_WRITE_FLUSH_OK`. The last marker is emitted only after `pros_ata_pio_panic_write_flush()` returns success.
- The script stops and reaps the first QEMU process before launching a second process with the exact same raw-image path. Boot 2 requires `CRASHREC_BOOT2_RECOVERY_OK`, emitted only after the production `pros_crash_record_peek()` reader validates the stored record.
- The script records the image SHA-256 after Boot 1 and requires it to remain unchanged throughout Boot 2; each QEMU/GDB phase is bounded.
- The Build/Boot workflow installs GDB for native-boot jobs, verifies it is available, and runs the dedicated two-boot controller after building the isolated test image. The ordinary kernel continues to be checked for absence of the test-only symbols.

**Current scope of the Boot-2 observer:** the production reader validates record magic, version, size, committed flag and checksum; the test gate additionally asserts sequence 1, vector 6, zero error code and nonzero RIP. CR2/timestamp equality and the full injected write/flush/torn/both-invalid integration matrix are not yet asserted by this host harness.

**Not yet proven:** this code has not yet passed its new exact-head CI run. Until the Build/Boot job completes with the explicit `REAL PANIC WRITE+FLUSH AND TWO-BOOT RECOVERY PASS` marker, treat the harness as implemented but unverified. The previous normal native BIOS boot still reports writer attachment with no record written; that old marker is not the two-boot test result.


## 2026-11-27 — GDB breakpoint safety refinement

The host controller now uses a GDB hardware breakpoint at `pros_crash_record_qemu_test_gate` rather than a software breakpoint. QEMU starts paused before BIOS execution; the gate symbol is in the later long-mode kernel mapping. A hardware breakpoint avoids patching the high virtual kernel code address while the guest is still in its pre-paging boot state. The trigger remains released only when execution reaches the post-attach gate. This refinement is awaiting exact-head Build/Boot validation.


## 2026-11-27 CI failure investigation

The exact-head Build/Boot run `45993110198` reached the GDB hardware breakpoint at `pros_crash_record_qemu_test_gate`, but the controller timed out waiting for `CRASHREC_WRITE_FLUSH_OK`. The Security Architecture workflow passed. This is a failed production persistence test, not a persistence PASS. The current logs do not yet establish whether the guest failed to resume after GDB detach, the trigger write was ineffective, or the production exception/write path failed. The controller now prints the trigger value and, on timeout, dumps the debugcon, GDB transcript, QEMU interrupt trace tail, and QEMU stderr so the next run can distinguish these cases. Re-run required; no merge permitted until the exact-head test is green and remaining failure-matrix gaps are addressed.


## 2026-11-27 follow-up CI result

The diagnostic commit's Build/Boot run `45995889015` failed before executing the GDB controller: Bash reported a syntax error at line 113. Root cause was over-escaped quotes in two optional GDB `printf` arguments. Removed those nonessential printf commands; retained the GDB breakpoint transcript, trigger write, trigger memory inspection (`x/wx`), and timeout log dumps. The prior run's failure therefore does not provide new evidence about the guest panic-write path. Re-run on the corrected script is required.


### 2026-11-27 — GDB trigger write correction

- Exact-head Build/Boot run `45996898178` reached the hardware breakpoint at `pros_crash_record_qemu_test_gate`, and Boot 1 emitted `CRASHREC_WAITING_FOR_GDB`.
- The controller then failed to release the gate: GDB reported that the trigger symbol had an unknown type, and the subsequent memory inspection still showed `0x00000000`. Consequently `CRASHREC_TRIGGER_RELEASED` and `CRASHREC_WRITE_FLUSH_OK` were absent. This is a test-controller issue; this run does not demonstrate a production ATA write failure.
- Corrected the GDB command to write the trigger through an explicit unsigned-int memory lvalue: `set {unsigned int} &pros_crash_record_qemu_test_trigger = 1`. The follow-up `x/wx` remains in place so the controller transcript proves the value changed before detach.
- Correction commit: `c75fb7c6e3b214377e4fd306e07b1b692f98537b`. Exact-head CI rerun is required; persistence remains NOT PASS until Boot 1 write+flush and Boot 2 recovery markers both appear.


### 2026-11-27 — numeric-address GDB trigger fix

- Exact-head Build/Boot run `45996898178` completed with failure in the two-boot harness. The GDB hardware breakpoint was hit and Boot 1 reached `CRASHREC_WAITING_FOR_GDB`, but the trigger readback remained zero; neither trigger-release nor write/flush marker appeared. Security Architecture passed.
- The attempted typed write still depended on GDB resolving a symbol whose ELF had no usable type metadata. The follow-up fix resolves `pros_crash_record_qemu_test_trigger` to its linked numeric address using `nm`, then writes and reads that numeric address using GDB's explicit `unsigned int` memory lvalue.
- Fix commit: `e3566c77017c99af934b4b5f6118a5055b23eaca`. This is a controller correction only. Await exact-head CI; do not infer production ATA write success or failure from the prior run.


### 2026-11-27 — production panic write reached; Boot 2 image-diff check refined

- Exact-head Build/Boot run `46001085450` reached the real production fatal path: GDB set the test trigger to `1`, Boot 1 emitted `CRASHREC_WRITE_FLUSH_OK`, and the controller confirmed production ATA sector write + flush.
- Boot 2 reached the recovery marker, but the harness rejected the run because the *whole disk image* hash changed. The native OS can legitimately update filesystem metadata during a normal second boot; whole-image immutability is therefore an invalid assertion for this test.
- Controller commit `68cbc9718188726ffc58a4a8b82a1c155b283bac` now snapshots exactly the two reserved crash-record sectors (LBA 2046–2047) after Boot 1 and compares those bytes after Boot 2. Whole-image hash remains diagnostic only; any change outside the reserved crash-record sectors is allowed, while mutation of the record reservation still fails.
- This is a test-oracle correction. It does not yet establish a PASS; exact-head CI must confirm Boot 2 recovery and unchanged reserved sectors.
