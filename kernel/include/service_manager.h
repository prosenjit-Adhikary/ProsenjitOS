#ifndef PROSENJIT_SERVICE_MANAGER_H
#define PROSENJIT_SERVICE_MANAGER_H

#include <stdint.h>

#define PROS_SERVICE_OK        0
#define PROS_SERVICE_EINVAL   -1
#define PROS_SERVICE_ENOSPC   -2
#define PROS_SERVICE_ENOENT   -3
#define PROS_SERVICE_EBUSY    -4
#define PROS_SERVICE_ELOOP   -5
#define PROS_SERVICE_EFAILED -6
#define PROS_SERVICE_ERETRY  -7

#define PROS_SERVICE_MAX        32u
#define PROS_SERVICE_MAX_DEPS    8u
#define PROS_SERVICE_MAX_RETRIES 3u
/* Includes the trailing NUL: service identifiers are at most 31 bytes. */
#define PROS_SERVICE_NAME_MAX    32u

enum pros_service_state {
    PROS_SERVICE_DEFINED = 0,
    PROS_SERVICE_STARTING,
    PROS_SERVICE_RUNNING,
    PROS_SERVICE_STOPPING,
    PROS_SERVICE_FAILED,
    PROS_SERVICE_STOPPED
};

typedef int (*pros_service_start_fn)(void *context);
typedef int (*pros_service_stop_fn)(void *context);

struct pros_service_definition {
    const char *name;
    pros_service_start_fn start;
    pros_service_stop_fn stop;
    void *context;
    uint32_t dependency_count;
    const char *dependencies[PROS_SERVICE_MAX_DEPS];
    uint32_t max_retries;
};

struct pros_service {
    struct pros_service_definition definition;
    enum pros_service_state state;
    uint32_t restart_count;
    uint8_t registered;
    uint8_t reserved[3];
    /*
     * The manager owns identifier storage. Registration must not retain
     * pointers into a caller's stack or mutable configuration buffer.
     * Callback functions and context are still caller-owned and must remain
     * valid for as long as this service is registered.
     */
    char name_storage[PROS_SERVICE_NAME_MAX];
    char dependency_storage[PROS_SERVICE_MAX_DEPS][PROS_SERVICE_NAME_MAX];
};

/*
 * Current kernel service-manager API is single-owner: callers must serialize
 * operations and must not re-enter the manager from a callback. Add explicit
 * synchronization before using it concurrently or from SMP runtime paths.
 */
int service_manager_init(void);
int service_register(const struct pros_service_definition *definition);
int service_start(const char *name);
int service_stop(const char *name);
int service_restart(const char *name);
int service_start_all(void);
int service_stop_all(void);
int service_status(const char *name, enum pros_service_state *state,
                   uint32_t *restart_count);
int service_manager_selftest(void);

#endif
