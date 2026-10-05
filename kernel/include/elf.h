#ifndef PROSENJIT_ELF_H
#define PROSENJIT_ELF_H

#include <stdint.h>

#define PROS_ELF_OK       0
#define PROS_ELF_EINVAL  -1
#define PROS_ELF_EFORMAT -2
#define PROS_ELF_ERANGE  -3

#define PROS_ELF_CLASS64 2u
#define PROS_ELF_DATA_LSB 1u
#define PROS_ELF_VERSION 1u
#define PROS_ELF_ET_EXEC 2u
#define PROS_ELF_ET_DYN 3u
#define PROS_ELF_MACHINE_X86_64 62u
#define PROS_ELF_PT_LOAD 1u
#define PROS_ELF_PF_X 1u
#define PROS_ELF_PF_W 2u
#define PROS_ELF_PF_R 4u

struct pros_elf_header64 {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct pros_elf_phdr64 {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

int pros_elf_validate_image(const void *image, uint64_t size,
                            struct pros_elf_header64 *header_out);
int pros_elf_validate_load_segments(const void *image, uint64_t size,
                                    const struct pros_elf_header64 *header);
int pros_elf_selftest(void);

#endif
