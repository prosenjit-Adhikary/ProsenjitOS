# ProsenjitOS Boot Architecture

## Architectural decision — 2026-09-29

ProsenjitOS will use an **original ProsenjitOS bootloader/boot protocol** as the preferred boot path. The project will not make GRUB or another traditional bootloader a mandatory dependency.

This does **not** mean removing MBR or GPT support. MBR and GPT are partition/storage metadata formats, not replacements for a bootloader. They will be implemented at the appropriate storage/partition layer when required for disk discovery and compatibility.

UEFI support will follow the standard UEFI OS-loader model: the ProsenjitOS loader will be a UEFI application, use firmware boot services during pre-boot, prepare the kernel environment, and then transfer control to the kernel. UEFI defines OS loaders as UEFI applications and provides boot services for loading images and accessing platform resources before `ExitBootServices()`.

## Preferred boot model

```text
Firmware
  |
  +-- Legacy BIOS
  |      |
  |      +--> ProsenjitOS legacy boot path
  |
  +-- UEFI
         |
         +--> ProsenjitOS UEFI loader
                    |
                    +--> detect boot device
                    +--> read required partition metadata
                    |      +--> MBR when applicable
                    |      +--> GPT when applicable
                    |
                    +--> locate ProsenjitOS boot/kernel files
                    +--> validate boot configuration
                    +--> verify kernel/component integrity
                    +--> prepare boot information
                    +--> transfer control
                              |
                              v
                       ProsenjitOS Kernel
                              |
                              v
                    Storage / VFS / Services
```

## Boot protocol boundary

The bootloader and kernel will communicate through a **stable ProsenjitOS Boot Protocol** rather than tightly coupling the kernel to one bootloader implementation.

Historical gap, now superseded by the version-2 implementation below: the current native BIOS handoff carries BIOS `DL` plus an EDD 3.0-validated legacy ISA/ATA channel and device path. The kernel only enables native ATA storage when that path is present and matches a supported standard channel/device. Exact-head BIOS/QEMU runtime verification is still pending; do not treat the source implementation as runtime proof.

The boot information contract should eventually contain only the information the kernel actually needs, such as:

- boot protocol version
- kernel load/entry information
- memory map
- framebuffer/display information when available
- firmware/ACPI information when available
- boot device identity
- partition/filesystem information needed during early startup
- loaded boot modules/initramfs information
- security/integrity verification results
- boot flags and recovery state

The kernel must validate the boot protocol version, sizes, addresses, and memory ranges before trusting supplied data.

## MBR/GPT policy

- **MBR:** support when required for legacy boot and partition discovery.
- **GPT:** primary modern partition-table target for UEFI systems.
- MBR/GPT parsing belongs to the partition/storage layer, not the kernel's core boot protocol.
- The bootloader may use partition metadata to locate the kernel, but the kernel must retain an independent storage/partition abstraction.
- Protective MBR handling for GPT media must not be confused with legacy MBR booting.

## Traditional bootloader compatibility

GRUB or another traditional bootloader may be supported as an **optional boot protocol adapter**, not as a required project dependency.

```text
                    ProsenjitOS Boot Protocol
                              |
              +---------------+---------------+
              |               |               |
       ProsenjitOS BL        GRUB       Other loader
              |               |               |
              +---------------+---------------+
                              |
                       ProsenjitOS Kernel
```

The kernel should not contain GRUB-specific logic. If a traditional bootloader is used, a small adapter/compatibility entry path should translate its boot information into the ProsenjitOS Boot Protocol.

This keeps the kernel independent from any particular bootloader while preserving practical interoperability.

## Security requirements

The boot architecture will mature in stages:

1. bootloader/kernel protocol validation
2. bounded memory-map and module validation
3. kernel/component integrity verification
4. signed boot components
5. Secure Boot integration where supported
6. measured/verified boot
7. rollback protection
8. recovery boot path

The security model assumes the bootloader can be attacked. Therefore, boot security must not depend on the bootloader being secret or obscure. Authentication, integrity, isolation, least privilege and verified boot are the security roots.

## Development order

### Stage A — current foundation
- preserve the existing bootable x86-64 path
- define the ProsenjitOS boot protocol
- keep the kernel independent from a specific traditional bootloader

### Stage B — native loader
- implement the ProsenjitOS UEFI loader
- implement/retain a legacy BIOS path where practical
- load the kernel through the ProsenjitOS boot protocol
- add strict validation and failure/recovery handling

### Stage C — storage-aware boot
- add MBR/GPT parsing as required
- add boot-device discovery
- locate kernel and boot modules through a narrow filesystem/storage interface
- keep FAT32/exFAT/NTFS/ProsenjitFS support behind the normal storage architecture

### Stage D — verified boot
- signed kernel and boot components
- integrity verification before execution
- Secure Boot integration
- measured boot
- rollback/recovery protection

### Stage E — interoperability
- optional GRUB/other-loader adapter
- test alternate boot paths against the same kernel boot protocol

## Important architectural rule

**Do not make the kernel depend on GRUB, MBR, GPT, or any single filesystem.**

The dependency direction should remain:

```text
Firmware
   |
Bootloader / Boot Adapter
   |
ProsenjitOS Boot Protocol
   |
Kernel
   |
Storage + VFS
   |
Partition Layer
   |
Block Devices
```

This allows the bootloader, partition format, filesystem, and kernel to evolve independently.

## Non-goals

- Do not create a new firmware standard.
- Do not replace UEFI with a proprietary firmware interface.
- Do not make MBR/GPT part of the kernel's core process or memory-management design.
- Do not make GRUB mandatory.
- Do not tightly link one bootloader implementation into the kernel.
- Do not sacrifice boot security for implementation secrecy.

## Validation requirement

Every bootloader milestone must have automated CI coverage for:

- successful kernel load
- malformed boot information rejection
- invalid memory-map rejection
- invalid module/address rejection
- kernel integrity failure handling
- expected kernel boot markers
- QEMU boot regression

The native loader must be introduced without breaking the existing boot/CI path.


### Native BIOS boot-device descriptor — initial versioned transport

The native BIOS stage-2 loader appends a ProsenjitOS-specific Multiboot2 tag (type `0x1000`) to its existing memory-map handoff. The current version-2 tag has a fixed 40-byte size: version, descriptor size, BIOS INT 13h transport, preserved BIOS drive number from `DL`, flags, normalized ATA I/O/control base addresses, and ATA master/slave selector. The tag is aligned using normal Multiboot2 eight-byte tag alignment and precedes the end tag. Version 1 was an intermediate source-only format and is superseded.

The kernel validates the descriptor only on the native boot path. It checks the boot-info pointer against the current 1 GiB identity-map limit, bounds the total structure size, validates every tag boundary and padding step, requires exactly one descriptor and a terminal end tag, and rejects unsupported version/size/transport/drive/flags/reserved values. A startup parser self-test covers a valid descriptor and malformed version, drive, and tag-size cases.

**Important limitation:** this descriptor carries BIOS drive identity; it does not identify an ATA channel/device. The current ATA PIO probe still selects primary-master independently. The kernel logs that the mapping is unresolved and does not equate BIOS `DL` with the ATA device selector. Do not use this metadata to authorize crash-record writes or infer that the native disk is primary-master. The next dependency is an EDD device-path/transport resolver (or another verified platform-specific mapping) that can prove the BIOS boot disk corresponds to the exact kernel block-device instance. Exact-head compile, native BIOS/QEMU execution, and parser self-test runtime remain unverified.


### EDD BIOS-to-legacy-ATA resolution (descriptor v2)

The native stage-2 loader now queries INT 13h AH=48 for the boot drive and accepts a direct ATA mapping only when the returned EDD 3.0 Device Path Information is internally valid:

- EDD signature \`0xBEDD\`, device-path length 36, reserved fields zero, and checksum over bytes 30–65 equal to zero;
- host bus exactly \`ISA \`, interface exactly \`ATA\` (space- or zero-padded);
- interface base I/O address exactly primary \`0x1F0\` or secondary \`0x170\`, with reserved interface-path bytes zero;
- ATA device path exactly master (0) or slave (1), with all reserved bytes zero.

For those two supported legacy channels, the kernel derives the standard control-port pair (\`0x3F6\` or \`0x376\`) and carries the normalized values in descriptor version 2. If AH=48, EDD 3.0 path information, checksum, bus/interface type, or any field validation fails, the loader still boots but marks the ATA path unavailable. The kernel refuses native ATA storage initialization in that case. PCI/ATAPI/USB/SCSI/RAID devices and non-standard legacy port layouts are deliberately unsupported by this direct PIO resolver; they require their own transport drivers and resolver.

The kernel parser validates the version-2 descriptor, permits either BIOS-drive-only metadata or a fully validated ATA path, and rejects mismatched port pairs, unsupported device numbers, and inconsistent flags. ATA PIO now exposes an explicit \`probe_at(io_base, ctrl_base, drive)\` entry point and accepts only the two standard ISA legacy channel pairs; native storage self-test and probe use the descriptor-resolved channel/device rather than silently selecting primary master.

Reference: Phoenix EDD 3.0, section 5.8 and tables 3–5, especially the device-path signature, ISA interface path, ATA device path and checksum rules: https://mbldr.sourceforge.net/specsedd30.pdf

This is still source-level evidence. EDD firmware behavior and the complete native boot-to-ATA path must be validated on exact-head QEMU/BIOS before this resolver is treated as runtime-proven.


### EDD 3.0 dual-format handling and QEMU PCI-IDE constraint

The stage-2 EDD query now reserves 74 bytes and recognizes both documented device-path formats: original Phoenix EDD 3.0 length 36 (66-byte result) and T13 EDD 3.0 length 44 (74-byte result). It validates the format-specific reserved bytes and checksum span (36 or 44 bytes beginning at offset 30), then applies common ISA/ATA path checks. A successful validation now returns immediately with the valid flag retained; failure clears all normalized ATA fields and returns unavailable.

**New platform constraint found in the existing CI QEMU command:** the native BIOS test starts the image with \`-drive ... if=ide\` on QEMU's default PC machine. QEMU's default PC emulates PCI IDE controllers, and SeaBIOS reports PCI host-bus identity for ATA disks behind a PCI controller. The current resolver deliberately accepts only EDD host bus \`ISA \`; therefore this exact QEMU setup will fail closed and will not reach the native ATA/ProsenjitFS milestones. This is expected with the present source, not a runtime failure result.

Next required slice: support EDD PCI interface paths only when the kernel verifies the referenced PCI function is class 01/subclass 01 (IDE), the selected channel is in compatibility mode (ProgIF native-mode enable bit clear), and the corresponding legacy I/O decode is enabled. Then map only channel 0/1 to the standard compatibility ports. Reject native-mode PCI IDE, AHCI/SATA, unknown class, invalid BDF/channel, and any mismatch. Alternatively, a dedicated ISA IDE test platform would need to be explicitly established; do not silently assume the default QEMU PCI controller is ISA.
