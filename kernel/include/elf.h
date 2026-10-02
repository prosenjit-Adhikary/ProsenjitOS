#ifndef PROSENJIT_ELF_H
#define PROSENJIT_ELF_H

#include <stdint.h>
#include "address_space.h"

#define PROS_ELF_OK          0
#define PROS_ELF_EINVAL     -1
#define PROS_ELF_ENOTSUP    -2
#define PROS_ELF_ENOMEM     -3
#define PROS_ELF_EBOUNDS    -4

int pros_elf64_load(const void *image, uint64_t image_size,
                    struct pros_address_space *space,
                    uint64_t *entry_out);
int pros_elf64_selftest(void);

#endif
