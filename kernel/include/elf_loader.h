#ifndef PROSENJIT_ELF_LOADER_H
#define PROSENJIT_ELF_LOADER_H
#include <stdint.h>
#define PROS_ELF_OK 0
#define PROS_ELF_EINVAL -1
#define PROS_ELF_ENOTSUP -2
#define PROS_ELF_EBOUNDS -3
int pros_elf64_validate(const uint8_t *, uint64_t, uint64_t *);
int pros_elf_selftest(void);
#endif
