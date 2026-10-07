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
    struct pros_vfs_provider first = {
        .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
        .flags = PROS_VFS_PROVIDER_PARTITION_AWARE |
                 PROS_VFS_PROVIDER_USERSPACE,
        .name = "provider-selftest",
        .ops = &ops,
        .private_data = 0
    };
    struct pros_vfs_provider duplicate = first;
    struct pros_vfs_provider invalid = first;

    /*
     * This self-test may run after real providers have registered.
     * It must validate the registry without requiring an empty registry
     * and without modifying live provider registrations.
     */
    if (pros_vfs_provider_register(0) != PROS_VFS_PROVIDER_EINVAL)
        return -1;

    invalid.abi_version++;
    if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
        return -2;

    /*
     * If the registry has capacity, exercise the complete mutation path
     * using a private test provider.  If it is full, the validation above
     * is still valid and the registry must remain untouched.
     */
    if (provider_count < PROSENJIT_VFS_PROVIDER_MAX) {
        if (pros_vfs_provider_find(first.name) != 0)
            return -3;

        if (pros_vfs_provider_register(&first) != PROS_VFS_PROVIDER_OK)
            return -4;

        if (pros_vfs_provider_find(first.name) != &first)
            return -5;

        if (pros_vfs_provider_register(&duplicate) != PROS_VFS_PROVIDER_EBUSY)
            goto fail_unregister;

        if (pros_vfs_provider_find("missing") != 0)
            goto fail_unregister;

        if (pros_vfs_provider_unregister(&duplicate) != PROS_VFS_PROVIDER_ENOENT)
            goto fail_unregister;

        if (pros_vfs_provider_unregister(&first) != PROS_VFS_PROVIDER_OK)
            return -6;

        if (pros_vfs_provider_find(first.name) != 0)
            return -7;
    }

    return PROS_VFS_PROVIDER_OK;

fail_unregister:
    (void)pros_vfs_provider_unregister(&first);
    return -8;
}
