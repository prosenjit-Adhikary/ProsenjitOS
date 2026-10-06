#ifndef PROSENJIT_ELF_LOADER_H
#define PROSENJIT_ELF_LOADER_H

#include <stdint.h>
#include "process.h"

#define PROS_ELF_OK 0
#define PROS_ELF_EINVAL -1
#define PROS_ELF_EFORMAT -2
#define PROS_ELF_EBOUNDS -3
#define PROS_ELF_ENOMEM -4
#define PROS_ELF_EPERM -5
#define PROS_ELF_EOVERFLOW -6
#define PROS_ELF_ECONFLICT -7

struct pros_elf_load_result {
    uint64_t entry;
    uint64_t image_start;
    uint64_t image_end;
    uint64_t stack_top;
    uint64_t mapped_pages;
};

int pros_elf_validate(const uint8_t *image, uint64_t size);
int pros_elf_selftest(void);
int pros_elf_load(struct pros_process *process,
                  const uint8_t *image,
                  uint64_t size,
                  struct pros_elf_load_result *result);

#endif
