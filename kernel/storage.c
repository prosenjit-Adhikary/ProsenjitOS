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
               PROS_STORAGE_EINVAL;

    rc = storage_read_partitions(device, table_type, partitions,
                                 PROS_STORAGE_MAX_VOLUMES, &partition_count);
    if (rc != PROS_PARTITION_OK)
        return rc == PROS_PARTITION_EIO ? PROS_STORAGE_EIO :
               PROS_STORAGE_EINVAL;

    for (uint32_t i = 0u; i < partition_count; ++i) {
        struct pros_partition_device view;
        enum pros_filesystem_type filesystem;

        if (vfs_probe_filesystem == 0)
            return PROS_STORAGE_EINVAL;

        rc = partition_device_init(&view, device, &partitions[i]);
        if (rc != PROS_PARTITION_OK)
            return rc == PROS_PARTITION_EIO ? PROS_STORAGE_EIO :
                   PROS_STORAGE_EINVAL;

        rc = vfs_probe_filesystem(&view.device, &filesystem);
        if (rc != PROS_VFS_OK)
            continue;

        if (filesystem != PROS_FS_FAT32 &&
            filesystem != PROS_FS_EXFAT &&
            filesystem != PROS_FS_NTFS)
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
    uint8_t sectors[6u * 512u];
};

static int storage_test_read(struct pros_block_device *device,
                             uint64_t lba, uint32_t count, void *buffer)
{
    struct storage_test_disk *disk =
        (struct storage_test_disk *)device->private_data;

    if (!disk || !buffer || lba > 6u || count > 6u - lba)
        return PROS_BLOCK_EINVAL;

    for (uint32_t i = 0u; i < count * 512u; ++i)
        ((uint8_t *)buffer)[i] = disk->sectors[lba * 512u + i];

    return PROS_BLOCK_OK;
}

static void storage_set_mbr_entry(uint8_t *sector, uint32_t index,
                                  uint8_t type, uint32_t first_lba,
                                  uint32_t sector_count)
{
    uint8_t *entry = sector + 446u + index * 16u;
    entry[4] = type;
    entry[8] = (uint8_t)first_lba;
    entry[9] = (uint8_t)(first_lba >> 8);
    entry[10] = (uint8_t)(first_lba >> 16);
    entry[11] = (uint8_t)(first_lba >> 24);
    entry[12] = (uint8_t)sector_count;
    entry[13] = (uint8_t)(sector_count >> 8);
    entry[14] = (uint8_t)(sector_count >> 16);
    entry[15] = (uint8_t)(sector_count >> 24);
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
    storage_set_mbr_entry(disk.sectors, 0u, 0x0Bu, 1u, 2u);
    storage_set_mbr_entry(disk.sectors, 1u, 0x07u, 3u, 2u);

    /* Partition 1: FAT32 probe. */
    {
        uint8_t *boot = disk.sectors + 512u;
        boot[3] = 'M'; boot[4] = 'S'; boot[5] = 'D'; boot[6] = 'O';
        boot[7] = 'S'; boot[11] = 0x00u; boot[12] = 0x02u;
        boot[13] = 0x01u; boot[510] = 0x55u; boot[511] = 0xAAu;
        boot[82] = 'F'; boot[83] = 'A'; boot[84] = 'T';
        boot[85] = '3'; boot[86] = '2';
    }

    /* Partition 2: exFAT probe. */
    {
        uint8_t *boot = disk.sectors + 3u * 512u;
        boot[3] = 'E'; boot[4] = 'X'; boot[5] = 'F';
        boot[6] = 'A'; boot[7] = 'T'; boot[8] = ' ';
        boot[9] = ' '; boot[10] = ' ';
        boot[11] = 0x00u; boot[12] = 0x02u;
        boot[13] = 0x01u; boot[510] = 0x55u; boot[511] = 0xAAu;
    }

    if (block_device_init(&device, 512u, 6u, storage_test_read, 0, &disk) !=
            PROS_BLOCK_OK)
        return -1;

    if (pros_storage_discover(&device, volumes, 2u, &count) !=
            PROS_STORAGE_OK)
        return -2;

    if (count != 2u ||
        volumes[0].partition.first_lba != 1u ||
        volumes[0].partition.sector_count != 2u ||
        volumes[0].filesystem != PROS_FS_FAT32 ||
        volumes[0].device.device.sector_count != 2u ||
        volumes[1].partition.first_lba != 3u ||
        volumes[1].filesystem != PROS_FS_EXFAT)
        return -3;

    /* Unknown filesystems are ignored, not guessed or mounted. */
    disk.sectors[3u * 512u + 3u] = 0u;
    if (pros_storage_discover(&device, volumes, 2u, &count) !=
            PROS_STORAGE_OK || count != 1u)
        return -4;

    /* A caller-provided capacity smaller than discovered volumes is safe. */
    disk.sectors[3u * 512u + 3u] = 'E';
    if (pros_storage_discover(&device, volumes, 1u, &count) !=
            PROS_STORAGE_EOVERFLOW)
        return -5;

    return PROS_STORAGE_OK;
}
