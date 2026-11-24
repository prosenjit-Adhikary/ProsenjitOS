#ifndef PROSENJIT_INIT_H
#define PROSENJIT_INIT_H

#include <stdint.h>
#include "elf_loader.h"
#include "security_admission.h"
#include "vfs.h"

#define PROS_INIT_OK 0
#define PROS_INIT_EINVAL -1
#define PROS_INIT_ENOENT -2
#define PROS_INIT_EIO -3
#define PROS_INIT_ESECURITY -4
#define PROS_INIT_EFORMAT -5
#define PROS_INIT_ENOMEM -6
#define PROS_INIT_EOVERFLOW -7

#define PROS_INIT_IMAGE_MAX (16ULL * 1024ULL * 1024ULL)

int pros_init_load(struct pros_vfs_mount *mount,
                   struct pros_process *process,
                   const struct pros_file_security_metadata *metadata,
                   struct pros_elf_load_result *result);
int pros_init_selftest(void);

#endif
