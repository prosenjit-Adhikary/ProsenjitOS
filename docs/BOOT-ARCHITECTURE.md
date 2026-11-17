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

## ProsenjitOS Boot Protocol v1 — contract draft (Phase 1)

This section defines the first versioned handoff contract. It is a specification, not a claim that the current kernel or loaders already implement it. The existing native BIOS handoff still passes the legacy native magic plus a Multiboot2-compatible information block; the GRUB path still passes the Multiboot2 magic and block. Both legacy paths must remain working during migration.

### ABI and ownership

- The protocol is a fixed-width, little-endian x86-64 handoff ABI. Its initial entry register convention is the existing 32-bit protected-mode entry convention: EAX carries the protocol magic and EBX carries the physical address of the information block. The kernel entry assembly must preserve and widen these values before using them.
- The top-level structure begins with: magic (32-bit), version (16-bit), header_size (16-bit), total_size (32-bit), flags (32-bit), and a 64-bit physical address identifying the structure itself. All fields have explicit widths; compiler-native pointers, size_t, bitfields, and implicit padding are forbidden in the wire ABI.
- The header then describes the memory-map array and module array using physical base, count, and entry-size fields. A v1 consumer must reject an entry size smaller than the v1 record size, a count/size multiplication overflow, a range extending beyond total_size or the declared structure allocation, and any physical range that wraps or is not addressable by the active loader/kernel.
- Memory-map records use 64-bit base and length plus a fixed-width type and attributes. Zero-length, wrapping, overlapping-contradictory, or unknown mandatory records must not be treated as usable RAM. Unknown memory types are reserved, never available.
- Module records contain a physical start, byte length, and bounded type/flags metadata. A module must be wholly contained in a validated reserved/loader-owned range; a malformed or overlapping module is rejected before it can be consumed.
- Optional framebuffer and ACPI/RSDP data are represented by explicit physical address/length fields and validity flags. They are optional capabilities, not prerequisites for boot. An absent or invalid optional record is ignored only where the flag says it is absent; a flagged malformed record rejects the handoff.
- Boot-device identity is opaque and versioned. It must not make the kernel depend on BIOS drive numbers, UEFI handles, MBR/GPT, or a particular filesystem.
- The loader owns the handoff block and all referenced data until the kernel has validated and copied any data it needs. The kernel must not retain loader-owned pointers after the ownership-transfer point unless the protocol explicitly transfers ownership.
- The protocol structure, arrays, and modules must reside in memory that the loader reserves and that the kernel excludes from allocator free ranges until copied or released. The loader must never label its own live handoff storage as available RAM.

### Validation and failure policy

The kernel-side parser must validate, in this order, before initializing the physical allocator:

1. Entry magic, supported major version, minimum header size, total size, and self-address consistency.
2. Every offset/count/entry-size calculation using overflow-safe arithmetic and bounds against the complete handoff allocation.
3. Physical address width constraints for the current boot mode, alignment where required, and exclusion from forbidden low-memory/kernel/loader regions.
4. Memory-map record types, attributes, lengths, overflow, and non-contradictory coverage. Only explicitly usable RAM enters the allocator.
5. Module ranges, ownership, bounds, and pairwise overlap; reject malformed mandatory modules.
6. Optional framebuffer and ACPI records independently, without allowing invalid optional metadata to corrupt core memory ownership.

On any malformed mandatory field, fail closed with a serial diagnostic and halt before the PMM can allocate from untrusted memory. Do not silently fall back from a malformed v1 block to interpreting the same address as Multiboot2. Legacy Multiboot2 and current native handoffs are selected only by their distinct entry magic during the migration period.

### Compatibility migration

- Step 1: add a shared C ABI header and pure validation/parser tests without changing the current handoff.
- Step 2: add a GRUB/Multiboot2 adapter that converts the existing Multiboot2 information into a validated v1 block.
- Step 3: change the native BIOS stage-2 producer to emit the same v1 block; keep the legacy path available until both QEMU boot paths pass.
- Step 4: make the kernel consume only the normalized v1 representation after the adapter boundary; remove legacy parsing only in a separately reviewed change after regression evidence.
- Step 5: implement the UEFI loader as another producer of the same v1 block. It must obtain the final memory map and call ExitBootServices() with the current map key before transferring control; it must not use boot services after exit.

### Acceptance tests (required before marking this contract implemented)

- Valid minimal v1 handoff and valid full memory map.
- Bad magic/version/header size/total size/self-address.
- Truncated arrays, undersized entry records, excessive counts, integer overflow, and physical-range wrap.
- Zero-length and contradictory memory-map entries; reserved and disabled ranges never become allocator RAM.
- Module out-of-bounds, overlap, malformed length, and reserved-range ownership failures.
- Invalid optional framebuffer/ACPI metadata and valid absence flags.
- Legacy GRUB and native BIOS boot remain green throughout adapter migration.
- QEMU tests prove that malformed mandatory handoffs halt before PMM initialization and that valid handoffs reach the normal kernel boot markers.

**Status:** contract draft authored for review. No ABI header, parser, adapter, producer migration, UEFI loader, build, or runtime test is claimed by this documentation change.

### Canonical v1 wire layout — review baseline

To remove ambiguity before writing the shared header, v1 uses one contiguous, loader-reserved handoff allocation. All offsets below are byte offsets from the allocation's physical base; no array pointer is a compiler-native pointer. The allocation is identified by the physical address passed in EBX and repeated in `self_phys`. All integers are little-endian. The v1 header is exactly 128 bytes; producers set `header_size = 128`, `total_size >= header_size`, and `total_size` is the complete allocation size. Unknown header flag bits must be zero for v1.

| Header offset | Field | Width | v1 meaning |
|---:|---|---:|---|
| 0 | magic | u32 | `0x31534F50` ("POS1" in little-endian bytes) |
| 4 | version | u16 | 1 |
| 6 | header_size | u16 | 128 |
| 8 | total_size | u32 | Complete allocation bytes |
| 12 | flags | u32 | Only defined v1 bits may be set |
| 16 | self_phys | u64 | Physical base of this allocation; must equal EBX |
| 24 | mmap_offset | u32 | Offset of memory-map records; zero iff count is zero |
| 28 | mmap_count | u32 | Number of memory-map records |
| 32 | mmap_entry_size | u32 | Exactly 24 for v1 |
| 36 | modules_offset | u32 | Offset of module records; zero iff count is zero |
| 40 | modules_count | u32 | Number of module records |
| 44 | module_entry_size | u32 | Exactly 24 for v1 |
| 48 | framebuffer_phys | u64 | Physical framebuffer base; zero when absent |
| 56 | framebuffer_size | u64 | Framebuffer byte span; zero when absent |
| 64 | framebuffer_width | u32 | Pixels |
| 68 | framebuffer_height | u32 | Pixels |
| 72 | framebuffer_pitch | u32 | Bytes per scanline |
| 76 | framebuffer_format | u32 | Zero when absent; defined values reserved for header |
| 80 | acpi_rsdp_phys | u64 | Physical RSDP address; zero when absent |
| 88 | acpi_rsdp_size | u32 | Validated byte length; zero when absent |
| 92 | boot_device_offset | u32 | Offset of opaque boot-device bytes; zero iff size is zero |
| 96 | boot_device_size | u32 | Byte length of opaque boot-device identity |
| 100 | boot_device_kind | u32 | Versioned protocol-defined kind; zero means unspecified |
| 104 | reserved | 24 bytes | Must be zero in v1 |

The memory-map record is exactly 24 bytes: `base:u64`, `length:u64`, `type:u32`, `attributes:u32`. Type 1 is usable RAM; every other type is reserved from the allocator. Unknown types are treated as reserved. Zero length and arithmetic wrap are invalid. Producers must normalize overlapping firmware descriptors into a non-contradictory map before handoff; the kernel rejects contradictory overlaps rather than guessing precedence.

The module record is exactly 24 bytes: `start:u64`, `length:u64`, `type:u32`, `flags:u32`. Module payloads are external physical ranges, not embedded in the handoff allocation. Every module range must be wholly covered by non-usable/reserved memory-map coverage and must not overlap the kernel image, another module, or the handoff allocation. Unknown mandatory module types reject the handoff; unknown optional types may be ignored only when the optional flag is set.

The boot-device byte payload is inside the handoff allocation and is opaque to the generic kernel parser. It must not contain a firmware handle or BIOS drive number as a kernel-facing identity. The producer may encode a stable device-path description using a separately versioned payload format. The kernel validates only the payload's offset and length at this layer.

For v1, the three in-allocation regions (header, memory-map array, module array, and boot-device payload when present) must be 8-byte aligned where applicable, lie wholly within `[0, total_size)`, and not overlap one another. A zero count requires both offset and entry-size to be zero; a nonzero count requires a nonzero offset and the exact v1 entry size. Count multiplication and offset addition must be checked before use. The physical interval `[self_phys, self_phys + total_size)` must not wrap and must be represented as reserved in the memory map. Optional framebuffer and ACPI payloads are external physical ranges and are validated independently against address-width and length constraints.

The initial EAX magic is `0x31534F50`; EBX is the 32-bit physical address of the allocation at entry. This v1 handoff therefore requires the allocation to be below 4 GiB at the initial protected-mode entry. The kernel entry must preserve EAX/EBX before changing modes. The 64-bit `self_phys` field remains authoritative for the allocation identity after entry. This layout is a review baseline; implementation must add compile-time size/offset assertions and host tests for every boundary rule before any producer migrates.


#### v1 flag and optional-record constants

- Header `flags`: must be zero in v1.
- Memory-map `attributes`: must be zero in v1; future meanings require a new protocol version.
- Module `flags`: bit 0 means optional; all other bits must be zero. An unknown module type with bit 0 clear is fatal; with bit 0 set it may be skipped after its range has still been validated.
- `boot_device_kind`: 0 means unspecified; 1 means the payload is the ProsenjitOS opaque device-identity payload v1. Other values are unsupported in v1 and must be treated as absent only if the payload is explicitly optional; otherwise reject.
- `framebuffer_format`: 0 means absent; 1 means 32-bit RGBX8888; 2 means 32-bit BGRX8888. For either present format, width and height must be nonzero, pitch must be at least width multiplied by four, and size must cover pitch multiplied by height without overflow.
- ACPI RSDP: when present, size must be at least 20 bytes; validate the signature and legacy checksum. For revision 2 or later, require at least 36 bytes and validate the extended checksum over the declared length. The bytes remain firmware-owned/read-only metadata and are not copied into allocator-usable memory by virtue of being referenced.
- Module type 1 is initramfs; type 2 is a driver bundle. Other types are reserved for future versions and follow the optional/mandatory rule above.

These constants are part of the v1 wire contract, not implementation claims. The parser must reject nonzero reserved bytes and unknown mandatory flag bits.
