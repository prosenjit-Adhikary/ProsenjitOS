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
- The header describes the in-allocation memory-map and module arrays using byte offsets relative to `self_phys`, counts, and entry sizes. Module records then identify their payloads by external physical start and byte length. A v1 consumer must reject any entry size that is not exactly the v1 record size, a count/size multiplication overflow, or an in-allocation region extending beyond `total_size` or the complete handoff allocation. External physical ranges (module payloads, ACPI RSDP, framebuffer) are validated separately for overflow, physical-address width, accessibility when dereferenced, ownership, and cross-object overlap; their payload lengths are not bounded by `total_size`.
- Memory-map records use 64-bit base and length plus a fixed-width type and attributes. Zero-length, wrapping, or overlapping records are invalid. Unknown memory types are reserved, never available; there is no separate 'mandatory memory-map record' flag in v1.
- Module records contain a physical start, byte length, and bounded type/flags metadata. A module must be wholly contained in a validated reserved/loader-owned range; a malformed or overlapping module is rejected before it can be consumed.
- Optional framebuffer and ACPI/RSDP data are represented by explicit physical address/length fields and sentinel values, not separate validity bits: all-zero fields mean absent; a partially populated or malformed present record rejects the handoff. They are optional capabilities, not prerequisites for boot.
- Boot-device identity is opaque and versioned. It must not make the kernel depend on BIOS drive numbers, UEFI handles, MBR/GPT, or a particular filesystem.
- The loader owns the handoff block and all referenced data until the kernel has validated and copied any data it needs. The kernel must not retain loader-owned pointers after the ownership-transfer point unless the protocol explicitly transfers ownership.
- The handoff block, module payloads, kernel image, and any loader memory that remains live after handoff must not be exposed to the PMM as free RAM. Producers should describe these ranges as non-usable/reserved. If a legacy adapter receives a map that labels the kernel image or another retained kernel-owned range as usable, the adapter/parser boundary must explicitly subtract that range before PMM initialization. Reclaimable loader memory may become usable only after the kernel has completed the documented ownership transfer and no retained pointer refers to it.

### Validation and failure policy

The kernel-side parser must validate, in this order, before initializing the physical allocator:

1. Entry magic, exact supported v1 version (`1`), exact header size (`128`), total size (128 bytes through 16 MiB), and self-address consistency.
2. Every offset/count/entry-size calculation using overflow-safe arithmetic and bounds against the complete handoff allocation.
3. Physical address width constraints for the current boot mode, alignment where required, and exclusion from forbidden low-memory/kernel/loader regions. Determine the effective `MAXPHYADDR` from the boot CPU before validating any physical range: use `CPUID.80000008H:EAX[7:0]` when that leaf is supported; otherwise require CPUID leaf 1 to report PAE and use the architectural 36-bit fallback; if PAE is absent, reject because the target x86-64 long-mode environment cannot be supported. Require the resulting width to be in the architectural range 32 through 52 bits. For every non-empty physical interval, checked-add `end = base + length` and require `base < 2^MAXPHYADDR` and exclusive `end <= 2^MAXPHYADDR`; the exact top boundary is valid, any byte at or above it is not. Apply this to every memory-map record and every physical object, including framebuffer MMIO, RSDP, modules, handoff allocation, kernel image, and retained loader ranges.
4. Memory-map record types, attributes, lengths, overflow, and non-contradictory coverage. Only explicitly usable RAM enters the allocator.
5. Module ranges, ownership, bounds, and pairwise overlap; reject malformed mandatory modules.
6. Framebuffer and ACPI records independently. All-zero absence sentinels are valid; a partially populated or malformed present record is a fatal v1 validation error before PMM initialization (v1 does not silently ignore malformed optional records).

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
- Truncated arrays, undersized or oversized v1 entry records, excessive counts, integer overflow, physical-range wrap, nonzero reserved bytes, and overlapping in-allocation regions.
- Zero-length and contradictory memory-map entries; reserved and disabled ranges never become allocator RAM.
- Module out-of-bounds, overlap, malformed length, and reserved-range ownership failures.
- Invalid optional framebuffer/ACPI metadata and valid all-zero absence sentinels.
- Legacy GRUB and native BIOS boot remain green throughout adapter migration.
- QEMU tests prove that malformed mandatory handoffs halt before PMM initialization and that valid handoffs reach the normal kernel boot markers.
- Physical-address tests cover the highest supported address, one-past-width rejection, 1 GiB early-dereference boundary, valid ranges crossing the 1 GiB allocator clipping boundary, and overflow at every external-object end calculation.
- Producer-normalization tests cover unsorted, overlapping, conflicting, disabled, unknown, and gapped firmware descriptors; verify reserved-wins interval splitting and exact sorted/disjoint output.
- E820 adapter tests cover 20-byte and 24-byte records; returned lengths below 20; partial 21–23-byte responses (legacy treatment, ignore partial extension); 24-byte-or-larger responses with enabled/disabled extended attributes; continuation after the 32-record capacity; incomplete firmware responses; and load/bounce ranges that overlap reserved descriptors.
- Multiboot2 adapter tests cover 8-byte information-base/total-size alignment, minimum, 16 MiB maximum, and overflowing total_size, tag-header/size/padding bounds including arithmetic-wrap attempts, unknown-tag skipping, early/missing/non-final terminators and trailing bytes, missing/duplicate mmap tags; module tag minimum size, bounded UTF-8/NUL validation, empty/reversed ranges, explicit type/optionality mapping, reserved coverage and every cross-object alias; duplicate mmap/ACPI/framebuffer singleton tags, entry strides greater than 24, non-multiple strides, nonzero entry versions, nonzero ignored source-entry reserved dwords, empty mmap payloads, order-independent usable/reserved overlap normalization; and framebuffer fixed/type-specific minimum sizes, indexed-palette arithmetic, exact RGB mask/position translation, unsupported layouts, and malformed geometry.
- UEFI producer tests cover descriptor-version/stride/truncation errors, page-count arithmetic overflow, ACPI configuration-table preference/copy, ExitBootServices invalid-key retry, and proof that no boot-service call occurs after successful exit.

**Status:** contract draft authored for review. No ABI header, parser, adapter, producer migration, UEFI loader, build, or runtime test is claimed by this documentation change.

### Canonical v1 wire layout — review baseline

To remove ambiguity before writing the shared header, v1 uses one contiguous, loader-reserved handoff allocation. All offsets below are byte offsets from the allocation's physical base; no array pointer is a compiler-native pointer. The allocation is identified by the physical address passed in EBX and repeated in `self_phys`. The allocation base (`self_phys`) must be 8-byte aligned; because EBX must equal this base, the entry address is aligned as well. All integers are little-endian. The v1 header is exactly 128 bytes; producers set `header_size = 128`, `total_size >= header_size`, and `total_size` is the complete allocation size. The complete header `flags` field must be zero in v1.

| Header offset | Field | Width | v1 meaning |
|---:|---|---:|---|
| 0 | magic | u32 | `0x31534F50` ("POS1" in little-endian bytes) |
| 4 | version | u16 | 1 |
| 6 | header_size | u16 | 128 |
| 8 | total_size | u32 | Complete allocation bytes |
| 12 | flags | u32 | Must be zero in v1 |
| 16 | self_phys | u64 | Physical base of this allocation; must equal EBX |
| 24 | mmap_offset | u32 | Nonzero offset of the mandatory memory-map array |
| 28 | mmap_count | u32 | Number of memory-map records |
| 32 | mmap_entry_size | u32 | Exactly 24 for v1 |
| 36 | modules_offset | u32 | Zero iff `modules_count` is zero; otherwise nonzero offset |
| 40 | modules_count | u32 | Number of module records |
| 44 | module_entry_size | u32 | Zero when `modules_count` is zero; otherwise exactly 24 for v1 |
| 48 | framebuffer_phys | u64 | Physical framebuffer base; zero when absent |
| 56 | framebuffer_size | u64 | Framebuffer byte span; zero when absent |
| 64 | framebuffer_width | u32 | Pixels |
| 68 | framebuffer_height | u32 | Pixels |
| 72 | framebuffer_pitch | u32 | Bytes per scanline |
| 76 | framebuffer_format | u32 | 0 absent; 1 RGBX8888; 2 BGRX8888 |
| 80 | acpi_rsdp_phys | u64 | Physical RSDP address; zero when absent |
| 88 | acpi_rsdp_size | u32 | Validated byte length; zero when absent |
| 92 | boot_device_offset | u32 | Offset of opaque boot-device bytes; zero iff size is zero |
| 96 | boot_device_size | u32 | Byte length of opaque boot-device identity |
| 100 | boot_device_kind | u32 | Versioned protocol-defined kind; zero means unspecified |
| 104 | reserved | 24 bytes | Must be zero in v1 |

The memory-map record is exactly 24 bytes: `base:u64`, `length:u64`, `type:u32`, `attributes:u32`. Type 1 is usable RAM; every other type is reserved from the allocator. Producers translating E820 must convert disabled descriptors (E820 extended-attribute bit 0 clear) to a non-usable type and must emit v1 `attributes = 0`; the E820 enabled bit is not copied into the v1 attributes field. Unknown types are treated as reserved. Zero length and arithmetic wrap are invalid. A v1 handoff must contain at least one memory-map record and at least one usable-RAM record. For the current 1 GiB PMM, the validated map must also leave at least one complete page of usable RAM below `0x40000000` after low-memory, kernel-image, handoff, module, and retained-loader exclusions; otherwise initialization fails before allocator use. Records must be sorted by ascending base and must not overlap at all; producers must normalize overlapping firmware descriptors before handoff. Gaps are implicitly unavailable. The kernel rejects unsorted or overlapping maps rather than guessing precedence.

The module record is exactly 24 bytes: `start:u64`, `length:u64`, `type:u32`, `flags:u32`. Module payloads are external physical ranges, not embedded in the handoff allocation. Every module range must be wholly covered by non-usable/reserved memory-map coverage (coverage may span adjacent reserved records without gaps) and must not overlap the kernel image, the handoff allocation, another module, the ACPI RSDP copy, or the framebuffer physical span. The kernel image, handoff allocation, module payloads, and ACPI RSDP copy are mutually disjoint physical objects; a present framebuffer span must also be disjoint from those RAM-resident objects. Producers must reject such aliases before publishing the handoff. Unknown mandatory module types reject the handoff; unknown optional types may be ignored only when the optional flag is set, but their ranges remain subject to all bounds, ownership, and non-overlap checks.

The boot-device byte payload is inside the handoff allocation and is opaque to the generic kernel parser. It must not contain a firmware handle or BIOS drive number as a kernel-facing identity. The producer may encode a stable device-path description using a separately versioned payload format. The kernel validates only the payload's offset and length at this layer.

For v1, `self_phys` must be 8-byte aligned so the fixed header's 64-bit fields and the allocation's ABI base have a stable alignment. `total_size` is at least 128 bytes and at most 16 MiB. The header occupies `[0, 128)`. The memory-map and (when present) module array offsets must be 8-byte aligned; the opaque boot-device byte payload has no additional alignment requirement. Every present in-allocation region must lie wholly within `[0, total_size)` and the regions must not overlap one another. The memory-map count is mandatory and nonzero, with a nonzero offset and entry size exactly 24. A zero module count requires both module offset and module entry size to be zero; a nonzero module count requires a nonzero offset and entry size exactly 24. A zero boot-device size requires zero offset and kind; a nonzero size requires a nonzero offset and supported nonzero kind. Count multiplication and offset addition must be checked before use. The physical interval `[self_phys, self_phys + total_size)` must not wrap and must be fully covered by non-usable/reserved memory-map records (adjacent reserved records may jointly cover it; gaps or any usable-RAM overlap reject). The framebuffer is a device/MMIO range and is not required to appear in the RAM memory map; validate its physical range for overflow/address-width and validate pitch/size arithmetic. ACPI RSDP bytes must be covered by non-usable memory-map entries and must not overlap usable RAM; validate their physical range before reading/checksumming them.

The initial EAX magic is `0x31534F50`; EBX is the 32-bit physical address of the allocation at entry. The current kernel bootstrap identity-maps only the first 1 GiB and the current PMM deliberately manages only that range. A memory-map record may describe physical RAM above this limit, but the initial PMM must add only the intersection of type-1 usable records with `[0, 0x40000000)`; it must never add or dereference the portion at or above 1 GiB. A record crossing the limit is clipped for allocator admission only after its full 64-bit range has passed overflow and physical-address-width validation. Until a later, separately validated high-memory mapping milestone, the complete handoff allocation and every external physical object that the early parser must dereference (module payloads if inspected at this stage, and ACPI RSDP bytes) must fit below 1 GiB; `self_phys + total_size` and each such dereferenced range must be checked for overflow and must not exceed `0x40000000`. The framebuffer's pixel/MMIO range is not dereferenced by the protocol parser and may lie above 1 GiB; a later framebuffer driver must establish a valid mapping before access. Its physical end must still be checked for arithmetic overflow and supported physical-address width. This is stricter than the 32-bit EBX ABI's 4 GiB ceiling and reflects the current kernel's actual addressability. The kernel entry must preserve EAX/EBX before changing modes. The 64-bit `self_phys` field remains authoritative for the allocation identity after entry. This layout is a review baseline; implementation must add compile-time size/offset assertions and host tests for every boundary rule before any producer migrates.




#### v1 physical-address-width contract

- The physical-address width is a property of the executing CPU/platform, not a value supplied by an untrusted handoff field. The loader and kernel must derive the same effective `MAXPHYADDR` from the boot CPU before accepting firmware physical ranges.
- Prefer `CPUID.80000008H:EAX[7:0]` when the maximum extended CPUID leaf is at least `0x80000008`. If that leaf is unavailable, require `CPUID.01H:EDX.PAE[6]` and use the architectural 36-bit fallback; if PAE is absent, reject because the target x86-64 long-mode environment cannot be supported. Reject if the derived width is outside 32..52 bits.
- The physical address limit is `1ULL << MAXPHYADDR`. All physical intervals are half-open. After checked 64-bit addition, require `base < limit` and `end <= limit`. An interval ending exactly at the limit is valid; an interval starting at the limit or extending one byte beyond it is invalid. Zero-length intervals are rejected wherever an object/range is present; zero-length memory-map records are always invalid.
- Apply the same rule to all memory-map records and to the handoff allocation, kernel image, modules, ACPI RSDP, framebuffer/MMIO span, and retained loader allocations. The current 1 GiB identity-map constraint remains an additional dereferenceability limit for early parser objects, not a substitute for MAXPHYADDR validation.
- Required tests: CPUID leaf-present and legacy-fallback paths; invalid/zero/>52 width rejection; highest representable byte; an exclusive end exactly equal to the physical limit; base equal to the limit; one-byte-past-limit; and overflow before width comparison.

#### v1 flag and optional-record constants

- Header `flags`: must be zero in v1.
- Memory-map `attributes`: must be zero in v1; future meanings require a new protocol version.
- Module `flags`: bit 0 means optional; all other bits must be zero. An unknown module type with bit 0 clear is fatal; with bit 0 set it may be skipped after its range has still been validated.
- `boot_device_kind`: 0 means no device identity and requires zero payload size/offset; 1 means the payload is the ProsenjitOS opaque device-identity payload v1 and requires a non-empty in-allocation payload. Every other value is unsupported and rejects the v1 handoff. There is no optional-unknown-kind escape in v1.
- For every present external byte range (module payload and ACPI RSDP), `length`/size must be nonzero and the physical start must be nonzero; the half-open end must be computed with checked 64-bit addition. A present framebuffer must likewise have nonzero `framebuffer_phys` and `framebuffer_size`; physical address zero is never a valid framebuffer base in v1. These checks supplement, and do not replace, reserved-map coverage and cross-object ownership checks.
- `framebuffer_format`: 0 means absent; 1 means 32-bit RGBX8888; 2 means 32-bit BGRX8888. The format names describe byte order in memory: format 1 stores R, G, B, X bytes; format 2 stores B, G, R, X bytes. For either present format, width and height must be nonzero. Compute `width * 4` in at least 64-bit arithmetic and require `pitch >= width * 4`; compute `pitch * height` in at least 64-bit arithmetic and require `framebuffer_size >= pitch * height`. Reject any multiplication or `framebuffer_phys + framebuffer_size` overflow and any physical-address-width violation. Validate the entire declared framebuffer span for overlap with the kernel image, handoff allocation, module payloads, and ACPI RSDP copy. The protocol parser validates metadata only; a display driver must establish a valid mapping before touching pixels.
- ACPI RSDP: when present, size must be at least 20 bytes; validate the signature and legacy checksum. For revision 2 or later, require at least 36 bytes, require the declared RSDP length to be at least 36 and no greater than `acpi_rsdp_size`, and validate the extended checksum over the declared length. The complete referenced range must be covered by non-usable/reserved memory-map records. Treat the RSDP as read-only metadata; its storage remains reserved and must not become allocator-usable merely because the kernel has read or copied it.
- Module type 1 is initramfs; type 2 is a driver bundle. Other types are reserved for future versions and follow the optional/mandatory rule above.

These constants are part of the v1 wire contract, not implementation claims. The parser must reject nonzero reserved bytes and unknown mandatory flag bits.


- Add negative tests for a present framebuffer with physical base zero, zero-sized or zero-based module payloads, and zero-based present ACPI RSDP; reject before any physical dereference.

#### v1 canonical absence-sentinel matrix

- Framebuffer absent: all six fields `framebuffer_phys`, `framebuffer_size`, `framebuffer_width`, `framebuffer_height`, `framebuffer_pitch`, and `framebuffer_format` must be zero. If any is nonzero, the record is present and must satisfy the complete format/geometry/range validation; partial population is invalid.
- ACPI absent: both `acpi_rsdp_phys` and `acpi_rsdp_size` must be zero. If either is nonzero, both must describe a complete, validated RSDP byte range.
- Modules absent: `modules_count`, `modules_offset`, and `module_entry_size` must all be zero. If present, count and offset are nonzero and entry size is exactly 24.
- Boot device absent: `boot_device_offset`, `boot_device_size`, and `boot_device_kind` must all be zero. If present, all three must be nonzero and the kind must be the supported v1 kind.
- These sentinel rules are exact v1 wire semantics; a producer must not use partially zero fields to imply a degraded/unknown optional record.

#### v1 review clarifications — 2026-11-17

- Corrected the header table so `mmap_offset` is always nonzero because the v1 memory map is mandatory and nonempty. The optional module-array offset remains zero only when its count is zero.
- Replaced the acceptance-test wording “valid absence flags” with “valid all-zero absence sentinels”; v1 does not define separate framebuffer/ACPI validity flags.
- Clarified physical ownership coverage: the handoff allocation, module payloads, and ACPI RSDP range must be fully covered by non-usable memory-map records. Adjacent reserved records may jointly cover a range, but gaps or any usable-RAM overlap reject it. Framebuffer MMIO is explicitly excluded from RAM-map coverage because it is not ordinary RAM.
- Corrected the current 1 GiB access constraint: the early parser must be able to dereference the handoff allocation and ACPI RSDP (and module bytes only if it actually inspects them). It does not read framebuffer pixels as part of protocol parsing. The framebuffer's physical range may therefore be above 1 GiB, but must pass overflow/address-width checks and must not be accessed until a later driver creates a valid mapping.
- Tightened ACPI RSDP validation: revision 2+ must provide a declared length no smaller than 36 bytes and no larger than the supplied `acpi_rsdp_size`; the complete range must remain reserved/non-usable.
- This is still a contract-only correction. No ABI header, parser, loader, or kernel behavior changed.


#### v1 review clarifications — 2026-11-21

- Require the handoff allocation base (`self_phys`, equal to EBX) to be 8-byte aligned; this removes ambiguity for the fixed header's 64-bit fields and the wire ABI base.
- Clarify that malformed or partially populated present framebuffer/ACPI records are fatal v1 validation errors before PMM initialization. Optional means the capability may be absent using its all-zero sentinel, not that malformed supplied metadata may be silently ignored.
- Require the kernel image and any loader memory retained after handoff to remain excluded from PMM free ranges. Legacy adapters/parsers must carve such ranges out if an incoming legacy map labels them usable. Reclaimable loader memory is eligible only after ownership transfer and after no retained pointer refers to it.
- Define the current 1 GiB PMM admission boundary: validate full 64-bit memory-map intervals, but add only their intersection with `[0, 0x40000000)` to the initial allocator. Never allocate from or dereference the portion at or above the limit until high-memory support is implemented.
- These are specification clarifications only; no ABI header, parser, producer, or runtime behavior changed.


- Physical-width acceptance tests must include the exact `2^MAXPHYADDR` exclusive-end boundary, a start at that boundary, one-byte-past-limit, CPUID leaf/fallback selection, and invalid reported widths.

#### v1 review clarifications — 2026-11-21 (continued)

- Tightened the generic ABI wording so memory-map and module `entry_size` must equal the exact v1 record size (24 bytes), matching the canonical wire table; larger records are not silently accepted in v1.
- Clarified E820 translation: disabled E820 descriptors (extended-attribute bit 0 clear) must be emitted as non-usable memory types, and the v1 `attributes` field remains zero. The firmware-specific enabled bit is not copied into the v1 wire record.


#### v1 review clarifications — 2026-11-21 (allocator viability)

- Tightened the minimum usable-memory condition: a map containing a type-1 record only above 1 GiB is not sufficient for the current kernel. After all mandatory reservations/exclusions and page alignment, at least one complete allocatable page must remain below `0x40000000`; otherwise v1 handoff validation/PMM initialization must fail before allocator use.


#### v1 final wire-contract consistency review — 2026-11-21

- Resolved the remaining alignment ambiguity: the handoff allocation base (`self_phys`) must be 8-byte aligned; the memory-map and module array offsets must also be 8-byte aligned. The boot-device payload is an opaque byte sequence and has no additional alignment requirement. A producer that cannot meet these requirements must not emit a v1 block.
- Made the fixed-size v1 rule explicit at every layer: `header_size` must equal 128, `mmap_entry_size` must equal 24, and a non-empty module array must use `module_entry_size` exactly 24. The generic introductory wording about a minimum header/entry size must not be interpreted as permitting larger v1 records; extensible layouts require a new protocol version.
- Clarified that `self_phys` is checked as a 64-bit physical address, must equal the zero-extended 32-bit EBX value captured at entry, and must satisfy the 8-byte alignment and current below-1-GiB dereferenceability constraints. The complete allocation end is checked with overflow-safe arithmetic.
- This closes specification ambiguities only. It does not implement or validate a parser, ABI header, boot producer, or runtime path.

#### v1 producer-normalization and source-adapter requirements — 2026-11-21

The fixed v1 wire format is deliberately stricter than firmware and legacy bootloader input formats. Every producer/adapter must normalize its source before emitting v1:

- Firmware memory-map gaps are unavailable; never synthesize usable RAM to fill a gap.
- Convert source descriptors to disjoint intervals before v1 serialization. Split at every descriptor start/end boundary. An interval may be emitted as type 1 only when it is covered by usable/enabled descriptors and no overlapping descriptor marks any part reserved, disabled, unusable, runtime, ACPI, MMIO, or otherwise non-usable. On any usable/non-usable conflict, non-usable wins. Unknown source types are non-usable. Merge adjacent intervals only when their final v1 type is identical. Emit sorted, non-overlapping records; reject arithmetic overflow, malformed descriptors, or an incomplete source map.
- Before the native BIOS loader reads into its staging buffer or kernel destination, apply the same conflict rule to the E820 source map. Both ranges must be covered by normalized usable intervals; the existence of one usable descriptor must not override a conflicting reserved descriptor. The legacy stage-2 source currently checks for a single enabled type-1 descriptor covering each range but does not normalize or detect conflicting overlaps; this is a migration requirement, not a current implementation claim.
- Validate the E820 returned record size before consuming a record: it must be at least the 20-byte base descriptor. For returned sizes 20 through 23 bytes, treat the entry as the legacy 20-byte form and ignore any partial trailing bytes; do not inspect the extended-attribute field. For sizes of at least 24 bytes, the extended-attribute enabled bit is meaningful and must be honored. A returned record shorter than 20 bytes is invalid. Disabled extended records are always non-usable. Do not silently truncate the E820 list; if the producer's bounded buffer fills while the continuation token says more entries remain, fail the boot.
- The native BIOS producer must reserve its stage-1/stage-2 image, E820 scratch buffer, Multiboot2 scratch block, stack, disk bounce buffer, kernel image, and any retained loader allocations before exposing memory to the PMM. The current low-memory reservation happens to cover several legacy scratch objects, but the v1 producer must express and test its complete live-allocation set rather than rely on incidental placement.

#### GRUB / Multiboot2 adapter input contract

- Validate the Multiboot2 information base is 8-byte aligned before any typed access and ensure the complete source block is readable below the current 1 GiB early identity-map limit. Keep the source block reserved until the adapter has finished copying/consuming all required fields. The fixed header is exactly 8 bytes; require `16 <= total_size <= 16 MiB`, require `total_size` to be a multiple of 8, and checked-add the end without physical-address overflow or exceeding the effective physical-address limit. The reserved dword is ignored, not required to be zero. Each tag must have at least its 8-byte header, a size of at least 8, and a checked unpadded end within the information block; never use unchecked `current + tag_size` expressions, even when the enclosing block was previously bounded. Compute the next tag as checked 8-byte alignment of that end and require it not to pass the block end. The type-0 terminator must have size exactly 8 and its unpadded end must equal the declared information-block end; reject missing, early, malformed, or non-final terminators and trailing bytes. Unknown information-tag types may be skipped only after their generic header, size, checked end, and aligned advancement have passed validation; never interpret unknown payloads. No tag parser may read beyond the declared information block.
- For every module tag (type 3), require the fixed 16-byte prefix plus at least one command-string byte, so `size >= 17`; require `mod_start < mod_end` and treat the source payload as the non-empty half-open interval `[mod_start, mod_end)`. The command string must be valid UTF-8 and contain a NUL terminator within the tag payload; do not scan beyond the tag. Validate each module payload against physical-width and reserved-map coverage rules, and reject overlap with the kernel, handoff allocation, other modules, ACPI RSDP copy, or framebuffer span. Module tags may repeat; process each independently and preserve deterministic order. Multiboot2's arbitrary command string does not itself define a ProsenjitOS v1 module type or optionality: the adapter must use an explicit, documented loader manifest/config mapping for v1 types (1 initramfs, 2 driver bundle) and the optional flag. Never infer type or optionality from a filename/path or silently classify an ambiguous module; reject an unmapped mandatory module.
- For the framebuffer tag (type 8), reject duplicate framebuffer tags rather than selecting one implicitly, and require the 32-byte fixed prefix before reading its fields. For RGB type 1, require the six channel-position/mask-size bytes to be present (minimum tag size 38); only translate a 32-bpp layout whose red, green, and blue masks are each exactly 8 bits and whose positions map exactly to the v1 byte layouts: red/green/blue positions 0/8/16 means RGBX bytes; 16/8/0 means BGRX bytes. Any other valid source layout is unsupported and becomes the all-zero v1 framebuffer sentinel. For indexed type 0, validate the palette-count field and checked palette byte count remain inside the tag, then publish absent because v1 has no indexed format. For EGA text or other well-formed unsupported framebuffer types, publish absent; malformed/truncated type-specific payloads or overflowing geometry reject. Ignore the Multiboot2 framebuffer reserved byte as specified; do not invent semantics for it.
- For the memory-map tag, require the complete tag to fit, entry_size >= 24, entry_size to be a multiple of 8, entry_version == 0, and (tag_size - 16) to be an exact multiple of entry_size. Read the defined first 24 bytes of each source entry; ignore the source entry's reserved dword as required by Multiboot2, and ignore any compatible trailing extension bytes. Then normalize and serialize exactly 24-byte v1 records. Reject duplicate mandatory memory-map tags rather than choosing one silently.
- Multiboot2 explicitly permits its memory-map entry to grow beyond 24 bytes and says the map can include the kernel, information block, segments, and modules in entries marked available; those objects therefore require explicit reservation/carve-out before v1 PMM admission. The adapter must not treat a type-1 source entry as proof that every byte in it is free.
- For ACPI tags, treat type 14 as the legacy RSDP copy and type 15 as the ACPI 2.0-or-later copy. Validate tag bounds before inspecting payloads. Prefer a valid type-15 copy when supplied; otherwise accept a valid type-14 copy. Reject duplicate tags of the same ACPI type. Copy the selected RSDP bytes into a separately reserved, early-accessible physical allocation (or an explicitly specified in-allocation region in a future ABI revision); do not publish a pointer into the Multiboot2 information block as if it were independently owned. Include that copy in the v1 reserved map and set the exact validated byte length. For revision 0, validate the 20-byte legacy structure. Revision 1 is unsupported and rejected. Revision 2 or later requires the 36-byte fixed prefix, a declared length no smaller than 36 and no larger than the supplied tag payload, and both checksums.
- ACPI's 16-byte search alignment applies when scanning BIOS search regions. Do not impose a universal 16-byte alignment rule on the v1 RSDP physical pointer merely because BIOS discovery scans on 16-byte boundaries; validate the source-specific pointer/copy bounds and the protocol's physical-address constraints.
- The RSDP physical source/copy is not required to be naturally aligned by the v1 ABI. The parser/adapter must validate the full byte range, copy it into suitably aligned local storage (or parse it with byte-safe loads), and must not cast an unaligned physical address directly to a packed/native structure pointer. The published physical copy remains reserved for as long as any kernel consumer may read it.
- Treat the kernel image, handoff allocation, module payloads, ACPI RSDP copy, and present framebuffer span as distinct physical objects. Reject any cross-object overlap, including module↔RSDP and module↔framebuffer aliases; checking only module↔module and module↔kernel is insufficient. All interval-end calculations use overflow-safe half-open ranges.
- For a valid but unsupported Multiboot2 framebuffer format (for example indexed or text mode, or a channel layout that cannot be represented exactly as v1 RGBX8888/BGRX8888), publish the v1 all-zero framebuffer absence sentinel. For a malformed framebuffer tag or arithmetic-overflowing geometry, reject the malformed handoff. For supported 32-bit RGB layouts, derive channel ordering from the source channel-position/mask metadata; do not infer RGBX versus BGRX from a generic RGB label alone. The v1 framebuffer byte span must cover pitch multiplied by height, and its range must be excluded from PMM admission if any source map interval would otherwise mark those bytes usable.



**Repository implementation inventory (2026-11-21):** inspected the current `boot/` directory and searched the repository for UEFI entry points and Boot Services interfaces (`EFI_SYSTEM_TABLE`, `EFI_BOOT_SERVICES`, `EFI_MEMORY_DESCRIPTOR`, `GetMemoryMap`, and `ExitBootServices`). The current boot directory contains the legacy native BIOS assembly path and GRUB/Multiboot2 configuration/header/entry files; no UEFI application entry point, EFI loader source, or executable memory-map/ExitBootServices implementation was found. Therefore, the UEFI bullets in this section remain **future producer requirements only**. Descriptor parsing, ACPI Configuration Table capture, map-key retry, post-exit service prohibition, and UEFI runtime behavior have not been implemented or source-tested. Re-run this inventory when a UEFI source tree is introduced; do not treat this absence as a defect in an implementation that does not yet exist.

#### UEFI producer memory-map and exit contract

- Validate the EFI memory-map descriptor version and descriptor stride; the stride must be at least the known descriptor structure size, and iteration must use the firmware-provided stride rather than sizeof as the step. Reject truncated or misaligned descriptor arrays and overflow in page-count-to-byte/range calculations.
- Normalize UEFI descriptors to the v1 binary usable/reserved model only after successful ExitBootServices. A descriptor may become type-1 usable only when firmware has relinquished it and no kernel, handoff, module, RSDP copy, loader, or other retained object still owns any part of it. Runtime-services, ACPI reclaim/NVS, MMIO, unusable, persistent memory without explicit support, and unknown types remain non-usable in this v1 PMM policy. Boot-services/loader ranges may be admitted only after the loader has ceased using them and all live allocations have been carved out.
- Capture the ACPI RSDP pointer from the EFI Configuration Table before ExitBootServices, preferring the ACPI 2.0-or-later GUID and falling back to the ACPI 1.0 GUID. Copy/validate the RSDP into reserved handoff-adjacent storage before relinquishing firmware services.
- Allocate all buffers needed for the final memory map, normalized v1 map, handoff block, and retained payload copies before the final GetMemoryMap/ExitBootServices sequence. After GetMemoryMap returns a map key, avoid boot-service calls that can change the map before ExitBootServices. If ExitBootServices reports an invalid map key, obtain a fresh map/key and retry within a bounded policy; do not continue using a stale key. After successful ExitBootServices, do not call boot services.
- These are source-adapter requirements for future implementation. They do not claim that a GRUB adapter, BIOS v1 producer, or UEFI loader currently exists.
