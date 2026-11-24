#ifndef PROSENJIT_LOG_PERSIST_H
#define PROSENJIT_LOG_PERSIST_H

#include <stdint.h>
#include "health.h"
#include "vfs.h"

#define PROS_LOG_PERSIST_OK       0
#define PROS_LOG_PERSIST_EINVAL  -1
#define PROS_LOG_PERSIST_EIO     -2
#define PROS_LOG_PERSIST_ENOTSUP -3
#define PROS_LOG_PERSIST_MAX_BYTES 65536u
#define PROS_LOG_PERSIST_ROTATE_BYTES 49152u
#define PROS_LOG_PERSIST_PATH_MAX 96u

struct pros_log_persist_policy {
    uint32_t max_bytes;
    uint32_t rotate_bytes;
    uint32_t max_files;
};

int pros_log_persist_init(void);
int pros_log_persist_attach(struct pros_vfs_mount *mount,
                            const char *path,
                            const struct pros_log_persist_policy *policy);
int pros_log_persist_emit(const struct pros_health_event *event);
int pros_log_persist_flush(void);
int pros_log_persist_available(void);
int pros_log_persist_detach(void);
int pros_log_persist_selftest(void);

#endif
