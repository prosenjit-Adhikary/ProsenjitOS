#ifndef PROSENJIT_ABI_BOOT_H
#define PROSENJIT_ABI_BOOT_H

#include <stddef.h>
#include <stdint.h>

#define PROSENJIT_BOOT_V1_MAGIC       0x31534F50u
#define PROSENJIT_BOOT_V1_VERSION     1u
#define PROSENJIT_BOOT_V1_HEADER_SIZE 128u
#define PROSENJIT_BOOT_V1_MAX_SIZE    (16u * 1024u * 1024u)
/* Parser work-budget limits; reject attacker-controlled counts before loops. */
#define PROSENJIT_BOOT_V1_MAX_MMAP_ENTRIES 4096u
#define PROSENJIT_BOOT_V1_MAX_MODULES       256u

#define PROSENJIT_BOOT_MEMORY_USABLE 1u
#define PROSENJIT_BOOT_MODULE_INITRAMFS 1u
#define PROSENJIT_BOOT_MODULE_DRIVER_BUNDLE 2u
#define PROSENJIT_BOOT_MODULE_OPTIONAL 1u

#define PROSENJIT_BOOT_FRAMEBUFFER_NONE 0u
#define PROSENJIT_BOOT_FRAMEBUFFER_RGBX8888 1u
#define PROSENJIT_BOOT_FRAMEBUFFER_BGRX8888 2u

#define PROSENJIT_BOOT_DEVICE_NONE 0u
#define PROSENJIT_BOOT_DEVICE_OPAQUE_V1 1u

#if defined(__GNUC__) || defined(__clang__)
#define PROSENJIT_BOOT_PACKED __attribute__((packed))
#else
#error "ProsenjitOS Boot ABI requires a compiler with packed-structure support"
#endif

/* Wire structures: fixed-width, little-endian, and independent of native pointers. */
struct pros_boot_v1_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    uint32_t flags;
    uint64_t self_phys;
    uint32_t mmap_offset;
    uint32_t mmap_count;
    uint32_t mmap_entry_size;
    uint32_t modules_offset;
    uint32_t modules_count;
    uint32_t module_entry_size;
    uint64_t framebuffer_phys;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_format;
    uint64_t acpi_rsdp_phys;
    uint32_t acpi_rsdp_size;
    uint32_t boot_device_offset;
    uint32_t boot_device_size;
    uint32_t boot_device_kind;
    uint8_t reserved[24];
} PROSENJIT_BOOT_PACKED;

struct pros_boot_v1_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
} PROSENJIT_BOOT_PACKED;

struct pros_boot_v1_module_entry {
    uint64_t start;
    uint64_t length;
    uint32_t type;
    uint32_t flags;
} PROSENJIT_BOOT_PACKED;

/* Parsed views borrow bytes from the caller; they never own or copy the block. */
struct pros_boot_v1_view {
    const struct pros_boot_v1_header *header;
    const struct pros_boot_v1_mmap_entry *mmap;
    const struct pros_boot_v1_module_entry *modules;
    const uint8_t *boot_device;
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct pros_boot_v1_header) == 128, "boot v1 header size");
_Static_assert(offsetof(struct pros_boot_v1_header, self_phys) == 16, "boot v1 self_phys offset");
_Static_assert(offsetof(struct pros_boot_v1_header, mmap_offset) == 24, "boot v1 mmap offset");
_Static_assert(offsetof(struct pros_boot_v1_header, framebuffer_phys) == 48, "boot v1 framebuffer offset");
_Static_assert(offsetof(struct pros_boot_v1_header, acpi_rsdp_phys) == 80, "boot v1 ACPI offset");
_Static_assert(offsetof(struct pros_boot_v1_header, reserved) == 104, "boot v1 reserved offset");
_Static_assert(sizeof(struct pros_boot_v1_mmap_entry) == 24, "boot v1 mmap entry size");
_Static_assert(sizeof(struct pros_boot_v1_module_entry) == 24, "boot v1 module entry size");
#endif

/* Returns zero only after the complete in-allocation structure is validated.
 * The compatibility entry point is restricted to the legacy 1-GiB window. */
int pros_boot_v1_parse(const void *allocation, size_t available_bytes,
                       uint64_t expected_phys, struct pros_boot_v1_view *out);
/* MAXPHYADDR-aware parser. physical_limit is the exclusive CPU-derived limit;
 * the handoff allocation itself still must fit the early identity-map window. */
int pros_boot_v1_parse_with_limit(const void *allocation, size_t available_bytes,
                                  uint64_t expected_phys, uint64_t physical_limit,
                                  struct pros_boot_v1_view *out);

/* Decode the boot CPU's effective MAXPHYADDR from captured CPUID values.
 * If leaf 0x80000008 is unavailable, require PAE and use the 36-bit
 * architectural fallback. Outputs are written only on success. */
int pros_boot_v1_decode_maxphyaddr(uint32_t max_extended_leaf,
                                   uint32_t extended_leaf_8_eax,
                                   uint32_t leaf_1_edx,
                                   uint8_t *physical_bits,
                                   uint64_t *physical_limit);

/* Validate a copied ACPI RSDP object before consuming any of its fields.
 * available_bytes must exactly match the v1 handoff's acpi_rsdp_size. */
int pros_boot_v1_validate_rsdp(const void *rsdp, size_t available_bytes);

#endif