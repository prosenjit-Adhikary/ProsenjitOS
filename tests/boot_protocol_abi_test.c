#include "pros_boot_protocol_v1.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        (void)fprintf(stderr, "FAIL: %s (line %d)\\n", #expr, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void)
{
    CHECK(PROS_BOOT_V1_MAGIC == UINT32_C(0x31534F50));
    CHECK(PROS_BOOT_V1_VERSION == 1);
    CHECK(sizeof(struct pros_boot_v1_header) == 128);
    CHECK(offsetof(struct pros_boot_v1_header, total_size) == 8);
    CHECK(offsetof(struct pros_boot_v1_header, modules_offset) == 36);
    CHECK(offsetof(struct pros_boot_v1_header, framebuffer_width) == 64);
    CHECK(offsetof(struct pros_boot_v1_header, boot_device_kind) == 100);
    CHECK(offsetof(struct pros_boot_v1_header, reserved) == 104);
    CHECK(sizeof(struct pros_boot_v1_mmap_entry) == 24);
    CHECK(sizeof(struct pros_boot_v1_module_entry) == 24);
    (void)puts("ProsenjitOS: Boot Protocol v1 ABI layout tests passed");
    return 0;
}
