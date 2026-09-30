#ifndef PROSENJIT_LOG_H
#define PROSENJIT_LOG_H

#include <stdint.h>
#include "health.h"
#include "vfs.h"

#define PROS_LOG_OK       PROS_HEALTH_OK
#define PROS_LOG_EINVAL   PROS_HEALTH_EINVAL
#define PROS_LOG_EIO      PROS_HEALTH_EIO

enum pros_log_sink {
    PROS_LOG_SINK_HEALTH = 1u
};

int pros_log_init(void);
int pros_log_emit(uint32_t severity, uint32_t category,
                  uint32_t source, uint32_t code, uint64_t value);
int pros_log_sink_available(uint32_t sink);
int pros_log_attach_persistent(struct pros_vfs_mount *mount, const char *path);
int pros_log_detach_persistent(void);
uint32_t pros_log_persistence_failures(void);
int pros_log_selftest(void);

#endif
