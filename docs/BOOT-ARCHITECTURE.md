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

- The protocol is a fixed-width, little-endian x86-64 handoff ABI. A producer emitting a v1 block enters with EAX=`0x31534F50` and EBX equal to the 32-bit physical base of that v1 allocation; the kernel entry assembly must preserve and widen both values before using them. During migration, legacy entry dispatch remains separate: GRUB enters with the Multiboot2 magic (`0x36D76289`) and a pointer to the original Multiboot2 information block, while the current native BIOS stage-2 enters with its legacy native magic (`0x534F5250`) and a pointer to its Multiboot2-compatible buffer. Those legacy magic values select their legacy adapter paths and must never be passed to the v1 parser as though they identified a v1 allocation. The adapter must produce/validate the normalized v1 block before the kernel treats `self_phys` as authoritative.
- The top-level structure begins with: magic (32-bit), version (16-bit), header_size (16-bit), total_size (32-bit), flags (32-bit), and a 64-bit physical address identifying the structure itself. All fields have explicit widths; compiler-native pointers, size_t, bitfields, and implicit padding are forbidden in the wire ABI.
- The header describes the in-allocation memory-map and module arrays using byte offsets relative to `self_phys`, counts, and entry sizes. Module records then identify their payloads by external physical start and byte length. A v1 consumer must reject any entry size that is not exactly the v1 record size, a count/size multiplication overflow, or an in-allocation region extending beyond `total_size` or the complete handoff allocation. External physical ranges (module payloads, ACPI RSDP, framebuffer) are validated separately for overflow, physical-address width, accessibility when dereferenced, ownership, and cross-object overlap; their payload lengths are not bounded by `total_size`.
- Canonical v1 memory-map records use 64-bit base and length plus a fixed-width type and attributes. Every record must have a nonzero length and a non-wrapping interval; canonical v1 records must be sorted and non-overlapping. This rule applies to the normalized v1 output, not raw firmware/source descriptors: E820, Multiboot2, and UEFI inputs may contain overlapping or unsorted descriptors and must be normalized with reserved/disabled/unknown ranges taking precedence before v1 publication. Unknown v1 memory types are reserved, never available; there is no separate 'mandatory memory-map record' flag in v1.
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
- v1 alignment and region-layout tests: reject an unaligned `self_phys`, `mmap_offset`, or present `modules_offset`; accept an unaligned opaque boot-device byte offset; reject offsets into the fixed header, out-of-allocation ranges, array-count multiplication overflow, and every pairwise overlap among header, mmap array, module array, and boot-device payload.
- Zero-length and contradictory memory-map entries; reserved and disabled ranges never become allocator RAM.
- Module out-of-bounds, overlap, malformed length, and reserved-range ownership failures.
- Reserved-ownership coverage tests for the complete handoff allocation, kernel image, every module payload, ACPI RSDP copy, and retained loader allocations: accept contiguous coverage across adjacent reserved records; reject any gap, usable overlap, zero-length object, overflow, or cross-object alias. Verify a framebuffer MMIO span is not required to be RAM-map-covered but is rejected when it aliases any RAM-resident object.
- Invalid optional framebuffer/ACPI metadata and valid all-zero absence sentinels.
- Legacy GRUB and native BIOS boot remain green throughout adapter migration.
- QEMU tests prove that malformed mandatory handoffs halt before PMM initialization and that valid handoffs reach the normal kernel boot markers.
- Physical-address tests cover the highest supported address, one-past-width rejection, 1 GiB early-dereference boundary, valid ranges crossing the 1 GiB allocator clipping boundary, and overflow at every external-object end calculation.
- ACPI RSDP boundary tests cover revision 0's 20-byte structure, rejection of revision 1, revision 2+ declared lengths equal to 36 and equal to the supplied size, declared length larger than the supplied size, truncated extended structures, and both checksum failures. Framebuffer tests cover `pitch == width * 4`, `pitch < width * 4`, exact `pitch * height` size, larger declared span, multiplication overflow, and a valid MMIO range above 1 GiB that is not dereferenced by the early parser.
- Producer-normalization tests cover unsorted, overlapping, conflicting, disabled, unknown, and gapped firmware descriptors; verify reserved-wins interval splitting and exact sorted/disjoint output.
- E820 adapter tests cover 20-byte and 24-byte records; returned lengths below 20; partial 21–23-byte responses (legacy treatment, ignore partial extension); 24-byte responses with the ACPI 6.6 extended-attribute reserved-bit requirements; E820 type 6 disabled entries; malformed reserved-bit values; a continuation token indicating more entries when the bounded 32-record buffer is full (must fail rather than truncate or silently continue with an incomplete map); incomplete firmware responses; and load/bounce ranges that overlap reserved descriptors.
- Multiboot2 adapter tests cover 8-byte information-base/total-size alignment, minimum, 16 MiB maximum, and overflowing total_size, tag-header/size/padding bounds including arithmetic-wrap attempts, unknown-tag skipping, early/missing/non-final terminators and trailing bytes, missing/duplicate mmap tags; module tag minimum size, bounded UTF-8/NUL validation, empty/reversed ranges, explicit type/optionality mapping, reserved coverage and every cross-object alias; duplicate mmap/ACPI/framebuffer singleton tags, entry strides greater than 24, non-multiple strides, nonzero entry versions, nonzero ignored source-entry reserved dwords, empty mmap payloads, order-independent usable/reserved overlap normalization; and framebuffer fixed/type-specific minimum sizes, indexed-palette arithmetic, exact RGB mask/position translation, unsupported layouts, and malformed geometry.
- UEFI producer tests cover descriptor-version/stride/truncation errors, page-count arithmetic overflow, every defined EFI memory-type classification, EFI_MEMORY_RUNTIME overriding an otherwise-usable descriptor type, EfiUnacceptedMemoryType exclusion until explicit acceptance, persistent-memory exclusion, ACPI reclaim/NVS lifetime, ACPI configuration-table preference/copy, ExitBootServices invalid-key retry, and proof that no boot-service call occurs after successful exit.

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
| 100 | boot_device_kind | u32 | 0 absent; 1 ProsenjitOS opaque device-identity payload v1; other values reject |
| 104 | reserved | 24 bytes | Must be zero in v1 |

The memory-map record is exactly 24 bytes: `base:u64`, `length:u64`, `type:u32`, `attributes:u32`. Type 1 is usable RAM; every other type is reserved from the allocator. Producers translating E820 must convert E820 type 6 (AddressRangeDisabled) to a non-usable type; under ACPI 6.6 extended-attribute bit 0 is reserved and must be set to 1, not an enabled/disabled flag and must emit v1 `attributes = 0`; the E820 enabled bit is not copied into the v1 attributes field. Unknown types are treated as reserved. Zero length and arithmetic wrap are invalid. A v1 handoff must contain at least one memory-map record and at least one usable-RAM record. For the current 1 GiB PMM, the validated map must also leave at least one complete page of usable RAM below `0x40000000` after low-memory, kernel-image, handoff, module, and retained-loader exclusions; otherwise initialization fails before allocator use. Records must be sorted by ascending base and must not overlap at all; producers must normalize overlapping firmware descriptors before handoff. Gaps are implicitly unavailable. The kernel rejects unsorted or overlapping maps rather than guessing precedence.

The module record is exactly 24 bytes: `start:u64`, `length:u64`, `type:u32`, `flags:u32`. Module payloads are external physical ranges, not embedded in the handoff allocation. Every module range must be wholly covered by non-usable/reserved memory-map coverage (coverage may span adjacent reserved records without gaps) and must not overlap the kernel image, the handoff allocation, another module, the ACPI RSDP copy, or the framebuffer physical span. The kernel image, handoff allocation, module payloads, and ACPI RSDP copy are mutually disjoint physical objects; a present framebuffer span must also be disjoint from those RAM-resident objects. The kernel image, handoff allocation, module payloads, ACPI RSDP copy, and every loader allocation retained after handoff must each be fully covered by non-usable/reserved memory-map records (adjacent records may jointly cover a range, but gaps or any usable overlap reject). If the incoming firmware map labels any such live object as usable, the producer/adapter must split and carve it out into explicit reserved coverage before publishing v1; merely omitting the object from PMM allocation without reflecting ownership in the normalized map is not a valid handoff. Producers must reject cross-object aliases before publishing the handoff. Unknown mandatory module types reject the handoff; unknown optional types may be ignored only when the optional flag is set, but their ranges remain subject to all bounds, ownership, and non-overlap checks.

The boot-device byte payload is inside the handoff allocation and is opaque to the generic kernel parser. Kind 0 means absent and requires zero offset/size; kind 1 means the ProsenjitOS opaque device-identity payload v1 and requires a non-empty payload; every other kind rejects v1. The payload must not expose a firmware handle or BIOS drive number as a kernel-facing identity. Its internal bytes follow the separately versioned device-identity payload definition; the generic kernel parser validates only the kind, offset, length, and allocation bounds at this layer.

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
- ACPI RSDP: when present, size must be at least 20 bytes; validate the signature and legacy checksum. For revision 2 or later, require at least 36 bytes, require the declared RSDP length to be at least 36 and no greater than `acpi_rsdp_size`, and validate the extended checksum over the declared length. The complete RSDP copy range must be covered by non-usable/reserved memory-map records. The RSDP is only a discovery structure: its RSDT/XSDT physical pointer does not mean the root table or any referenced ACPI table has been copied, mapped, validated, or made owned by the kernel. Before dereferencing a root/table pointer, the ACPI subsystem must independently validate its physical interval against MAXPHYADDR and checked length arithmetic, establish a mapping for the complete bytes it reads, and validate the applicable signature, length, and checksum (FACS has its own structure/validation rules and is not an ordinary checksummed SDT). Prefer XSDT when valid and present; otherwise use RSDT. ACPI tables and FACS may be outside the early 1-GiB identity map, so the early boot parser must not follow these pointers before a suitable mapping exists. The RSDP copy and all firmware-owned ACPI table storage remain excluded from PMM while referenced. v1 collapses all non-usable memory classes to reserved and has no ACPI-reclaim release state; therefore this v1 policy must not return ACPI reclaim or NVS ranges to PMM merely because the RSDP was copied, ACPI was discovered, or a table was parsed. A future protocol revision may define a separately verified ACPI-reclaim lifecycle; NVS/FACS storage must remain preserved while firmware/ACPI requires it.
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
- Clarified E820 translation: ACPI 6.6 identifies disabled ranges by E820 type 6, not by extended-attribute bit 0. Bit 0 is reserved and must be set to 1; it is not an enabled/disabled flag. Type 6 must be emitted as non-usable memory, and the v1 `attributes` field remains zero.


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
- Validate the E820 returned record size before consuming a record: it must be at least the 20-byte base descriptor. For returned sizes 20 through 23 bytes, treat the entry as the legacy 20-byte form and ignore any partial trailing bytes; do not inspect the extended-attribute field. For a 24-byte response, validate the complete extended-attribute dword: bit 0 must be set and bits 2:1 must be clear; bit 0 is reserved, not an enabled/disabled flag. Classify disabled memory by E820 type 6. A returned record shorter than 20 bytes is invalid. Do not silently truncate the E820 list; if the producer's bounded buffer fills while the continuation token says more entries remain, fail the boot.
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
- Normalize UEFI descriptors to the v1 binary usable/reserved model only after successful ExitBootServices. The potentially usable UEFI types are EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData, and EfiConventionalMemory; admit their bytes as v1 type 1 only after firmware has relinquished control and every loader/kernel/handoff/module/RSDP/retained allocation has been carved out. A descriptor with EFI_MEMORY_RUNTIME set is non-usable and must be preserved regardless of its nominal memory type; the flag requires runtime mapping/retention and must never be discarded during type conversion. EfiRuntimeServicesCode/Data, EfiReservedMemoryType, EfiACPIReclaimMemory, EfiACPIMemoryNVS, EfiUnusableMemory, MMIO/port-space, EfiPalCode, persistent memory, EfiUnacceptedMemoryType, and unknown/reserved/vendor-defined types remain non-usable in this v1 PMM policy. In particular, unaccepted memory must not enter PMM until a future, explicit memory-acceptance implementation has accepted and validated it; ACPI reclaim and NVS remain reserved for the lifetime of this v1 policy, even though the UEFI specification permits ACPI reclaim memory to become available after ACPI is enabled. Persistent memory requires a separately designed and tested policy before admission.
- Capture the ACPI RSDP pointer from the EFI Configuration Table before ExitBootServices, preferring the ACPI 2.0-or-later GUID and falling back to the ACPI 1.0 GUID. Copy/validate the RSDP into reserved handoff-adjacent storage before relinquishing firmware services.
- Allocate all buffers needed for the final memory map, normalized v1 map, handoff block, and retained payload copies before the final GetMemoryMap/ExitBootServices sequence. After GetMemoryMap returns a map key, avoid boot-service calls that can change the map before ExitBootServices. If ExitBootServices reports an invalid map key, obtain a fresh map/key and retry within a bounded policy; do not continue using a stale key. After successful ExitBootServices, do not call boot services.
- These are source-adapter requirements for future implementation. They do not claim that a GRUB adapter, BIOS v1 producer, or UEFI loader currently exists.


#### v1 source-adapter acceptance closure — 2026-11-21

The following source-specific cases are mandatory additions to the adapter acceptance matrix. They refine the preceding E820, Multiboot2, and UEFI requirements; they do not claim that any producer or test implementation exists.

**E820 returned-length and bounded collection**
- Drive the BIOS mock with returned ECX values 19, 20, 21, 22, 23, 24, and greater than 24. Reject values below 20; consume only the 20-byte base descriptor for 20–23; validate the complete extended-attribute reserved bits only for exactly 24 bytes; classify disabled memory by E820 type 6; ignore bytes beyond the defined 24-byte prefix.
- For 20–23-byte responses, poison the scratch extension bytes and prove they cannot change descriptor classification. For 24-byte responses, require extended-attribute bit 0 = 1 and bits 2:1 = 0; test E820 type 6 as disabled independently of the attributes.
- Verify the returned ECX is captured and validated before any extension-field read. The current legacy `collect_e820` requests 24 bytes and preinitializes the extension to 1, but does not inspect returned ECX; therefore it does not yet satisfy the 20–23-byte rule.
- Exercise continuation-token termination, carry/signature failures after one or more records, the exact 32-record final slot, and the 32-record-full case with a nonzero continuation token. The latter must fail without serializing a truncated map.
- Feed conflicting usable/reserved and enabled/disabled overlaps in both input orders; require identical reserved-wins normalized output. Verify the complete stage-1/stage-2 image, E820 and Multiboot2 scratch buffers, stack, disk bounce buffer, kernel destination, and retained loader allocations are reserved before load-range admission and before PMM exposure.

**Multiboot2 source-tag boundaries**
- For the mandatory mmap tag, test entry strides 23, 24, 32, 40, and a value not divisible by 8; reject strides below 24 or not divisible by 8, reject nonzero entry_version, and reject payload sizes not exactly divisible by the stride. For strides above 24, read only the defined 24-byte prefix and safely skip the extension bytes.
- Test repeated module tags in deterministic source order, each with an independently bounded NUL-terminated UTF-8 command string; ensure one malformed module rejects the adapter result and no command string is read past its tag.
- Test ACPI type-15 preference when both type 14 and type 15 are present and valid; type-14 fallback when type 15 is absent; duplicate tags of either singleton type; truncated tag prefixes; and invalid RSDP payloads. A present malformed higher-priority type-15 tag must reject rather than silently downgrade to type 14. Verify the selected bytes are copied to a separately reserved range and do not alias the source information block or any other physical object.
- For framebuffer tags, test the ignored reserved byte with both zero and nonzero values; test the exact 32-byte common prefix, RGB-specific 38-byte minimum, indexed palette count arithmetic, duplicate singleton rejection, supported RGBX/BGRX layouts, unsupported-but-well-formed formats mapping to the absent sentinel, and malformed/truncated layouts rejecting.
- Verify source information-block reserved dword is ignored even when nonzero, tag padding bytes are never interpreted as payload, and every aligned next-tag calculation is checked independently from the unpadded tag end.

- Add ACPI root/table traversal tests: revision-2+ RSDP with XSDT must select XSDT; legacy revision-0 uses RSDT; reject zero, out-of-width, overflowing, or unmapped root pointers before dereference. Validate the complete SDT header before trusting Length, require the full table mapping and checked end, and verify signature/length/checksum plus RSDT 4-byte or XSDT 8-byte entry-array divisibility. Test child-table pointers that are invalid, aliased, duplicated, or outside MAXPHYADDR; do not read a child header until its physical range is mapped. Treat FACS as a special live structure (not an ordinary SDT checksum case), preserve/map it in place when required, and never free NVS/FACS storage through PMM. Verify ACPI reclaim/NVS remains reserved under v1 even after table discovery or copying, since v1 does not encode a reclaimable ACPI state.

**UEFI descriptor and ExitBootServices boundaries**
- Require DescriptorSize to be at least the known EFI_MEMORY_DESCRIPTOR size; iterate by the returned stride, reject a truncated map whose byte length is not an exact multiple of DescriptorSize, reject unsupported descriptor versions, and validate descriptor-array alignment before typed access.
- For every descriptor, reject NumberOfPages == 0, checked-multiply NumberOfPages by 4096, checked-add PhysicalStart plus byte length, and validate the complete physical interval against MAXPHYADDR. Test exact upper-bound success and one-byte/page overflow failures.
- Add a complete source-type mapping matrix: only memory explicitly usable under the v1 post-exit ownership policy may become type 1; runtime-services, ACPI reclaim/NVS, MMIO, unusable, persistent-without-support, unknown, and any still-live loader allocation remain non-usable. Boot-services code/data may become usable only after successful exit and after all live objects are carved out. Unknown future types must never become usable by default.
- Test an initially undersized GetMemoryMap buffer, EFI_BUFFER_TOO_SMALL, allocation of a larger buffer with explicit slack beyond the returned required size, and a fresh GetMemoryMap after the final allocation because allocations can change the map.
- Test a stale MapKey, a successful fresh-map retry, repeated invalid-key failures reaching a bounded retry limit, and failure without transferring control when retries are exhausted. After the first ExitBootServices attempt, only Memory Allocation Services (including GetMemoryMap and the required allocation operations) may be used for retry; no protocol, console, timer, image, or unrelated Boot Services calls are permitted. After successful ExitBootServices, no Boot Services or EFI device-handle-based protocol calls are permitted.
- Test that all allocations and ACPI RSDP copies needed after exit are completed before the final map/exit sequence, and that no post-exit code relies on a boot-services-owned buffer or firmware handle.

Authoritative source cross-checks:
- UEFI Specification 2.10, GetMemoryMap and ExitBootServices: https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html
- GNU GRUB Multiboot2 Specification 2.0, boot information and memory-map tags: https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html

These additions are specification and test-plan requirements only. No ABI header, parser, E820 producer, GRUB adapter, UEFI loader, build, host test, firmware test, or QEMU runtime behavior was implemented or executed by this documentation update.


#### Native BIOS E820-to-PMM source audit — 2026-11-21

A source-level cross-check of `boot/native_stage2.asm` and `kernel/memory.c` confirms the following implementation gaps. These are findings against the current legacy path, not claims that the v1 adapter exists.

1. **Reserved-overlap precedence is missing before BIOS reads.** `validate_usable_range` returns success as soon as one enabled type-1 E820 record covers the requested interval. It does not detect a reserved/disabled/unknown descriptor overlapping that usable record. A conflicting descriptor can therefore be ignored when deciding whether `LOAD_BUFFER..LOAD_LIMIT` or the kernel destination may be written. The future check must normalize the complete map with reserved-wins precedence and require full coverage by the normalized usable union.
2. **Adjacent usable descriptors are not combined.** The current single-record containment check rejects a requested range that is fully covered by two adjacent usable records. This is fail-closed, but it is not the required normalized-interval behavior and needs an explicit adjacent-coverage acceptance case.
3. **The legacy Multiboot2 producer forwards raw E820 intervals.** `build_mb2_info` copies source entries in firmware order and changes disabled entries to type 2, but does not split/normalize overlapping intervals, resolve usable-versus-reserved conflicts, sort, or merge adjacent equal-type ranges. It must not be used as proof of a normalized v1 map.
4. **The current PMM ignores non-available entries rather than applying them as exclusions.** `physical_memory_init` calls `mark_available` for each Multiboot2 type-1 record and ignores reserved/unknown records. If a reserved record overlaps a usable record, the overlapping pages can still become allocator-managed/free. The later fixed reservations for low memory, kernel image, and MBI do not solve arbitrary firmware reserved overlaps. The v1 adapter/parser must normalize first and expose only final disjoint usable intervals to the PMM.
5. **The existing PMM parser is not a safe v1 consumer.** It does not require a final end tag, does not prove a mandatory mmap tag was present exactly once, does not reject a partial trailing mmap entry, and does not validate entry_version or source-map order/overlap. These remain legacy migration defects and must not be conflated with v1 parser behavior.

**Required focused regression matrix before any implementation is accepted:**
- A usable E820 interval fully covering each BIOS write target with a nested, partial, exact-boundary, or adjacent reserved/disabled/unknown overlap: reject the write before issuing disk I/O or copying bytes.
- Identical overlapping descriptor sets in multiple input orders: normalized output must be byte-identical, sorted, disjoint, and reserved-wins.
- A target range covered by two adjacent usable descriptors: accept after normalization; a one-byte gap: reject.
- A range partially outside all descriptors, a zero-length descriptor, an arithmetic-wrapping descriptor, a base above 4 GiB, and a range ending exactly at the 4 GiB boundary.
- PMM ingestion of usable plus reserved overlap in both orders: reserved pages must never enter `managed_bitmap` or `page_bitmap`; unknown types and gaps remain unavailable.
- Verify the E820 scratch buffer, Multiboot2 scratch block, stage-2 image, active stack, BIOS bounce buffer, kernel destination, and all retained allocations remain reserved before the allocator can return any of their pages.
- Verify PMM admission clips a valid 64-bit usable interval at 1 GiB only after the full source interval passes overflow/address-width validation; no page at or above the 1 GiB boundary becomes allocatable.

**No source implementation changed in this audit.** The current BIOS range checker, raw E820-to-Multiboot2 copier, and legacy PMM behavior remain as observed above. This is a source audit and acceptance-plan refinement only; no compiler, host regression, BIOS/QEMU runtime, or CI result is claimed.


#### Multiboot2 parser source audit — 2026-11-21

The current legacy `kernel/memory.c::physical_memory_init` parser was checked directly against the Multiboot2 information-block/tag layout. The following are confirmed source-level findings; this is not the v1 parser.

**Confirmed parser gaps**
1. The information-block physical address is not checked for the required 8-byte alignment before typed 32-bit tag reads. The total-size word is dereferenced before any alignment/range validation.
2. Tag-end arithmetic uses `current + tag->size` in the bounds comparison without first checked-adding into a separate end value. The later aligned-next calculation checks only the addition of 7, not the preceding tag-end addition.
3. An end tag causes an immediate `break`; the parser does not require its size to equal 8, does not require it to end exactly at `total_size`, and does not reject a missing end tag. Reaching the end without a valid terminator can fall through to allocator setup.
4. The mmap tag is not required exactly once. Repeated mmap tags are all processed; a missing mmap is rejected only indirectly when the final free-page count is zero, rather than by an explicit mandatory-tag check.
5. Mmap `entry_version` is not validated. `entry_size` is only checked to be >=24; non-8-byte strides are accepted, and payload remainder bytes smaller than one entry are silently ignored because the loop stops when fewer than `entry_size` bytes remain.
6. Mmap entries are applied incrementally to global allocator bitmaps while parsing. If a malformed later tag/entry is found, the function returns an error after partially mutating allocator state. The current caller in `kernel/kernel.c` halts on a nonzero return, so it does not continue into normal allocation, but the parser itself is not transactional and must not be reused by a future recoverable boot path.
7. The parser ignores module, framebuffer, ACPI, and other tags rather than validating their lengths, duplicate/singleton rules, and referenced physical ranges. This is acceptable only as a deliberately narrow legacy PMM consumer; it is not sufficient for Boot Protocol v1.
8. `physical_memory_init` consumes each type-1 entry directly through `mark_available`; it does not first normalize the complete map or subtract overlapping reserved/unknown entries. This confirms the previously documented reserved-wins/PMM issue.

**Required acceptance cases**
- Information-block address aligned to 8, and misaligned by 1, 2, and 4 bytes; reject misaligned input before reading typed fields.
- Total size at minimum, maximum accepted 16 MiB, one below minimum, one above maximum, and base+size overflow; reject before any tag dereference.
- Tag size 0–7, exact 8, exact remaining bytes, one byte beyond remaining bytes, and tag-end arithmetic overflow; validate checked unaligned end and checked 8-byte aligned successor independently.
- End tag size 7, 8, 9, truncated header, missing end tag, end tag before trailing bytes, and end tag exactly at the final 8 bytes; accept only the exact required terminator placement and reject all trailing/unparsed bytes.
- Zero, one, and two mmap tags; require exactly one. Test mmap header size 15/16/17; entry sizes 23/24/32/40 and non-8-byte stride; entry_version 0 and nonzero; exact payload divisibility and every possible 1..entry_size-1 trailing remainder.
- Empty mmap payload, zero-length entries, base+length overflow, page-rounding overflow, physical-limit boundary, and unknown type values; malformed arithmetic rejects, unknown types never become available.
- Malformed final tag after valid usable entries; allocator-visible state must remain unchanged (validate into temporary normalized storage, then commit atomically) and no allocator API may become available on failure.
- Repeated module/framebuffer/ACPI singleton tags, truncated fixed prefixes, malformed external ranges, and overlapping module/loader/kernel/MBI objects; v1 adapter/parser rejects before PMM initialization.
- For every malformed case, verify the kernel caller follows a terminal fail-closed path and no subsequent allocation, paging, DMA, or user mapping path executes.

**Scope clarification:** the current kernel caller explicitly halts when `physical_memory_init` returns nonzero. That protects the current boot from continuing after a parser error, but it does not cure parser-side partial bitmap mutation, and it does not make the legacy parser conformant to the v1 contract. No implementation or runtime test was changed or run by this audit.


#### ACPI, framebuffer, and module ownership source audit — 2026-11-21

This pass checked the current native BIOS producer (`boot/native_stage2.asm`), the common kernel entry (`kernel/kernel.c`), the legacy PMM parser (`kernel/memory.c`), and the display backend (`kernel/display.c` / `kernel/include/display.h`).

**Observed producer/consumer behavior**
- The native BIOS stage-2 producer builds a small Multiboot2-compatible block containing only one mmap tag and the end tag. It does not emit module, framebuffer, or ACPI tags. This is an explicit limitation of the current native BIOS producer, not proof that those facilities are unnecessary for v1.
- `kernel_main` accepts either the Multiboot2/GRUB magic or the native magic, but both branches pass the same `boot_info` pointer to `physical_memory_init`. There is no adapter dispatch that normalizes the two legacy handoffs into a separate validated v1 structure.
- The PMM parser recognizes only the mmap tag and end tag. It does not parse, validate, or reserve Multiboot2 module payload ranges. Therefore a GRUB handoff containing modules is not currently safe to treat as a fully supported module-owning boot contract: module payload pages may be admitted as usable RAM if the firmware map labels them available.
- No ACPI/RSDP parser or RSDP reference was found in the inspected kernel source/search. The current boot path therefore has no source-level RSDP signature, revision, checksum, extended-length, or physical-range validation.
- No Multiboot2 framebuffer tag parser or boot-framebuffer consumer was found. The display subsystem is a generic driver-backend interface; its self-test uses a synthetic framebuffer address and does not establish a real boot framebuffer mapping.
- `pros_display_get_info` checks nonzero width/height, configured dimension maxima, `pitch >= width`, and nonzero framebuffer address/size. It does not calculate a format-aware minimum byte span using checked `pitch * height` (and bytes-per-pixel where applicable), nor verify that `framebuffer_size` covers that span. `pros_display_present` only compares the caller's byte count with the advertised framebuffer size. This is a display-backend validation gap, separate from Multiboot2 parsing.

**Required ACPI/RSDP acceptance cases**
- Accept absent ACPI metadata only through the documented all-zero absence representation.
- Validate signature, supported revision, minimum structure size, 20-byte legacy checksum, and (revision 2+) declared length plus extended checksum over exactly the declared length.
- Reject revision 1 if the v1 contract intentionally supports only revision 0 and revision 2+, reject declared lengths below the revision-specific minimum or larger than the validated supplied extent, and reject any base+length overflow.
- Validate physical-address width, required alignment, dereference accessibility, and ownership before reading RSDP bytes. Do not assume that an ACPI table's physical address is RAM or may be passed to the PMM.
- Test truncated first 20 bytes, truncated extended header, invalid signature, invalid legacy checksum, invalid extended checksum, unsupported revision, zero/short/oversized length, wraparound, and overlap with kernel/module/loader/handoff allocations.

**Required framebuffer acceptance cases**
- Treat an all-zero record as absent; reject partially populated records, unsupported pixel formats, zero dimensions, invalid pitch, and invalid physical address/size.
- Derive bytes-per-pixel from the validated pixel format; checked-multiply minimum row bytes and pitch-by-height, reject overflow, and require the advertised span to cover the full visible surface.
- Checked-add framebuffer base+span and validate physical-address width before mapping. A framebuffer is typically MMIO/reserved space: do not require it to be covered by usable RAM, and never add its pages to PMM. Reject aliases with RAM-resident kernel, module, handoff, or loader objects.
- Test exact-size and one-byte-short buffers, pitch padding, zero/maximum dimensions, each supported pixel format, arithmetic overflow, physical-limit crossing, and overlap with every RAM-owned boot object.
- Verify mapping cache policy/permissions independently of the boot record: framebuffer MMIO must not become ordinary writable executable RAM.

**Required module ownership acceptance cases**
- Validate every module tag's fixed header, string termination/bounds, start < end, physical-width bounds, and checked end arithmetic before dereferencing payload.
- Reject empty/reversed ranges, truncated tags, malformed command strings, duplicate module IDs where IDs are defined, pairwise module overlap, and overlap with kernel image, handoff block, active loader/stack, E820/MB2 scratch, or other retained objects.
- Require every module payload to be wholly contained in a validated loader-owned/reserved region (allow coverage across adjacent reserved descriptors only when there is no gap); reject usable overlap or any uncovered byte.
- Reserve all module payload pages before PMM admission. Reclaim a module only after its consumer has completed and explicitly releases ownership; never infer reclamation merely from parsing the tag.
- Test no modules, one module, maximum module count, adjacent non-overlapping modules, exact-boundary adjacency, nested/partial overlaps, address wrap, physical-width overflow, and map-order invariance.

**Fail-closed migration rule**
The existing legacy BIOS producer may continue emitting only mmap+end while the native loader is still minimal, but the GRUB adapter and eventual native/UEFI v1 producers must normalize and validate all supported optional records before PMM initialization. The kernel must not treat the absence of a parser as successful validation. The existing generic display self-test is not evidence of boot framebuffer validation; no ACPI, real framebuffer, or module runtime support is claimed by this audit.

No implementation changed and no compiler, CI, QEMU, firmware, ACPI, or physical display test was run in this documentation pass.


#### Additional Multiboot2 map and early-access cross-check — 2026-11-21

Cross-checked the parser against the GNU GRUB Multiboot2 specification, especially the information-block alignment, memory-map entry layout, and loader-owned object rules.

- The specification requires the information structure to start on an 8-byte boundary and each tag to start on an 8-byte boundary. The current parser does not check the information-block alignment before its first typed dereference.
- The memory-map `entry_size` is guaranteed by the specification to be a multiple of 8, and `entry_version` is currently 0. The current parser checks only `entry_size >= 24`; it accepts non-multiple-of-8 strides and nonzero versions, potentially reading later entries from invalid offsets.
- The Multiboot2 map is advisory and can describe RAM that also contains the kernel, MBI, segments, and modules; the OS must preserve those objects. The current PMM explicitly reserves the MBI and kernel image, but not module payloads.
- The kernel bootstrap page tables identity-map only physical 0..1 GiB. The Multiboot2 specification allows the information block to be placed anywhere in memory. Current `physical_memory_init` dereferences `multiboot_info_address` to read `total_size` before proving the full block is inside the currently accessible identity map. A valid but above-1-GiB MBI can therefore fault during early parsing rather than being rejected cleanly. v1 must either provide an early mapping/access mechanism or reject any boot object not accessible under the active early map before dereferencing it.
- A further arithmetic flaw exists in the current range admission helper: `mark_available` treats `base + length` wraparound as if the range ended at `PROSENJIT_PHYS_LIMIT`. A malformed type-1 entry with a low base and overflowing length can therefore mark pages from that base up to the 1-GiB PMM ceiling as available. Overflow must be a hard parse failure before any allocator bitmap mutation; clipping is valid only for a non-wrapping range whose end legitimately exceeds the allocator ceiling.
- The Multiboot2 information-block `reserved` dword is specified as always zero but explicitly to be ignored by the OS image; do not add a nonzero-rejection rule for that legacy field. This is distinct from reserved fields in the new ProsenjitOS v1 ABI, whose validation rules are defined separately.

**Additional required tests**
- MBI at every alignment residue 1..7; reject before typed dereference.
- MBI starting below 1 GiB but ending exactly at 1 GiB, ending one byte above it, and starting above 1 GiB; validate mapping/accessibility before reading the complete block.
- Mmap entry sizes 24, 32, 40, and larger valid multiples of 8; reject 25, 31, and other non-multiple-of-8 values. Reject nonzero entry_version.
- Type-1 range with `base + length` wrapping UINT64_MAX, valid range crossing the 1-GiB allocator ceiling, and range ending exactly at the ceiling; overflow must reject without changing any bitmap, while valid crossing ranges may be clipped only after full-width validation.
- Verify Multiboot2 legacy reserved dwords are ignored per specification, while v1 reserved-byte rules remain independently enforced.

Specification reference: GNU GRUB, *Multiboot2 Specification*, sections 3.6.1, 3.6.2, and 3.6.8.

### Native BIOS raw-image BSS initialization requirement

The current native BIOS producer converts the linked ELF kernel with `objcopy -O binary`, computes the disk payload size from that raw file, and loads only those sectors before jumping to `_start`. The linker script places `.bss` in a separate `SHT_NOBITS` output section between `__kernel_bss_start` and `__kernel_bss_end`; the raw payload therefore does not carry the zero-initialized storage bytes that an ELF loader accounts for through the load segment's memory size. The native stage-2 path has no corresponding BSS-clear loop before entry. By contrast, the GRUB path loads the ELF image and applies its ELF segment memory-size semantics.

This is a concrete native-vs-GRUB handoff mismatch: zero-initialized C static-storage objects and assembly BSS storage are not guaranteed to begin as zero on the native path. Some current subsystems explicitly reset their own state, and the entry code explicitly clears its page-table pages, but those local resets do not satisfy the whole-kernel BSS contract. In particular, future or existing globals may silently depend on the C zero-initialization guarantee.

**Required producer/entry behavior**
- The native BIOS producer must obtain the exact linker-defined BSS start/end (or an equivalently validated image manifest range), check ordering, alignment, physical-width and overflow, and zero the complete BSS interval before entering code that reads or writes BSS-backed state.
- The BSS clear must not erase the boot handoff values or the active low-memory stage-2 stack. Keep the boot arguments in registers or protected storage until the clear completes, then pass the documented magic and information pointer in EAX/EBX.
- Ensure the clear covers all `SHT_NOBITS`/COMMON storage, including the bootstrap stack, page tables, TSS/IST storage, and C globals, and does not extend beyond the reserved kernel image allocation.
- GRUB/ELF and native/raw producers must produce equivalent zero-initialized BSS state at the common `_start` entry. Do not rely on firmware RAM being zero.

**Required regression cases**
- Fill the entire native kernel BSS interval with a nonzero pattern before the loader's clear step; verify every byte is zero at `_start` while EAX/EBX retain the exact native magic and handoff pointer.
- Place sentinels immediately before and after the BSS interval; verify neither boundary byte is modified.
- Reject reversed, empty when the linked image declares non-empty BSS, overflowing, out-of-kernel, or unaligned BSS ranges before the final jump.
- Compare ELF program-header file-size/memory-size coverage with linker BSS symbols and the native flat-image manifest; require complete BSS coverage without overlapping the loaded file payload or stage-2 scratch memory.
- Boot the same deterministic image through GRUB and native BIOS and compare selected zero-initialized global state at the common entry and after early initialization.

This is a source-audit finding and implementation requirement only. No loader, entry assembly, linker script, or kernel source was changed in this PR; no build or firmware/QEMU runtime test was run.

### Native BIOS BSS physical-range validation and image reservation

The native build derives `NATIVE_KERNEL_SECTORS` from the byte length of `objcopy -O binary` output. Stage-2 `validate_load_ranges` then checks the bounce buffer and only `[KERNEL_LOAD, KERNEL_LOAD + KERNEL_SECTORS * 512)`. That is the disk-backed flat-image extent, not necessarily the complete in-memory kernel extent: the linker-defined `__kernel_end` follows `.bss`, while `.bss` is `SHT_NOBITS` and is not represented by ordinary file payload bytes. The current stage-2 therefore does not prove that the full `[__kernel_start, __kernel_end)` interval is covered by usable firmware RAM before the kernel begins using BSS.

The PMM does later reserve `[__kernel_start, __kernel_end)` through `reserve_range`, which is the correct ownership intent once `physical_memory_init` runs. This does not close the earlier loader gap: the BSS must already be valid, writable RAM and zeroed before `_start` reads/writes BSS-backed variables, including `boot_magic`, `boot_info`, bootstrap page tables, and stack storage. Loader validation must therefore cover the full linker memory extent before any BSS clear or kernel entry; the PMM reservation remains a separate later defense against allocator reuse.

**Required native image manifest and validation**
- Export and consume both the flat file extent and the complete memory extent: file start/end (or file byte count), `__kernel_start`, `__kernel_bss_start`, `__kernel_bss_end`, and `__kernel_end`. Do not infer BSS length from raw file size or sector count.
- Validate all arithmetic with checked wide intermediates: `KERNEL_LOAD + file_bytes`, `KERNEL_LOAD + memory_bytes`, sector rounding, and end-exclusive ranges. Reject wraparound, reversed bounds, values outside the supported physical-address width, and a memory end beyond the early identity-map/PMM ceiling.
- Before clearing BSS, require the complete kernel memory interval (including BSS) to be covered by safe usable E820 memory. A single-record coverage check may conservatively reject adjacent usable records, but must never accept a range where any reserved, disabled, or unknown descriptor overlaps it; normalize/split the map with reserved-wins semantics or prove equivalent coverage.
- Check the full memory interval against stage-1/stage-2, E820/MB2 buffers, BIOS bounce buffer, loader scratch/stack, and the ProsenjitFS partition/disk layout. The current fixed 1-MiB kernel destination is distinct from low-memory loader areas, but future layout changes must retain explicit overlap checks.
- Only after validation, zero `[KERNEL_LOAD + (__kernel_bss_start - __kernel_start), KERNEL_LOAD + (__kernel_bss_end - __kernel_start))` (using checked offsets), then pass the native magic and MB2 pointer to `_start`. The clear must precede any kernel instruction that depends on BSS and must not overwrite stage-2's low-memory stack or boot arguments.
- Verify the PMM continues to reserve the complete linker interval `[__kernel_start, __kernel_end)` after initialization, including the zeroed BSS pages, and that malformed E820 input cannot make any part allocator-visible.

**Additional regression cases**
- Make the flat file end before BSS by construction and verify the manifest still reports the larger complete memory extent.
- E820 coverage ends at the file end but not the BSS end: reject before the clear and before the final jump.
- E820 records split at a page boundary across adjacent usable entries: accept only if the normalized union covers the full interval and no reserved/disabled/unknown entry overlaps any part.
- Place a reserved/disabled descriptor over only the first, middle, or final BSS page while a larger usable descriptor overlaps it: reject (reserved-wins).
- Verify all BSS pages are zero at `_start`, then verify PMM allocation never returns any page in `[__kernel_start, __kernel_end)`.
- Exercise exact file-end, exact memory-end, one-byte-short memory coverage, one-page-short coverage, 1-GiB ceiling, arithmetic overflow, and collision with each loader/partition range.

This supplements the BSS zero-initialization finding above. It is a source-audit/specification requirement only; no implementation or runtime test was performed.
### E820 usable-range coverage must reject 64-bit end overflow

A further check of `boot/native_stage2.asm::validate_usable_range` found that an E820 entry whose base high dword is zero is treated as covering the requested low-memory range immediately when the length high dword is nonzero. The code does not first calculate and validate the complete 64-bit exclusive end `base + length`. A malformed descriptor whose 64-bit addition wraps can therefore be treated as an unbounded/covering range even though its mathematical interval is invalid. This check is used before writing the BIOS bounce buffer and kernel image, so it must fail closed.

Required behavior: validate the full-width base and length, reject zero-length and any `base + length` overflow before comparing coverage, then compare the checked exclusive end against the requested end. A valid range extending above 4 GiB may still cover a low-memory target; a high length dword is not by itself proof of a valid interval. Keep the existing 32-bit destination constraints separate from validation of the full-width E820 descriptor.

Required tests: (1) low base + valid length crossing 4 GiB; (2) low base + `UINT64_MAX` length causing wrap; (3) base near `UINT64_MAX` with small length causing wrap; (4) zero length; (5) exact requested end; and (6) one byte short. Every malformed descriptor must be rejected before any BIOS disk read or kernel copy.

Source audit only; no stage-2 code or tests were changed/run.

### Native BIOS E820 carry-flag termination compatibility

- Re-read `boot/native_stage2.asm::collect_e820` against the ACPI System Address Map Interface specification, section 15.1 (INT 15h, E820h), including the output semantics for CF and EBX.
- The current collector treats every carry-flag result as fatal, even after one or more valid descriptors have already been returned.
- ACPI specifies that a BIOS may indicate that the final descriptor was already returned on a previous iteration by returning CF set on the subsequent E820 call; when CF is set, the caller ignores the other returned register values. Therefore, unconditional failure on a later carry can reject a valid, complete firmware map on BIOS implementations using this termination convention.
- Future collector behavior must distinguish: (1) CF set before any valid descriptor: fail because no usable map was obtained; (2) CF set after at least one valid descriptor in response to a nonzero continuation token: accept as end-of-map only under the documented E820 termination convention and do not consume EAX/ECX/EBX or the descriptor buffer from that failed call; (3) successful call: validate SMAP signature and returned ECX before consuming the record, then pass EBX unchanged to the next iteration. Keep the existing hard record-capacity failure when a successful descriptor leaves a nonzero continuation token after slot 32.
- Add tests for first-call carry, carry on the next call after one descriptor with a nonzero continuation token, ordinary EBX=0 termination, a full 32-record map ending with EBX=0, and a 32nd successful record with EBX still nonzero. Confirm no descriptor bytes or register values from a CF-set call are consumed.
- This is a source/specification finding and future acceptance-test requirement only. No loader code, ABI, tests, build, firmware run, or QEMU runtime was changed or executed.
- Reference: ACPI Specification 6.6, section 15.1, INT 15h E820h output semantics: https://uefi.org/specs/ACPI/6.6/15_System_Address_Map_Interfaces.html .


### E820 returned descriptor length upper-bound requirement

- The current collector supplies ECX=24 as the destination capacity but does not validate the returned ECX at all.
- The E820 interface defines returned ECX as the number of descriptor bytes supplied, bounded by the size passed by the caller. For this 24-byte caller buffer, a successful response with ECX>24 is malformed and must be rejected; it must not be interpreted as permission to consume or trust bytes outside the 24-byte slot.
- The accepted response lengths must be explicit: ECX<20 rejects; ECX=20 uses only the legacy 20-byte descriptor; ECX=21..23 still uses only the legacy 20-byte fields and must not treat the partially returned extension as valid; ECX=24 permits inspection of the complete extended-attributes dword. ECX>24 rejects.
- Add successful-call boundary tests for 19, 20, 21, 23, 24, and 25 bytes, including a prefilled extension sentinel proving that a 20–23-byte response cannot be treated as a complete extended-attribute dword. Carry-set termination remains a separate control-flow result: ignore returned ECX and all other outputs when CF is set.
- Documentation/test-plan refinement only; no BIOS code or executable test was changed or run.


### ACPI 6.6 E820 disabled-range and extended-attribute semantics

- A specification-version cross-check found that earlier notes described E820 extended-attribute bit 0 as an enabled/disabled flag. That interpretation is not correct for the current ACPI 6.6 contract: Table 15.1 defines AddressRangeDisabled as E820 type 6, while Table 15.5 says extended-attribute bit 0 is reserved and must be set to 1; bits 2:1 are reserved and must be set to 0.
- The current stage-2 code labels bit 0 as the enabled flag in `validate_usable_range` and `build_mb2_info`. This is a stale interpretation. The new producer must classify disabled memory by `Type == 6`, must not infer enabled/disabled status from bit 0, and must validate the current reserved-bit constraints for a complete 24-byte descriptor. A 20-byte descriptor has no extension; for 21–23-byte responses the extension must not be interpreted as a complete 32-bit field.
- Existing behavior is conservative in one respect: a type-6 descriptor does not pass the stage-2 type-1 usable-range check, and the kernel's allocator only admits type-1 ranges. The issue is nevertheless a real specification mismatch: valid type-1 descriptors are subject to the wrong attribute test, malformed attribute values are not explicitly rejected, and the earlier documentation/test plan incorrectly called bit 0 an enabled flag.
- Multiboot2 translation must keep every non-usable E820 type non-allocatable. Type 6 can be emitted as Multiboot2 type 2 for an explicit reserved classification; unknown/non-usable types must never be promoted to type 1. The future v1 memory-map record uses type 1 only for validated usable RAM and zeroes its ABI-specific attributes field.
- Regression tests: type 1 with valid 24-byte attributes (bit 0 set, bits 2:1 clear); type 6 with valid attributes; invalid bit 0 clear; invalid bits 2:1 nonzero; 20-byte record with no extension; 21–23-byte records with poisoned extension tail; unknown types 7/8/9; and verify only type 1 is ever admitted to PMM after validation.
- This section supersedes the earlier E820 extended-attribute enabled-bit wording in this document. Source/specification audit only; no stage-2 implementation, tests, build, firmware, or QEMU runtime changed or ran.
- Reference: ACPI 6.6 section 15, Tables 15.1, 15.4, and 15.5: https://uefi.org/specs/ACPI/6.6/15_System_Address_Map_Interfaces.html .


### Multiboot2 producer/consumer serialization and reserved-overlap audit — 2026-11-21

A source-level cross-check of `boot/native_stage2.asm::build_mb2_info` and `kernel/memory.c::physical_memory_init` found the following distinctions:

- The native producer emits an 8-byte-aligned MBI at `MB2_BUFFER=0x6000`, followed by one mmap tag and an 8-byte end tag. The mmap tag uses `entry_size=24`, `entry_version=0`, and its computed payload size is `16 + 24 * descriptor_count`. With the collector's 32-record bound, the resulting MBI occupies at most 800 bytes. The E820 staging area starts at `0x5000` and occupies at most 772 bytes, so these two fixed buffers do not overlap in the current layout.
- The producer copies each 24-byte E820 record in source order and writes zero to the Multiboot2 entry's reserved dword. It does not sort, split, merge, or normalize overlapping/adjacent ranges.
- The producer currently tests E820 extended-attribute bit 0 after copying the entry and changes the emitted mmap type to 2 when that bit is clear. Under ACPI 6.6, bit 0 is reserved and must be set for a valid complete 24-byte response; it is not an enabled flag. A malformed clear bit must be rejected by the future validated producer, not silently reinterpreted. This fallback is conservative (it does not make the range available), but it is still an implementation/specification mismatch.
- A valid E820 type-6 disabled descriptor is copied as Multiboot2 type 6. The Multiboot2 specification defines type 1 as available and treats all other currently unrecognized types as reserved; therefore type 6 remains non-allocatable in a compliant consumer. E820 types 2, 3, 4, 5, and unknown values are likewise copied without promotion to type 1. This source check does not establish that malformed attributes are rejected.
- The kernel parser only calls `mark_available` for entries whose type is exactly 1. However, it never applies reserved entries as exclusions and never builds a normalized map with reserved-wins semantics. Consequently, if an available type-1 entry overlaps a reserved/disabled/unknown entry, the type-1 interval can still be admitted regardless of the reserved entry's position in source order. The later explicit reservations for low memory, kernel image, and MBI do not protect arbitrary reserved overlaps elsewhere. This is a confirmed PMM safety gap.
- The parser accepts mmap tags with `entry_size >= 24` but does not require a multiple of 8, and it does not require `entry_version == 0`. Its loop silently ignores any trailing bytes smaller than one complete entry. It also accepts multiple mmap tags, does not explicitly require exactly one mmap tag, and does not prove that a valid end tag of size 8 is present exactly at the end of `total_size`; it breaks on the first type-0 tag without validating that tag's size or final position. These are independent malformed-input acceptance gaps.
- The MBI total-size upper bound and checked MBI base-plus-total-size addition are performed before walking tags. Thus the later tag-end addition is bounded by the validated MBI interval, but the parser still needs explicit end-tag/final-position and mmap-structure validation. It also dereferences the first MBI dword before checking the required 8-byte MBI alignment or proving the full MBI is accessible under the current early identity map; the earlier alignment/accessibility findings remain open.

**Required producer and parser behavior**

1. Validate complete E820 records against ACPI 6.6 before serialization. For a 24-byte response require bit 0 set and bits 2:1 clear; do not reinterpret bit 0 as enabled/disabled. Classify disabled memory by E820 type 6. Reject malformed attributes before publishing a map.
2. Preserve the invariant that only validated E820 type 1 can become Multiboot2 type 1. Map all other E820 types to a documented non-usable Multiboot2 classification (type 2 is suitable for explicit reserved/disabled classification); never promote unknown types.
3. Normalize the complete map before PMM mutation: checked non-wrapping intervals, deterministic sorting, split at all boundaries, and reserved/disabled/unknown precedence over available memory. Adjacent usable intervals may be merged only after all exclusions have been applied.
4. Validate exactly one mmap tag, `entry_version == 0`, `entry_size >= 24` and divisible by 8, and an exact payload multiple of `entry_size`. Reject a partial trailing entry.
5. Require an 8-byte-aligned MBI base before typed access, validate the complete MBI range is accessible under the active early mapping, require the end tag to have type 0 and size 8, and require its aligned end to equal the MBI's declared end exactly. Reject missing, early, duplicate, or malformed terminators.
6. Parse and validate the complete map into temporary normalized state before changing allocator bitmaps. Any malformed descriptor must leave PMM state untouched.

**Additional regression cases**

- E820 type 1 valid attributes, type 6 disabled, unknown types 7/8/9, and invalid attribute bit patterns; verify only valid type 1 can become MB2 type 1.
- Type-1 available interval overlapped by type-2 reserved, type-6 disabled, ACPI NVS, bad-memory, and unknown intervals; test every source-order permutation and verify reserved-wins.
- Adjacent usable entries, nested overlaps, identical duplicate entries, zero-length entries, and overflowing end addresses.
- Native maximum 32-record MBI size, exact buffer boundaries, and sentinels around the E820 and MBI buffers.
- MBI base alignment residues 1..7; end tag missing, size 0/7/9, early terminator, trailing bytes after terminator, and terminator exactly at the declared end.
- Mmap tag missing, duplicated, truncated header, entry version 1, entry sizes 24/32/40, invalid non-multiple-of-8 sizes 25/31, and payload remainder 1..(entry_size-1).
- Inject a malformed later descriptor after valid entries and verify no allocator bitmap or free-page counter changes.

This is a source/specification audit and acceptance-test update only. No loader, parser, PMM implementation, executable test, build, firmware, or QEMU runtime was changed or run. No runtime PASS is claimed.

Specification reference: GNU GRUB, *Multiboot2 Specification*, sections 3.6.1, 3.6.2, and 3.6.8: https://www.gnu.org/software/grub/manual/multiboot2/html_node/Boot-information-format.html .


### Module, ACPI RSDP, and framebuffer ownership audit — 2026-11-21

A source-level review of the exact PR branch checked `boot/native_stage2.asm`, `boot/grub.cfg`, `kernel/memory.c`, `kernel/kernel.c`, `kernel/display.c`, and `kernel/include/display.h`.

**Boot modules and ownership**
- The current native BIOS producer constructs only a memory-map tag and an end tag; it does not load or emit module tags.
- GRUB is configured to load the ELF kernel with `multiboot2`, but the kernel parser in `kernel/memory.c` only recognizes mmap tag type 6. It does not parse module tag type 3, validate `mod_start < mod_end`, validate the module command-line NUL terminator within the tag, check payload physical bounds/overlaps, or reserve module payload pages.
- `physical_memory_init()` reserves low memory, `[__kernel_start,__kernel_end)`, and the MBI itself. It does not reserve module payloads. Thus a GRUB-provided module can remain marked allocator-managed/available and be overwritten by page allocation. This is a confirmed ownership gap for any boot path that supplies modules; the current native stage-2 path does not currently supply one.
- Required future behavior: validate each module tag and payload using checked half-open ranges, enforce `start < end`, bounded UTF-8/NUL-terminated command string, physical-width and accessible-range constraints, reject forbidden overlaps (kernel, MBI, other modules, loader-owned ranges), and reserve every payload before allocator publication. Define explicit lifetime/release semantics; do not release initramfs/module pages until all consumers have finished.

**ACPI RSDP**
- The current native stage-2 producer emits no ACPI old/new RSDP tag. The current kernel boot parser recognizes only the mmap tag and has no ACPI tag dispatch or RSDP validation path.
- No source evidence was found for an active RSDP consumer that checks signature, legacy checksum, revision policy, extended length bounds, extended checksum, or physical accessibility. ACPI/RSDP discovery and validation are therefore not implemented in these inspected boot paths; this is not a claim that the firmware has no RSDP.
- Required future behavior: recognize Multiboot2 old-RSDP type 14 and new-RSDP type 15; validate the tag's minimum payload before reading; validate the signature and 20-byte checksum; apply the documented revision policy (revision 0 legacy length, reject unsupported revision 1, revision 2+ validate declared length and extended checksum); bound the declared length within the tag; and validate any physical address/range before dereferencing. Preserve the RSDP copy as owned boot data until ACPI table discovery has safely copied or mapped the required tables.

**Framebuffer**
- The current kernel parser does not recognize Multiboot2 framebuffer tag type 8, and native stage-2 does not emit one. The inspected `kernel/display.c` provides a generic backend contract and a synthetic self-test backend using address `0xE0000000`; it is not a boot framebuffer parser or a firmware framebuffer mapping implementation.
- The display API checks nonzero dimensions, maximum width/height, `pitch >= width`, and nonzero framebuffer/size, but it does not validate format-specific bytes-per-pixel, checked `pitch * height`, complete physical end-address arithmetic, framebuffer tag variant payload lengths, or ownership/reservation/mapping of the physical framebuffer aperture. The `pitch >= width` check alone is insufficient because pitch is measured in bytes while width is measured in pixels.
- Required future behavior: parse the complete framebuffer tag before use; validate tag size for indexed/direct-RGB/EGA-text variants; validate dimensions, supported bpp and channel masks; compute minimum row bytes and `pitch * height` with checked arithmetic; validate physical base plus computed size and supported address width; map the aperture with appropriate device/cache attributes; prevent PMM from allocating or aliasing those pages without incorrectly treating MMIO as ordinary RAM. For EGA text, use character-cell semantics rather than pixel bytes-per-pixel.

**Cross-object ownership requirements**
- Validate all boot-owned intervals together before PMM mutation: kernel image including BSS, MBI, module payloads, retained loader data, ACPI RSDP/table copies, and framebuffer MMIO aperture. Reject arithmetic overflow and forbidden overlaps; reserve RAM-backed objects, and represent MMIO apertures as device mappings rather than ordinary allocatable RAM.
- A malformed later tag must not leave partially updated PMM bitmaps. Parse into bounded temporary state, validate all cross-references and interval relationships, then commit the normalized memory map and reservations atomically.

**Regression matrix**
- Module tags: truncated header; size below fixed fields; `start == end`; `start > end`; end overflow; payload above supported physical limit; missing NUL; NUL exactly at final allowed byte; module overlaps kernel/BSS, MBI, another module, reserved memory, or loader scratch; module pages remain unavailable until explicit release; malformed later module leaves allocator state unchanged.
- ACPI: old/new tags with truncated payload; invalid signature; bad legacy checksum; unsupported revision; revision 2+ length below minimum, beyond tag, beyond allowed cap, or overflowing; bad extended checksum; duplicate conflicting RSDP tags; inaccessible physical table range; preserve valid RSDP copy across later allocator activity.
- Framebuffer: tag shorter than common fields; indexed palette count/array truncation; direct-RGB channel positions/masks outside bpp or overlapping; unsupported framebuffer type; zero dimensions/bpp; width-to-row-byte overflow; pitch smaller than minimum row bytes; `pitch * height` overflow; base+size overflow; aperture outside supported physical width; overlap with kernel/MBI/modules; verify framebuffer pages are not returned by PMM and device mapping uses non-cacheable/write-combining policy as supported.
- Cross-object matrix: every pairwise overlap among kernel, MBI, modules, RSDP copies, retained loader objects, and framebuffer aperture; adjacent non-overlapping ranges; exact-end boundaries; overflow and partial-page boundaries.

This is a source/specification audit and test-plan update only. No implementation or executable tests were changed or run; no build, firmware, or QEMU runtime PASS is claimed.

Specification reference: GNU GRUB, *Multiboot2 Specification*, boot-information format, module tag (3), framebuffer tag (8), and ACPI old/new RSDP tags (14/15): https://www.gnu.org/software/grub/manual/multiboot2/html_node/Boot-information-format.html .


### BIOS/GRUB adapter convergence and early physical-access audit — 2026-11-21

- Rechecked `boot/entry.asm`, `boot/native_stage2.asm`, `kernel/kernel.c`, and `kernel/memory.c` against the v1 producer/consumer contract.
- Both current legacy paths converge on the same 32-bit `_start` entry and then the same `kernel_main` / Multiboot2 PMM parser. GRUB supplies Multiboot2 magic plus its information pointer; native BIOS supplies its distinct legacy magic plus the stage-2-built Multiboot2-compatible block at `MB2_BUFFER`. The native producer currently places that block at physical `0x6000`, whereas the GRUB-supplied Multiboot2 information block is not guaranteed by the protocol to be below 1 GiB.
- `_start` saves the incoming magic and information address in 32-bit storage, creates only a 0..1-GiB identity map using 2-MiB pages, and forwards the original values to C. `physical_memory_init` dereferences the information pointer to read `total_size` before establishing that the complete block is accessible in that map. This confirms an early-access fault risk for a valid GRUB block outside the mapped range; it is not evidence that current GRUB always places it there.
- The current native low-address MBI is within the bootstrap identity map, but that placement is an implementation detail of the legacy BIOS adapter, not a guarantee that the future v1 ABI or every producer can rely on. The v1 adapter must either construct/copy its complete normalized handoff into a verified early-accessible region or establish temporary mappings before any dereference. It must validate the entire source and destination intervals, not only their first byte.
- The v1 contract's MAXPHYADDR discovery and validation must precede physical-object access. A physical address being below MAXPHYADDR does not imply it is currently mapped or safe to dereference. Keep three independent checks: (1) non-wrapping interval within MAXPHYADDR, (2) accessible through the active early virtual mapping or a deliberately established mapping, and (3) ownership/reservation plus non-aliasing. Framebuffer MMIO is a special case: validate its physical interval and mapping attributes, but do not read/dereference it as ordinary RAM during early parsing.
- The current kernel entry ABI is still legacy: `kernel_main(uint32_t boot_magic, uint32_t boot_info)` and `physical_memory_init(uint64_t multiboot_info_address)` consume the Multiboot2-compatible block. No v1 magic dispatch, 64-bit v1 handoff pointer path, normalized v1 object, or shared BIOS/GRUB v1 adapter was found in the inspected sources. The two legacy inputs therefore do not yet converge on a validated v1 model; the architecture document describes a migration target only.
- Required convergence rule: each producer-specific adapter must translate its source into the same canonical v1 semantic object (same sorted/disjoint memory map, reserved-wins result, module classification/ownership, ACPI copy semantics, framebuffer format/physical span, boot-device identity semantics, and retained-allocation inventory). Do not compare raw source descriptors byte-for-byte: E820, Multiboot2, and future UEFI descriptors have different encodings and metadata. Compare the normalized v1 result and rejection behavior for equivalent fixtures.
- Required ordering before PMM publication: establish CPU physical-width capability; validate/copy source metadata through an accessible mapping; validate every v1 in-allocation region and external physical interval; normalize all firmware ranges with checked arithmetic and reserved-wins splitting; validate pairwise object aliases and ownership coverage; calculate the 1-GiB allocator intersection without treating clipping as validation; then atomically publish the completed map and reservations. Any failure before the final commit must leave allocator state unchanged and halt before allocation.
- Regression matrix additions: native MBI at its current low address and at exact 1-GiB boundary cases; GRUB MBI wholly below, straddling, and wholly above 1 GiB; source block whose first byte is mapped but final byte is not; v1 block copied to an early mapped destination; physical range below MAXPHYADDR but unmapped; physical range mapped but above MAXPHYADDR; framebuffer MMIO above 1 GiB accepted without early dereference; and equivalent E820/Multiboot2 fixture pairs producing identical canonical v1 records and identical accept/reject outcomes.
- This remains a source audit and specification update only. No ABI/parser/adapter implementation, executable test, build, firmware test, or QEMU runtime was changed or run.



### PMM atomicity and firmware-reserved page ownership audit — 2026-11-21

**Exact source paths reviewed:** `kernel/memory.c` (`physical_memory_init`, `mark_available`, `reserve_range`) and `kernel/kernel.c` (`kernel_main` caller).

**Confirmed parser-state mutation before complete validation**
- `physical_memory_init` clears the global page/managed/user bitmaps and counters before it validates the MBI pointer, total size, tags, mmap structure, or all entries.
- While iterating the mmap tag, it calls `mark_available` immediately for each type-1 entry. A malformed later tag/entry can then return an error after earlier records have already modified the global allocator state. This violates the v1 atomic-publication contract: reject must leave the pre-call allocator state unchanged.
- Scope qualification: the current `kernel_main` caller checks the nonzero return and enters a permanent `cli; hlt` loop before announcing PMM online or allocating a test page. Thus this audit does not claim the current boot continues allocating after this parser error. The defect is still real parser-side partial mutation and prevents safe reuse, recoverable error handling, and a strict transactional contract.
- Required architecture: parse into bounded temporary descriptors; validate the complete MBI/tag/end-tag/mmap structure and every checked interval; normalize all records and ownership; compute the final allocator page set in scratch state; only then publish all bitmap/counter state in one commit. Every failure path before commit must preserve the previous PMM state byte-for-byte (or preserve an explicitly documented uninitialized state).

**Confirmed firmware-reserved overlap gap**
- The current parser invokes `mark_available` only for Multiboot2 type-1 entries. It does not use firmware non-available entries as exclusions and does not split overlaps with reserved/disabled/unknown ranges before making pages allocatable.
- `mark_available` rounds usable intervals inward: first page is ceil(base/4 KiB), final page is floor(end/4 KiB). `reserve_range` rounds explicit reservations outward: first page is floor(base/4 KiB), final page is ceil(end/4 KiB). Those rounding directions are individually conservative, but they do not solve firmware-map overlap because firmware-reserved intervals are never passed to `reserve_range` or otherwise subtracted.
- Example: usable `[0x100000,0x102000)` plus reserved `[0x101000,0x101001)`. The usable pass can mark both 4-KiB pages free; the reserved byte lies in the second page, but no firmware-reserved exclusion clears it. The second page can therefore be returned by PMM despite containing reserved memory.
- Required rule: normalize all firmware intervals before page admission. For every page intersecting the allocator window, it is allocatable only if the complete page is covered by usable RAM and no byte of that page intersects any reserved, disabled, unknown, device, or otherwise non-usable interval. Reserved wins independent of descriptor order. Partial-page usable coverage is insufficient; any partial-page non-usable overlap excludes the entire page.
- Explicit boot-object reservations (kernel, MBI, modules, RSDP copies, retained loader allocations) must be applied to the same scratch ownership model before commit. MMIO apertures remain device mappings and never become PMM RAM pages.

**Regression matrix additions**
- Atomicity: valid usable entry followed by truncated tag; valid usable entry followed by malformed mmap stride; malformed second mmap entry; duplicate mmap tag; missing/early/non-final end tag; checked-add overflow in a later entry. Snapshot every bitmap/counter before call and assert exact unchanged state after each rejection.
- Reserved-wins ordering: usable then reserved, reserved then usable, duplicate and overlapping reserved records, type-6 disabled, unknown type, and adjacent reserved intervals. Results must be order-independent.
- Page boundaries: reserved byte at page start, final byte, and middle; reserved interval crossing a page boundary; usable interval ending one byte short of a full page; usable interval covering exactly one page; adjacent usable pages; exact 1-GiB allocator end; interval starting at 1 GiB; valid range crossing 1 GiB; arithmetic overflow near UINT64_MAX.
- Ownership composition: firmware-reserved page overlapping kernel/MBI/module/RSDP/retained allocation; object reservation adjacent to a usable page; object interval partial-page overlap; MMIO aperture sharing a 4-KiB page with reported usable RAM (page excluded from PMM and mapped only with device semantics).
- Allocation oracle: after successful normalization, allocate every page in a small synthetic window and prove no returned page intersects any non-usable byte or retained object; compare output for permuted input descriptor order.

**Status:** source audit and documentation/test-plan update only. No PMM implementation, executable test, compiler run, kernel build, firmware test, or QEMU runtime was changed or run.


### PMM bitmap residency and allocator-window audit — 2026-11-21

**Exact source paths reviewed:** `kernel/memory.c`, `kernel/include/memory.h`, and `kernel/linker.ld`.

- `PROSENJIT_PHYS_LIMIT` is exactly 1 GiB and the page size is 4096 bytes, yielding 262,144 allocator pages. `BITMAP_WORDS` is 4096, so each 64-bit bitmap occupies 32 KiB and the three static bitmaps (`page_bitmap`, `managed_bitmap`, and `user_bitmap`) occupy 96 KiB in total, excluding counters and alignment.
- The linker places `.bss` between `__kernel_bss_start` and `__kernel_bss_end`, with `__kernel_start` at the image start and `__kernel_end` after the aligned BSS. `physical_memory_init` reserves the complete `[__kernel_start, __kernel_end)` interval after importing available pages. Therefore the bitmap backing storage is covered by the kernel reservation in the current linker layout; no bitmap-self-allocation defect was confirmed in this audit.
- The allocator deliberately clips its managed physical window to 1 GiB, matching the current bootstrap identity-map limit. This is a scope limit, not validation of firmware intervals above 1 GiB: the v1 adapter must validate the complete physical interval and ownership before deriving the allocator-window intersection. A range crossing the limit must be clipped only after checked non-wrapping arithmetic and reserved-wins normalization.
- Page zero is excluded by the existing low-memory reservation, and `physical_page_alloc` uses physical address zero as its failure sentinel. Preserve this invariant explicitly in the v1 allocator contract and tests.
- The current initialization order still clears global bitmap state and incrementally calls `mark_available` before complete MBI validation; the transactional failure finding recorded above remains. Likewise, current `reserve_range`/ `mark_available` silently clip overflow or out-of-window intervals and do not replace the required producer-side checked interval validation.

**Regression requirements:** assert the compiled bitmap sizes and BSS containment; assert the complete kernel extent (including all allocator metadata) is excluded after PMM initialization; verify page zero is never returned; test exact 1-GiB end, a range crossing 1 GiB, a range beginning at 1 GiB, and overflowing intervals before clipping; verify the same canonical allocator map for descriptor permutations.

**Status:** source/linker audit and documentation only. No allocator implementation, executable test, compiler run, kernel build, or QEMU runtime was changed or run.


### PMM allocation/free ownership and synchronization audit — 2026-11-21

**Exact source paths reviewed:** `kernel/memory.c`, `kernel/include/memory.h`, `kernel/usb_dma.c`, `kernel/paging.c`, `kernel/address_space.c`, `kernel/heap.c`, `kernel/elf_loader.c`, and `kernel/thread.c`.

**Verified positive properties**
- `physical_page_free` rejects unaligned, out-of-window, unmanaged, and already-free pages. A successful free clears the user-mappable bit, sets the free bit, and increments the free counter exactly once; a second free is ignored.
- `physical_page_free_contiguous` validates alignment, nonzero count, complete in-window extent using subtraction-safe bounds, and every page's managed/allocated state before changing any bitmap. Its mutation pass then clears user classification, sets each free bit, and updates `free_pages` by the validated count. This is all-or-nothing for the validation failures represented by its current state checks.
- `physical_page_alloc_contiguous` validates nonzero bounded page count, power-of-two page alignment, free-page capacity, and scans for a complete run of managed+free pages before committing the run. The USB DMA wrapper adds a separate fixed-size ownership registry, allocation IDs, and a lock; it validates the exact registry tuple before scrubbing and freeing a DMA allocation.
- Existing kernel consumers use single-page allocation/free for page-table, heap, ELF, address-space, and thread-stack lifecycle; the USB DMA path uses the contiguous API and its registry. No direct caller misuse was established by this source pass.

**Contract gaps / requirements**
- The generic PMM API has no per-allocation provenance record. `physical_page_free_contiguous(address,count)` proves that every page is currently managed and allocated, but cannot prove that the caller owns that exact run as one allocation rather than a combination of independently allocated pages. The USB DMA wrapper supplies that stronger ownership proof for its own allocations; other future contiguous consumers must provide an equivalent token/registry or the PMM API must grow one.
- The generic PMM functions contain no lock or atomic bitmap/counter protocol. This audit does not assert a currently reachable SMP race because the execution topology was not established here. The architecture contract must nevertheless state that all PMM state transitions are serialized (including interrupt/re-entrant contexts), or add allocator synchronization before concurrent CPUs/contexts can call allocation/free paths.
- The single-page API does not expose an allocation token or distinguish page-allocation classes; it relies on the caller's lifecycle ownership. Preserve the invariant that a page is unmapped and no longer referenced before it is returned to PMM. A user-mappable bit is a classification, not a substitute for mapping-reference accounting.
- `free_pages`, `total_pages`, `managed_bitmap`, `page_bitmap`, and `user_bitmap` must be checked as one invariant set: free implies managed; allocated managed pages are not free; reserved pages are not managed; user-mappable implies managed and allocated; `free_pages == popcount(page_bitmap)`; and `total_pages == popcount(managed_bitmap)`.

**Regression requirements:** mixed single-page and contiguous allocations; freeing a span containing one already-free or unmanaged page must leave all pages and counters unchanged; unaligned/zero/overflow/out-of-range frees; adjacent independent allocations passed to generic contiguous-free (define whether caller contract or token validation rejects); double-free; user classification cleared on every successful free; failed free preserves all bitmap/counter snapshots; interleaved allocate/free under the documented serialization mechanism; and DMA stale-token, duplicate-free, and registry-exhaustion cases.

**Status:** source audit and specification update only. No PMM implementation, executable tests, compiler run, kernel build, or QEMU runtime was changed or run.

### Address-space page-table allocation rollback audit — 2026-11-21

Source reviewed on this branch: `kernel/address_space.c::address_space_map_user_page` (including the intermediate PML4/PDPT/PD permission checks and existing failure cleanup).

**Confirmed partial-hierarchy leak on rejected mappings:** the function can allocate and attach a previously absent PDPT, then encounter an existing invalid/supervisor-only/large-page PDPT entry and return directly. Likewise, after creating a PDPT and a PD, it can encounter an invalid/supervisor-only/large-page PDE and return directly. These validation-return paths do not consistently detach and free the hierarchy levels created earlier in the same call. The leaf PTE is not installed, but allocator pages and empty parent entries can remain. This is a resource leak / partial structural mutation on a failed map, not evidence of a leaf mapping being installed.

The existing `ensure_table` allocation-failure and occupied-leaf paths do perform some rollback, but that does not cover the earlier permission/large-page validation returns. The rollback must be based on exactly which levels this invocation created, and must detach parent entries before freeing their child table pages. It must never free pre-existing shared or caller-owned hierarchy levels.

**Required regression matrix:**
- Start with an empty user PML4 slot; force an existing invalid/supervisor-only/large-page PDPT child at the next level after the call has created its PDPT. Assert failure, original parent entry restored, and PMM allocated/free counts plus bitmap snapshots unchanged.
- Start with a missing PDPT and missing PD, then place an invalid/supervisor-only/large-page PDE at the next level; assert both newly created ancestors are detached and reclaimed.
- Repeat with pre-existing PDPT/PD ancestors to prove rollback preserves them and only releases levels created by this invocation.
- Exercise allocation failure at each hierarchy level and occupied-leaf rejection; verify no leaked pages, no dangling parent entries, and no mutation to unrelated mappings.
- Include user and supervisor ancestor permission combinations, large-page entries at each supported level, and repeated fail/retry cycles.

**Status:** confirmed by source inspection; documentation/specification only. No implementation, executable test, compiler run, kernel build, or QEMU runtime was changed or run. This finding remains open for a separately reviewed implementation change.


### PMM consumer teardown dependency cross-check — 2026-11-21

The allocator-consumer review also traced process wait/reap through `kernel/process.c` and `kernel/thread.c`. This exposed no new PMM bitmap defect, but it reconfirmed an existing lifecycle ordering blocker documented in `docs/NATIVE-SPAWN-CONTRACT.md`: `process_wait()` calls `process_release_resources()` (close handles, then destroy the address space) before `process_unregister()` invokes the stronger `thread_reap_process()` validation. That later validation checks each owned kernel stack, active TSS RSP0, and current RSP; a rejection can therefore occur after address-space/handle teardown, after which the caller restores the process state to ZOMBIE despite resources already being released.

Treat this as a cross-subsystem teardown dependency, not as a newly discovered allocator implementation defect. Before wait/reap is relied on as transactional, complete all non-mutating checks—including thread-stack/TSS/RSP and inactive-CR3 checks—before the first destructive close/free, or implement explicit retryable teardown progress. The full required preflight and fault-injection matrix remains in `docs/NATIVE-SPAWN-CONTRACT.md`; this boot/PMM specification must not imply that allocator correctness alone guarantees safe process teardown.

**Status:** exact-branch source cross-check and documentation only. No lifecycle/PMM implementation, executable test, compiler/build, or QEMU runtime was changed or run.


### Kernel page-table reclamation audit — 2026-11-21

The kernel VMM path has a separate reclamation asymmetry from the user address-space path. `kernel/paging.c::paging_unmap_page()` clears the leaf PTE and invalidates that VA, but does not test whether the PT, PD, or PDPT became empty, detach an empty child from its parent, or return the now-unused table page to the PMM. In contrast, `address_space_unmap_user_page()` explicitly walks upward and reclaims empty user hierarchy levels.

Consequently, successful kernel mappings that are later unmapped can leave allocator-backed page-table pages resident. Repeated mappings in previously unused hierarchy regions followed by unmaps can accumulate this metadata even when all leaf mappings are gone. This is a page-table metadata retention/leak; it is not a claim that `paging_unmap_page()` should free the caller's leaf physical page.

**Required contract and regression:** define explicit ownership for kernel-root table pages and whether empty tables are reclaimed or deliberately cached; if reclaimed, clear the parent entry before freeing only an empty, exclusively owned child, preserve shared/bootstrap mappings and large-page ancestors, and perform the required TLB invalidation. Test map/unmap/re-map in fresh PT/PD/PDPT branches, multiple live siblings, removal of the final sibling, MMIO partial-map rollback and region unmap, and PMM allocation/free counters plus parent-entry snapshots. Verify that leaf physical-page lifetime remains with its caller. Do not blindly free shared or pre-existing tables.

**Status:** confirmed by source inspection; specification only. No paging implementation, executable test, compiler/build, or QEMU runtime was changed or run.


## Upstream boot-protocol reuse audit — 2026-11-21

The following upstream projects were inspected against the current v1 wire contract and the existing BIOS/GRUB entry paths. This is a reuse decision record, not a claim that their code has been imported.

### Reuse decisions

1. **Limine protocol / Limine bootloader**
   - References: https://github.com/Limine-Bootloader/limine-protocol and https://github.com/Limine-Bootloader/limine/tree/v12.x
   - Reuse as design/reference material for protocol revisioning, explicit feature availability, bootloader/kernel separation, and the C header's fixed-width type discipline.
   - Do not adopt Limine's wire ABI as ProsenjitOS v1: Limine is a 64-bit request/response protocol with 64-bit pointers and bootloader-reclaimable response objects, whereas ProsenjitOS v1 deliberately uses a fixed 128-byte self-identifying allocation, 32-bit entry registers for the bootstrap handoff, 32-bit in-allocation offsets, and explicit reserved-map ownership. A wholesale switch would change the approved contract and complicate the current BIOS migration.
   - The Limine protocol repository's header is marked 0BSD; the Limine bootloader source uses the BSD 2-Clause license. If any actual source is later copied, retain the exact applicable upstream notices and record the pinned commit/path. No Limine source was copied by this audit.

2. **rust-osdev/multiboot2**
   - Reference: https://github.com/rust-osdev/multiboot2
   - Use as an independent behavioral reference for Multiboot2 tag-sequence validation, end-tag requirements, bounded tag parsing, and test-case comparison.
   - The crate is Rust and the current ProsenjitOS kernel/build is freestanding C/assembly; it is not a drop-in dependency. Its unsafe load API also requires the caller to provide a pointer readable for the complete reported information size, so ProsenjitOS must establish physical-range validity and active-map accessibility before dereferencing the source block.
   - The upstream repository is dual-licensed MIT OR Apache-2.0. Prefer the MIT grant if a separable source component is ever reused in this MIT-licensed repository, retain its notice, and review the exact file's license/provenance. No Rust source was copied.

3. **GNU GRUB Multiboot2 specification**
   - Reference: https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html
   - Treat the published specification as the normative compatibility contract for GRUB input: tag framing/alignment, end tag, memory-map entry stride/version, module, ACPI, and framebuffer tags.
   - Do not transplant GRUB implementation code into the MIT-licensed ProsenjitOS tree as part of this work. GRUB source includes GPL-3.0-or-later files; use the specification and independently written adapter logic unless a deliberate project-wide licensing decision authorizes otherwise. The specification's own copying notice must be followed if its text is reproduced or modified.

### Implementation boundary

- The ProsenjitOS v1 ABI remains the canonical kernel-facing contract. Upstream formats are parsed only at adapter boundaries and normalized into v1; no upstream boot protocol replaces v1.
- The existing native BIOS and GRUB paths remain legacy inputs until their respective adapters are implemented and both boot regressions pass.
- The Multiboot2 adapter must accept source mmap strides permitted by the specification (at least the defined 24-byte prefix, with valid stride/version/alignment and exact payload divisibility), but its normalized v1 output always uses the exact 24-byte v1 record.
- Reference implementations are test oracles and review aids, not evidence that ProsenjitOS has implemented the corresponding behavior. No parser, ABI header, producer, loader, runtime test, build, or QEMU behavior is implemented or claimed by this audit.


#### ACPI/UEFI source and physical-mapping implementation audit — 2026-11-21

A direct source search and review at this PR head (`kernel/memory.c`, `kernel/paging.c`, `kernel/kernel.c`, `kernel/include/memory.h`, `kernel/include/paging.h`, and `boot/native_stage2.asm`) confirms these implementation boundaries:

1. **No ACPI discovery/parser is present in the reviewed tree.** Searches for RSDP, XSDT/RSDT, ACPI table parsing, and EFI Configuration Table handling found no executable ACPI discovery or table-consumption path. The only ACPI references are boot-contract documentation. Therefore the RSDP/table lifetime rules above are future requirements, not implemented behavior.
2. **No UEFI loader is present.** The reviewed boot path consists of native BIOS stage 2 and GRUB/Multiboot2 entry/configuration. No `EFI_SYSTEM_TABLE`, `EFI_BOOT_SERVICES`, `EFI_MEMORY_DESCRIPTOR`, `GetMemoryMap`, or `ExitBootServices` implementation was found. UEFI descriptor normalization and runtime handoff remain future work.
3. **The current kernel VMM cannot map RAM-backed ACPI tables above 1 GiB.** `paging_init()` identity-maps only `[0, PROSENJIT_PHYS_LIMIT)` where the limit is 1 GiB. `paging_map_page()` rejects physical addresses at or above that limit. A separate `paging_map_mmio_region()` can map higher physical addresses, but deliberately applies write-through plus cache-disable flags for device MMIO. That helper is not an acceptable substitute for a normal cacheable RAM mapping for ACPI tables. A future ACPI implementation needs a distinct, bounded, page-aligned RAM/firmware-table mapping API with the correct memory type/cache attributes, mapping rollback, and explicit lifetime/unmap policy.
4. **The high-physical MMIO mapper uses a fixed 52-bit mask, not the runtime CPU physical-address width.** Its range check accepts up to `PAGE_MASK | 0xFFF` and does not consult CPUID-derived MAXPHYADDR. This is not sufficient for a CPU whose supported physical width is below 52 bits. Before this mapping path is used for general platform ranges, validate the complete half-open physical interval against the CPU-derived width; retain device-specific cache policy separately from address-width validation.
5. **The legacy PMM remains the current boot memory authority.** `physical_memory_init()` clears allocator bitmaps before validating the Multiboot2 block, reads the block directly, and incrementally admits type-1 entries; it does not yet consume the canonical v1 map or perform reserved-wins normalization. This remains a legacy-path limitation and must not be described as v1 parser behavior.

**Required follow-up acceptance cases:**
- No ACPI pointer is dereferenced before a complete range has passed overflow/MAXPHYADDR checks and a mapping for the required bytes is established.
- Map ACPI table RAM both below and above 1 GiB through the dedicated cacheable RAM mapping path; reject out-of-width, overflowing, zero, and inaccessible ranges before dereference.
- Assert the RAM table mapper does not inherit MMIO UC/WC flags, and the MMIO mapper does not silently become a cacheable RAM mapper.
- Exercise mapping allocation failure at each page-table level and prove all newly created mappings/page-table pages are rolled back without changing pre-existing mappings.
- On a simulated CPU with MAXPHYADDR below 52, reject physical ranges above that width even though they fit the architectural PTE field mask.
- Verify no ACPI or UEFI runtime PASS is reported until actual source, host tests, firmware/QEMU tests, and exact-head CI evidence exist.

This is a source-level audit and documentation/test-plan update only. No ACPI subsystem, UEFI loader, RAM mapping API, MAXPHYADDR enforcement, parser, executable test, build, firmware test, or QEMU runtime behavior was changed or run.


#### PMM transaction and entry-handoff source audit — 2026-11-21

Reviewed the current branch's `kernel/memory.c`, `kernel/kernel.c`, `kernel/include/memory.h`, and `boot/entry.asm` at the PR base head. The implementation remains a legacy Multiboot2/native-magic path; the canonical v1 handoff described above is not yet consumed.

**Confirmed current-source findings**

1. `physical_memory_init()` clears all three global bitmaps and allocator counters before it validates the input pointer, block size, tags, or memory map. A malformed block therefore destroys any previous allocator state before returning an error; it is not transactional.
2. The function reads `total_size` from the physical handoff pointer before proving that the first header bytes are inside the active identity map or even that the address is within the supported physical width. The current bootstrap identity maps only 0..1 GiB. A GRUB MBI outside that window can fault before a clean validation failure.
3. The parser stops when it sees an end tag but does not require exactly one end tag at the exact end of the declared block, nor does it prove that an end tag was seen. It does not require exactly one memory-map tag. It accepts `entry_size >= 24` but does not reject a trailing partial record when `(mmap_size - 16) % entry_size != 0`; the loop silently ignores the remainder. It also does not validate `entry_version`.
4. `tag->size` is checked with `current + tag->size > end`; use subtraction-safe validation (`tag->size <= end - current`) before forming the sum. The current block-end overflow check is present, but it does not make every later addition safe when the address is near the integer limit.
5. `mark_available()` rounds inward, which is appropriate for candidate usable intervals, but it clips overflowing or above-1-GiB ranges rather than rejecting malformed source intervals. It admits each type-1 entry immediately and does not subtract overlapping reserved/disabled/unknown descriptors. `reserve_range()` is applied only after all available entries have been admitted; it protects low memory, kernel image, and the MBI, but no module payloads or other retained loader objects are discovered/reserved here.
6. `boot/entry.asm` stores EAX/EBX into 32-bit `boot_magic`/`boot_info` slots and passes them to `kernel_main(uint32_t,uint32_t)` via EDI/ESI. This is the existing legacy ABI only; it cannot carry the v1 64-bit physical pointer and does not dispatch the v1 magic. Both legacy magic values are checked in C, but both currently feed the same Multiboot2-tag parser.
7. The native BIOS path is therefore expected to produce the Multiboot2-compatible tag buffer consumed by this legacy parser; the GRUB path supplies its own MBI. Neither path currently constructs the canonical v1 block at the entry boundary.

**Required implementation and regression sequence**
- First create a pure bounded Multiboot2-to-intermediate parser that validates pointer accessibility, total-size bounds, exact tag framing/end tag, exactly one mmap, supported mmap version, exact stride/remainder, and all checked arithmetic without touching global PMM state.
- Normalize all source descriptors into a bounded temporary interval set with reserved-wins semantics; reject malformed/wrapping records; compute module/MBI/kernel/loader ownership before publishing any allocator bitmap.
- Only after every tag, interval, and retained-object range passes validation should the implementation atomically initialize PMM. Inject failure at every parse/normalization/ownership stage and assert all bitmap/counter snapshots remain unchanged.
- Establish an explicit bootstrap accessibility check before dereferencing any physical handoff pointer. The legacy MBI path must not silently assume all GRUB information is below 1 GiB; either provide a safe early mapping/copy path or reject inaccessible input without dereference.
- Add a separate v1 entry dispatch that preserves the 32-bit initial handoff registers but widens them safely into the C ABI, validates v1 magic and 64-bit physical base consistency, and keeps both legacy magic paths isolated behind adapters until both boot routes pass regression tests.
- Add malformed-input cases for missing/duplicate/end-tag-not-last end tags, missing/duplicate mmap tags, invalid entry version, stride smaller/larger than supported v1, partial trailing entry, tag-size overflow, pointer outside early mapping, overlapping usable/reserved ranges, module overlap, and failures after earlier candidate intervals have been parsed.

**Status:** source audit and specification/test-plan update only. No parser, PMM implementation, assembly ABI, executable test, build, or QEMU runtime was changed or run. The current code findings are not a v1 implementation or a v1 test PASS.


#### Native BIOS E820 producer and load-range audit — 2026-11-21

Reviewed `boot/native_stage2.asm` at the current PR head, including `collect_e820`, `validate_usable_range`, `build_mb2_info`, and the native handoff. This is a source-level audit of the current BIOS producer, not a v1 implementation.

**Confirmed behavior and gaps**

1. The stage-2 collector has a fixed 32-record E820 buffer and correctly fails rather than silently truncating when firmware reports more records. It rejects carry/signature failures and an empty map. It requests 24-byte records and preinitializes the extended-attribute dword to enabled so a legacy 20-byte response can be represented.
2. It does not validate the returned E820 `ECX` record size. The implementation assumes the BIOS supplied at least the 20-byte legacy fields and at most the 24 bytes it reserved/copies. A malformed/unsupported returned size can leave fields stale or ambiguously interpreted. Add explicit acceptance for exactly the supported 20-byte and 24-byte forms (or a documented safe larger-prefix policy) and reject undersized/unsupported lengths before retaining the entry.
3. `validate_usable_range()` accepts a requested staging/kernel interval when one enabled type-1 E820 record contains it. It does not check whether any overlapping type-2/reserved, type-3/ACPI-reclaim, type-4/NVS, type-5/bad, type-6/disabled, or unknown record intersects that interval. Therefore a firmware map with a broad usable descriptor overlapping a reserved/disabled descriptor can pass the load preflight even though reserved-wins normalization should exclude the overlap.
4. `build_mb2_info()` copies every collected E820 descriptor into a Multiboot2 mmap entry and maps a disabled E820 record to type 2. This preserves non-usable information for the consumer, but it does not normalize overlaps or sort/split the source map. The legacy PMM parser then admits type-1 entries incrementally and ignores non-type-1 entries as exclusions, so the producer and consumer together do not implement reserved-wins semantics.
5. The stage-2 buffer layout is bounded for the current maximum: 32 records produce one mmap tag plus one end tag, and the current 0x6000 MBI is within the kernel's initial 0..1 GiB identity map. The MBI itself is reserved by the current PMM after parsing. The E820 scratch and BIOS bounce buffer are below 1 MiB and are covered by the kernel's blanket low-memory reservation; the kernel image is separately reserved. These observations do not solve future v1 ownership for additional retained loader allocations.
6. The generated mmap tag uses entry_size 24 and version 0; the end tag is 8 bytes. With at most 32 entries, the resulting information block remains far below the 16-bit size arithmetic limit used in this routine. The native producer still emits a legacy Multiboot2-compatible block, not the canonical v1 header.

**Required follow-up**
- Validate E820 returned record length before accepting each record; keep 20-byte legacy records distinct from 24-byte extended records and validate the defined extended-attribute bits for the latter.
- Make load-range validation use the complete E820 set: checked interval arithmetic, enabled usable coverage, and rejection if any reserved/disabled/unknown descriptor overlaps the target range. Prefer a bounded normalized interval set with reserved-wins precedence, rather than a single-record containment check.
- Add BIOS-stage2 fixtures for source-order permutations, broad usable plus narrow reserved overlap, disabled overlap, unknown type overlap, duplicate/adjacent usable records, 20-byte/24-byte records, undersized/oversized returned lengths, and a full 32-record map followed by another continuation.
- Compare the stage-2-produced Multiboot2 map and the future canonical v1 adapter result against the same normalized semantic fixtures; keep legacy output until native and GRUB adapter paths pass the same ownership and acceptance matrix.

**Status:** source inspection and documentation/test-plan update only. No stage-2 assembly, parser, executable test, build, BIOS boot, or QEMU runtime was changed or run.


#### GRUB configuration, Multiboot2 header, and native image-layout audit — 2026-11-21

Reviewed `boot/grub.cfg`, `boot/multiboot2_header.asm`, `kernel/linker.ld`, `scripts/build.sh`, and the native stage-1/stage-2 load path at this PR head.

**GRUB path — confirmed current configuration**

1. `boot/grub.cfg` contains one `multiboot2 /boot/prosenjit.elf` command followed by `boot`. It has no `module`/`module2` or initrd directive. Therefore the checked-in GRUB menu currently does not configure a boot module; the module-ownership bug remains a latent risk for future/custom GRUB entries or externally supplied modules, not evidence that this exact menu currently loads one.
2. `boot/multiboot2_header.asm` contains only the mandatory Multiboot2 header fields and the 8-byte end tag. It contains no information-request header tag. `scripts/build.sh` verifies the ELF with `grub-file --is-x86-multiboot2`, validates GRUB script syntax, creates the ISO, and checks that the embedded ELF/config match the source. Those checks do not prove that the runtime MBI contains the mmap tag required by `physical_memory_init()`, nor do they test MBI physical placement/accessibility. Add an explicit header information-request policy for required boot data and an artifact/runtime assertion for the actual mmap tag; do not assume `grub-file` validates runtime boot information.
3. The linker places the kernel at 1 MiB, retains `.multiboot` in `.text`, and defines a complete kernel range through `.bss`/`__kernel_end`. The build script's Multiboot2 check is a header-format check, not a check that every future adapter-owned range is reserved or mapped.
4. The checked-in GRUB config currently loads only the ELF kernel. There is no current GRUB module command, ACPI/ framebuffer consumption directive, or UEFI boot path in these files. This narrows present configuration but does not eliminate the need for defensive parsing of optional tags and external bootloader configurations.

**Native image path — confirmed current configuration**

1. The build script derives `KERNEL_SECTORS` from the linked flat ELF size, calculates `_start`'s actual linked offset, and passes both values to stage 2. It verifies the stage-2 image size, disk-image placement/readback, and kernel payload readback. These are build-artifact checks; they do not validate E820 overlap semantics or run the BIOS image.
2. Stage 2 checks that the entire sector-rounded kernel destination `[KERNEL_LOAD, KERNEL_LOAD + KERNEL_SECTORS*512)` is covered by one enabled E820 type-1 record. However, it does not check for any overlapping reserved/disabled/unknown descriptor. This independently confirms the reserved-wins load-preflight gap described in the preceding audit.
3. The stage-2 E820 collector retains raw records (up to 32) and the MBI builder emits them in returned order. No canonical sort/split/overlap normalization occurs before transfer to the kernel.
4. Stage 1 reads stage 2 through BIOS EDD at LBA 1 and stage 2 reads the kernel from a fixed computed LBA; the native path is a fixed disk layout, not a filesystem-based kernel discovery path. This is intentional current architecture and should not be conflated with the later storage-aware boot stage.

**Required follow-up**
- Decide and encode the exact Multiboot2 information-request tag set needed by the GRUB adapter (at minimum the memory map, plus only the optional tags that the adapter actually consumes). Validate header tag framing and required-tag policy independently of the `grub-file` check.
- Add a real GRUB/QEMU capture test that verifies the produced MBI's tag inventory, mmap stride/version, complete MBI range accessibility, and MBI placement relative to the early mapping. The test must fail if a required tag is absent or the MBI is inaccessible.
- Keep module ownership tests even though the checked-in GRUB menu currently has no module directive; add a module only in a controlled fixture and prove its pages are reserved before PMM publication.
- Keep native load preflight fail-closed when any reserved/disabled/unknown E820 descriptor overlaps the sector-rounded kernel or staging destination; test all source-order permutations.

**Status:** source inspection and documentation/test-plan update only. No GRUB header, config, native assembly, build script, executable test, ISO build, BIOS boot, or QEMU runtime was changed or run.


#### Multiboot2 information-request semantics and MBI placement — specification cross-check — 2026-11-21

Cross-checked the current header against the GNU GRUB Multiboot2 Specification v2.0 (sections 3.1.3–3.1.4 and 3.6.1–3.6.8).

**Specification facts that constrain the adapter**

1. Header information-request tag is type 1 with a u16 flags field, u32 size, and u32 requested-tag IDs. Its own tag framing is 8-byte aligned and included in `header_length`; the header checksum must include the complete updated length. The current header has no such request tag.
2. A non-optional request means the loader must understand/support the requested tag; it does **not** guarantee the information will be present when the information itself is unavailable. Therefore a required mmap request is not a substitute for validating that exactly one usable mmap tag actually arrived. If absent, the kernel's current PMM path cannot initialize and must fail closed before publishing allocator state.
3. The MBI fixed header is 8 bytes (`total_size`, reserved), begins 8-byte aligned, and the total size includes the terminating end tag. Each tag's `size` excludes alignment padding; next tag begins at the next 8-byte boundary. The MBI may be placed anywhere in memory other than memory reserved for the kernel and boot modules. The OS must preserve it until finished using it.
4. The memory-map tag contains `entry_size` and `entry_version`; v0 entries have a 24-byte fixed prefix. The specification permits future larger entries, while current v1 policy may deliberately require exact 24-byte entries at its adapter boundary. Legacy Multiboot2 parsing should explicitly document whether it accepts larger compatible strides; it must not silently accept a partial trailing record or unsupported version.
5. The GRUB information-request header should request only data actually required by the adapter. For the current GRUB/BIOS route, type 6 (memory map) is the mandatory candidate. Types 3 (modules), 8 (framebuffer), 14/15 (ACPI), and 17/18 (EFI map/boot-services state) are conditional and must not be treated as guaranteed merely because requested. Optional facilities should remain optional in the canonical v1 object with explicit absence sentinels.

**Confirmed interaction with current ProsenjitOS early mapping**

- `boot/entry.asm` currently identity-maps only the first 1 GiB. The MBI specification does not promise that the MBI itself lies below 1 GiB. Thus the current C parser's first `total_size` dereference can fault if GRUB places the MBI above the active early map. This is a source-level architectural risk; no observed runtime failure is claimed.
- The v1 contract must require either (a) the complete MBI physical interval to be inside a verified active early mapping before any dereference, or (b) a bounded temporary mapping mechanism that can safely map and validate the MBI. Merely checking `total_size` after dereference is too late.
- The loader must retain the MBI and any modules/other referenced objects until the kernel has copied/consumed them. PMM exclusion must include those ranges before allocator publication; current `physical_memory_init()` only explicitly reserves low memory, kernel image, and MBI, not module payloads.

**Test requirements added**
- Header parser: validate 8-byte alignment, tag sizes, end tag, information-request payload length/alignment, request flags, full header length, and checksum after tag insertion.
- GRUB capture: test MBI physically below and above 1 GiB (or prove loader placement is constrained by a supported mechanism); reject inaccessible MBI before reading its first field.
- Required mmap absent/duplicate/malformed: fail without changing PMM state; optional module/ACPI/framebuffer absent: produce exact v1 absence sentinels.
- MBI retention: ensure MBI, module payloads, kernel and other retained loader objects remain excluded from PMM until copied/released under an explicit lifecycle rule.

**Implementation decision remains open:** do not add an information-request tag or change early page tables in this documentation-only PR. The implementation PR must choose the request list and safe MBI access strategy together, then test both GRUB and native BIOS adapters against the same normalized v1 validation contract.


#### Early MBI reachability and bootstrap page-table dependency audit — 2026-11-21

Re-read `boot/entry.asm`, `kernel/kernel.c`, `kernel/memory.c`, `kernel/paging.c`, and `kernel/include/memory.h` together to establish the exact order of operations before selecting a safe MBI access design.

**Verified current execution order**

1. `boot/entry.asm` saves Multiboot EAX/EBX into 32-bit BSS slots, constructs a bootstrap PML4/PDPT/single page directory, maps physical 0–1 GiB with 512 2-MiB identity pages, enables long mode, and calls `kernel_main` with the original values in EDI/ESI.
2. `kernel_main(uint32_t boot_magic, uint32_t boot_info)` initializes serial/VGA, validates the magic, initializes IDT/TSS/timer, and only then calls `physical_memory_init(boot_info)`.
3. `physical_memory_init()` reads the first 32-bit MBI `total_size` field directly before validating whether the complete MBI range is mapped or otherwise accessible. `paging_init()` is called much later, after PMM initialization and PMM self-tests. Therefore the later 4-KiB page-table mapper cannot solve the first MBI dereference problem.
4. The legacy Multiboot2 i386 entry pointer is 32-bit. A valid pointer can therefore refer above 1 GiB but below 4 GiB, while the current bootstrap mapping covers only the first 1 GiB. The current code has no early MBI-specific mapping or verified placement restriction. Native BIOS stage 2 currently emits its MBI at `0x6000`, but that does not constrain GRUB placement.
5. `paging_init()` later builds a fresh kernel page table and maps only `PROSENJIT_PHYS_LIMIT` (1 GiB) as identity memory. This is the allocator's current managed limit, not proof that all bootloader handoff objects are below 1 GiB.

**Design consequence / safe dependency order**

- MBI accessibility must be solved in the 32-bit bootstrap assembly before entering C, or by a separate minimal early mapper that itself does not depend on PMM or on dereferencing the MBI. A post-PMM `paging_init()` change is too late.
- Candidate strategy A: extend bootstrap identity mapping to cover the full 32-bit physical pointer domain (0–4 GiB), using enough static page-directory pages, then validate the complete MBI interval before parsing. This is simple for reachability but increases low-level mapping and memory-table footprint; physical-address-width/PAE constraints and bootstrap table storage must be checked.
- Candidate strategy B: add a minimal, bounded early mapping window for the MBI interval. This requires safe page-table construction and validation of the 32-bit physical interval without reading MBI contents first; ensure no collision with kernel, stack, or bootstrap tables and handle MBI crossing page boundaries.
- Candidate strategy C: rely on a loader placement guarantee only if the selected loader interface explicitly provides a verifiable guarantee and the boot adapter rejects any pointer outside it before dereference. The generic Multiboot2 contract does not supply a first-1-GiB guarantee, so this cannot be assumed for GRUB.
- Regardless of strategy, validate pointer alignment, checked `base + total_size`, a conservative total-size cap, complete mapped interval, and tag framing before any variable-length access. A bounded copy into kernel-owned memory is preferable before releasing or reusing loader-owned storage, but copy destination allocation must itself be safe before PMM is initialized (static bounded scratch or bootstrap-reserved storage).
- Keep the PMM limit (1 GiB) separate from MBI reachability: MBI can be readable above the PMM limit while still being reserved and never admitted to the allocator.

**Test matrix added**
- MBI base at 1 GiB−page, exactly 1 GiB, 1 GiB+page, near 4 GiB, and ranges crossing the 1-GiB boundary; verify no first-field dereference occurs until the full range is reachable.
- Null, unaligned, wrapping, truncated, excessive-size, and end-tag-crossing MBI ranges; reject before parser state/PMM mutation.
- Bootstrap page-table allocation/storage bounds and collision tests for every early mapping strategy; verify original boot registers survive long-mode transition.
- Assert the early mapping is active before the first MBI load and that later `paging_init()` preserves only the intended mapping/lifetime contract.

**Status:** source-order audit and documentation only. No assembly/page tables/parser/PMM code changed; no compiler, boot, QEMU, or runtime tests executed.


#### Bootstrap 4-GiB identity-map feasibility and bounds — 2026-11-21

A concrete source-level sizing check was completed for the full-32-bit-pointer-domain bootstrap option:

- The Multiboot2 i386 handoff provides the MBI physical pointer in EBX (32-bit). The current entry saves it in a 32-bit BSS slot and later passes it through ESI; this preserves the complete protocol pointer width. A future v1 ABI with a 64-bit `self_phys` must use a distinct handoff path and must not silently reuse this legacy register contract.
- Current `boot/entry.asm` reserves five contiguous 4-KiB pages under `page_tables`: PML4, PDPT, one named page directory, and two additional pages. Current code clears all five pages but links only PDPT[0] to the first directory and fills that directory with 512 2-MiB PDEs, covering 1 GiB.
- Mapping the full 0–4-GiB 32-bit physical pointer domain with 2-MiB pages needs one PML4, one PDPT, and four page directories (six 4-KiB pages total). Thus the current five-page region is short by exactly one page if all existing pages are dedicated to this hierarchy. Four PDPT entries (0–3) would point to the four directories; each directory covers one 1-GiB quarter. This is a straightforward static bootstrap-table expansion candidate, not yet an implementation decision.
- The MBI start pointer is 32-bit, but the complete `[base, base + total_size)` interval must still be checked with overflow-safe arithmetic. A buffer whose start is below 4 GiB but whose end exceeds 4 GiB must be rejected before reading any bytes beyond the mapped domain. The existing 16-MiB size cap is a current parser policy, not a Multiboot2 protocol guarantee; v1 should define its own explicit bounded cap.
- The 2-MiB identity map makes addresses reachable, not valid RAM. The kernel must still validate the MBI interval against the loader's actual placement/ownership contract and must not touch arbitrary mapped MMIO/reserved addresses. Mapping 0–4 GiB also does not mean PMM should manage 0–4 GiB; PMM remains limited to its separately defined 1-GiB allocator window until deliberately expanded.
- Bootstrap tables are in the kernel BSS, so the kernel image reservation covers their storage once PMM initialization is reached. The tables must be initialized before CR3 load, remain intact through the early parser, and not be repurposed while the bootstrap mapping is active.

**Implementation preconditions / tests**
- Add the sixth page to the static bootstrap table region (or prove an alternative storage source), zero the entire region, and build PDPT[0..3] -> four distinct directories with exact 2-MiB identity entries.
- Verify boundaries at 1 GiB, 2 GiB, 3 GiB, and 4 GiB−1; verify no wraparound entry or accidental PDPT[4] is emitted.
- Verify MBI ranges wholly below 4 GiB are readable, and ranges whose checked end exceeds 4 GiB are rejected before crossing the limit. Include zero, unaligned, near-top, overflow, and maximum-accepted-size cases.
- Verify kernel code, bootstrap stack, GDT, IDT/TSS setup, and all bootstrap page-table pages remain reachable after CR3/long-mode transition; retain the native BIOS path whose MBI currently sits at `0x6000`.
- Capture both GRUB and native boot paths under QEMU, and test malformed MBI cases with no PMM state publication. No test is considered passed until it runs on the exact implementation commit.

**Status:** source-based sizing and design note only. No `boot/entry.asm`, paging, PMM, or parser code changed; no compiler, boot, QEMU, or runtime tests executed.


#### Legacy Multiboot2 parser atomicity and bounded staging audit — 2026-11-21

A focused read of `kernel/memory.c` confirms that the current legacy parser does not satisfy the v1 fail-before-PMM-mutation contract:

- `physical_memory_init()` clears the live `page_bitmap`, `managed_bitmap`, `user_bitmap`, and allocator counters before it validates the MBI pointer or structure. A malformed input therefore destroys any previous allocator state before returning an error.
- It then calls `mark_available()` for each type-1 mmap entry while parsing. If a later tag/entry is malformed, the function returns an error after earlier entries have already changed the live allocator bitmaps/counters. There is no rollback or temporary publication boundary. This is a confirmed source-level atomicity defect in the legacy path; no runtime reproduction was performed.
- The parser reads `total_size` from the physical pointer before checking `base + total_size` overflow or proving the complete range accessible. This reinforces the already documented early-MBI reachability defect.
- It accepts `entry_size >= 24`, does not validate `entry_version`, and does not reject a trailing partial mmap record. It breaks on an end tag but does not prove exactly one end tag exists at the exact end of `total_size`; duplicate mmap tags are processed incrementally. Tag bounds use `current + tag->size > end` rather than a subtraction-first bound check.
- It admits usable entries immediately and reserves only low memory, kernel image, and the MBI. It does not perform reserved-wins normalization against overlapping non-usable records, nor reserve module payloads/other retained loader objects.

**Required architecture for a safe v1 parser/PMM boundary**

1. Establish a readable bounded byte view of the complete handoff before parsing. Physical mapping/access checks belong to the boot adapter/early-access layer; the pure parser must not independently dereference an unchecked physical pointer.
2. Validate the fixed header, every in-allocation region, exact singleton/end-tag rules for legacy adapters, every record stride/version, every checked interval, all external objects, ownership coverage, and all pairwise alias rules into temporary state.
3. Normalize raw memory descriptors into a canonical sorted/disjoint reserved-wins representation before allocator admission. Never publish type-1 ranges while still parsing untrusted records.
4. Build a complete candidate PMM bitmap/counter state separately from the live allocator state, apply low-memory/kernel/handoff/module/retained-object reservations, and verify allocator viability. Publish by a single controlled commit only after every validation and construction step succeeds. On any error, live PMM state must remain unchanged/uninitialized.
5. Use a bounded, explicitly capped record count and fixed/static scratch storage (or another demonstrably safe pre-PMM reservation); do not put attacker-controlled arrays on the bootstrap stack. The current 16-MiB allocation cap alone permits far more records than a small fixed scratch array can hold. The v1 contract still needs an explicit maximum memory-map/module count or a bounded streaming normalization algorithm before implementation; reject over-cap input rather than truncate.
6. Keep the parser pure and host-testable: byte-buffer input + bounded output model + error code, with no allocator side effects, physical dereferences, logging dependencies, or global state. The PMM commit layer consumes only the fully validated canonical model.

**New regression requirements**
- Seed live PMM state, inject malformed input after one or more otherwise-valid usable entries, and assert every bitmap/counter remains byte-for-byte unchanged.
- Fail at each parse/normalization/reservation/candidate-build stage and verify no partial publication.
- Missing, duplicate, early, duplicate, non-final, and absent end tags; trailing bytes; duplicate mmap singleton; mmap entry version/stride/remainder errors; arithmetic-wrap attempts; malformed later tags after valid earlier mmap entries.
- Exceed every configured record/scratch capacity by one; reject deterministically without stack growth, truncation, or PMM mutation.
- Verify reserved-wins normalization independent of source order and full ownership reservations before candidate PMM commit.

**Status:** source audit and documentation only. No `kernel/memory.c`, parser, PMM bitmap, or boot adapter code changed; no compiler, host test, boot, QEMU, or runtime test executed. The explicit v1 maximum-record policy remains an open design decision and must be resolved before parser implementation.


#### Bounded record-count and reserved-wins normalization policy — 2026-11-22

The 16-MiB handoff byte cap is not, by itself, a useful working-set bound: a 24-byte memory-map record could otherwise be repeated hundreds of thousands of times. Define explicit count limits before implementing any parser or producer:

- **Raw source memory-map descriptors:** at most 1,024 per adapter input (Multiboot2/firmware/native E820). Reject a source with more records; never truncate. Native BIOS E820 currently has a stricter 32-record producer buffer, which remains valid under this common upper bound.
- **Canonical v1 memory-map records:** at most 2,047. With at most 1,024 raw intervals, their unique start/end boundaries produce at most 2,047 non-empty elementary intervals; merging adjacent equal-type intervals can only reduce this count. A canonical v1 parser validates this bound and does not normalize an already canonical map.
- **Module records:** at most 256. Validate all module ranges and pairwise aliases within this bound; reject the 257th record. Current checked-in GRUB/native boot configs load no modules, so this is a forward-compatible ceiling rather than a claim about current runtime use.

**Bounded producer normalization algorithm**

1. Parse and validate each raw descriptor's checked half-open interval and source classification into a bounded static raw-record array (maximum 1,024). Reject zero length, wrap, unsupported malformed fields, and over-cap count.
2. Insert each start/end into a static boundary array (maximum 2,048 u64 values), sort and deduplicate it. No input-sized VLA or heap allocation is permitted.
3. For each adjacent unique boundary pair, scan the bounded raw descriptors: if any non-usable/disabled/unknown descriptor covers the whole elementary interval, classify it reserved; otherwise classify usable only if at least one usable descriptor covers it; if no descriptor covers it, omit the gap. This makes reserved-wins independent of source order.
4. Emit sorted intervals to a static canonical output buffer (maximum 2,047 records), coalescing adjacent intervals with identical final type. Reject output-capacity exhaustion rather than truncate. Verify final sorted/disjoint/nonzero/non-wrapping invariants before serialization.
5. Complexity is bounded by sorting at most 2,048 boundaries plus at most 2,047 × 1,024 descriptor checks (about 2.1 million coverage checks), a finite boot-time upper bound. Scratch sizing for this reference implementation: raw descriptors 24 KiB (1,024 × 24), boundaries 16 KiB (2,048 × 8), canonical output 49,128 bytes (2,047 × 24), total 89,704 bytes before small counters/alignment. Storage is static/reserved, never stack or PMM allocated before validation.

**Kernel v1 parser and legacy adapter distinction**

- The v1 kernel parser receives a bounded byte buffer and checks `mmap_count <= 2,047`, `modules_count <= 256`, exact entry sizes, and all arithmetic/ownership rules. It validates canonical ordering and disjointness; it does not sort, normalize, or repair producer mistakes.
- A legacy Multiboot2 adapter validates tag framing and `entry_size >= 24` according to the source protocol, requires the mmap payload length to be an exact multiple of its entry stride, caps decoded records at 1,024, and normalizes raw records into the v1 canonical form. A nonzero unsupported source `entry_version` or unsupported stride policy is rejected, not guessed.
- The native E820 producer may retain its current 32-entry firmware buffer, but it must use the same reserved-wins classifier for load/bounce ranges and output normalization. Its lower cap is allowed; it must never claim an incomplete E820 map is complete.
- Module overlap checks are pairwise under the 256 cap. Ownership coverage is checked independently; a module's presence does not permit it to overlap the handoff, kernel, ACPI RSDP, another module, or framebuffer span.

**Acceptance tests**
- Exactly 1,024 raw descriptors accepted; 1,025 rejected before output publication. Exactly 2,047 canonical records accepted; 2,048 rejected. Exactly 256 modules accepted; 257 rejected.
- Worst-case alternating descriptor boundaries yield no more than 2,047 elementary intervals; verify output merging and deterministic output for every permutation of the same descriptor set.
- Reserved/disabled/unknown overlap wins regardless of source order; gaps remain absent; adjacent equal types coalesce; adjacent different types remain separate.
- Scratch buffers have compile-time size assertions, are outside the bootstrap stack, are covered by kernel-image reservation, and are never exposed to PMM as free RAM.
- Every capacity failure leaves the destination handoff unpublished and PMM unchanged. No truncation or partial output is accepted.

**Status:** explicit bounded policy and reference algorithm documented for review. Limits are a deliberate v1 ABI constraint and should receive independent review before implementation. No parser/producer code or runtime tests changed/executed.


#### Scratch-memory arithmetic and linker reservation cross-check — 2026-11-22

A source-level cross-check against `kernel/memory.c`, `kernel/linker.ld`, and `scripts/build.sh` adds these constraints and corrects the previous scratch sum:

- `BITMAP_WORDS` is 4,096 and each of the three current PMM bitmaps is 32,768 bytes, so the existing `page_bitmap`, `managed_bitmap`, and `user_bitmap` occupy **98,304 bytes (96 KiB)** in static storage. The counters add only 24 bytes.
- The proposed normalization arrays total **90,088 bytes**, not 89,704: raw descriptors 1,024 × 24 = 24,576; boundary array 2,048 × 8 = 16,384; canonical output 2,047 × 24 = 49,128. The prior total omitted 384 bytes; this is corrected here.
- The known subtotal for these PMM bitmaps plus the proposed three normalization arrays is **188,392 bytes (about 183.98 KiB)**, excluding all other kernel `.bss`, alignment, parser state, stacks, and any optional module-copy buffer. The 256 module records can be validated in their already bounded handoff region without copying them into a second 6,144-byte array; if an implementation chooses to copy them, add that amount explicitly to its BSS budget.
- `kernel/linker.ld` places `*(.bss*)` between `__kernel_bss_start` and `__kernel_bss_end`, then defines `__kernel_end` after page alignment. The current PMM reserves `[__kernel_start, __kernel_end)`, so static normalization arrays linked into kernel `.bss` are covered by the existing kernel-image reservation. They must still be initialized before use and must not be separately admitted as usable RAM.
- `scripts/build.sh` links the kernel without emitting a linker map (`-Map`) or a post-link BSS-size report. The repository source therefore does **not** establish current total `.bss` size or remaining physical-memory headroom. Do not claim the additional ~88 KiB is available until a build artifact reports `__kernel_bss_start`, `__kernel_bss_end`, `__kernel_end`, and total load/reservation size. The build's `nm` use for `_start` is not a BSS-size measurement.
- The existing 96-KiB bitmap footprint is static and already part of the kernel image reservation; the proposed ~88-KiB normalization scratch is additional. The current build does not prove whether this increase is acceptable for the target boot-memory envelope. Require a linker-map/symbol-size budget and a low-memory QEMU case before selecting static capacities as final.

**Status:** arithmetic and source/linker reservation relationship verified by inspection; exact total BSS/headroom remains unmeasured because no linker map/build artifact is available in this docs-only audit. No build or runtime test executed.
