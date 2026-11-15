# ProsenjitOS CI Validation Policy

## Purpose

CI validates the operating system at the narrowest layer that still exercises the changed contract. Boot media are delivery/test adapters; they are not the operating system itself. Building every media format for every documentation, tooling, or unrelated source change wastes runner time and obscures the signal from relevant tests.

## Required validation layers

| Change scope | Required CI validation |
|---|---|
| Documentation-only or metadata-only | Kernel compile/link and repository policy/security checks; no boot media build |
| Any kernel or shared OS source change | Full kernel compile/link plus relevant host-side unit and static contract tests |
| Kernel/runtime changes under `kernel/`, `include/`, or `third_party/` | GRUB/Multiboot2 ISO build and QEMU integration boot, including the existing storage/filesystem/USB runtime markers |
| GRUB/Multiboot2 boot path or ISO configuration | ISO artifact validation and ISO QEMU boot |
| Native BIOS stage-1/stage-2, shared kernel entry, ATA PIO, partition, block, storage, or ProsenjitFS disk-boot path | Native BIOS disk-image build and native BIOS QEMU boot | 
| Build orchestration only (`scripts/build.sh` or `Makefile`) | Build both boot artifacts and verify their structure; do not boot them in QEMU unless boot/runtime code also changed |
| Native UEFI loader (when implemented) | UEFI application build and firmware/QEMU boot tests |
| Release candidate | Full supported-media build and boot matrix, including ISO, native BIOS, and UEFI when available |

## Build target separation

`scripts/build.sh` always compiles and links the kernel. It accepts two optional environment variables:

- `PROSENJITOS_BUILD_ISO=1|0` controls GRUB/Multiboot2 ISO creation and ISO artifact verification.
- `PROSENJITOS_BUILD_NATIVE_BIOS=1|0` controls the native BIOS disk-image creation and its layout/formatter safety checks.

Both default to `1` to preserve the existing developer command `make iso` and full local build behavior. CI selects artifact creation independently from QEMU boot execution. A build-script/Makefile-only change validates both artifact builders but does not automatically boot either image.

## Safety rules

1. Never skip kernel compilation for a kernel-source PR.
2. Never skip the matching boot test when its loader, boot protocol, or boot-media contract changes.
3. Filesystem and storage changes retain their relevant integration coverage; native ProsenjitFS disk-path changes trigger native BIOS QEMU.
4. USB/xHCI and filesystem-provider changes remain covered by the ISO QEMU test's attached USB MSC fixture and required serial markers.
5. Unknown base revisions fail safe by building and boot-testing all supported current media paths.
6. Release validation remains comprehensive; PR targeting is not a substitute for the release matrix.
7. QEMU/software-model tests must not be represented as physical-hardware or real power-loss validation.

## Current workflow mapping

The Build workflow computes changed paths from the PR base/head (or push before/after) and independently selects four gates: ISO build, ISO QEMU boot, native BIOS image build, and native BIOS QEMU boot. QEMU and media-specific dependencies are installed only when the corresponding test is selected. The Security architecture workflow remains a separate PR gate. A skipped boot-media test is intentional only when the corresponding boot/media contract is outside the changed path set.
