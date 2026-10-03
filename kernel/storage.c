#include "storage.h"

static int storage_partition_table(struct pros_block_device *device,
                                   enum pros_partition_type *type)
{
    int rc = partition_probe(device, type);
    if (rc != PROS_PARTITION_OK)
        return rc;
    return PROS_PARTITION_OK;
}

static int storage_read_partitions(struct pros_block_device *device,
                                   enum pros_partition_type type,
                                   struct pros_partition *partitions,
                                   uint32_t capacity,
                                   uint32_t *count)
{
    if (type == PROS_PARTITION_MBR)
        return partition_read_mbr(device, partitions, capacity, count);
    if (type == PROS_PARTITION_GPT)
        return partition_read_gpt(device, partitions, capacity, count);
    return PROS_PARTITION_ENOTSUP;
}

int pros_storage_discover(struct pros_block_device *device,
                          struct pros_storage_volume *volumes,
                          uint32_t capacity,
                          uint32_t *count)
{
    struct pros_partition partitions[PROS_STORAGE_MAX_VOLUMES];
    enum pros_partition_type table_type;
    uint32_t partition_count = 0u;
    uint32_t found = 0u;
    int rc;

    if (!device || !volumes || !count || capacity == 0u ||
        capacity > PROS_STORAGE_MAX_VOLUMES)
        return PROS_STORAGE_EINVAL;

    *count = 0u;
    rc = storage_partition_table(device, &table_type);
    if (rc != PROS_PARTITION_OK)
        return rc == PROS_PARTITION_EIO ? PROS_STORAGE_EIO :
               rc == PROS_PARTITION_ENOTSUP ? PROS_STORAGE_ENOTSUP :
               PROS_STORAGE_EINVAL;

    rc = storage_read_partitions(device, table_type, partitions,
                                 PROS_STORAGE_MAX_VOLUMES, &partition_count);
    if (rc != PROS_PARTITION_OK)
        return rc == PROS_PARTITION_EIO ? PROS_STORAGE_EIO :
               rc == PROS_PARTITION_ENOTSUP ? PROS_STORAGE_ENOTSUP :
               PROS_STORAGE_EINVAL;

    for (uint32_t i = 0u; i < partition_count; ++i) {
        struct pros_partition_device view;
        enum pros_filesystem_type filesystem;


        rc = partition_device_init(&view, device, &partitions[i]);
        if (rc != PROS_PARTITION_OK)
            return rc == PROS_PARTITION_EIO ? PROS_STORAGE_EIO :
                   PROS_STORAGE_EINVAL;

        rc = vfs_probe_filesystem(&view.device, &filesystem);
        if (rc != PROS_VFS_OK)
            continue;

        if (filesystem != PROS_FS_PROSENJIT)
            continue;

        if (found >= capacity)
            return PROS_STORAGE_EOVERFLOW;

        volumes[found].partition = partitions[i];
        volumes[found].device = view;
        volumes[found].filesystem = filesystem;
        ++found;
    }

    *count = found;
    return PROS_STORAGE_OK;
}

struct storage_test_disk {
    uint8_t sectors[3u * 512u];
};

static int storage_test_read(struct pros_block_device *device,
                             uint64_t lba, uint32_t count, void *buffer)
{
    struct storage_test_disk *disk =
        (struct storage_test_disk *)device->private_data;

    if (!disk || !buffer || lba > 3u || count > 3u - lba)
        return PROS_BLOCK_EINVAL;

    for (uint32_t i = 0u; i < count * 512u; ++i)
        ((uint8_t *)buffer)[i] = disk->sectors[lba * 512u + i];

    return PROS_BLOCK_OK;
}

int pros_storage_selftest(void)
{
    struct storage_test_disk disk;
    struct pros_block_device device;
    struct pros_storage_volume volumes[2];
    uint32_t count = 0u;

    for (uint32_t i = 0u; i < sizeof(disk.sectors); ++i)
        disk.sectors[i] = 0u;

    disk.sectors[510] = 0x55u;
    disk.sectors[511] = 0xAAu;
    disk.sectors[446 + 4] = 0x0Bu;
    disk.sectors[446 + 8] = 1u;
    disk.sectors[446 + 12] = 2u;

    if (block_device_init(&device, 512u, 3u, storage_test_read, 0, &disk) !=
            PROS_BLOCK_OK)
        return -1;

    /*
     * Compatibility filesystem probes are intentionally absent from the
     * kernel. A disk containing a legacy filesystem signature must therefore
     * produce no supported storage volume here.
     */
    if (pros_storage_discover(&device, volumes, 2u, &count) !=
            PROS_STORAGE_OK || count != 0u)
        return -2;

    return PROS_STORAGE_OK;
}
