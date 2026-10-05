#ifndef PROSENJIT_ELF_LOADER_H
#define PROSENJIT_ELF_LOADER_H

#include <stdint.h>
#include "process.h"

#define PROS_ELF_LOAD_OK       0
#define PROS_ELF_LOAD_EINVAL  -1
#define PROS_ELF_LOAD_EFORMAT -2
#define PROS_ELF_LOAD_ENOMEM  -3
#define PROS_ELF_LOAD_EOVERLAP -4

/*
 * Loads an already-admitted ELF64 image into the supplied process address
 * space. The image remains caller-owned. No filesystem or policy decision is
 * made here; security admission belongs above this loader.
 */
int pros_elf_load_process(struct pros_process *process,
                           const void *image, uint64_t size,
                           uint64_t *entry_out,
                           uint64_t *stack_top_out);

int pros_elf_loader_selftest(void);

#endif
