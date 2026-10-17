#include "dml_runtime.h"

static int runtime_valid(const struct pros_dml_runtime *runtime)
{
    return runtime && runtime->magic == PROS_DML_RUNTIME_MAGIC &&
           runtime->manager &&
           runtime->manager->magic == PROS_DML_MANAGER_MAGIC &&
           runtime->manager->count <= PROS_DML_MAX_DESKTOPS &&
           runtime->ops.start && runtime->ops.stop &&
           runtime->active_count <= PROS_DML_MAX_DESKTOPS;
}

static int backend_status(int status)
{
    if (status == 0)
        return PROS_DML_OK;
    return status < 0 ? status : PROS_DML_EBACKEND;
}

static int order_contains(const struct pros_dml_runtime *runtime,
                          uint32_t index)
{
    for (uint32_t i = 0; i < runtime->active_count; ++i) {
        if (runtime->active_order[i] == index)
            return 1;
    }
    return 0;
}

static int order_remove(struct pros_dml_runtime *runtime, uint32_t index)
{
    for (uint32_t i = 0; i < runtime->active_count; ++i) {
        if (runtime->active_order[i] == index) {
            for (uint32_t j = i + 1u; j < runtime->active_count; ++j)
                runtime->active_order[j - 1u] = runtime->active_order[j];
            --runtime->active_count;
            return 1;
        }
    }
    return 0;
}

static int runtime_stop_index(struct pros_dml_runtime *runtime,
                              uint32_t index)
{
    struct pros_dml_entry *entry;
    enum pros_dml_session_state state;
    int rc;

    if (!runtime_valid(runtime) || index >= runtime->manager->count ||
        !runtime->manager->entries[index].registered ||
        !order_contains(runtime, index))
        return PROS_DML_EINVAL;

    entry = &runtime->manager->entries[index];
    rc = pros_dml_get_state(runtime->manager, entry->manifest.id, &state);
    if (rc != PROS_DML_OK)
        return rc;
    if (state != PROS_DML_SESSION_RUNNING &&
        state != PROS_DML_SESSION_FAILED)
        return PROS_DML_ESTATE;

    rc = pros_dml_transition(runtime->manager, entry->manifest.id,
                             PROS_DML_SESSION_STOPPING);
    if (rc != PROS_DML_OK)
        return rc;

    rc = backend_status(runtime->ops.stop(&entry->manifest,
                            runtime->process_handles[index],
                            runtime->context));
    if (rc != PROS_DML_OK) {
        (void)pros_dml_transition(runtime->manager, entry->manifest.id,
                                  PROS_DML_SESSION_FAILED);
        return rc;
    }

    rc = pros_dml_transition(runtime->manager, entry->manifest.id,
                             PROS_DML_SESSION_STOPPED);
    if (rc != PROS_DML_OK) {
        /* The process is stopped, but state storage is inconsistent. */
        runtime->process_handles[index] = 0u;
        (void)order_remove(runtime, index);
        return rc;
    }

    runtime->process_handles[index] = 0u;
    (void)order_remove(runtime, index);
    return PROS_DML_OK;
}

int pros_dml_runtime_init(struct pros_dml_runtime *runtime,
                          struct pros_dml_manager *manager,
                          const struct pros_dml_runtime_ops *ops,
                          void *context)
{
    if (!runtime || !manager ||
        manager->magic != PROS_DML_MANAGER_MAGIC ||
        manager->count > PROS_DML_MAX_DESKTOPS ||
        !ops || !ops->start || !ops->stop)
        return PROS_DML_EINVAL;

    runtime->magic = 0u;
    runtime->manager = manager;
    runtime->ops = *ops;
    runtime->context = context;
    runtime->active_count = 0u;
    runtime->active_root = PROS_DML_NO_SELECTION;
    for (uint32_t i = 0; i < PROS_DML_MAX_DESKTOPS; ++i) {
        runtime->process_handles[i] = 0u;
        runtime->active_order[i] = 0u;
    }
    runtime->magic = PROS_DML_RUNTIME_MAGIC;
    return PROS_DML_OK;
}

int pros_dml_runtime_start(struct pros_dml_runtime *runtime,
                           const char *desktop_id)
{
    uint32_t order[PROS_DML_MAX_DESKTOPS];
    uint32_t order_count = 0u;
    uint32_t root_index = PROS_DML_NO_SELECTION;
    uint32_t started_this_call[PROS_DML_MAX_DESKTOPS];
    uint32_t started_count = 0u;
    int rc;

    if (!runtime_valid(runtime) || !desktop_id)
        return PROS_DML_EINVAL;
    if (runtime->active_count != 0u)
        return PROS_DML_ESTATE;

    rc = pros_dml_resolve_order(runtime->manager, desktop_id, order,
                                PROS_DML_MAX_DESKTOPS, &order_count);
    if (rc != PROS_DML_OK)
        return rc;
    if (order_count == 0u)
        return PROS_DML_EINVAL;
    root_index = order[order_count - 1u];

    for (uint32_t i = 0; i < order_count; ++i) {
        uint32_t index = order[i];
        struct pros_dml_entry *entry = &runtime->manager->entries[index];
        enum pros_dml_session_state state;
        uint64_t process_handle = 0u;

        rc = pros_dml_get_state(runtime->manager, entry->manifest.id, &state);
        if (rc != PROS_DML_OK)
            goto rollback;
        if (state == PROS_DML_SESSION_FAILED) {
            /* A failed launch has no tracked live process after clean rollback. */
            rc = pros_dml_transition(runtime->manager, entry->manifest.id,
                                     PROS_DML_SESSION_STOPPED);
            if (rc != PROS_DML_OK)
                goto rollback;
            state = PROS_DML_SESSION_STOPPED;
        }
        if (state != PROS_DML_SESSION_STOPPED) {
            rc = PROS_DML_ESTATE;
            goto rollback;
        }

        rc = pros_dml_transition(runtime->manager, entry->manifest.id,
                                 PROS_DML_SESSION_STARTING);
        if (rc != PROS_DML_OK)
            goto rollback;

        rc = backend_status(runtime->ops.start(&entry->manifest,
                                                &process_handle,
                                                runtime->context));
        if (rc != PROS_DML_OK) {
            (void)pros_dml_transition(runtime->manager, entry->manifest.id,
                                      PROS_DML_SESSION_FAILED);
            goto rollback;
        }

        runtime->process_handles[index] = process_handle;
        runtime->active_order[runtime->active_count++] = index;
        started_this_call[started_count++] = index;

        rc = pros_dml_transition(runtime->manager, entry->manifest.id,
                                 PROS_DML_SESSION_RUNNING);
        if (rc != PROS_DML_OK) {
            /* The process exists, so keep it tracked for a later stop attempt. */
            (void)pros_dml_transition(runtime->manager, entry->manifest.id,
                                      PROS_DML_SESSION_FAILED);
            runtime->active_root = root_index;
            return PROS_DML_EROLLBACK;
        }
    }

    runtime->active_root = root_index;
    return PROS_DML_OK;

rollback:
    while (started_count != 0u) {
        uint32_t index = started_this_call[--started_count];
        int stop_rc = runtime_stop_index(runtime, index);
        if (stop_rc != PROS_DML_OK) {
            runtime->active_root = root_index;
            return PROS_DML_EROLLBACK;
        }
    }
    runtime->active_root = PROS_DML_NO_SELECTION;
    return rc;
}

int pros_dml_runtime_stop(struct pros_dml_runtime *runtime)
{
    if (!runtime_valid(runtime))
        return PROS_DML_EINVAL;
    while (runtime->active_count != 0u) {
        uint32_t index = runtime->active_order[runtime->active_count - 1u];
        int rc = runtime_stop_index(runtime, index);
        if (rc != PROS_DML_OK)
            return rc;
    }
    runtime->active_root = PROS_DML_NO_SELECTION;
    return PROS_DML_OK;
}

int pros_dml_runtime_active(const struct pros_dml_runtime *runtime,
                            const char **desktop_id)
{
    if (!runtime_valid(runtime) || !desktop_id)
        return PROS_DML_EINVAL;
    if (runtime->active_root == PROS_DML_NO_SELECTION)
        return PROS_DML_ENOENT;
    if (runtime->active_root >= runtime->manager->count ||
        !runtime->manager->entries[runtime->active_root].registered)
        return PROS_DML_ESTATE;
    *desktop_id = runtime->manager->entries[runtime->active_root].manifest.id;
    return PROS_DML_OK;
}
