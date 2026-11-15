# ProsenjitOS CI Validation Policy

## Purpose

CI validates a change at the narrowest layer that still exercises the changed contract. Boot media are delivery and test adapters; they are not the operating system itself. An architecture document is not executable and must never trigger kernel compilation, ISO/native image creation, or QEMU boot by itself.

The selector is dependency-aware rather than a list of only the most visible source filenames. It is maintained in `scripts/classify_ci_changes.sh` and regression-tested by `scripts/test_ci_change_mapping.sh`.

## Required validation layers

| Change scope | Required CI validation |
|---|---|
| Markdown/documentation only | Changed Markdown code-fence and local-link checks; the separate security-design contract gate remains a PR gate. No kernel build, boot-media build, or QEMU boot. |
| Any kernel translation unit, kernel-private header, shared `include/` ABI/header, or vendored FatFs source/header | Kernel compile/link, ISO artifact build, and ISO QEMU integration boot. The ISO boot executes the common kernel self-test suite, so this is the conservative runtime gate for kernel and shared-interface changes. |
| Memory, heap, address-space, ELF, process, scheduler, IPC, syscall, service manager, security, driver/device, PCI/USB, display, network, or other kernel runtime source/header | Same kernel + ISO build + ISO QEMU gate; relevant host-side source-invariant tests are added where an existing test script covers the changed contract. |
| VFS, FatFs adapter/provider, block, ATA, partition, storage, or ProsenjitFS source/header/format contract | Kernel + ISO build/boot, filesystem diagnostics where applicable, and native BIOS build/QEMU persistent-disk integration. |
| USB MSC QEMU fixture generator (`scripts/create_usb_dual_fs_image.py`) | ISO artifact build and ISO QEMU integration boot, because the boot test consumes the generated USB disk fixture. |
| Native ProsenjitFS image formatter (`tools/mkprosenjitfs.c`) | Native BIOS image build and native BIOS QEMU persistent-disk test; it does not affect the ISO kernel/runtime contract. |
| GRUB configuration | ISO build and ISO QEMU boot. |
| Shared entry, Multiboot2 header, or kernel linker layout | ISO build/boot and native BIOS build/boot; the common entry also runs the scheduler/interrupt-frame source-invariant checks. |
| Native BIOS stage-1/stage-2 loader | Native BIOS disk-image build and native BIOS QEMU boot. |
| Build orchestration only (`scripts/build.sh` or `Makefile`) | Kernel compile/link and both boot artifact builders; no QEMU boot unless boot/runtime code also changed. |
| Desktop manager/userspace DML code or its host test | Desktop manager host unit test and shell syntax checks. |
| Security Control Center userspace service/protocol or its host test | Security service host test and shell syntax check. |
| Timer/interrupt, interrupt-frame scheduler, IPC wakeup, syscall-frame, or guarded-thread-stack source contracts | Timer/IRQ + scheduler/IPC + stack-guard source-invariant scripts; kernel changes also select ISO QEMU through the general kernel rule. |
| QEMU boot-timeout policy script | QEMU timeout policy test. |
| CI workflow or changed-path selector | Conservative full implemented validation matrix plus CI path-mapping and QEMU timeout-policy regression tests. |
| Unknown/missing base revision | Conservative full implemented validation matrix. |
| Native UEFI loader (when implemented) | UEFI application build and firmware/QEMU boot tests. |
| Release candidate | Full supported-media build and boot matrix, including ISO, native BIOS, and UEFI when available. |

## Runtime coverage and dependency rationale

The ISO QEMU test validates the common kernel runtime through required serial milestones. Its current markers cover, among other things:

- physical memory, page allocation, paging, heap, address-space isolation, and ELF loading;
- process lifecycle, resource cleanup, parent/child ordering, thread/run-queue scheduling, guarded stacks, interrupt-frame switching, IPC wake/wakeup, and syscall ABI/policy;
- persistent crash records, security policy/W^X/NX, service-manager lifecycle, and driver/device registry contracts;
- block storage, partition discovery, FatFs adapter/provider, VFS I/O/security/provider registry, and ProsenjitFS format/mount/allocation/journal/recovery;
- PCI enumeration, xHCI controller/command flow, USB mass-storage BOT/SCSI, USB filesystem write/flush/remount/readback;
- display-device binding, network protocol/socket self-tests, and CPL3 userspace/IPC execution.

Therefore, a kernel source or shared header change must not rely on a narrow handwritten list of a few runtime filenames: every kernel source/header/vendor implementation change selects the ISO integration boot. This prevents a newly added kernel file or a previously omitted subsystem such as heap, object, IPC, ELF loader, service manager, or crash-record code from silently receiving compile-only coverage.

The native BIOS QEMU test repeats the common runtime milestones and adds native handoff, ATA PIO, MBR/partition, and persistent ProsenjitFS disk-path checks. Native BIOS testing is therefore selected for the storage/filesystem/VFS dependency chain and native loader/shared-entry changes, not for every unrelated kernel edit.

The USB MSC image generator is part of the ISO QEMU test's fixture dependency. A change to that generator must run the integration test even though it is not kernel source.

## Existing host-side test inventory

These are the host-side checks that currently exist and are selected by the classifier:

| Test | Source/contract it covers |
|---|---|
| `scripts/test_desktop_manager.sh` | `user/desktop/desktop_manager.c` and `tests/desktop_manager_test.c` |
| `scripts/test_security_control_center_service.sh` | Security Control Center userspace service/protocol and its test fixture |
| `scripts/test_timer_irq_assembly.sh` | IDT/timer ISR call alignment, EOI ordering, interrupt-frame ABI, scheduler handoff, syscall frame and IPC wakeup source invariants |
| `scripts/test_kernel_stack_guard.sh` | Thread stack arena/guard mapping, paging integration and boot self-test marker |
| `scripts/test_qemu_boot_timeout.sh` | Both ISO and native BIOS QEMU timeout minimums |
| `scripts/test_ci_change_mapping.sh` | Regression cases for documentation-only, kernel, shared-storage, FatFs, USB fixture, build-script, shared-entry, native-loader, desktop, scheduler/IPC, timeout-policy and workflow changes |

The Security architecture workflow is a separate PR gate. It currently checks the security architecture markers in `docs/SECURITY-MALWARE.md` and `docs/ROADMAP.md`; it is not a general source-code security analyzer.

## Known coverage gaps (do not overstate)

- There is no independent host unit-test suite yet for every kernel subsystem. The current runtime integration coverage is concentrated in the ISO/native QEMU boot self-tests described above.
- Windows PE/NT compatibility, driver compatibility, network, display, and several security subcomponents do not currently have a complete standalone unit/ABI conformance suite. Kernel compile plus the existing boot markers are not equivalent to exhaustive compatibility testing.
- The filesystem `objdump` step is an informational diagnostic (its grep commands are allowed to find no symbol); it is not a pass/fail filesystem regression test.
- QEMU and memory-backed/disk-image self-tests are software-model integration tests. They do not prove physical-hardware behavior or real power-loss durability.
- Native UEFI validation remains pending until a native UEFI loader exists.

Missing subsystem tests should be added in separate, subsystem-scoped work rather than being represented as if they already exist in this selector PR.

## Build target separation

`scripts/build.sh` always compiles and links the kernel. It accepts two optional environment variables:

- `PROSENJITOS_BUILD_ISO=1|0` controls GRUB/Multiboot2 ISO creation and ISO artifact verification.
- `PROSENJITOS_BUILD_NATIVE_BIOS=1|0` controls native BIOS disk-image creation and its layout/formatter safety checks.

Both default to `1` to preserve the existing developer command `make iso` and full local build behavior. CI selects artifact creation independently from QEMU boot execution. A build-script/Makefile-only change validates both artifact builders but does not automatically boot either image.

## Safety rules

1. Never skip kernel compilation for a kernel-source or shared-ABI change.
2. Never skip ISO runtime integration for a kernel source/header/vendor implementation change; the ISO test is the common kernel runtime regression gate.
3. Never skip the matching boot test when its loader, boot protocol, or boot-media contract changes.
4. Native storage/filesystem/VFS changes retain native BIOS persistent-disk integration as well as ISO provider/runtime coverage.
5. Do not run unrelated host tests merely because they are inexpensive or already present.
6. Workflow/selector changes and unknown base revisions fail safe to the full implemented matrix.
7. CI selector changes must pass the path-mapping regression script; QEMU timeout policy must be checked whenever its script or workflow changes.
8. Release validation remains comprehensive; PR targeting is not a substitute for the release matrix.
9. QEMU/software-model tests must not be represented as physical-hardware or real power-loss validation.

## Current workflow mapping

The Build workflow computes changed paths from the PR base/head (or push before/after) and delegates selection to `scripts/classify_ci_changes.sh`. It independently selects documentation checks, kernel compile/link, desktop/security/timer source-invariant tests, filesystem diagnostics, ISO build/boot, native BIOS build/boot, CI mapping regressions, and QEMU timeout-policy checks. CI workflow/selector changes select the conservative full matrix. QEMU and media-specific dependencies are installed only when the corresponding build/boot is selected. The Security architecture workflow remains a separate PR gate.
