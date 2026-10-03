#ifndef PROSENJIT_ELF_H
#define PROSENJIT_ELF_H

#include <stdint.h>
#include "address_space.h"

#define PROS_ELF_OK 0
#define PROS_ELF_EINVAL -1
#define PROS_ELF_EFORMAT -2
#define PROS_ELF_EOVERFLOW -3
#define PROS_ELF_ENOMEM -4
#define PROS_ELF_EUNSUPPORTED -5

#define PROS_ELF_PT_LOAD 1u
#define PROS_ELF_PF_X 1u
#define PROS_ELF_PF_W 2u
#define PROS_ELF_PF_R 4u
#define PROS_ELF_ET_EXEC 2u
#define PROS_ELF_ET_DYN 3u
#define PROS_ELF_EM_X86_64 62u

struct pros_elf_load_result {
    uint64_t entry;
    uint64_t image_start;
    uint64_t image_end;
    uint32_t loaded_segments;
};

int pros_elf64_load(struct pros_address_space *space,
                    const void *image, uint64_t image_size,
                    struct pros_elf_load_result *result);
int pros_elf64_validate(const void *image, uint64_t image_size,
                        uint64_t *entry_out);
int pros_elf64_selftest(void);

#endif
