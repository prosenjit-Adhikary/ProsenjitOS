#include "vfs_provider.h"

#define PROS_VFS_PROVIDER_OK       0
#define PROS_VFS_PROVIDER_EINVAL  -1
#define PROS_VFS_PROVIDER_EBUSY   -2
#define PROS_VFS_PROVIDER_ENOSPC  -3
#define PROS_VFS_PROVIDER_ENOENT  -4

static struct pros_vfs_provider *providers[PROSENJIT_VFS_PROVIDER_MAX];
static uint32_t provider_count;

static int provider_name_equal(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (*a != *b)
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

static int provider_name_valid(const char *name)
{
    if (!name || name[0] == '\0')
        return 0;
    /*
     * The storage is NAME_MAX + 1 bytes including the terminator. Inspect
     * exactly those bytes; never probe name[NAME_MAX + 1] when the name is
     * unterminated.
     */
    for (uint32_t i = 0u; i < PROSENJIT_VFS_PROVIDER_NAME_MAX + 1u; ++i) {
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
        if (provider_name_equal(providers[i]->name, name))
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

static void provider_test_name(char *name, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "__vfs_provider_selftest_";
    uint32_t i = 0u;

    while (prefix[i] != '\0') {
        name[i] = prefix[i];
        ++i;
    }
    name[i++] = hex[(value >> 4) & 0xFu];
    name[i++] = hex[value & 0xFu];
    name[i] = '\0';
}

static void provider_test_overflow_name(char *name, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "__vfs_provider_overflow_";
    uint32_t i = 0u;

    while (prefix[i] != '\0') {
        name[i] = prefix[i];
        ++i;
    }
    name[i++] = hex[(value >> 4) & 0xFu];
    name[i++] = hex[value & 0xFu];
    name[i] = '\0';
}

static int provider_registry_preserved(
    struct pros_vfs_provider *const *snapshot, uint32_t snapshot_count)
{
    if (provider_count != snapshot_count)
        return 0;
    for (uint32_t i = 0u; i < snapshot_count; ++i)
        if (providers[i] != snapshot[i])
            return 0;
    return 1;
}

int pros_vfs_provider_selftest(void)
{
    static const struct pros_vfs_provider_ops ops = {
        .probe = provider_test_probe,
        .mount = provider_test_mount,
        .unmount = provider_test_unmount
    };
    struct pros_vfs_provider *snapshot[PROSENJIT_VFS_PROVIDER_MAX];
    struct pros_vfs_provider tests[PROSENJIT_VFS_PROVIDER_MAX];
    struct pros_vfs_provider overflow;
    struct pros_vfs_provider invalid;
    struct pros_vfs_provider duplicate;
    uint32_t initial_count = provider_count;
    uint32_t test_count = 0u;

    /*
     * Snapshot exact live providers before mutation. The test providers are
     * appended after this prefix and are removed in reverse order, so real
     * providers are never unregistered or reordered by the self-test.
     */
    if (initial_count > PROSENJIT_VFS_PROVIDER_MAX)
        return -1;
    for (uint32_t i = 0u; i < initial_count; ++i) {
        if (!providers[i])
            return -2;
        snapshot[i] = providers[i];
    }

    if (pros_vfs_provider_register(0) != PROS_VFS_PROVIDER_EINVAL)
        return -3;

    invalid.abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION + 1u;
    invalid.flags = 0u;
    invalid.name[0] = 'x';
    invalid.name[1] = '\0';
    invalid.ops = &ops;
    invalid.private_data = 0;
    if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
        return -4;

    invalid.abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION;
    invalid.name[0] = '\0';
    if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
        return -5;

    for (uint32_t i = 0u; i <= PROSENJIT_VFS_PROVIDER_NAME_MAX; ++i)
        invalid.name[i] = 'x';
    if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
        return -6;

    invalid.name[0] = 'x';
    invalid.name[1] = '\0';
    invalid.flags = 1u << 31;
    if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
        return -7;

    invalid.flags = 0u;
    invalid.ops = 0;
    if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
        return -8;

    {
        static const struct pros_vfs_provider_ops no_probe = {
            .probe = 0, .mount = provider_test_mount,
            .unmount = provider_test_unmount
        };
        static const struct pros_vfs_provider_ops no_mount = {
            .probe = provider_test_probe, .mount = 0,
            .unmount = provider_test_unmount
        };
        static const struct pros_vfs_provider_ops no_unmount = {
            .probe = provider_test_probe, .mount = provider_test_mount,
            .unmount = 0
        };
        invalid.ops = &no_probe;
        if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
            return -9;
        invalid.ops = &no_mount;
        if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
            return -10;
        invalid.ops = &no_unmount;
        if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
            return -11;
    }

    if (pros_vfs_provider_find(0) != 0 ||
        pros_vfs_provider_find("") != 0)
        return -12;

    /*
     * Reserve a valid overflow name before filling the registry. Once all
     * slots are occupied, no name can be discovered by searching the live
     * registry. Keep this namespace separate from the normal test names.
     */
    overflow.abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION;
    overflow.flags = 0u;
    overflow.ops = &ops;
    overflow.private_data = 0;
    overflow.name[0] = '\0';
    for (uint32_t value = 0u; value < 256u; ++value) {
        provider_test_overflow_name(overflow.name, value);
        if (!pros_vfs_provider_find(overflow.name))
            break;
    }
    if (pros_vfs_provider_find(overflow.name) != 0)
        return -13;

    while (provider_count < PROSENJIT_VFS_PROVIDER_MAX) {
        struct pros_vfs_provider *p = &tests[test_count];
        p->abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION;
        p->flags = PROS_VFS_PROVIDER_PARTITION_AWARE |
                   PROS_VFS_PROVIDER_USERSPACE;
        p->ops = &ops;
        p->private_data = 0;

        for (uint32_t value = 0u; value < 256u; ++value) {
            provider_test_name(p->name, value);
            if (!pros_vfs_provider_find(p->name))
                break;
        }
        if (pros_vfs_provider_find(p->name) != 0)
            goto fail;

        if (pros_vfs_provider_register(p) != PROS_VFS_PROVIDER_OK)
            goto fail;
        ++test_count;

        if (pros_vfs_provider_find(p->name) != p)
            goto fail;
    }

    if (test_count != PROSENJIT_VFS_PROVIDER_MAX - initial_count)
        goto fail;

    if (initial_count != 0u)
        duplicate = *snapshot[0];
    else
        duplicate = tests[0];
    if (pros_vfs_provider_register(&duplicate) != PROS_VFS_PROVIDER_EBUSY)
        goto fail;

    invalid.abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION;
    invalid.flags = 0u;
    invalid.ops = &ops;
    invalid.private_data = 0;
    provider_test_name(invalid.name, 0xEEu);
    for (uint32_t value = 0u; value < 256u; ++value) {
        provider_test_name(invalid.name, value);
        if (!pros_vfs_provider_find(invalid.name))
            break;
    }
    if (pros_vfs_provider_find(invalid.name) != 0 ||
        pros_vfs_provider_unregister(&invalid) != PROS_VFS_PROVIDER_ENOENT)
        goto fail;

    if (pros_vfs_provider_register(&overflow) != PROS_VFS_PROVIDER_ENOSPC)
        goto fail;

    if (test_count != 0u)
    for (uint32_t cycle = 0u; cycle < 8u; ++cycle) {
        struct pros_vfs_provider repeat = {
            .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
            .flags = 0u,
            .name = { 0 },
            .ops = &ops,
            .private_data = 0
        };
        provider_test_name(repeat.name, 0x80u + cycle);

        if (pros_vfs_provider_unregister(&tests[test_count - 1u]) !=
            PROS_VFS_PROVIDER_OK)
            goto fail;
        --test_count;

        if (pros_vfs_provider_register(&repeat) != PROS_VFS_PROVIDER_OK ||
            pros_vfs_provider_find(repeat.name) != &repeat)
            goto fail;
        if (pros_vfs_provider_unregister(&repeat) != PROS_VFS_PROVIDER_OK ||
            pros_vfs_provider_find(repeat.name) != 0)
            goto fail;

        if (pros_vfs_provider_register(&tests[test_count]) !=
            PROS_VFS_PROVIDER_OK)
            goto fail;
        ++test_count;
    }

    for (uint32_t i = test_count; i > 0u; --i) {
        if (pros_vfs_provider_unregister(&tests[i - 1u]) !=
            PROS_VFS_PROVIDER_OK)
            return -13;
    }
    test_count = 0u;

    if (!provider_registry_preserved(snapshot, initial_count))
        return -14;

    /*
     * Provider lifetime is intentionally not inferred here: the current ABI
     * has no active-mount reference counter. These calls only validate that
     * the required callback ABI is callable and rejects invalid arguments.
     */
    {
        struct pros_vfs_provider callback_provider = {
            .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
            .flags = 0u,
            .name = "__vfs_provider_callback",
            .ops = &ops,
            .private_data = 0
        };
        if (ops.probe((struct pros_block_device *)1) != PROS_VFS_PROVIDER_OK ||
            ops.probe(0) != PROS_VFS_PROVIDER_EINVAL ||
            ops.mount(&callback_provider,
                      (struct pros_vfs_mount *)1,
                      (struct pros_block_device *)1,
                      0, 0, 0) != PROS_VFS_PROVIDER_OK ||
            ops.mount(0,
                      (struct pros_vfs_mount *)1,
                      (struct pros_block_device *)1,
                      0, 0, 0) != PROS_VFS_PROVIDER_EINVAL ||
            ops.unmount(&callback_provider, (struct pros_vfs_mount *)1) !=
                PROS_VFS_PROVIDER_OK ||
            ops.unmount(0, (struct pros_vfs_mount *)1) !=
                PROS_VFS_PROVIDER_EINVAL)
            return -15;
    }

    return PROS_VFS_PROVIDER_OK;

fail:
    while (test_count > 0u) {
        (void)pros_vfs_provider_unregister(&tests[test_count - 1u]);
        --test_count;
    }
    return provider_registry_preserved(snapshot, initial_count) ? -16 : -17;
}
