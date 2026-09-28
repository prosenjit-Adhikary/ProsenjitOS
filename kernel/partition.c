#include "partition.h"

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

int partition_probe(struct pros_block_device *device,
                    enum pros_partition_type *table_type)
{
    uint8_t sector[512];
    if (!device || !table_type || device->sector_size < sizeof(sector))
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 0, 1, sector) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (le16(sector + 510) != 0xAA55)
        return PROS_PARTITION_ENOTSUP;
    *table_type = PROS_PARTITION_MBR;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t *entry = sector + 446 + i * 16;
        if (entry[4] == 0xEE) {
            *table_type = PROS_PARTITION_GPT;
            break;
        }
    }
    return PROS_PARTITION_OK;
}

int partition_read_mbr(struct pros_block_device *device,
                       struct pros_partition *partitions,
                       uint32_t capacity, uint32_t *count)
{
    uint8_t sector[512];
    uint32_t found = 0;
    if (!device || !partitions || !count || capacity == 0 || device->sector_size < 512)
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 0, 1, sector) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (le16(sector + 510) != 0xAA55)
        return PROS_PARTITION_ENOTSUP;
    for (uint32_t i = 0; i < 4 && found < capacity; ++i) {
        const uint8_t *entry = sector + 446 + i * 16;
        uint32_t first = le32(entry + 8);
        uint32_t sectors = le32(entry + 12);
        if (entry[4] == 0 || sectors == 0)
            continue;
        partitions[found].first_lba = first;
        partitions[found].sector_count = sectors;
        partitions[found].mbr_type = entry[4];
        partitions[found].table_type = PROS_PARTITION_MBR;
        for (uint32_t b = 0; b < 16; ++b)
            partitions[found].type_guid[b] = 0;
        ++found;
    }
    *count = found;
    return PROS_PARTITION_OK;
}

int partition_read_gpt(struct pros_block_device *device,
                       struct pros_partition *partitions,
                       uint32_t capacity, uint32_t *count)
{
    uint8_t header[512];
    uint8_t entry[1024];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t found = 0;
    if (!device || !partitions || !count || capacity == 0 || device->sector_size < 512)
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 1, 1, header) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (header[0] != 'E' || header[1] != 'F' || header[2] != 'I' || header[3] != ' ' ||
        header[4] != 'P' || header[5] != 'A' || header[6] != 'R' || header[7] != 'T')
        return PROS_PARTITION_ENOTSUP;
    entries_lba = le64(header + 72);
    entry_count = le32(header + 80);
    entry_size = le32(header + 84);
    if (entry_size < 128 || entry_size > device->sector_size ||
        entry_size > 1024u || entries_lba >= device->sector_count)
        return PROS_PARTITION_EINVAL;
    for (uint32_t i = 0; i < entry_count && found < capacity; ++i) {
        uint64_t byte_offset = (uint64_t)i * entry_size;
        uint64_t lba = entries_lba + byte_offset / device->sector_size;
        uint32_t offset = (uint32_t)(byte_offset % device->sector_size);
        if (lba >= device->sector_count)
            return PROS_PARTITION_EINVAL;
        uint32_t sectors_to_read =
            (offset + entry_size > device->sector_size) ? 2u : 1u;
        if (sectors_to_read == 2u && lba + 1u >= device->sector_count)
            return PROS_PARTITION_EINVAL;
        if (block_device_read(device, lba, sectors_to_read, entry) != PROS_BLOCK_OK)
            return PROS_PARTITION_EIO;
        uint8_t empty = 1;
        for (uint32_t b = 0; b < 16; ++b)
            if (entry[offset + b] != 0) { empty = 0; break; }
        if (empty)
            continue;
        uint64_t first_lba = le64(entry + offset + 32);
        uint64_t last_lba = le64(entry + offset + 40);
        if (last_lba < first_lba || last_lba >= device->sector_count)
            return PROS_PARTITION_EINVAL;
        partitions[found].first_lba = first_lba;
        partitions[found].sector_count = last_lba - first_lba + 1u;
        partitions[found].mbr_type = 0;
        partitions[found].table_type = PROS_PARTITION_GPT;
        for (uint32_t b = 0; b < 16; ++b)
            partitions[found].type_guid[b] = entry[offset + b];
        ++found;
    }
    *count = found;
    return PROS_PARTITION_OK;
}

static int partition_device_read(struct pros_block_device *device,
                                 uint64_t lba, uint32_t count, void *buffer)
{
    struct pros_partition_device *view;
    if (!device || !buffer || !device->private_data)
        return PROS_BLOCK_EINVAL;
    view = (struct pros_partition_device *)device->private_data;
    if (!view->parent || lba > device->sector_count ||
        count > device->sector_count - lba)
        return PROS_BLOCK_EINVAL;
    return block_device_read(view->parent, view->first_lba + lba,
                             count, buffer);
}

static int partition_device_write(struct pros_block_device *device,
                                  uint64_t lba, uint32_t count,
                                  const void *buffer)
{
    struct pros_partition_device *view;
    if (!device || !buffer || !device->private_data)
        return PROS_BLOCK_EINVAL;
    view = (struct pros_partition_device *)device->private_data;
    if (!view->parent || !view->parent->write ||
        lba > device->sector_count || count > device->sector_count - lba)
        return PROS_BLOCK_EINVAL;
    return block_device_write(view->parent, view->first_lba + lba,
                              count, buffer);
}

int partition_device_init(struct pros_partition_device *view,
                          struct pros_block_device *parent,
                          const struct pros_partition *partition)
{
    if (!view || !parent || !partition || partition->sector_count == 0u ||
        partition->first_lba >= parent->sector_count ||
        partition->sector_count > parent->sector_count - partition->first_lba)
        return PROS_PARTITION_EINVAL;
    view->parent = parent;
    view->first_lba = partition->first_lba;
    if (block_device_init(&view->device, parent->sector_size,
                          partition->sector_count, partition_device_read,
                          parent->write ? partition_device_write : 0, view) !=
        PROS_BLOCK_OK)
        return PROS_PARTITION_EINVAL;
    return PROS_PARTITION_OK;
}

static uint8_t partition_test_media[4 * 512];

static int partition_test_read(struct pros_block_device *device,
                               uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    if (!buffer || lba > 4u || count > 4u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t i = 0; i < count * 512u; ++i)
        ((uint8_t *)buffer)[i] = partition_test_media[lba * 512u + i];
    return PROS_BLOCK_OK;
}

static int partition_test_write(struct pros_block_device *device,
                                uint64_t lba, uint32_t count,
                                const void *buffer)
{
    (void)device;
    if (!buffer || lba > 4u || count > 4u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t i = 0; i < count * 512u; ++i)
        partition_test_media[lba * 512u + i] = ((const uint8_t *)buffer)[i];
    return PROS_BLOCK_OK;
}

int partition_selftest(void)
{
    struct pros_block_device parent;
    struct pros_partition_device view;
    struct pros_partition part;
    uint8_t buffer[512];

    for (uint32_t i = 0; i < sizeof(partition_test_media); ++i)
        partition_test_media[i] = 0;
    partition_test_media[510] = 0x55;
    partition_test_media[511] = 0xAA;
    partition_test_media[446 + 4] = 0x0Bu;
    partition_test_media[446 + 8] = 1u;
    partition_test_media[446 + 12] = 2u;

    if (block_device_init(&parent, 512, 4, partition_test_read,
                          partition_test_write, 0) != PROS_BLOCK_OK)
        return -1;
    struct pros_partition partitions[4];
    uint32_t count = 0;
    if (partition_read_mbr(&parent, partitions, 4, &count) !=
            PROS_PARTITION_OK ||
        count != 1u || partitions[0].first_lba != 1u ||
        partitions[0].sector_count != 2u)
        return -1;
    part = partitions[0];
    if (partition_device_init(&view, &parent, &part) != PROS_PARTITION_OK)
        return -1;
    if (block_device_read(&view.device, 0, 1, buffer) != PROS_BLOCK_OK)
        return -1;
    if (buffer[0] != partition_test_media[512])
        return -1;
    if (block_device_read(&view.device, 2, 1, buffer) != PROS_BLOCK_EINVAL)
        return -1;
    return PROS_PARTITION_OK;
}
