# Filesystem-backed `/init`: boot integration dependency audit

Status: design/audit note only. This is not evidence that a filesystem-backed userspace init can boot yet.

## Current boot-path facts

The normal kernel path currently does the following:

1. Runs `pros_init_selftest()`, which exercises negative filesystem-boundary cases.
2. Runs partition discovery, VFS-provider registry, storage-volume discovery and ProsenjitFS self-tests.
3. Initializes the driver manager and other kernel facilities.
4. Builds an ELF image from embedded `user_text_start` bytes in `kernel/kernel.c`.
5. Calls `pros_elf_load_with_args(&init_process, boot_user_elf, ...)` and verifies mappings.
6. Switches the address space and enters CPL3 using the embedded test image.

The normal path does not call `pros_init_load()`. The storage and filesystem self-tests are not a mounted root filesystem and do not prove that a real `/init` file is available during boot.

The hardware path is also not yet connected to this sequence: `kernel/kernel.c` includes the ATA PIO interface but does not call `pros_ata_pio_probe()` or `pros_storage_discover()` in the normal boot path. The existing ATA PIO probe targets the primary ATA master, while the documented ISO-only QEMU smoke command does not establish a writable root-disk fixture. Therefore there is currently no proven live block device → partition → active ProsenjitFS root mount → `/init` chain to pass into the loader. Do not make boot depend on a disk that the boot configuration has not explicitly attached and validated.

## Why the loader must not be wired in blindly

`pros_init_load()` requires a non-null, active `pros_vfs_mount`, a live process object, and security metadata. It opens `/init` through VFS, reads a bounded image, performs file admission and execution-security admission, and then calls the ELF loader.

At the current boot call site, no production root-mount object is passed to this function. Calling it with a fabricated or inactive mount would fail by design; bypassing the admission path or reusing test-only storage would create a false success rather than a bootable system.

## Required integration sequence

1. **Real device discovery:** initialize the supported block-device path and identify a boot/root volume; do not infer a real device from self-test fixtures.
2. **Partition validation:** use the existing partition parser and validate volume bounds against the parent device before creating a mount.
3. **Filesystem mount:** initialize the supported filesystem provider with the discovered partition/device, establish the root vnode, and activate the mount. Propagate mount and I/O errors.
4. **Init-file admission:** open `/init` through that active mount, enforce size bounds, validate file metadata and execution-security policy, then load ELF into a dedicated process address space.
5. **Process ownership/lifetime:** ensure the process object, its address space and loader-owned mappings outlive the initial entry; define failure cleanup before replacing the embedded test path.
6. **Execution transfer:** verify the loaded entry and stack permissions, establish the correct process/thread/current-process state, switch address space, and enter CPL3.
7. **Failure behavior:** if the root volume, `/init`, security admission, ELF load or process setup fails, emit a specific serial error and halt safely; never silently fall back to a less-secure execution path.
8. **Regression tests:** cover absent device/partition/file, inactive mount, malformed metadata, denied execution, truncated ELF, allocation failure, stack-construction rollback, and successful filesystem-backed boot.
9. **Only then** make the filesystem-backed init the default boot path and retain the embedded image only as an explicitly selected test fixture if useful.

## Required proof

- Local clean build with the project's warning policy.
- QEMU serial log from the exact tested commit showing real block-device/partition discovery, mount activation, security admission, ELF load, and CPL3 entry from the filesystem-backed `/init`.
- Negative boot tests demonstrate fail-closed behavior.
- Existing ProsenjitFS, VFS, partition, security, process and ELF regression suites still pass.

## Scope boundary

This note does not change ProsenjitFS, the bootloader, network stack, or hardware drivers. The current code does not yet provide sufficient evidence to safely replace the embedded CPL3 test image with a filesystem-backed init.