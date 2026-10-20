#include "dml_runtime.h"

/*
 * Backend callbacks are synchronous and must not re-enter the runtime. Keep
 * this guard outside the public runtime object so runtime_init() can reject a
 * callback trying to reset the same or another runtime without first reading
 * possibly uninitialized caller-provided storage. The DML control plane is
 * single-owner/serialized, so this guard is intentionally not a lock.
 */
static uint32_t runtime_callback_active;

/*
 * The manager is public storage. Validate the registry's contiguous-prefix
 * invariant here too: runtime init/stop must not accept a manager with holes
 * or hidden registrations merely because no manifest lookup is needed yet.
 */
static int runtime_manager_valid(const struct pros_dml_manager *manager)
{
    if (!manager || manager->magic != PROS_DML_MANAGER_MAGIC ||
        manager->count > PROS_DML_MAX_DESKTOPS)
        return 0;

    for (uint32_t i = 0; i < PROS_DML_MAX_DESKTOPS; ++i) {
        if (i < manager->count) {
            if (manager->entries[i].registered != 1u)
                return 0;
        } else if (manager->entries[i].registered != 0u) {
            return 0;
        }
    }
    return 1;
}

static int runtime_valid(const struct pros_dml_runtime *runtime)
{
    uint8_t active[PROS_DML_MAX_DESKTOPS] = {0};

    if (!runtime || runtime->magic != PROS_DML_RUNTIME_MAGIC ||
        !runtime_manager_valid(runtime->manager) ||
        !runtime->ops.start || !runtime->ops.stop ||
        runtime->active_count > PROS_DML_MAX_DESKTOPS)
        return 0;

    /*
     * Public runtime storage is mutable. Validate both directions of the
     * handle/order relationship so an untracked live handle cannot be silently
     * forgotten, and stale order entries cannot be mistaken for active tasks.
     */
    for (uint32_t i = 0; i < runtime->active_count; ++i) {
        uint32_t index = runtime->active_order[i];

        if (index >= runtime->manager->count ||
            !runtime->manager->entries[index].registered ||
            runtime->process_handles[index] == PROS_DML_INVALID_PROCESS_HANDLE ||
            active[index])
            return 0;
        active[index] = 1u;
    }
    for (uint32_t i = runtime->active_count; i < PROS_DML_MAX_DESKTOPS; ++i) {
        if (runtime->active_order[i] != 0u)
            return 0;
    }
    for (uint32_t i = 0; i < PROS_DML_MAX_DESKTOPS; ++i) {
        if (active[i]) {
            if (runtime->process_handles[i] == PROS_DML_INVALID_PROCESS_HANDLE)
                return 0;
        } else if (runtime->process_handles[i] != PROS_DML_INVALID_PROCESS_HANDLE) {
            return 0;
        }
    }

    if (runtime->active_count == 0u) {
        if (runtime->active_root != PROS_DML_NO_SELECTION)
            return 0;
    } else if (runtime->active_root != PROS_DML_NO_SELECTION &&
               (runtime->active_root >= runtime->manager->count ||
                !active[runtime->active_root])) {
        return 0;
    }
    return 1;
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
            runtime->active_order[runtime->active_count] = 0u;
            if (runtime->active_root == index)
                runtime->active_root = PROS_DML_NO_SELECTION;
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

    runtime_callback_active = 1u;
    rc = backend_status(runtime->ops.stop(&entry->manifest,
                            runtime->process_handles[index],
                            runtime->context));
    runtime_callback_active = 0u;
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
    if (runtime_callback_active)
        return PROS_DML_ESTATE;
    if (!runtime || !runtime_manager_valid(manager) ||
        !ops || !ops->start || !ops->stop)
        return PROS_DML_EINVAL;

    /*
     * A fresh runtime has no trustworthy process handles. Refuse to attach to
     * a manager with live or failed sessions rather than orphaning processes
     * or incorrectly resetting lifecycle state.
     */
    for (uint32_t i = 0; i < manager->count; ++i) {
        enum pros_dml_session_state state;

        if (!manager->entries[i].registered)
            continue;
        if (pros_dml_get_state(manager, manager->entries[i].manifest.id,
                               &state) != PROS_DML_OK)
            return PROS_DML_ESTATE;
        if (state != PROS_DML_SESSION_STOPPED)
            return PROS_DML_ESTATE;
    }

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

    if (runtime_callback_active)
        return PROS_DML_ESTATE;
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

        runtime_callback_active = 1u;
        rc = backend_status(runtime->ops.start(&entry->manifest,
                                                &process_handle,
                                                runtime->context));
        runtime_callback_active = 0u;
        if (rc != PROS_DML_OK) {
            (void)pros_dml_transition(runtime->manager, entry->manifest.id,
                                      PROS_DML_SESSION_FAILED);
            /*
             * A defensive adapter may have created a process before a later
             * launch step failed. If it published a nonzero handle, retain
             * ownership and route it through the same reverse-order rollback
             * path. If stop fails, runtime_stop_index leaves the handle tracked
             * in FAILED state so a later runtime_stop() can retry cleanup.
             */
            if (process_handle != PROS_DML_INVALID_PROCESS_HANDLE) {
                runtime->process_handles[index] = process_handle;
                runtime->active_order[runtime->active_count++] = index;
                started_this_call[started_count++] = index;
                if (index == root_index)
                    runtime->active_root = root_index;
            }
            goto rollback;
        }
        /*
         * Zero is reserved as the invalid handle. Treat a backend that reports
         * success without publishing a usable handle as a contract violation;
         * never record an un-stoppable process as an active session.
         */
        if (process_handle == PROS_DML_INVALID_PROCESS_HANDLE) {
            (void)pros_dml_transition(runtime->manager, entry->manifest.id,
                                      PROS_DML_SESSION_FAILED);
            rc = PROS_DML_EBACKEND;
            goto rollback;
        }

        runtime->process_handles[index] = process_handle;
        runtime->active_order[runtime->active_count++] = index;
        started_this_call[started_count++] = index;
        if (index == root_index)
            runtime->active_root = root_index;

        rc = pros_dml_transition(runtime->manager, entry->manifest.id,
                                 PROS_DML_SESSION_RUNNING);
        if (rc != PROS_DML_OK) {
            /* The process exists, so keep it tracked for a later stop attempt. */
            (void)pros_dml_transition(runtime->manager, entry->manifest.id,
                                      PROS_DML_SESSION_FAILED);
            runtime->active_root = order_contains(runtime, root_index)
                                      ? root_index : PROS_DML_NO_SELECTION;
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
            runtime->active_root = order_contains(runtime, root_index)
                                       ? root_index : PROS_DML_NO_SELECTION;
            return PROS_DML_EROLLBACK;
        }
    }
    runtime->active_root = PROS_DML_NO_SELECTION;
    return rc;
}

int pros_dml_runtime_stop(struct pros_dml_runtime *runtime)
{
    if (runtime_callback_active)
        return PROS_DML_ESTATE;
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
    if (runtime_callback_active)
        return PROS_DML_ESTATE;
    if (!runtime_valid(runtime) || !desktop_id)
        return PROS_DML_EINVAL;
    /*
     * A root ID alone is not proof of a live session: the runtime structure is
     * public storage and may be corrupted. A selected root must be represented
     * in the tracked active dependency order before it can be reported active.
     */
    if (runtime->active_count == 0u)
        return PROS_DML_ENOENT;
    /* Retained processes with no live root are failed/incomplete, not idle. */
    if (runtime->active_root == PROS_DML_NO_SELECTION)
        return PROS_DML_ESTATE;
    if (runtime->active_root >= runtime->manager->count ||
        !runtime->manager->entries[runtime->active_root].registered ||
        !order_contains(runtime, runtime->active_root))
        return PROS_DML_ESTATE;
    {
        enum pros_dml_session_state state;
        int rc = pros_dml_get_state(
            runtime->manager,
            runtime->manager->entries[runtime->active_root].manifest.id,
            &state);
        if (rc != PROS_DML_OK)
            return rc;
        /* A retained root may only be awaiting cleanup after a failure. */
        if (state != PROS_DML_SESSION_RUNNING)
            return PROS_DML_ESTATE;
    }
    *desktop_id = runtime->manager->entries[runtime->active_root].manifest.id;
    return PROS_DML_OK;
}
