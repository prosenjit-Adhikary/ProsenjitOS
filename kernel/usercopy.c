#include "usercopy.h"
#include "address_space.h"

extern int copy_from_user_asm(void *destination, const void *source, uint64_t size);
extern int copy_to_user_asm(void *destination, const void *source, uint64_t size);

int copy_from_user(const struct pros_address_space *space,
                   void *kernel_destination,
                   uint64_t user_source,
                   uint64_t size) {
    if (!space || !kernel_destination || size == 0)
        return -1;
    if (address_space_validate_user_range(space, user_source, size, 0) != 0)
        return -1;
    return copy_from_user_asm(kernel_destination,
                               (const void *)(uintptr_t)user_source, size);
}

int copy_to_user(const struct pros_address_space *space,
                 uint64_t user_destination,
                 const void *kernel_source,
                 uint64_t size) {
    if (!space || !kernel_source || size == 0)
        return -1;
    if (address_space_validate_user_range(space, user_destination, size, 1) != 0)
        return -1;
    return copy_to_user_asm((void *)(uintptr_t)user_destination,
                            kernel_source, size);
}
