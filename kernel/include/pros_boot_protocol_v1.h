#ifndef PROSENJITOS_BOOT_PROTOCOL_V1_H
#define PROSENJITOS_BOOT_PROTOCOL_V1_H

/*
 * ProsenjitOS Boot Protocol v1 — wire ABI only.
 *
 * This header describes the serialized little-endian x86-64 handoff. It does
 * not imply that any current loader emits v1 or that the kernel consumes it.
 * All multi-byte fields are little-endian; structures are packed to preserve
 * the specified byte offsets. Producers must zero all reserved fields.
 */
#include <stddef.h>
#include <stdint.h>

#define PROS_BOOT_V1_MAGIC             UINT32_C(0x31534F50)
#define PROS_BOOT_V1_VERSION           UINT16_C(1)
#define PROS_BOOT_V1_HEADER_SIZE       UINT16_C(128)
#define PROS_BOOT_V1_MMAP_ENTRY_SIZE   UINT32_C(24)
#define PROS_BOOT_V1_MODULE_ENTRY_SIZE UINT32_C(24)
#define PROS_BOOT_V1_MAX_TOTAL_SIZE    UINT32_C(0x01000000)
#define PROS_BOOT_V1_EARLY_PHYS_LIMIT  UINT64_C(0x40000000)

#define PROS_BOOT_V1_MODULE_INITRAMFS  UINT32_C(1)
#define PROS_BOOT_V1_MODULE_DRIVER_BUNDLE UINT32_C(2)
#define PROS_BOOT_V1_MODULE_OPTIONAL   UINT32_C(1)

#define PROS_BOOT_V1_MEMORY_USABLE     UINT32_C(1)

#define PROS_BOOT_V1_DEVICE_NONE       UINT32_C(0)
#define PROS_BOOT_V1_DEVICE_OPAQUE_V1  UINT32_C(1)

#define PROS_BOOT_V1_FRAMEBUFFER_RGBX8888 UINT32_C(1)
#define PROS_BOOT_V1_FRAMEBUFFER_BGRX8888 UINT32_C(2)

#if defined(__GNUC__) || defined(__clang__)
#define PROS_BOOT_PACKED __attribute__((packed))
#else
#define PROS_BOOT_PACKED
#error "ProsenjitOS Boot Protocol v1 requires a compiler with packed-structure support"
#endif

struct PROS_BOOT_PACKED pros_boot_v1_header {
    uint32_t magic;                 /* 0 */
    uint16_t version;               /* 4 */
    uint16_t header_size;            /* 6 */
    uint32_t total_size;             /* 8 */
    uint32_t flags;                  /* 12; v1 requires zero */
    uint64_t self_phys;              /* 16 */
    uint32_t mmap_offset;            /* 24; mandatory, nonzero */
    uint32_t mmap_count;             /* 28; mandatory, nonzero */
    uint32_t mmap_entry_size;        /* 32; exactly 24 */
    uint32_t modules_offset;         /* 36; zero iff count is zero */
    uint32_t modules_count;          /* 40 */
    uint32_t module_entry_size;      /* 44; zero iff count is zero, else 24 */
    uint64_t framebuffer_phys;       /* 48 */
    uint64_t framebuffer_size;      /* 56 */
    uint32_t framebuffer_width;      /* 64 */
    uint32_t framebuffer_height;     /* 68 */
    uint32_t framebuffer_pitch;      /* 72 */
    uint32_t framebuffer_format;     /* 76 */
    uint64_t acpi_rsdp_phys;         /* 80 */
    uint32_t acpi_rsdp_size;         /* 88 */
    uint32_t boot_device_offset;     /* 92 */
    uint32_t boot_device_size;       /* 96 */
    uint32_t boot_device_kind;       /* 100 */
    uint8_t reserved[24];            /* 104; all zero */
};

struct PROS_BOOT_PACKED pros_boot_v1_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;             /* v1 requires zero */
};

struct PROS_BOOT_PACKED pros_boot_v1_module_entry {
    uint64_t start;
    uint64_t length;
    uint32_t type;
    uint32_t flags;                  /* bit 0 optional; other bits zero */
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct pros_boot_v1_header) == 128,
               "Boot Protocol v1 header must be exactly 128 bytes");
_Static_assert(offsetof(struct pros_boot_v1_header, self_phys) == 16,
               "Boot Protocol v1 self_phys offset mismatch");
_Static_assert(offsetof(struct pros_boot_v1_header, mmap_offset) == 24,
               "Boot Protocol v1 mmap_offset offset mismatch");
_Static_assert(offsetof(struct pros_boot_v1_header, framebuffer_phys) == 48,
               "Boot Protocol v1 framebuffer offset mismatch");
_Static_assert(offsetof(struct pros_boot_v1_header, acpi_rsdp_phys) == 80,
               "Boot Protocol v1 ACPI offset mismatch");
_Static_assert(offsetof(struct pros_boot_v1_header, reserved) == 104,
               "Boot Protocol v1 reserved offset mismatch");
_Static_assert(sizeof(struct pros_boot_v1_mmap_entry) == 24,
               "Boot Protocol v1 memory-map entry must be 24 bytes");
_Static_assert(sizeof(struct pros_boot_v1_module_entry) == 24,
               "Boot Protocol v1 module entry must be 24 bytes");
#endif

#undef PROS_BOOT_PACKED
#endif
