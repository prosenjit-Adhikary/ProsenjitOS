#include "desktop_manager.h"

static int manager_valid(const struct pros_dml_manager *manager)
{
    if (!manager || manager->magic != PROS_DML_MANAGER_MAGIC ||
        manager->count > PROS_DML_MAX_DESKTOPS)
        return 0;

    /*
     * Registry slots are a contiguous prefix: registration writes exactly at
     * count, then increments count. Public storage corruption must not create
     * hidden entries or holes that can be overwritten by a later registration.
     */
    for (uint32_t i = 0; i < PROS_DML_MAX_DESKTOPS; ++i) {
        if ((i < manager->count) !=
            (manager->entries[i].registered != 0u))
            return 0;
    }
    return 1;
}

static int id_valid(const char *id)
{
    uint32_t length = 0;

    if (!id || id[0] == '\0')
        return 0;
    for (; length < PROS_DML_ID_MAX; ++length) {
        char c = id[length];
        if (c == '\0')
            break;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return 0;
    }
    if (length == 0 || length >= PROS_DML_ID_MAX)
        return 0;
    return !(length == 1u && id[0] == '.') &&
           !(length == 2u && id[0] == '.' && id[1] == '.');
}

static int path_component_is_dot(const char *path, uint32_t start,
                                 uint32_t end)
{
    uint32_t length = end - start;
    return (length == 1u && path[start] == '.') ||
           (length == 2u && path[start] == '.' && path[start + 1u] == '.');
}

static int relative_entry_path_valid(const char *path)
{
    uint32_t length = 0;
    uint32_t component_start = 0;

    if (!path || path[0] == '\0' || path[0] == '/')
        return 0;

    for (; length < PROS_DML_ENTRY_MAX; ++length) {
        char c = path[length];
        if (c == '\0')
            break;
        if (c == '\\')
            return 0;
        if (c == '/') {
            if (length == component_start ||
                path_component_is_dot(path, component_start, length))
                return 0;
            component_start = length + 1u;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return 0;
    }

    if (length == 0 || length >= PROS_DML_ENTRY_MAX ||
        length == component_start ||
        path_component_is_dot(path, component_start, length))
        return 0;
    return 1;
}

static int id_equal(const char *a, const char *b)
{
    uint32_t i = 0;

    if (!a || !b)
        return 0;
    while (i < PROS_DML_ID_MAX && a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i])
            return 0;
        ++i;
    }
    return i < PROS_DML_ID_MAX && a[i] == b[i];
}

static int manifest_validate(const struct pros_dml_manager *manager,
                             const struct pros_dml_manifest *manifest)
{
    if (!manager || !manifest)
        return PROS_DML_EINVAL;
    if (manifest->struct_size < sizeof(*manifest) ||
        manifest->abi_version != PROS_DML_ABI_VERSION)
        return PROS_DML_EABI;
    if (!id_valid(manifest->id) ||
        !relative_entry_path_valid(manifest->entry_point) ||
        manifest->dependency_count > PROS_DML_MAX_DEPENDENCIES)
        return PROS_DML_EINVAL;
    if ((manifest->required_features & ~manager->supported_features) != 0u)
        return PROS_DML_EFEATURE;

    for (uint32_t d = 0; d < manifest->dependency_count; ++d) {
        if (!id_valid(manifest->dependencies[d]) ||
            id_equal(manifest->id, manifest->dependencies[d]))
            return PROS_DML_EINVAL;
        for (uint32_t previous = 0; previous < d; ++previous) {
            if (id_equal(manifest->dependencies[d],
                         manifest->dependencies[previous]))
                return PROS_DML_EINVAL;
        }
    }
    return PROS_DML_OK;
}

static int dml_find(const struct pros_dml_manager *manager, const char *id)
{
    if (!manager_valid(manager) || !id_valid(id))
        return -1;
    for (uint32_t i = 0; i < manager->count; ++i) {
        if (manager->entries[i].registered &&
            id_equal(manager->entries[i].manifest.id, id))
            return (int)i;
    }
    return -1;
}

static int dml_check_dependencies(const struct pros_dml_manager *manager,
                                  uint32_t index,
                                  uint8_t visiting[PROS_DML_MAX_DESKTOPS],
                                  uint8_t visited[PROS_DML_MAX_DESKTOPS])
{
    const struct pros_dml_manifest *manifest;

    if (!manager_valid(manager) || index >= manager->count ||
        index >= PROS_DML_MAX_DESKTOPS || !visiting || !visited ||
        !manager->entries[index].registered)
        return PROS_DML_EINVAL;
    if (visiting[index])
        return PROS_DML_ELOOP;
    if (visited[index])
        return PROS_DML_OK;

    manifest = &manager->entries[index].manifest;
    /* The manager is public storage; reject corrupted counts before indexing. */
    if (manifest->dependency_count > PROS_DML_MAX_DEPENDENCIES)
        return PROS_DML_EINVAL;
    visiting[index] = 1u;
    for (uint32_t d = 0; d < manifest->dependency_count; ++d) {
        int dependency = dml_find(manager, manifest->dependencies[d]);
        int rc;

        if (dependency < 0) {
            visiting[index] = 0u;
            return PROS_DML_ENODEP;
        }
        rc = dml_check_dependencies(manager, (uint32_t)dependency,
                                    visiting, visited);
        if (rc != PROS_DML_OK) {
            visiting[index] = 0u;
            return rc;
        }
    }
    visiting[index] = 0u;
    visited[index] = 1u;
    return PROS_DML_OK;
}

int pros_dml_init(struct pros_dml_manager *manager,
                  uint32_t supported_features)
{
    if (!manager)
        return PROS_DML_EINVAL;
    for (uint32_t i = 0; i < PROS_DML_MAX_DESKTOPS; ++i)
        manager->entries[i] = (struct pros_dml_entry){0};
    manager->count = 0u;
    manager->selected_index = PROS_DML_NO_SELECTION;
    manager->supported_features = supported_features;
    manager->magic = PROS_DML_MANAGER_MAGIC;
    return PROS_DML_OK;
}

int pros_dml_register(struct pros_dml_manager *manager,
                      const struct pros_dml_manifest *manifest)
{
    int rc;

    if (!manager_valid(manager))
        return PROS_DML_EINVAL;
    rc = manifest_validate(manager, manifest);
    if (rc != PROS_DML_OK)
        return rc;
    if (dml_find(manager, manifest->id) >= 0)
        return PROS_DML_EEXIST;
    if (manager->count >= PROS_DML_MAX_DESKTOPS)
        return PROS_DML_ENOSPC;

    manager->entries[manager->count].manifest = *manifest;
    manager->entries[manager->count].registered = 1u;
    manager->entries[manager->count].reserved[0] = 0u;
    manager->entries[manager->count].reserved[1] = 0u;
    manager->entries[manager->count].reserved[2] = 0u;
    manager->entries[manager->count].session_state =
        (uint32_t)PROS_DML_SESSION_STOPPED;
    ++manager->count;
    return PROS_DML_OK;
}

static int dml_build_order(const struct pros_dml_manager *manager,
                           uint32_t index,
                           uint8_t visiting[PROS_DML_MAX_DESKTOPS],
                           uint8_t visited[PROS_DML_MAX_DESKTOPS],
                           uint32_t order[PROS_DML_MAX_DESKTOPS],
                           uint32_t *order_count)
{
    const struct pros_dml_manifest *manifest;

    if (!manager_valid(manager) || index >= manager->count ||
        index >= PROS_DML_MAX_DESKTOPS || !visiting || !visited ||
        !order || !order_count || !manager->entries[index].registered)
        return PROS_DML_EINVAL;
    if (visiting[index])
        return PROS_DML_ELOOP;
    if (visited[index])
        return PROS_DML_OK;

    manifest = &manager->entries[index].manifest;
    /* The manager is public storage; reject corrupted counts before indexing. */
    if (manifest->dependency_count > PROS_DML_MAX_DEPENDENCIES)
        return PROS_DML_EINVAL;
    visiting[index] = 1u;
    for (uint32_t d = 0; d < manifest->dependency_count; ++d) {
        int dependency = dml_find(manager, manifest->dependencies[d]);
        int rc;

        if (dependency < 0) {
            visiting[index] = 0u;
            return PROS_DML_ENODEP;
        }
        rc = dml_build_order(manager, (uint32_t)dependency, visiting, visited,
                             order, order_count);
        if (rc != PROS_DML_OK) {
            visiting[index] = 0u;
            return rc;
        }
    }
    visiting[index] = 0u;
    visited[index] = 1u;
    if (*order_count >= PROS_DML_MAX_DESKTOPS)
        return PROS_DML_ENOSPC;
    order[(*order_count)++] = index;
    return PROS_DML_OK;
}

int pros_dml_resolve_order(const struct pros_dml_manager *manager,
                           const char *desktop_id,
                           uint32_t *out_indices,
                           uint32_t capacity,
                           uint32_t *out_count)
{
    uint8_t visiting[PROS_DML_MAX_DESKTOPS] = {0};
    uint8_t visited[PROS_DML_MAX_DESKTOPS] = {0};
    uint32_t order[PROS_DML_MAX_DESKTOPS] = {0};
    uint32_t order_count = 0u;
    int index;
    int rc;

    if (!out_count)
        return PROS_DML_EINVAL;
    *out_count = 0u;
    if (!manager_valid(manager) || !id_valid(desktop_id) ||
        (capacity != 0u && !out_indices))
        return PROS_DML_EINVAL;

    index = dml_find(manager, desktop_id);
    if (index < 0)
        return PROS_DML_ENOENT;
    rc = dml_build_order(manager, (uint32_t)index, visiting, visited,
                         order, &order_count);
    if (rc != PROS_DML_OK)
        return rc;

    *out_count = order_count;
    if (capacity < order_count)
        return PROS_DML_ENOSPC;
    for (uint32_t i = 0; i < order_count; ++i)
        out_indices[i] = order[i];
    return PROS_DML_OK;
}

int pros_dml_preflight(const struct pros_dml_manager *manager,
                       const char *desktop_id)
{
    uint8_t visiting[PROS_DML_MAX_DESKTOPS] = {0};
    uint8_t visited[PROS_DML_MAX_DESKTOPS] = {0};
    int index;

    if (!manager_valid(manager) || !id_valid(desktop_id))
        return PROS_DML_EINVAL;
    index = dml_find(manager, desktop_id);
    if (index < 0)
        return PROS_DML_ENOENT;
    return dml_check_dependencies(manager, (uint32_t)index,
                                  visiting, visited);
}

int pros_dml_select(struct pros_dml_manager *manager,
                    const char *desktop_id)
{
    int rc;
    int index;

    if (!manager_valid(manager) || !id_valid(desktop_id))
        return PROS_DML_EINVAL;
    rc = pros_dml_preflight(manager, desktop_id);
    if (rc != PROS_DML_OK)
        return rc;
    index = dml_find(manager, desktop_id);
    if (index < 0)
        return PROS_DML_ENOENT;
    manager->selected_index = (uint32_t)index;
    return PROS_DML_OK;
}

int pros_dml_get(const struct pros_dml_manager *manager,
                 const char *desktop_id,
                 struct pros_dml_manifest *out_manifest)
{
    int index;

    if (!manager_valid(manager) || !out_manifest || !id_valid(desktop_id))
        return PROS_DML_EINVAL;
    index = dml_find(manager, desktop_id);
    if (index < 0)
        return PROS_DML_ENOENT;
    *out_manifest = manager->entries[index].manifest;
    return PROS_DML_OK;
}

int pros_dml_selected(const struct pros_dml_manager *manager,
                      struct pros_dml_manifest *out_manifest)
{
    if (!manager_valid(manager) || !out_manifest)
        return PROS_DML_EINVAL;
    if (manager->selected_index == PROS_DML_NO_SELECTION)
        return PROS_DML_ENOENT;
    if (manager->selected_index >= manager->count ||
        !manager->entries[manager->selected_index].registered)
        return PROS_DML_EINVAL;
    *out_manifest = manager->entries[manager->selected_index].manifest;
    return PROS_DML_OK;
}

static int dml_transition_allowed(enum pros_dml_session_state current,
                                    enum pros_dml_session_state next)
{
    switch (current) {
    case PROS_DML_SESSION_STOPPED:
        return next == PROS_DML_SESSION_STARTING;
    case PROS_DML_SESSION_STARTING:
        return next == PROS_DML_SESSION_RUNNING ||
               next == PROS_DML_SESSION_FAILED ||
               next == PROS_DML_SESSION_STOPPED;
    case PROS_DML_SESSION_RUNNING:
        return next == PROS_DML_SESSION_STOPPING ||
               next == PROS_DML_SESSION_FAILED;
    case PROS_DML_SESSION_STOPPING:
        return next == PROS_DML_SESSION_STOPPED ||
               next == PROS_DML_SESSION_FAILED;
    case PROS_DML_SESSION_FAILED:
        return next == PROS_DML_SESSION_STOPPING ||
               next == PROS_DML_SESSION_STOPPED;
    default:
        return 0;
    }
}

int pros_dml_get_state(const struct pros_dml_manager *manager,
                       const char *desktop_id,
                       enum pros_dml_session_state *out_state)
{
    int index;

    if (!manager_valid(manager) || !id_valid(desktop_id) || !out_state)
        return PROS_DML_EINVAL;
    index = dml_find(manager, desktop_id);
    if (index < 0)
        return PROS_DML_ENOENT;
    /* Public manager storage may be corrupted; never report an invalid state
     * as a successful observation to lifecycle callers. */
    if (manager->entries[index].session_state >
        (uint32_t)PROS_DML_SESSION_FAILED)
        return PROS_DML_ESTATE;
    *out_state = (enum pros_dml_session_state)
        manager->entries[index].session_state;
    return PROS_DML_OK;
}

int pros_dml_transition(struct pros_dml_manager *manager,
                        const char *desktop_id,
                        enum pros_dml_session_state next_state)
{
    int index;
    enum pros_dml_session_state current;

    if (!manager_valid(manager) || !id_valid(desktop_id) ||
        (int32_t)next_state < (int32_t)PROS_DML_SESSION_STOPPED ||
        (int32_t)next_state > (int32_t)PROS_DML_SESSION_FAILED)
        return PROS_DML_EINVAL;
    index = dml_find(manager, desktop_id);
    if (index < 0)
        return PROS_DML_ENOENT;
    current = (enum pros_dml_session_state)
        manager->entries[index].session_state;
    if (!dml_transition_allowed(current, next_state))
        return PROS_DML_ESTATE;
    manager->entries[index].session_state = (uint32_t)next_state;
    return PROS_DML_OK;
}

uint32_t pros_dml_count(const struct pros_dml_manager *manager)
{
    if (!manager_valid(manager))
        return 0u;
    return manager->count;
}
