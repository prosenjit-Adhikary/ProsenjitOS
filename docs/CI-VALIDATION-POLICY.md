# ProsenjitOS CI Validation Policy

## Purpose

CI validates a change at the narrowest layer that still exercises the changed contract. Boot media are delivery and test adapters; they are not the operating system itself. An architecture document is not executable and must never trigger kernel compilation, ISO/native image creation, or QEMU boot by itself.

## Required validation layers

| Change scope | Required CI validation |
|---|---|
| Architecture/design/documentation only | Changed Markdown structure and local-link checks; applicable architecture/security contract checks. No kernel build, boot-media build, or QEMU boot. |
| Kernel source or shared ABI change | Kernel compile/link plus only the host-side or integration tests mapped to the affected subsystem. |
| Kernel execution, memory, scheduler, syscall, driver, VFS, USB, network, display, or security runtime paths | ISO build and ISO QEMU integration boot when the changed contract is exercised by the ISO runtime test. |
| Filesystem/storage, ATA, block, partition, or ProsenjitFS disk path | Relevant filesystem diagnostics, ISO integration where the VFS/provider contract is exercised, and native BIOS build/QEMU when the disk-boot contract is affected. |
| GRUB/Multiboot2 configuration or shared kernel entry | Matching ISO build and QEMU boot; shared entry/header changes also require native BIOS build and QEMU boot. |
| Native BIOS stage-1/stage-2 loader | Native BIOS disk-image build and native BIOS QEMU boot. |
| Build orchestration only (`scripts/build.sh` or `Makefile`) | Kernel compile/link and both boot artifact builders; no QEMU boot unless boot/runtime code also changed. |
| Desktop, security service, or timer/IRQ host-side code | Only the corresponding unit/static test suite. |
| Native UEFI loader (when implemented) | UEFI application build and firmware/QEMU boot tests. |
| Release candidate | Full supported-media build and boot matrix, including ISO, native BIOS, and UEFI when available. |

## Build target separation

`scripts/build.sh` always compiles and links the kernel. It accepts two optional environment variables:

- `PROSENJITOS_BUILD_ISO=1|0` controls GRUB/Multiboot2 ISO creation and ISO artifact verification.
- `PROSENJITOS_BUILD_NATIVE_BIOS=1|0` controls native BIOS disk-image creation and its layout/formatter safety checks.

Both default to `1` to preserve the existing developer command `make iso` and full local build behavior. CI selects artifact creation independently from QEMU boot execution. A build-script/Makefile-only change validates both artifact builders but does not automatically boot either image.

## Safety rules

1. Never skip kernel compilation for a kernel-source or shared-ABI change.
2. Never skip the matching boot test when its loader, boot protocol, or boot-media contract changes.
3. Do not run unrelated subsystem tests merely because they are inexpensive or already present in the workflow.
4. Filesystem and storage changes retain their relevant integration coverage; native ProsenjitFS disk-path changes trigger native BIOS QEMU.
5. USB/xHCI and filesystem-provider changes remain covered by the ISO QEMU test's attached USB MSC fixture and required serial markers.
6. Unknown base revisions fail safe by selecting the conservative full validation matrix.
7. Release validation remains comprehensive; PR targeting is not a substitute for the release matrix.
8. QEMU/software-model tests must not be represented as physical-hardware or real power-loss validation.

## Current workflow mapping

The Build workflow computes changed paths from the PR base/head (or push before/after) and independently selects documentation checks, kernel compilation, desktop/security/timer/filesystem host checks, ISO build, ISO QEMU boot, native BIOS image build, and native BIOS QEMU boot. Runtime integration is selected only for mapped subsystem paths; unrelated host tests are skipped. QEMU and media-specific dependencies are installed only when the corresponding test is selected. The Security architecture workflow remains a separate PR gate.
