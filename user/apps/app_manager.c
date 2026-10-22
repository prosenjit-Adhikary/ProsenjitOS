#include "app_manager.h"

static int manager_valid(const struct pros_app_manager *manager)
{
    return manager && manager->magic == PROS_APP_MANAGER_MAGIC &&
           manager->count <= PROS_APP_MAX_ENTRIES;
}

static int id_valid(const char *id)
{
    uint32_t length = 0u;

    if (!id || id[0] == '\0')
        return 0;
    for (; length < PROS_APP_ID_MAX; ++length) {
        char c = id[length];
        if (c == '\0')
            break;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    if (length == 0u || length >= PROS_APP_ID_MAX)
        return 0;
    return !(length == 1u && id[0] == '.') &&
           !(length == 2u && id[0] == '.' && id[1] == '.');
}

static int component_is_dot(const char *path, uint32_t start, uint32_t end)
{
    uint32_t length = end - start;
    return (length == 1u && path[start] == '.') ||
           (length == 2u && path[start] == '.' && path[start + 1u] == '.');
}

static int entry_path_valid(const char *path)
{
    uint32_t length = 0u;
    uint32_t component_start = 0u;

    if (!path || path[0] == '\0' || path[0] == '/')
        return 0;
    for (; length < PROS_APP_ENTRY_MAX; ++length) {
        char c = path[length];
        if (c == '\0')
            break;
        if (c == '\\' || c == ':')
            return 0;
        if (c == '/') {
            if (length == component_start ||
                component_is_dot(path, component_start, length))
                return 0;
            component_start = length + 1u;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    if (length == 0u || length >= PROS_APP_ENTRY_MAX ||
        length == component_start ||
        component_is_dot(path, component_start, length))
        return 0;
    return 1;
}

static int id_equal(const char *a, const char *b)
{
    uint32_t i = 0u;

    if (!a || !b)
        return 0;
    while (i < PROS_APP_ID_MAX && a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i])
            return 0;
        ++i;
    }
    return i < PROS_APP_ID_MAX && a[i] == b[i];
}

static int backend_valid(uint32_t backend)
{
    return backend == PROS_APP_BACKEND_NATIVE ||
           backend == PROS_APP_BACKEND_LINUX ||
           backend == PROS_APP_BACKEND_WINDOWS;
}

static int manager_find(const struct pros_app_manager *manager,
                        const char *app_id)
{
    if (!manager_valid(manager) || !id_valid(app_id))
        return -1;
    for (uint32_t i = 0u; i < manager->count; ++i) {
        if (manager->entries[i].registered &&
            id_equal(manager->entries[i].manifest.id, app_id))
            return (int)i;
    }
    return -1;
}

static int manifest_validate(const struct pros_app_manager *manager,
                             const struct pros_app_manifest *manifest)
{
    if (!manager || !manifest)
        return PROS_APP_EINVAL;
    if (manifest->struct_size < sizeof(*manifest) ||
        manifest->abi_version != PROS_APP_ABI_VERSION)
        return PROS_APP_EABI;
    if (!id_valid(manifest->id) || !entry_path_valid(manifest->entry_point) ||
        manifest->dependency_count > PROS_APP_MAX_DEPENDENCIES)
        return PROS_APP_EINVAL;
    if (!backend_valid(manifest->backend))
        return PROS_APP_EBACKEND;
    /*
     * Registration is not installation. Only staged, unverified manifests
     * enter the registry; trusted verification and install commit are separate
     * explicit transitions below.
     */
    if (manifest->install_state != PROS_APP_STAGED)
        return PROS_APP_ESTATE;
    if (manifest->trust_state != PROS_APP_TRUST_UNKNOWN)
        return PROS_APP_ETRUST;
    if ((manifest->required_features & ~manager->supported_features) != 0u)
        return PROS_APP_EFEATURE;

    for (uint32_t d = 0u; d < manifest->dependency_count; ++d) {
        if (!id_valid(manifest->dependencies[d]) ||
            id_equal(manifest->id, manifest->dependencies[d]))
            return PROS_APP_EINVAL;
        for (uint32_t previous = 0u; previous < d; ++previous) {
            if (id_equal(manifest->dependencies[d],
                         manifest->dependencies[previous]))
                return PROS_APP_EINVAL;
        }
    }
    return PROS_APP_OK;
}

static int check_dependencies(const struct pros_app_manager *manager,
                              uint32_t index,
                              uint8_t visiting[PROS_APP_MAX_ENTRIES],
                              uint8_t visited[PROS_APP_MAX_ENTRIES])
{
    const struct pros_app_manifest *manifest;

    if (!manager_valid(manager) || index >= manager->count ||
        index >= PROS_APP_MAX_ENTRIES || !visiting || !visited ||
        !manager->entries[index].registered)
        return PROS_APP_EINVAL;
    if (visiting[index])
        return PROS_APP_ELOOP;
    if (visited[index])
        return PROS_APP_OK;

    manifest = &manager->entries[index].manifest;
    if (manifest->trust_state != PROS_APP_TRUST_VERIFIED)
        return PROS_APP_ETRUST;
    if (manifest->install_state != PROS_APP_INSTALLED)
        return PROS_APP_ESTATE;

    visiting[index] = 1u;
    for (uint32_t d = 0u; d < manifest->dependency_count; ++d) {
        int dependency = manager_find(manager, manifest->dependencies[d]);
        int rc;

        if (dependency < 0) {
            visiting[index] = 0u;
            return PROS_APP_ENODEP;
        }
        /*
         * Dependency IDs belong to one package ecosystem. Do not silently
         * combine native, Linux, and Windows dependency namespaces.
         */
        if (manager->entries[dependency].manifest.backend !=
            manifest->backend) {
            visiting[index] = 0u;
            return PROS_APP_EBACKEND;
        }
        rc = check_dependencies(manager, (uint32_t)dependency,
                                visiting, visited);
        if (rc != PROS_APP_OK) {
            visiting[index] = 0u;
            return rc;
        }
    }
    visiting[index] = 0u;
    visited[index] = 1u;
    return PROS_APP_OK;
}

int pros_app_init(struct pros_app_manager *manager,
                  uint32_t supported_features)
{
    if (!manager)
        return PROS_APP_EINVAL;
    /*
     * Initialization must accept an uninitialized caller-owned object, so it
     * cannot inspect transaction_active before establishing manager state.
     * Storage callbacks are trusted adapters and must never call init().
     */
    for (uint32_t i = 0u; i < PROS_APP_MAX_ENTRIES; ++i)
        manager->entries[i] = (struct pros_app_entry){0};
    manager->count = 0u;
    manager->supported_features = supported_features;
    manager->magic = PROS_APP_MANAGER_MAGIC;
    manager->transaction_active = 0u;
    return PROS_APP_OK;
}

int pros_app_register(struct pros_app_manager *manager,
                      const struct pros_app_manifest *manifest)
{
    int rc;

    if (!manager_valid(manager))
        return PROS_APP_EINVAL;
    if (manager->transaction_active != 0u)
        return PROS_APP_ESTATE;
    rc = manifest_validate(manager, manifest);
    if (rc != PROS_APP_OK)
        return rc;
    if (manager_find(manager, manifest->id) >= 0)
        return PROS_APP_EEXIST;
    if (manager->count >= PROS_APP_MAX_ENTRIES)
        return PROS_APP_ENOSPC;

    manager->entries[manager->count].manifest = *manifest;
    manager->entries[manager->count].registered = 1u;
    ++manager->count;
    return PROS_APP_OK;
}


static int validate_install_graph(const struct pros_app_manager *manager,
                                  uint32_t index,
                                  uint32_t root_index,
                                  uint8_t visiting[PROS_APP_MAX_ENTRIES],
                                  uint8_t visited[PROS_APP_MAX_ENTRIES])
{
    const struct pros_app_manifest *manifest;

    if (!manager_valid(manager) || index >= manager->count ||
        index >= PROS_APP_MAX_ENTRIES || !visiting || !visited ||
        !manager->entries[index].registered)
        return PROS_APP_EINVAL;
    if (visiting[index])
        return PROS_APP_ELOOP;
    if (visited[index])
        return PROS_APP_OK;

    manifest = &manager->entries[index].manifest;
    if (manifest->trust_state != PROS_APP_TRUST_VERIFIED)
        return PROS_APP_ETRUST;
    if (index != root_index &&
        manifest->install_state != PROS_APP_INSTALLED)
        return PROS_APP_ESTATE;

    visiting[index] = 1u;
    for (uint32_t d = 0u; d < manifest->dependency_count; ++d) {
        int dependency = manager_find(manager, manifest->dependencies[d]);
        int rc;

        if (dependency < 0) {
            visiting[index] = 0u;
            return PROS_APP_ENODEP;
        }
        if (manager->entries[dependency].manifest.backend != manifest->backend) {
            visiting[index] = 0u;
            return PROS_APP_EBACKEND;
        }
        rc = validate_install_graph(manager, (uint32_t)dependency, root_index,
                                    visiting, visited);
        if (rc != PROS_APP_OK) {
            visiting[index] = 0u;
            return rc;
        }
    }
    visiting[index] = 0u;
    visited[index] = 1u;
    return PROS_APP_OK;
}

int pros_app_mark_verified(struct pros_app_manager *manager,
                           const char *app_id)
{
    int index;

    if (!manager_valid(manager) || !id_valid(app_id))
        return PROS_APP_EINVAL;
    if (manager->transaction_active != 0u)
        return PROS_APP_ESTATE;
    index = manager_find(manager, app_id);
    if (index < 0)
        return PROS_APP_ENOENT;
    if (manager->entries[index].manifest.install_state != PROS_APP_STAGED)
        return PROS_APP_ESTATE;
    manager->entries[index].manifest.trust_state = PROS_APP_TRUST_VERIFIED;
    return PROS_APP_OK;
}

int pros_app_commit_install(struct pros_app_manager *manager,
                            const char *app_id)
{
    uint8_t visiting[PROS_APP_MAX_ENTRIES] = {0};
    uint8_t visited[PROS_APP_MAX_ENTRIES] = {0};
    int index;
    int rc;

    if (!manager_valid(manager) || !id_valid(app_id))
        return PROS_APP_EINVAL;
    if (manager->transaction_active != 0u)
        return PROS_APP_ESTATE;
    index = manager_find(manager, app_id);
    if (index < 0)
        return PROS_APP_ENOENT;
    if (manager->entries[index].manifest.install_state != PROS_APP_STAGED)
        return PROS_APP_ESTATE;
    if (manager->entries[index].manifest.trust_state != PROS_APP_TRUST_VERIFIED)
        return PROS_APP_ETRUST;

    rc = validate_install_graph(manager, (uint32_t)index, (uint32_t)index,
                                visiting, visited);
    if (rc != PROS_APP_OK)
        return rc;

    /* This in-memory transition is the registry commit point only. */
    manager->entries[index].manifest.install_state = PROS_APP_INSTALLED;
    return PROS_APP_OK;
}

int pros_app_disable(struct pros_app_manager *manager, const char *app_id)
{
    int index;

    if (!manager_valid(manager) || !id_valid(app_id))
        return PROS_APP_EINVAL;
    if (manager->transaction_active != 0u)
        return PROS_APP_ESTATE;
    index = manager_find(manager, app_id);
    if (index < 0)
        return PROS_APP_ENOENT;
    if (manager->entries[index].manifest.install_state != PROS_APP_INSTALLED)
        return PROS_APP_ESTATE;

    /* Do not strand an installed dependent in an unusable state. */
    for (uint32_t i = 0u; i < manager->count; ++i) {
        const struct pros_app_manifest *candidate =
            &manager->entries[i].manifest;
        if ((int)i == index ||
            candidate->install_state != PROS_APP_INSTALLED)
            continue;
        for (uint32_t d = 0u; d < candidate->dependency_count; ++d) {
            if (id_equal(candidate->dependencies[d], app_id))
                return PROS_APP_EBUSY;
        }
    }

    manager->entries[index].manifest.install_state = PROS_APP_DISABLED;
    return PROS_APP_OK;
}

int pros_app_uninstall(struct pros_app_manager *manager, const char *app_id)
{
    int index;

    if (!manager_valid(manager) || !id_valid(app_id))
        return PROS_APP_EINVAL;
    if (manager->transaction_active != 0u)
        return PROS_APP_ESTATE;
    index = manager_find(manager, app_id);
    if (index < 0)
        return PROS_APP_ENOENT;

    for (uint32_t i = 0u; i < manager->count; ++i) {
        const struct pros_app_manifest *candidate =
            &manager->entries[i].manifest;
        if ((int)i == index)
            continue;
        for (uint32_t d = 0u; d < candidate->dependency_count; ++d) {
            if (id_equal(candidate->dependencies[d], app_id))
                return PROS_APP_EBUSY;
        }
    }

    for (uint32_t i = (uint32_t)index; i + 1u < manager->count; ++i)
        manager->entries[i] = manager->entries[i + 1u];
    --manager->count;
    manager->entries[manager->count] = (struct pros_app_entry){0};
    return PROS_APP_OK;
}

int pros_app_preflight(const struct pros_app_manager *manager,
                       const char *app_id)
{
    uint8_t visiting[PROS_APP_MAX_ENTRIES] = {0};
    uint8_t visited[PROS_APP_MAX_ENTRIES] = {0};
    int index;

    if (!manager_valid(manager) || !id_valid(app_id))
        return PROS_APP_EINVAL;
    index = manager_find(manager, app_id);
    if (index < 0)
        return PROS_APP_ENOENT;
    return check_dependencies(manager, (uint32_t)index, visiting, visited);
}

int pros_app_get(const struct pros_app_manager *manager,
                 const char *app_id,
                 struct pros_app_manifest *out_manifest)
{
    int index;

    if (!manager_valid(manager) || !out_manifest || !id_valid(app_id))
        return PROS_APP_EINVAL;
    index = manager_find(manager, app_id);
    if (index < 0)
        return PROS_APP_ENOENT;
    *out_manifest = manager->entries[index].manifest;
    return PROS_APP_OK;
}

uint32_t pros_app_count(const struct pros_app_manager *manager)
{
    if (!manager_valid(manager))
        return 0u;
    return manager->count;
}


static int install_rollback(const struct pros_app_install_ops *ops,
                            const struct pros_app_manifest *manifest)
{
    return ops->rollback(ops->context, manifest) == 0
               ? PROS_APP_OK
               : PROS_APP_EROLLBACK;
}

int pros_app_install_transaction(struct pros_app_manager *manager,
                                  const char *app_id,
                                  const struct pros_app_install_ops *ops)
{
    uint8_t visiting[PROS_APP_MAX_ENTRIES] = {0};
    uint8_t visited[PROS_APP_MAX_ENTRIES] = {0};
    struct pros_app_manifest manifest;
    int index;
    int rc;

    if (!manager_valid(manager) || !id_valid(app_id) || !ops ||
        !ops->stage || !ops->publish || !ops->rollback)
        return PROS_APP_EINVAL;

    index = manager_find(manager, app_id);
    if (index < 0)
        return PROS_APP_ENOENT;

    manifest = manager->entries[index].manifest;
    if (manifest.install_state != PROS_APP_STAGED)
        return PROS_APP_ESTATE;
    if (manifest.trust_state != PROS_APP_TRUST_VERIFIED)
        return PROS_APP_ETRUST;

    /*
     * Fail before touching storage when dependencies are absent, untrusted,
     * disabled, cyclic, or belong to a different backend ecosystem.
     */
    rc = validate_install_graph(manager, (uint32_t)index, (uint32_t)index,
                                visiting, visited);
    if (rc != PROS_APP_OK)
        return rc;

    /*
     * The storage adapter owns temporary files and atomic publication. This
     * coordinator commits registry state only after publish() succeeds.
     */
    /*
     * Guard registry mutations while external storage callbacks execute.
     * The registry is single-owner; this flag is a reentrancy guard, not a
     * substitute for synchronization between concurrent threads.
     */
    if (manager->transaction_active != 0u)
        return PROS_APP_ESTATE;
    manager->transaction_active = 1u;

    if (ops->stage(ops->context, &manifest) != 0) {
        rc = install_rollback(ops, &manifest);
        manager->transaction_active = 0u;
        return rc == PROS_APP_OK ? PROS_APP_EIO : PROS_APP_EROLLBACK;
    }

    if (ops->publish(ops->context, &manifest) != 0) {
        rc = install_rollback(ops, &manifest);
        manager->transaction_active = 0u;
        return rc == PROS_APP_OK ? PROS_APP_EIO : PROS_APP_EROLLBACK;
    }

    manager->transaction_active = 0u;
    rc = pros_app_commit_install(manager, app_id);
    if (rc != PROS_APP_OK) {
        if (install_rollback(ops, &manifest) != PROS_APP_OK)
            return PROS_APP_EROLLBACK;
        return rc;
    }

    return PROS_APP_OK;
}
