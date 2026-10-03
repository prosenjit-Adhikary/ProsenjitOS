#include "fs_provider.h"

static struct pros_fs_provider *providers[PROS_FS_PROVIDER_REGISTRY_MAX];

static int provider_valid(const struct pros_fs_provider *provider)
{
    if (!provider ||
        !provider->transport || !provider->transport->request)
        return 0;
    if (provider->descriptor.abi_version != PROS_FS_PROVIDER_ABI_VERSION ||
        provider->descriptor.descriptor_size !=
            sizeof(struct pros_fs_provider_descriptor) ||
        provider->descriptor.provider_type == PROS_FS_PROVIDER_UNKNOWN)
        return 0;
    return 1;
}

int pros_fs_provider_registry_init(void)
{
    for (uint32_t i = 0u; i < PROS_FS_PROVIDER_REGISTRY_MAX; ++i)
        providers[i] = 0;
    return 0;
}

int pros_fs_provider_register(struct pros_fs_provider *provider)
{
    if (!provider_valid(provider))
        return PROS_FS_PROVIDER_EINVAL;

    for (uint32_t i = 0u; i < PROS_FS_PROVIDER_REGISTRY_MAX; ++i) {
        if (providers[i] &&
            providers[i]->descriptor.provider_type ==
                provider->descriptor.provider_type)
            return PROS_FS_PROVIDER_EBUSY;
    }

    for (uint32_t i = 0u; i < PROS_FS_PROVIDER_REGISTRY_MAX; ++i) {
        if (!providers[i]) {
            providers[i] = provider;
            provider->registered = 1u;
            return PROS_FS_PROVIDER_OK;
        }
    }
    return PROS_FS_PROVIDER_EBUSY;
}

int pros_fs_provider_unregister(struct pros_fs_provider *provider)
{
    if (!provider || !provider->registered)
        return PROS_FS_PROVIDER_EINVAL;

    for (uint32_t i = 0u; i < PROS_FS_PROVIDER_REGISTRY_MAX; ++i) {
        if (providers[i] == provider) {
            providers[i] = 0;
            provider->registered = 0u;
            return PROS_FS_PROVIDER_OK;
        }
    }
    return PROS_FS_PROVIDER_EINVAL;
}

const struct pros_fs_provider *
pros_fs_provider_find(uint32_t provider_type)
{
    if (provider_type == PROS_FS_PROVIDER_UNKNOWN)
        return 0;

    for (uint32_t i = 0u; i < PROS_FS_PROVIDER_REGISTRY_MAX; ++i)
        if (provider_valid(providers[i]) &&
            providers[i]->descriptor.provider_type == provider_type)
            return providers[i];

    return 0;
}

static int provider_request(const struct pros_fs_provider *provider,
                            uint32_t opcode,
                            uint64_t object_id,
                            uint64_t *value0)
{
    struct pros_fs_provider_request request = {0};
    struct pros_fs_provider_response response = {0};

    if (!provider_valid(provider))
        return PROS_FS_PROVIDER_EINVAL;

    request.opcode = opcode;
    request.size = sizeof(request);
    request.request_id = 1u;
    request.object_id = object_id;

    if (provider->transport->request(provider->transport->context,
                                     &request, &response) != 0)
        return PROS_FS_PROVIDER_EIO;

    if (response.request_id != request.request_id)
        return PROS_FS_PROVIDER_EIO;

    if (value0)
        *value0 = response.value0;
    return response.status;
}

int pros_fs_provider_probe(struct pros_block_device *device,
                           uint32_t provider_type)
{
    const struct pros_fs_provider *provider;
    uint64_t probe_id = 0u;

    if (!device)
        return PROS_FS_PROVIDER_EINVAL;

    provider = pros_fs_provider_find(provider_type);
    if (!provider)
        return PROS_FS_PROVIDER_ENOSYS;

    /*
     * The device is intentionally not encoded as a raw kernel pointer in
     * the provider ABI. A future transport supplies a capability-scoped
     * device reference through its context.
     */
    (void)device;
    return provider_request(provider, PROS_FS_PROVIDER_OP_PROBE, 0u,
                            &probe_id);
}

int pros_fs_provider_probe_any(struct pros_block_device *device,
                               uint32_t *provider_type)
{
    static const uint32_t order[] = {
        PROS_FS_PROVIDER_FAT32,
        PROS_FS_PROVIDER_EXFAT,
        PROS_FS_PROVIDER_NTFS
    };
    if (!device || !provider_type)
        return PROS_FS_PROVIDER_EINVAL;
    *provider_type = PROS_FS_PROVIDER_UNKNOWN;
    for (uint32_t i = 0u; i < sizeof(order) / sizeof(order[0]); ++i) {
        int rc = pros_fs_provider_probe(device, order[i]);
        if (rc == PROS_FS_PROVIDER_OK) {
            *provider_type = order[i];
            return PROS_FS_PROVIDER_OK;
        }
        if (rc != PROS_FS_PROVIDER_ENOSYS &&
            rc != PROS_FS_PROVIDER_ENOENT &&
            rc != PROS_FS_PROVIDER_ECORRUPT)
            return rc;
    }
    return PROS_FS_PROVIDER_ENOENT;
}

int pros_fs_provider_mount(struct pros_block_device *device,
                           uint32_t provider_type,
                           uint64_t *provider_mount_id)
{
    const struct pros_fs_provider *provider;
    uint64_t mount_id = 0u;
    int rc;

    if (!device || !provider_mount_id)
        return PROS_FS_PROVIDER_EINVAL;

    provider = pros_fs_provider_find(provider_type);
    if (!provider)
        return PROS_FS_PROVIDER_ENOSYS;

    (void)device;
    rc = provider_request(provider, PROS_FS_PROVIDER_OP_MOUNT, 0u,
                          &mount_id);
    if (rc != PROS_FS_PROVIDER_OK)
        return rc;

    if (mount_id == 0u)
        return PROS_FS_PROVIDER_EIO;

    *provider_mount_id = mount_id;
    return PROS_FS_PROVIDER_OK;
}

struct provider_test_context {
    uint32_t calls;
};

static int provider_test_block_read(struct pros_block_device *device,
                                    uint64_t lba, uint32_t count,
                                    void *buffer)
{
    (void)device; (void)lba; (void)count; (void)buffer;
    return PROS_BLOCK_OK;
}

static int provider_test_request(void *context,
                                 const struct pros_fs_provider_request *request,
                                 struct pros_fs_provider_response *response)
{
    struct provider_test_context *test = context;
    if (!test || !request || !response ||
        request->size != sizeof(*request))
        return -1;

    ++test->calls;
    response->request_id = request->request_id;
    response->status = PROS_FS_PROVIDER_OK;
    response->value0 = 0x50465331ULL;
    return 0;
}

int pros_fs_provider_selftest(void)
{
    struct provider_test_context context = {0};
    static const struct pros_fs_provider_transport transport = {
        provider_test_request, &context
    };
    struct pros_fs_provider provider = {0};
    struct pros_block_device device;
    uint64_t mount_id = 0u;

    provider.descriptor.abi_version = PROS_FS_PROVIDER_ABI_VERSION;
    provider.descriptor.provider_type = PROS_FS_PROVIDER_FAT32;
    provider.descriptor.descriptor_size =
        sizeof(provider.descriptor);
    provider.descriptor.capabilities =
        PROS_FS_PROVIDER_CAP_READ | PROS_FS_PROVIDER_CAP_STAT;
    provider.transport = &transport;

    if (pros_fs_provider_registry_init() != 0 ||
        pros_fs_provider_register(&provider) != PROS_FS_PROVIDER_OK ||
        pros_fs_provider_find(PROS_FS_PROVIDER_FAT32) != &provider)
        return -1;

    if (block_device_init(&device, 512u, 1u, provider_test_block_read,
                          0, 0) != PROS_BLOCK_OK)
        return -2;

    if (pros_fs_provider_probe(&device, PROS_FS_PROVIDER_FAT32) !=
            PROS_FS_PROVIDER_OK ||
        pros_fs_provider_mount(&device, PROS_FS_PROVIDER_FAT32,
                                &mount_id) != PROS_FS_PROVIDER_OK ||
        mount_id != 0x50465331ULL ||
        context.calls != 2u)
        return -3;

    if (pros_fs_provider_probe(&device, PROS_FS_PROVIDER_NTFS) !=
            PROS_FS_PROVIDER_ENOSYS ||
        pros_fs_provider_unregister(&provider) != PROS_FS_PROVIDER_OK ||
        pros_fs_provider_find(PROS_FS_PROVIDER_FAT32) != 0)
        return -4;

    return 0;
}
