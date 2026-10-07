#include "vfs_provider.h"

#define PROS_VFS_PROVIDER_OK       0
#define PROS_VFS_PROVIDER_EINVAL  -1
#define PROS_VFS_PROVIDER_EBUSY   -2
#define PROS_VFS_PROVIDER_ENOSPC  -3
#define PROS_VFS_PROVIDER_ENOENT  -4

static struct pros_vfs_provider *providers[PROSENJIT_VFS_PROVIDER_MAX];
static uint32_t provider_count;

static int provider_name_valid(const char *name)
{
    if (!name || name[0] == '\0')
        return 0;
    for (uint32_t i = 0u; i <= PROSENJIT_VFS_PROVIDER_NAME_MAX; ++i) {
        if (name[i] == '\0')
            return 1;
    }
    return 0;
}

static int provider_ops_valid(const struct pros_vfs_provider_ops *ops)
{
    return ops && ops->probe && ops->mount && ops->unmount;
}

static int provider_index(const struct pros_vfs_provider *provider)
{
    if (!provider)
        return -1;
    for (uint32_t i = 0u; i < provider_count; ++i)
        if (providers[i] == provider)
            return (int)i;
    return -1;
}

int pros_vfs_provider_register(struct pros_vfs_provider *provider)
{
    if (!provider ||
        provider->abi_version != PROSENJIT_VFS_PROVIDER_ABI_VERSION ||
        !provider_name_valid(provider->name) ||
        (provider->flags & ~(PROS_VFS_PROVIDER_READ_ONLY |
                             PROS_VFS_PROVIDER_PARTITION_AWARE |
                             PROS_VFS_PROVIDER_USERSPACE)) != 0u ||
        !provider_ops_valid(provider->ops))
        return PROS_VFS_PROVIDER_EINVAL;

    if (pros_vfs_provider_find(provider->name))
        return PROS_VFS_PROVIDER_EBUSY;
    if (provider_count >= PROSENJIT_VFS_PROVIDER_MAX)
        return PROS_VFS_PROVIDER_ENOSPC;

    providers[provider_count++] = provider;
    return PROS_VFS_PROVIDER_OK;
}

int pros_vfs_provider_unregister(struct pros_vfs_provider *provider)
{
    int index = provider_index(provider);
    if (index < 0)
        return PROS_VFS_PROVIDER_ENOENT;

    for (uint32_t i = (uint32_t)index + 1u; i < provider_count; ++i)
        providers[i - 1u] = providers[i];
    providers[--provider_count] = 0;
    return PROS_VFS_PROVIDER_OK;
}

const struct pros_vfs_provider *pros_vfs_provider_find(const char *name)
{
    if (!provider_name_valid(name))
        return 0;
    for (uint32_t i = 0u; i < provider_count; ++i)
        if (__builtin_strcmp(providers[i]->name, name) == 0)
            return providers[i];
    return 0;
}

static int provider_test_probe(struct pros_block_device *device)
{
    return device ? 0 : PROS_VFS_PROVIDER_EINVAL;
}

static int provider_test_mount(struct pros_vfs_provider *provider,
                               struct pros_vfs_mount *mount,
                               struct pros_block_device *device,
                               struct pros_partition_device *view,
                               const struct pros_partition *partition,
                               uint8_t read_only)
{
    (void)view;
    (void)partition;
    (void)read_only;
    return provider && mount && device ? 0 : PROS_VFS_PROVIDER_EINVAL;
}

static int provider_test_unmount(struct pros_vfs_provider *provider,
                                 struct pros_vfs_mount *mount)
{
    return provider && mount ? 0 : PROS_VFS_PROVIDER_EINVAL;
}

int pros_vfs_provider_selftest(void)
{
    static const struct pros_vfs_provider_ops ops = {
        .probe = provider_test_probe,
        .mount = provider_test_mount,
        .unmount = provider_test_unmount
    };
    static const char *const test_names[] = {
        "provider-selftest-0",
        "provider-selftest-1",
        "provider-selftest-2",
        "provider-selftest-3",
        "provider-selftest-4",
        "provider-selftest-5",
        "provider-selftest-6",
        "provider-selftest-7",
        "provider-selftest-8"
    };
    static const char *const capacity_names[] = {
        "provider-capacity-test-0",
        "provider-capacity-test-1",
        "provider-capacity-test-2",
        "provider-capacity-test-3",
        "provider-capacity-test-4",
        "provider-capacity-test-5",
        "provider-capacity-test-6",
        "provider-capacity-test-7",
        "provider-capacity-test-8"
    };
    struct pros_vfs_provider first = {
        .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
        .flags = PROS_VFS_PROVIDER_PARTITION_AWARE |
                 PROS_VFS_PROVIDER_USERSPACE,
        .name = "provider-selftest-0",
        .ops = &ops,
        .private_data = 0
    };
    struct pros_vfs_provider duplicate;
    struct pros_vfs_provider invalid = first;
    struct pros_vfs_provider temporary[PROSENJIT_VFS_PROVIDER_MAX];
    uint32_t temporary_count = 0u;
    uint32_t test_name_index = 0u;
    uint32_t capacity_name_index = 0u;
    uint32_t initial_count = provider_count;
    struct pros_vfs_provider *initial_providers[PROSENJIT_VFS_PROVIDER_MAX];

    for (uint32_t i = 0u; i < PROSENJIT_VFS_PROVIDER_MAX; ++i)
        initial_providers[i] = providers[i];

    /*
     * The self-test may run after real providers have registered. It must
     * exercise the registry without overwriting, unregistering, or otherwise
     * modifying any live provider.
     */
    if (pros_vfs_provider_register(0) != PROS_VFS_PROVIDER_EINVAL)
        return -1;

    invalid.abi_version++;
    if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
        return -2;

    /*
     * Exercise duplicate registration against an actual live provider when
     * one exists. The copy has the same name but a different address, so a
     * successful duplicate registration would expose a real name-collision
     * bug without requiring any mutation of the live provider.
     */
    if (initial_count != 0u) {
        duplicate = *providers[0];
        if (!providers[0]->name[0] ||
            pros_vfs_provider_find(providers[0]->name) != providers[0] ||
            pros_vfs_provider_register(&duplicate) != PROS_VFS_PROVIDER_EBUSY)
            return -3;
        if (pros_vfs_provider_unregister(&duplicate) != PROS_VFS_PROVIDER_ENOENT)
            return -4;
    }

    /*
     * When capacity exists, select a deterministic unused name. There are
     * MAX+1 candidates, so at least one is free whenever the registry has a
     * free slot. Never assume that a fixed self-test name is globally unused.
     */
    if (initial_count < PROSENJIT_VFS_PROVIDER_MAX) {
        while (test_name_index <
                   (uint32_t)(sizeof(test_names) / sizeof(test_names[0])) &&
               pros_vfs_provider_find(test_names[test_name_index]) != 0)
            ++test_name_index;
        if (test_name_index >=
            (uint32_t)(sizeof(test_names) / sizeof(test_names[0])))
            return -5;

        for (uint32_t i = 0u; i <= PROSENJIT_VFS_PROVIDER_NAME_MAX; ++i)
            first.name[i] = '\0';
        for (uint32_t i = 0u; test_names[test_name_index][i] != '\0'; ++i)
            first.name[i] = test_names[test_name_index][i];

        if (pros_vfs_provider_register(&first) != PROS_VFS_PROVIDER_OK ||
            pros_vfs_provider_find(first.name) != &first)
            goto fail;

        duplicate = first;
        if (pros_vfs_provider_register(&duplicate) != PROS_VFS_PROVIDER_EBUSY)
            goto fail;

        /*
         * Do not use a fixed "missing" name here: a legitimate live provider
         * could own that name. Select a deterministic name that is absent from
         * the registry at this point, just like the registration candidates.
         */
        test_name_index = 0u;
        while (test_name_index <
                   (uint32_t)(sizeof(test_names) / sizeof(test_names[0])) &&
               pros_vfs_provider_find(test_names[test_name_index]) != 0)
            ++test_name_index;
        if (test_name_index >=
            (uint32_t)(sizeof(test_names) / sizeof(test_names[0])))
            goto fail;
        if (pros_vfs_provider_find(test_names[test_name_index]) != 0)
            goto fail;

        if (pros_vfs_provider_unregister(&duplicate) !=
            PROS_VFS_PROVIDER_ENOENT)
            goto fail;
        if (pros_vfs_provider_unregister(&first) != PROS_VFS_PROVIDER_OK ||
            pros_vfs_provider_find(first.name) != 0)
            goto fail;
    }

    /*
     * Exercise the full-capacity boundary without permanently consuming any
     * registry slots. Temporary providers are all stack-owned and are removed
     * before returning. If the registry started full, the boundary is already
     * represented by the existing live providers and no mutation is needed.
     */
    while (provider_count < PROSENJIT_VFS_PROVIDER_MAX) {
        uint32_t selected = capacity_name_index;
        while (selected <
                   (uint32_t)(sizeof(capacity_names) / sizeof(capacity_names[0])) &&
               pros_vfs_provider_find(capacity_names[selected]) != 0)
            ++selected;
        if (selected >=
            (uint32_t)(sizeof(capacity_names) / sizeof(capacity_names[0])))
            goto fail;

        capacity_name_index = selected + 1u;
        temporary[temporary_count] = first;
        for (uint32_t i = 0u; i <= PROSENJIT_VFS_PROVIDER_NAME_MAX; ++i)
            temporary[temporary_count].name[i] = '\0';
        for (uint32_t i = 0u;
             capacity_names[selected][i] != '\0'; ++i)
            temporary[temporary_count].name[i] =
                capacity_names[selected][i];

        if (pros_vfs_provider_register(&temporary[temporary_count]) !=
            PROS_VFS_PROVIDER_OK)
            goto fail;
        ++temporary_count;
    }

    {
        struct pros_vfs_provider overflow = first;
        uint32_t selected = 0u;

        while (selected <
                   (uint32_t)(sizeof(capacity_names) / sizeof(capacity_names[0])) &&
               pros_vfs_provider_find(capacity_names[selected]) != 0)
            ++selected;
        if (selected >=
            (uint32_t)(sizeof(capacity_names) / sizeof(capacity_names[0])))
            goto fail;

        for (uint32_t i = 0u; i <= PROSENJIT_VFS_PROVIDER_NAME_MAX; ++i)
            overflow.name[i] = '\0';
        for (uint32_t i = 0u; capacity_names[selected][i] != '\0'; ++i)
            overflow.name[i] = capacity_names[selected][i];

        if (pros_vfs_provider_register(&overflow) != PROS_VFS_PROVIDER_ENOSPC)
            goto fail;
    }

    for (uint32_t i = 0u; i < temporary_count; ++i)
        if (pros_vfs_provider_unregister(&temporary[i]) !=
            PROS_VFS_PROVIDER_OK)
            goto fail;

    /*
     * The complete self-test must be observationally pure: every provider
     * pointer and the provider count must match the state at entry.
     */
    if (provider_count != initial_count)
        return -6;
    for (uint32_t i = 0u; i < PROSENJIT_VFS_PROVIDER_MAX; ++i)
        if (providers[i] != initial_providers[i])
            return -7;

    return PROS_VFS_PROVIDER_OK;

fail:
    while (temporary_count != 0u) {
        --temporary_count;
        (void)pros_vfs_provider_unregister(&temporary[temporary_count]);
    }
    (void)pros_vfs_provider_unregister(&first);
    if (provider_count != initial_count)
        return -8;
    for (uint32_t i = 0u; i < PROSENJIT_VFS_PROVIDER_MAX; ++i)
        if (providers[i] != initial_providers[i])
            return -9;
    return -10;
}
