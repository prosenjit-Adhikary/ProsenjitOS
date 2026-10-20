#ifndef PROSENJIT_USER_DML_RUNTIME_H
#define PROSENJIT_USER_DML_RUNTIME_H

#include "desktop_manager.h"

#define PROS_DML_RUNTIME_MAGIC 0x44524D31u
#define PROS_DML_INVALID_PROCESS_HANDLE 0u

/*
 * Platform adapter for real process operations. start() must return zero only
 * after a process was created and a usable opaque handle was written. On
 * failure it must not leave a live process behind. stop() returns zero only
 * after the process is no longer running. Callbacks execute synchronously.
 */
struct pros_dml_runtime_ops {
    int (*start)(const struct pros_dml_manifest *manifest,
                 uint64_t *process_handle, void *context);
    int (*stop)(const struct pros_dml_manifest *manifest,
                uint64_t process_handle, void *context);
};

/*
 * Single-owner session orchestrator. One root desktop session may be active
 * at a time. Dependencies are started in dependency-first order and stopped
 * in reverse order. This module does not itself implement process creation;
 * the adapter must use a real, permission-checked OS process API.
 */
struct pros_dml_runtime {
    uint32_t magic;
    struct pros_dml_manager *manager;
    struct pros_dml_runtime_ops ops;
    void *context;
    uint64_t process_handles[PROS_DML_MAX_DESKTOPS];
    uint32_t active_order[PROS_DML_MAX_DESKTOPS];
    uint32_t active_count;
    uint32_t active_root;
};

int pros_dml_runtime_init(struct pros_dml_runtime *runtime,
                          struct pros_dml_manager *manager,
                          const struct pros_dml_runtime_ops *ops,
                          void *context);
int pros_dml_runtime_start(struct pros_dml_runtime *runtime,
                           const char *desktop_id);
int pros_dml_runtime_stop(struct pros_dml_runtime *runtime);
int pros_dml_runtime_active(const struct pros_dml_runtime *runtime,
                            const char **desktop_id);

#endif
