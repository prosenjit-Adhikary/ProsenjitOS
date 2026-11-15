# ProsenjitOS Regression Matrix

## Purpose

This matrix records the automated validation layers that should remain green while storage, boot, VFS, security, kernel, and compatibility work proceeds.

## Boot paths

| Area | Required validation | Status |
|---|---|---|
| GRUB/Multiboot2 | ISO build + QEMU boot markers | Required for kernel/shared-ABI and ISO/GRUB changes; mandatory in release validation |
| Native BIOS | Native disk-image build + QEMU boot markers | Required for native loader/shared-entry and storage/filesystem/VFS disk-path changes; mandatory in release validation |
| Native BIOS handoff | Boot protocol, memory-map, ATA/partition and kernel handoff markers | Required whenever native BIOS boot validation runs |
| Native UEFI | Loader build + QEMU/firmware validation | Pending |

## Dependency-aware CI selection

The selector lives in `scripts/classify_ci_changes.sh`; its regression tests live in `scripts/test_ci_change_mapping.sh`.

- Documentation-only Markdown changes run Markdown structure/local-link validation and the separate security-design PR gate; they do not build or boot the OS.
- Every kernel source, kernel-private header, shared ABI/header, and vendored FatFs implementation change runs kernel compile/link plus ISO build and ISO QEMU integration. This deliberately catches indirect dependencies and newly added kernel translation units instead of relying on an incomplete allowlist of known filenames.
- Storage, ATA, block, partition, VFS, FatFs provider, and ProsenjitFS changes also run native BIOS build/QEMU persistent-disk integration.
- Shared entry/Multiboot2 and kernel linker-layout changes run both boot paths; native stage-1/stage-2 changes run native BIOS.
- Build orchestration-only changes build both artifacts but do not QEMU-boot unchanged runtime code.
- CI workflow/selector changes and unknown base revisions fail safe to the full implemented validation matrix.

See [CI Validation Policy](CI-VALIDATION-POLICY.md) for the test inventory, dependency rationale, and current coverage gaps.

## Existing host-side test coverage

| Area | Existing automated test | Scope |
|---|---|---|
| Desktop manager | `scripts/test_desktop_manager.sh` | Host unit test for desktop manager |
| Security Control Center | `scripts/test_security_control_center_service.sh` | Host test for isolated userspace service |
| Timer/interrupt/scheduler/IPC | `scripts/test_timer_irq_assembly.sh` | Assembly/source invariants for interrupt frame, EOI, scheduler handoff and IPC wakeup |
| Thread stack guard | `scripts/test_kernel_stack_guard.sh` | Source invariants for guarded kernel-thread stacks |
| QEMU timeout policy | `scripts/test_qemu_boot_timeout.sh` | Ensures both QEMU boot commands have the required timeout |
| CI selector | `scripts/test_ci_change_mapping.sh` | Regression tests for representative changed-path dependency classes |

The ISO/native QEMU boot tests execute additional in-kernel self-tests. They are integration coverage, not a replacement for dedicated host unit tests or hardware testing. The filesystem objdump output is diagnostic only, not a regression test.

## Storage layers

| Area | Validation | Status |
|---|---|---|
| Block device | Read/write contract and bounds self-tests | Exercised by kernel boot integration; dedicated host suite not present |
| Partition | MBR/GPT discovery self-test | Exercised by ISO/native boot integration |
| External filesystem providers | Provider ABI/registration compatibility when introduced | Future dedicated suite |
| ProsenjitFS | Format/mount/allocation/journal/recovery memory-backed regression | Required boot integration gate |
| Real disk | ATA PIO -> MBR -> ProsenjitFS persistent QEMU test | Required native BIOS gate |

## ProsenjitFS crash/recovery coverage

The deterministic native-filesystem self-test executes a VFS overwrite with simulated volatile device-cache loss and interruption at transaction stages 1–9. Stages before durable journal commit must preserve the old file image; stages after durable commit must recover the new image through redo replay. The matrix remounts the same durable medium after each simulated crash and verifies file contents.

Interrupted recovery is also exercised at pre-flush and post-flush boundaries. The failed recovery mount is followed by simulated power loss and a second mount, verifying idempotent replay of the committed replacement-image transaction.

The deterministic regression suite should continue to cover:

1. valid committed journal transaction replay;
2. incomplete/torn transaction discard;
3. invalid journal checksum rejection;
4. metadata checksum/range validation;
5. allocator state remaining consistent after recovery;
6. writable mount exposure only after recovery succeeds.

These tests validate filesystem logic but do not replace physical power-loss testing.

## Native BIOS safety checks

The native BIOS path should continue to validate:

- BIOS EDD capability before INT 13h extensions;
- boot-drive preservation across stage transitions;
- bounded stage-2 and kernel sizes;
- disk-read failure handling;
- A20 availability before protected-mode memory copies;
- valid memory-map construction;
- bounded kernel handoff addresses.

## CI change rule

A change that affects one subsystem should preserve all unrelated regression markers. If a required marker disappears, the change is incomplete even when the modified subsystem itself builds. A selector change must also update and pass the CI path-mapping regression cases.

## Known dependency boundaries

- Native ProsenjitFS QEMU disk testing uses the kernel-visible ATA PIO controller path and is a required CI gate.
- Native filesystem-aware boot requires a stable boot protocol plus storage/partition discovery.
- Future compatibility filesystem providers must not couple their parsers to kernel-core; provider I/O must use the common block/device contract.
- UEFI boot validation requires the native UEFI loader before Secure Boot/verified-boot work can be meaningfully tested.

## Completion principle

A milestone is complete only when its implementation, regression coverage, CI gate, and documentation agree. Deterministic tests must not be described as physical hardware durability validation.
