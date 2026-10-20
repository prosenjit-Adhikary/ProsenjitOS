#ifndef PROSENJIT_INIT_H
#define PROSENJIT_INIT_H

#include <stdint.h>
#include "elf_loader.h"
#include "security_admission.h"
#include "vfs.h"
#include "process.h"
#include "thread.h"

#define PROS_INIT_OK 0
#define PROS_INIT_EINVAL -1
#define PROS_INIT_ENOENT -2
#define PROS_INIT_EIO -3
#define PROS_INIT_ESECURITY -4
#define PROS_INIT_EFORMAT -5
#define PROS_INIT_ENOMEM -6
#define PROS_INIT_EOVERFLOW -7
#define PROS_INIT_ESTART -8

#define PROS_INIT_IMAGE_MAX (16ULL * 1024ULL * 1024ULL)

int pros_init_load_path(struct pros_vfs_mount *mount,
                        const char *path,
                        struct pros_process *process,
                        const struct pros_file_security_metadata *metadata,
                        struct pros_elf_load_result *result);
int pros_init_load(struct pros_vfs_mount *mount,
                   struct pros_process *process,
                   const struct pros_file_security_metadata *metadata,
                   struct pros_elf_load_result *result);
/*
 * Load and queue a child process from the executable at the supplied path.
 * The caller owns the process/thread objects and the supplied kernel stack.
 * A successful return means the validated CPL3 thread is queued, not that it
 * has already run. On failure, the child is reaped when rollback succeeds.
 */
int pros_init_launch(struct pros_vfs_mount *mount,
                     const char *path,
                     struct pros_process *parent,
                     struct pros_process *process,
                     struct pros_thread *thread,
                     uint64_t pid,
                     uint64_t tid,
                     uint32_t security_level,
                     const struct pros_file_security_metadata *metadata,
                     uint64_t kernel_stack_bottom,
                     uint64_t kernel_stack_top);
int pros_init_selftest(void);

#endif
