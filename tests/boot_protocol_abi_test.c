#include "pros_boot_protocol_v1.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        (void)fprintf(stderr, "FAIL: %s (line %d)\n", #expr, __LINE__); \
        return 1; \
    } \
} while (0)

#define CHECK_OFFSET(type, member, expected) \
    CHECK(offsetof(type, member) == (expected))

int main(void)
{
    CHECK(PROS_BOOT_V1_MAGIC == UINT32_C(0x31534F50));
    CHECK(PROS_BOOT_V1_VERSION == 1);
    CHECK(PROS_BOOT_V1_HEADER_SIZE == 128);
    CHECK(sizeof(struct pros_boot_v1_header) == 128);

    CHECK_OFFSET(struct pros_boot_v1_header, magic, 0);
    CHECK_OFFSET(struct pros_boot_v1_header, version, 4);
    CHECK_OFFSET(struct pros_boot_v1_header, header_size, 6);
    CHECK_OFFSET(struct pros_boot_v1_header, total_size, 8);
    CHECK_OFFSET(struct pros_boot_v1_header, flags, 12);
    CHECK_OFFSET(struct pros_boot_v1_header, self_phys, 16);
    CHECK_OFFSET(struct pros_boot_v1_header, mmap_offset, 24);
    CHECK_OFFSET(struct pros_boot_v1_header, mmap_count, 28);
    CHECK_OFFSET(struct pros_boot_v1_header, mmap_entry_size, 32);
    CHECK_OFFSET(struct pros_boot_v1_header, modules_offset, 36);
    CHECK_OFFSET(struct pros_boot_v1_header, modules_count, 40);
    CHECK_OFFSET(struct pros_boot_v1_header, module_entry_size, 44);
    CHECK_OFFSET(struct pros_boot_v1_header, framebuffer_phys, 48);
    CHECK_OFFSET(struct pros_boot_v1_header, framebuffer_size, 56);
    CHECK_OFFSET(struct pros_boot_v1_header, framebuffer_width, 64);
    CHECK_OFFSET(struct pros_boot_v1_header, framebuffer_height, 68);
    CHECK_OFFSET(struct pros_boot_v1_header, framebuffer_pitch, 72);
    CHECK_OFFSET(struct pros_boot_v1_header, framebuffer_format, 76);
    CHECK_OFFSET(struct pros_boot_v1_header, acpi_rsdp_phys, 80);
    CHECK_OFFSET(struct pros_boot_v1_header, acpi_rsdp_size, 88);
    CHECK_OFFSET(struct pros_boot_v1_header, boot_device_offset, 92);
    CHECK_OFFSET(struct pros_boot_v1_header, boot_device_size, 96);
    CHECK_OFFSET(struct pros_boot_v1_header, boot_device_kind, 100);
    CHECK_OFFSET(struct pros_boot_v1_header, reserved, 104);

    CHECK(sizeof(struct pros_boot_v1_mmap_entry) == 24);
    CHECK_OFFSET(struct pros_boot_v1_mmap_entry, base, 0);
    CHECK_OFFSET(struct pros_boot_v1_mmap_entry, length, 8);
    CHECK_OFFSET(struct pros_boot_v1_mmap_entry, type, 16);
    CHECK_OFFSET(struct pros_boot_v1_mmap_entry, attributes, 20);

    CHECK(sizeof(struct pros_boot_v1_module_entry) == 24);
    CHECK_OFFSET(struct pros_boot_v1_module_entry, start, 0);
    CHECK_OFFSET(struct pros_boot_v1_module_entry, length, 8);
    CHECK_OFFSET(struct pros_boot_v1_module_entry, type, 16);
    CHECK_OFFSET(struct pros_boot_v1_module_entry, flags, 20);

    (void)puts("ProsenjitOS: Boot Protocol v1 ABI layout tests passed");
    return 0;
}
