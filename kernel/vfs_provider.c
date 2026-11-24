#include "vfs_provider.h"
#include "vfs.h"

#define PROS_VFS_PROVIDER_OK       0
#define PROS_VFS_PROVIDER_EINVAL  -1
#define PROS_VFS_PROVIDER_EBUSY   -2
#define PROS_VFS_PROVIDER_ENOSPC  -3
#define PROS_VFS_PROVIDER_ENOENT  -4
#define PROS_VFS_PROVIDER_MOUNTING 0x80000000u
#define PROS_VFS_PROVIDER_MOUNT_COUNT_MASK 0x7FFFFFFFu

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
    for (uint32_t i = 0u; i <= PROSENJIT_VFS_PROVIDER_NAME_MAX; ++i) {
        if (name[i] == '\0')
            return 1;
    }
    return 0;
}

static int provider_ops_valid(const struct pros_vfs_provider_ops *ops)
{
    return ops && ops->probe && ops->mount && ops->sync && ops->unmount;
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

    /* Registration starts with no live mounts, regardless of caller storage. */
    __atomic_store_n(&provider->active_mounts, 0u, __ATOMIC_RELEASE);
    providers[provider_count++] = provider;
    return PROS_VFS_PROVIDER_OK;
}

int pros_vfs_provider_unregister(struct pros_vfs_provider *provider)
{
    int index = provider_index(provider);
    uint32_t expected = 0u;
    if (index < 0)
        return PROS_VFS_PROVIDER_ENOENT;

    /* Close mount admission only when no mounts remain. */
    if (!__atomic_compare_exchange_n(&provider->active_mounts, &expected,
                                     PROS_VFS_PROVIDER_MOUNTING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return PROS_VFS_PROVIDER_EBUSY;

    for (uint32_t i = (uint32_t)index + 1u; i < provider_count; ++i)
        providers[i - 1u] = providers[i];
    providers[--provider_count] = 0;
    return PROS_VFS_PROVIDER_OK;
}


int pros_vfs_provider_mount_acquire(struct pros_vfs_provider *provider)
{
    uint32_t state;
    if (provider_index(provider) < 0)
        return PROS_VFS_PROVIDER_ENOENT;
    state = __atomic_load_n(&provider->active_mounts, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((state & PROS_VFS_PROVIDER_MOUNTING) != 0u)
            return PROS_VFS_PROVIDER_EBUSY;
        if ((state & PROS_VFS_PROVIDER_MOUNT_COUNT_MASK) ==
            PROS_VFS_PROVIDER_MOUNT_COUNT_MASK)
            return PROS_VFS_PROVIDER_ENOSPC;
        if (__atomic_compare_exchange_n(&provider->active_mounts, &state,
                                        state + 1u, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return PROS_VFS_PROVIDER_OK;
    }
}

int pros_vfs_provider_mount_release(struct pros_vfs_provider *provider)
{
    uint32_t state;
    if (!provider)
        return PROS_VFS_PROVIDER_EINVAL;
    state = __atomic_load_n(&provider->active_mounts, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((state & PROS_VFS_PROVIDER_MOUNTING) != 0u ||
            (state & PROS_VFS_PROVIDER_MOUNT_COUNT_MASK) == 0u)
            return PROS_VFS_PROVIDER_EINVAL;
        if (__atomic_compare_exchange_n(&provider->active_mounts, &state,
                                        state - 1u, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return PROS_VFS_PROVIDER_OK;
    }
}


int pros_vfs_provider_mount_device(
    struct pros_vfs_provider *provider, struct pros_vfs_mount *mount,
    struct pros_block_device *device, struct pros_partition_device *view,
    const struct pros_partition *partition, uint8_t read_only)
{
    struct pros_block_device *mount_device = view ? &view->device : device;
    int rc;

    if (!provider || !mount || !device || (view && !partition) ||
        (partition && !view) || mount->active || mount->root ||
        mount->provider || mount->device || mount->private_data ||
        __atomic_load_n(&mount->lifecycle_state, __ATOMIC_ACQUIRE) != 0u)
        return PROS_VFS_EINVAL;

    rc = pros_vfs_provider_mount_acquire(provider);
    if (rc != PROS_VFS_PROVIDER_OK)
        return rc;

    if ((provider->flags & PROS_VFS_PROVIDER_READ_ONLY) != 0u)
        read_only = 1u;
    rc = provider->ops->mount(provider, mount, device, view, partition,
                              read_only ? 1u : 0u);
    if (rc != PROS_VFS_PROVIDER_OK) {
        /*
         * A conforming provider rolls back before returning failure. Defend
         * against a callback that leaks partial mount state: retain the pin
         * and attach the provider so explicit unmount can abort and clean up.
         */
        if (mount->active || mount->root || mount->device ||
            mount->private_data || mount->provider ||
            __atomic_load_n(&mount->lifecycle_state, __ATOMIC_ACQUIRE) != 0u) {
            mount->provider = provider;
            return rc;
        }
        (void)pros_vfs_provider_mount_release(provider);
        return rc;
    }

    /* Providers prepare an inactive mount and root; the bridge activates it. */
    if (!mount->device || mount->device != mount_device || !mount->root ||
        !mount->root->initialized ||
        mount->root->type != PROS_VFS_NODE_DIRECTORY || mount->active ||
        mount->provider ||
        __atomic_load_n(&mount->lifecycle_state, __ATOMIC_ACQUIRE) != 0u) {
        /*
         * A successful callback owns any partial allocations on contract
         * violation; do not release the pin while its private state survives.
         */
        mount->provider = provider;
        return PROS_VFS_EINVAL;
    }

    mount->provider = provider;
    if (read_only ||
        (provider->flags & PROS_VFS_PROVIDER_READ_ONLY) != 0u)
        mount->read_only = 1u;
    rc = vfs_mount_activate(mount);
    if (rc != PROS_VFS_OK) {
        /* Keep the pin and provider association for explicit rollback. */
        return rc;
    }
    return PROS_VFS_PROVIDER_OK;
}

int pros_vfs_provider_unmount_device(struct pros_vfs_mount *mount)
{
    struct pros_vfs_provider *provider;
    int rc;

    if (!mount || !mount->provider)
        return PROS_VFS_PROVIDER_EINVAL;
    provider = mount->provider;

    /* Flush before draining VFS references; a failure leaves the mount live. */
    if (mount->active) {
        rc = provider->ops->sync(provider, mount);
        if (rc != PROS_VFS_PROVIDER_OK)
            return rc;
        rc = vfs_mount_deactivate_provider(mount, provider);
    } else {
        rc = vfs_mount_abort_provider(mount, provider);
    }
    if (rc != PROS_VFS_OK)
        return rc;

    /* Root vnode teardown is complete; provider-private state may now die. */
    rc = provider->ops->unmount(provider, mount);
    if (rc != PROS_VFS_PROVIDER_OK)
        return rc;

    mount->private_data = 0;
    mount->provider = 0;
    rc = pros_vfs_provider_mount_release(provider);
    return rc;
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


int pros_vfs_provider_probe_device(struct pros_block_device *device,
                                   uint32_t required_flags,
                                   struct pros_vfs_provider **result)
{
    if (!device || !result)
        return PROS_VFS_PROVIDER_EINVAL;
    *result = 0;

    for (uint32_t i = 0u; i < provider_count; ++i) {
        struct pros_vfs_provider *provider = providers[i];
        int rc;
        if (!provider || !provider_ops_valid(provider->ops))
            return PROS_VFS_PROVIDER_EINVAL;
        if ((provider->flags & required_flags) != required_flags)
            continue;
        rc = provider->ops->probe(device);
        if (rc == PROS_VFS_PROVIDER_OK) {
            *result = provider;
            return PROS_VFS_PROVIDER_OK;
        }
        if (rc == PROS_VFS_PROVIDER_NO_MATCH)
            continue;
        return rc;
    }
    return PROS_VFS_PROVIDER_NO_MATCH;
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

static int provider_test_sync(struct pros_vfs_provider *provider,
                              struct pros_vfs_mount *mount)
{
    return provider && mount ? 0 : PROS_VFS_PROVIDER_EINVAL;
}

static int provider_test_unmount(struct pros_vfs_provider *provider,
                                 struct pros_vfs_mount *mount)
{
    return provider && mount ? 0 : PROS_VFS_PROVIDER_EINVAL;
}


static struct pros_vfs_vnode provider_bridge_test_root;
static int provider_bridge_sync_result;
static int provider_bridge_unmount_result;
static uint32_t provider_bridge_sync_calls;
static uint32_t provider_bridge_unmount_calls;

static int provider_bridge_test_lookup(struct pros_vfs_vnode *directory,
                                       const char *name,
                                       struct pros_vfs_vnode **result)
{
    (void)directory;
    (void)name;
    if (result)
        *result = 0;
    return PROS_VFS_ENOENT;
}

static int provider_bridge_test_readdir(struct pros_vfs_vnode *directory,
                                        uint64_t *cookie,
                                        struct pros_vfs_dirent *entry)
{
    (void)directory;
    (void)cookie;
    (void)entry;
    return PROS_VFS_ENOENT;
}

static const struct pros_vfs_vnode_ops provider_bridge_test_vnode_ops = {
    .lookup = provider_bridge_test_lookup,
    .readdir = provider_bridge_test_readdir
};

static int provider_bridge_test_read(struct pros_block_device *device,
                                     uint64_t lba, uint32_t count,
                                     void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    return PROS_BLOCK_OK;
}

static int provider_bridge_test_mount(struct pros_vfs_provider *provider,
                                      struct pros_vfs_mount *mount,
                                      struct pros_block_device *device,
                                      struct pros_partition_device *view,
                                      const struct pros_partition *partition,
                                      uint8_t read_only)
{
    struct pros_block_device *mount_device = view ? &view->device : device;
    int rc;
    if (!provider || !mount || !mount_device || (view && !partition) ||
        (!view && partition))
        return PROS_VFS_PROVIDER_EINVAL;
    rc = vfs_mount_init(mount, PROS_FS_PROVIDER, mount_device, provider,
                        read_only);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_vnode_init(&provider_bridge_test_root, mount, 0,
                        PROS_VFS_NODE_DIRECTORY,
                        &provider_bridge_test_vnode_ops, 0, provider, 0u);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_mount_set_root(mount, &provider_bridge_test_root);
    if (rc != PROS_VFS_OK)
        return rc;
    (void)vfs_vnode_put(&provider_bridge_test_root);
    return PROS_VFS_PROVIDER_OK;
}

static int provider_bridge_test_mount_incomplete(
    struct pros_vfs_provider *provider, struct pros_vfs_mount *mount,
    struct pros_block_device *device, struct pros_partition_device *view,
    const struct pros_partition *partition, uint8_t read_only)
{
    struct pros_block_device *mount_device = view ? &view->device : device;
    (void)partition;
    if (!provider || !mount || !mount_device)
        return PROS_VFS_PROVIDER_EINVAL;
    /*
     * Deliberately violate the successful-mount contract by omitting a root.
     * The bridge must retain the provider pin for explicit unmount rollback.
     */
    return vfs_mount_init(mount, PROS_FS_PROVIDER, mount_device, provider,
                          read_only) == PROS_VFS_OK ?
           PROS_VFS_PROVIDER_OK : PROS_VFS_PROVIDER_EINVAL;
}

static uint32_t provider_bridge_incomplete_unmount_calls;

static int provider_bridge_test_incomplete_unmount(
    struct pros_vfs_provider *provider, struct pros_vfs_mount *mount)
{
    if (!provider || !mount || mount->active || mount->root ||
        mount->device != 0 || mount->provider != provider)
        return PROS_VFS_PROVIDER_EINVAL;
    ++provider_bridge_incomplete_unmount_calls;
    mount->private_data = 0;
    return PROS_VFS_PROVIDER_OK;
}

static uint32_t provider_bridge_partial_unmount_calls;
static uint32_t provider_bridge_partial_mount_cookie;

static int provider_bridge_test_mount_partial_failure(
    struct pros_vfs_provider *provider, struct pros_vfs_mount *mount,
    struct pros_block_device *device, struct pros_partition_device *view,
    const struct pros_partition *partition, uint8_t read_only)
{
    struct pros_block_device *mount_device = view ? &view->device : device;
    (void)partition;
    if (!provider || !mount || !mount_device)
        return PROS_VFS_PROVIDER_EINVAL;
    if (vfs_mount_init(mount, PROS_FS_PROVIDER, mount_device, provider,
                       read_only) != PROS_VFS_OK)
        return PROS_VFS_PROVIDER_EINVAL;
    mount->private_data = &provider_bridge_partial_mount_cookie;
    return -74;
}

static int provider_bridge_test_partial_unmount(
    struct pros_vfs_provider *provider, struct pros_vfs_mount *mount)
{
    if (!provider || !mount || mount->active || mount->root ||
        mount->device || mount->provider != provider ||
        mount->private_data != &provider_bridge_partial_mount_cookie)
        return PROS_VFS_PROVIDER_EINVAL;
    ++provider_bridge_partial_unmount_calls;
    mount->private_data = 0;
    return PROS_VFS_PROVIDER_OK;
}

static int provider_bridge_test_mount_failure(
    struct pros_vfs_provider *provider, struct pros_vfs_mount *mount,
    struct pros_block_device *device, struct pros_partition_device *view,
    const struct pros_partition *partition, uint8_t read_only)
{
    (void)mount;
    (void)device;
    (void)view;
    (void)partition;
    (void)read_only;
    return provider ? -73 : PROS_VFS_PROVIDER_EINVAL;
}

static int provider_bridge_test_sync(struct pros_vfs_provider *provider,
                                     struct pros_vfs_mount *mount)
{
    if (!provider || !mount)
        return PROS_VFS_PROVIDER_EINVAL;
    ++provider_bridge_sync_calls;
    return provider_bridge_sync_result;
}

static int provider_bridge_test_unmount(struct pros_vfs_provider *provider,
                                        struct pros_vfs_mount *mount)
{
    if (!provider || !mount)
        return PROS_VFS_PROVIDER_EINVAL;
    ++provider_bridge_unmount_calls;
    return provider_bridge_unmount_result;
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
        .sync = provider_test_sync,
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
            .sync = provider_test_sync, .unmount = provider_test_unmount
        };
        static const struct pros_vfs_provider_ops no_mount = {
            .probe = provider_test_probe, .mount = 0,
            .sync = provider_test_sync, .unmount = provider_test_unmount
        };
        static const struct pros_vfs_provider_ops no_sync = {
            .probe = provider_test_probe, .mount = provider_test_mount,
            .sync = 0, .unmount = provider_test_unmount
        };
        static const struct pros_vfs_provider_ops no_unmount = {
            .probe = provider_test_probe, .mount = provider_test_mount,
            .sync = provider_test_sync, .unmount = 0
        };
        invalid.ops = &no_probe;
        if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
            return -9;
        invalid.ops = &no_mount;
        if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
            return -10;
        invalid.ops = &no_sync;
        if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
            return -11;
        invalid.ops = &no_unmount;
        if (pros_vfs_provider_register(&invalid) != PROS_VFS_PROVIDER_EINVAL)
            return -12;
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
     * Exercise mount pins without requiring a spare registry slot. If a real
     * provider exists, pin it and verify unregister is busy without removing
     * it. Otherwise use a temporary provider and test the full lifecycle.
     */
    {
        struct pros_vfs_provider callback_provider = {
            .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
            .flags = 0u,
            .name = "__vfs_provider_callback",
            .ops = &ops,
            .private_data = 0
        };
        struct pros_vfs_provider *pin_target = 0;
        uint8_t temporary = 0u;

        if (initial_count != 0u) {
            pin_target = snapshot[0];
        } else {
            if (pros_vfs_provider_register(&callback_provider) !=
                PROS_VFS_PROVIDER_OK)
                return -15;
            pin_target = &callback_provider;
            temporary = 1u;
        }

        if (pros_vfs_provider_mount_acquire(pin_target) !=
                PROS_VFS_PROVIDER_OK ||
            pros_vfs_provider_unregister(pin_target) !=
                PROS_VFS_PROVIDER_EBUSY ||
            pros_vfs_provider_mount_release(pin_target) !=
                PROS_VFS_PROVIDER_OK ||
            pros_vfs_provider_mount_release(pin_target) !=
                PROS_VFS_PROVIDER_EINVAL)
            return -15;

        if (temporary) {
            if (pros_vfs_provider_unregister(pin_target) !=
                    PROS_VFS_PROVIDER_OK ||
                pros_vfs_provider_mount_acquire(pin_target) !=
                    PROS_VFS_PROVIDER_ENOENT)
                return -15;
        }

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


    /*
     * Exercise the real bridge when a registry slot is available. This
     * verifies sync-before-drain, direct-deactivate rejection, cleanup retry,
     * and balanced provider pins using a synthetic in-memory root vnode.
     */
    if (provider_count < PROSENJIT_VFS_PROVIDER_MAX) {
        static const struct pros_vfs_provider_ops bridge_ops = {
            .probe = provider_test_probe,
            .mount = provider_bridge_test_mount,
            .sync = provider_bridge_test_sync,
            .unmount = provider_bridge_test_unmount
        };
        static struct pros_vfs_provider bridge_provider = {
            .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
            .flags = 0u,
            .name = "__vfs_provider_bridge_test",
            .ops = &bridge_ops,
            .private_data = 0
        };
        static struct pros_vfs_mount bridge_mount;
        static struct pros_block_device bridge_device;

        if (pros_vfs_provider_register(&bridge_provider) !=
                PROS_VFS_PROVIDER_OK ||
            block_device_init(&bridge_device, 512u, 8u,
                              provider_bridge_test_read, 0, 0) !=
                PROS_BLOCK_OK)
            return -18;

        provider_bridge_sync_result = -71;
        provider_bridge_unmount_result = -72;
        provider_bridge_sync_calls = 0u;
        provider_bridge_unmount_calls = 0u;

        if (pros_vfs_provider_mount_device(&bridge_provider, &bridge_mount,
                                           &bridge_device, 0, 0, 0u) !=
                PROS_VFS_PROVIDER_OK ||
            !bridge_mount.active || bridge_mount.provider != &bridge_provider ||
            __atomic_load_n(&bridge_provider.active_mounts,
                            __ATOMIC_ACQUIRE) != 1u ||
            vfs_mount_deactivate(&bridge_mount) != PROS_VFS_EBUSY)
            return -19;

        if (pros_vfs_provider_unmount_device(&bridge_mount) != -71 ||
            !bridge_mount.active || provider_bridge_unmount_calls != 0u)
            return -20;

        provider_bridge_sync_result = PROS_VFS_PROVIDER_OK;
        if (pros_vfs_provider_unmount_device(&bridge_mount) != -72 ||
            bridge_mount.active || bridge_mount.provider != &bridge_provider ||
            __atomic_load_n(&bridge_provider.active_mounts,
                            __ATOMIC_ACQUIRE) != 1u)
            return -21;

        provider_bridge_unmount_result = PROS_VFS_PROVIDER_OK;
        if (pros_vfs_provider_unmount_device(&bridge_mount) !=
                PROS_VFS_PROVIDER_OK ||
            bridge_mount.provider || bridge_mount.private_data ||
            __atomic_load_n(&bridge_provider.active_mounts,
                            __ATOMIC_ACQUIRE) != 0u ||
            provider_bridge_sync_calls != 2u ||
            provider_bridge_unmount_calls != 2u ||
            pros_vfs_provider_unregister(&bridge_provider) !=
                PROS_VFS_PROVIDER_OK)
            return -22;
    }

    /*
     * A mount callback that fails before publishing resources must release
     * the bridge's temporary provider pin and leave the caller's mount
     * reusable. This protects the callback-failure rollback boundary.
     */
    {
        static const struct pros_vfs_provider_ops failing_mount_ops = {
            .probe = provider_test_probe,
            .mount = provider_bridge_test_mount_failure,
            .sync = provider_test_sync,
            .unmount = provider_test_unmount
        };
        static struct pros_vfs_provider failing_mount_provider = {
            .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
            .flags = 0u,
            .name = "__vfs_provider_mount_fail",
            .ops = &failing_mount_ops,
            .private_data = 0
        };
        struct pros_vfs_mount failed_mount = {0};
        struct pros_block_device failed_device = {0};

        if (pros_vfs_provider_register(&failing_mount_provider) !=
                PROS_VFS_PROVIDER_OK ||
            block_device_init(&failed_device, 512u, 8u,
                              provider_bridge_test_read, 0, 0) !=
                PROS_BLOCK_OK)
            return -23;

        if (pros_vfs_provider_mount_device(&failing_mount_provider,
                                           &failed_mount, &failed_device,
                                           0, 0, 0u) != -73 ||
            __atomic_load_n(&failing_mount_provider.active_mounts,
                            __ATOMIC_ACQUIRE) != 0u ||
            failed_mount.active || failed_mount.root ||
            failed_mount.provider || failed_mount.device ||
            failed_mount.private_data ||
            pros_vfs_provider_unregister(&failing_mount_provider) !=
                PROS_VFS_PROVIDER_OK)
            return -24;
    }

    /*
     * Defensive rollback for a provider that violates the failure contract
     * and returns an error with partial mount state still published.
     */
    {
        static const struct pros_vfs_provider_ops partial_failure_ops = {
            .probe = provider_test_probe,
            .mount = provider_bridge_test_mount_partial_failure,
            .sync = provider_test_sync,
            .unmount = provider_bridge_test_partial_unmount
        };
        static struct pros_vfs_provider partial_failure_provider = {
            .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
            .flags = 0u,
            .name = "__vfs_provider_partial_fail",
            .ops = &partial_failure_ops,
            .private_data = 0
        };
        struct pros_vfs_mount partial_mount = {0};
        struct pros_block_device partial_device = {0};

        provider_bridge_partial_unmount_calls = 0u;
        if (pros_vfs_provider_register(&partial_failure_provider) !=
                PROS_VFS_PROVIDER_OK ||
            block_device_init(&partial_device, 512u, 8u,
                              provider_bridge_test_read, 0, 0) !=
                PROS_BLOCK_OK)
            return -27;

        if (pros_vfs_provider_mount_device(&partial_failure_provider,
                                           &partial_mount, &partial_device,
                                           0, 0, 0u) != -74 ||
            partial_mount.active || partial_mount.root ||
            partial_mount.provider != &partial_failure_provider ||
            partial_mount.device != &partial_device ||
            partial_mount.private_data !=
                &provider_bridge_partial_mount_cookie ||
            __atomic_load_n(&partial_failure_provider.active_mounts,
                            __ATOMIC_ACQUIRE) != 1u ||
            pros_vfs_provider_unmount_device(&partial_mount) !=
                PROS_VFS_PROVIDER_OK ||
            partial_mount.active || partial_mount.root ||
            partial_mount.provider || partial_mount.device ||
            partial_mount.private_data ||
            __atomic_load_n(&partial_failure_provider.active_mounts,
                            __ATOMIC_ACQUIRE) != 0u ||
            provider_bridge_partial_unmount_calls != 1u ||
            pros_vfs_provider_unregister(&partial_failure_provider) !=
                PROS_VFS_PROVIDER_OK)
            return -28;
    }

    /*
     * A callback that reports success without publishing a valid root is a
     * provider contract violation. The bridge must retain the provider pin
     * and association until explicit rollback invokes provider unmount.
     */
    {
        static const struct pros_vfs_provider_ops incomplete_mount_ops = {
            .probe = provider_test_probe,
            .mount = provider_bridge_test_mount_incomplete,
            .sync = provider_test_sync,
            .unmount = provider_bridge_test_incomplete_unmount
        };
        static struct pros_vfs_provider incomplete_mount_provider = {
            .abi_version = PROSENJIT_VFS_PROVIDER_ABI_VERSION,
            .flags = 0u,
            .name = "__vfs_provider_incomplete",
            .ops = &incomplete_mount_ops,
            .private_data = 0
        };
        struct pros_vfs_mount incomplete_mount = {0};
        struct pros_block_device incomplete_device = {0};

        provider_bridge_incomplete_unmount_calls = 0u;
        if (pros_vfs_provider_register(&incomplete_mount_provider) !=
                PROS_VFS_PROVIDER_OK ||
            block_device_init(&incomplete_device, 512u, 8u,
                              provider_bridge_test_read, 0, 0) !=
                PROS_BLOCK_OK)
            return -25;

        if (pros_vfs_provider_mount_device(&incomplete_mount_provider,
                                           &incomplete_mount,
                                           &incomplete_device, 0, 0, 0u) !=
                PROS_VFS_EINVAL ||
            incomplete_mount.active || incomplete_mount.root ||
            incomplete_mount.provider != &incomplete_mount_provider ||
            incomplete_mount.device != &incomplete_device ||
            __atomic_load_n(&incomplete_mount_provider.active_mounts,
                            __ATOMIC_ACQUIRE) != 1u ||
            pros_vfs_provider_unmount_device(&incomplete_mount) !=
                PROS_VFS_PROVIDER_OK ||
            incomplete_mount.active || incomplete_mount.root ||
            incomplete_mount.provider || incomplete_mount.device ||
            incomplete_mount.private_data ||
            __atomic_load_n(&incomplete_mount_provider.active_mounts,
                            __ATOMIC_ACQUIRE) != 0u ||
            provider_bridge_incomplete_unmount_calls != 1u ||
            pros_vfs_provider_unregister(&incomplete_mount_provider) !=
                PROS_VFS_PROVIDER_OK)
            return -26;
    }

    return PROS_VFS_PROVIDER_OK;

fail:
    while (test_count > 0u) {
        (void)pros_vfs_provider_unregister(&tests[test_count - 1u]);
        --test_count;
    }
    return provider_registry_preserved(snapshot, initial_count) ? -16 : -17;
}
