#include "usercopy.h"
#include "address_space.h"

static void byte_copy_from_user(void *destination,
                                const uint8_t *source,
                                uint64_t size) {
    uint8_t *dst = (uint8_t *)destination;
    for (uint64_t i = 0; i < size; ++i)
        dst[i] = source[i];
}

static void byte_copy_to_user(uint8_t *destination,
                              const void *source,
                              uint64_t size) {
    const uint8_t *src = (const uint8_t *)source;
    for (uint64_t i = 0; i < size; ++i)
        destination[i] = src[i];
}

int copy_from_user(const struct pros_address_space *space,
                   void *kernel_destination,
                   uint64_t user_source,
                   uint64_t size) {
    if (!space || !kernel_destination || size == 0)
        return -1;
    if (address_space_validate_user_range(space, user_source, size, 0) != 0)
        return -1;

    byte_copy_from_user(kernel_destination,
                        (const uint8_t *)(uintptr_t)user_source, size);
    return 0;
}

int copy_to_user(const struct pros_address_space *space,
                 uint64_t user_destination,
                 const void *kernel_source,
                 uint64_t size) {
    if (!space || !kernel_source || size == 0)
        return -1;
    if (address_space_validate_user_range(space, user_destination, size, 1) != 0)
        return -1;

    byte_copy_to_user((uint8_t *)(uintptr_t)user_destination,
                      kernel_source, size);
    return 0;
}
