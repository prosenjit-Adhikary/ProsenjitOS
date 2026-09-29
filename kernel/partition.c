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
    if (!device || !table_type || device->sector_size != sizeof(sector))
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 0, 1, sector) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (le16(sector + 510) != 0xAA55)
        return PROS_PARTITION_ENOTSUP;
    *table_type = PROS_PARTITION_MBR;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t *entry = sector + 446 + i * 16;
        if (entry[4] == 0xEE) {
            uint8_t header[512];
            if (block_device_read(device, 1, 1, header) != PROS_BLOCK_OK)
                return PROS_PARTITION_EIO;
            if (header[0] == 'E' && header[1] == 'F' && header[2] == 'I' &&
                header[3] == ' ' && header[4] == 'P' && header[5] == 'A' &&
                header[6] == 'R' && header[7] == 'T')
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
    if (!device || !partitions || !count || capacity == 0 || device->sector_size != 512u)
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 0, 1, sector) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (le16(sector + 510) != 0xAA55)
        return PROS_PARTITION_ENOTSUP;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t *entry = sector + 446 + i * 16;
        uint64_t first = le32(entry + 8);
        uint64_t sectors = le32(entry + 12);
        if (entry[4] == 0 || sectors == 0)
            continue;
        /* Never expose an MBR extent that wraps or exceeds the device. */
        if (first >= device->sector_count ||
            sectors > device->sector_count - first)
            return PROS_PARTITION_EINVAL;
        if (found >= capacity)
            continue;
        partitions[found].first_lba = first;
        partitions[found].sector_count = sectors;
        partitions[found].mbr_type = entry[4];
        partitions[found].table_type = PROS_PARTITION_MBR;
        for (uint32_t b = 0; b < 16; ++b)
            partitions[found].type_guid[b] = 0;
        ++found;
    }
    /* MBR primary entries must not overlap one another. */
    for (uint32_t i = 0; i < found; ++i) {
        uint64_t a_end = partitions[i].first_lba + partitions[i].sector_count;
        for (uint32_t j = i + 1u; j < found; ++j) {
            uint64_t b_end = partitions[j].first_lba + partitions[j].sector_count;
            if (partitions[i].first_lba < b_end &&
                partitions[j].first_lba < a_end)
                return PROS_PARTITION_EINVAL;
        }
    }
    *count = found;
    return PROS_PARTITION_OK;
}

int partition_read_gpt(struct pros_block_device *device,
                       struct pros_partition *partitions,
                       uint32_t capacity, uint32_t *count)
{
    uint8_t header[512];
    uint8_t entry[1536];
    uint64_t entries_lba;
    uint32_t header_size;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t found = 0;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    if (!device || !partitions || !count || capacity == 0 || device->sector_size != 512u)
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 1, 1, header) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (header[0] != 'E' || header[1] != 'F' || header[2] != 'I' || header[3] != ' ' ||
        header[4] != 'P' || header[5] != 'A' || header[6] != 'R' || header[7] != 'T')
        return PROS_PARTITION_ENOTSUP;
    header_size = le32(header + 12);
    current_lba = le64(header + 24);
    backup_lba = le64(header + 32);
    first_usable_lba = le64(header + 40);
    last_usable_lba = le64(header + 48);
    entries_lba = le64(header + 72);
    entry_count = le32(header + 80);
    entry_size = le32(header + 84);
    if (header_size < 92u || header_size > device->sector_size ||
        current_lba != 1u || backup_lba >= device->sector_count ||
        backup_lba == current_lba || first_usable_lba > last_usable_lba ||
        last_usable_lba >= device->sector_count ||
        entry_count == 0u || entry_size < 128u ||
        entry_size > device->sector_size || entry_size > 1024u ||
        (entry_size & (entry_size - 1u)) != 0u ||
        entries_lba >= device->sector_count)
        return PROS_PARTITION_EINVAL;
    uint64_t entry_bytes = (uint64_t)entry_count * entry_size;
    if (entry_bytes == 0u ||
        entry_bytes > UINT64_MAX - (device->sector_size - 1u))
        return PROS_PARTITION_EINVAL;
    uint64_t entry_sectors = entry_bytes / device->sector_size;
    if (entry_bytes % device->sector_size != 0u)
        ++entry_sectors;
    if (entry_sectors == 0u ||
        entry_sectors > device->sector_count - entries_lba)
        return PROS_PARTITION_EINVAL;
    uint64_t entries_end = entries_lba + entry_sectors;
    if (entries_end > first_usable_lba || backup_lba <= last_usable_lba)
        return PROS_PARTITION_EINVAL;
    for (uint32_t i = 0; i < entry_count && found < capacity; ++i) {
        uint64_t byte_offset = (uint64_t)i * entry_size;
        uint64_t lba = entries_lba + byte_offset / device->sector_size;
        uint32_t offset = (uint32_t)(byte_offset % device->sector_size);
        if (lba >= device->sector_count)
            return PROS_PARTITION_EINVAL;
        uint32_t sectors_to_read =
            (offset + entry_size + device->sector_size - 1u) /
            device->sector_size;
        if (sectors_to_read == 0u || sectors_to_read > 3u ||
            sectors_to_read > device->sector_count - lba)
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
        if (last_lba < first_lba || first_lba < first_usable_lba ||
            last_lba > last_usable_lba || last_lba >= device->sector_count)
            return PROS_PARTITION_EINVAL;
        partitions[found].first_lba = first_lba;
        partitions[found].sector_count = last_lba - first_lba + 1u;
        partitions[found].mbr_type = 0;
        partitions[found].table_type = PROS_PARTITION_GPT;
        for (uint32_t b = 0; b < 16; ++b)
            partitions[found].type_guid[b] = entry[offset + b];
        ++found;
    }
    /* GPT partition extents must not overlap. */
    for (uint32_t i = 0; i < found; ++i) {
        uint64_t a_end = partitions[i].first_lba + partitions[i].sector_count;
        for (uint32_t j = i + 1u; j < found; ++j) {
            uint64_t b_end = partitions[j].first_lba + partitions[j].sector_count;
            if (partitions[i].first_lba < b_end &&
                partitions[j].first_lba < a_end)
                return PROS_PARTITION_EINVAL;
        }
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

    /*
     * GPT regression: exercise an entry that crosses a sector boundary.
     * A 256-byte entry starts at offset 256 for the second entry, so its
     * tail crosses a sector boundary and must be read as a two-sector span.
     */
    static uint8_t gpt_media[4u * 512u];
    for (uint32_t i = 0u; i < sizeof(gpt_media); ++i)
        gpt_media[i] = 0u;
    gpt_media[512u + 0u] = 'E';
    gpt_media[512u + 1u] = 'F';
    gpt_media[512u + 2u] = 'I';
    gpt_media[512u + 3u] = ' ';
    gpt_media[512u + 4u] = 'P';
    gpt_media[512u + 5u] = 'A';
    gpt_media[512u + 6u] = 'R';
    gpt_media[512u + 7u] = 'T';
    gpt_media[446u + 4u] = 0xEEu;
    gpt_media[510u] = 0x55u;
    gpt_media[511u] = 0xAAu;
    gpt_media[512u + 12u] = 92u;
    gpt_media[512u + 24u] = 1u;
    gpt_media[512u + 32u] = 3u;
    gpt_media[512u + 40u] = 2u;
    gpt_media[512u + 48u] = 3u;
    gpt_media[512u + 72u] = 2u;
    gpt_media[512u + 80u] = 2u;
    gpt_media[512u + 84u] = 0u;
    gpt_media[512u + 85u] = 1u;
    gpt_media[2u * 512u + 300u] = 0x11u;
    /* With a 4-sector device, usable LBAs 2..3 form the valid span. */
    gpt_media[2u * 512u + 332u] = 2u;
    gpt_media[2u * 512u + 340u] = 3u;
    gpt_media[2u * 512u + 341u] = 0u;
    gpt_media[2u * 512u + 342u] = 0u;
    gpt_media[2u * 512u + 343u] = 0u;
    gpt_media[2u * 512u + 344u] = 0u;
    gpt_media[2u * 512u + 345u] = 0u;
    gpt_media[2u * 512u + 346u] = 0u;
    gpt_media[2u * 512u + 347u] = 0u;

    struct pros_block_device gpt_parent;
    if (block_device_init(&gpt_parent, 512u, 4u, partition_test_read, 0, 0) !=
        PROS_BLOCK_OK)
        return -2;
    /*
     * Reuse the callback's media backing by copying the GPT image into the
     * existing test medium before probing.
     */
    for (uint32_t i = 0u; i < sizeof(gpt_media); ++i) {
        if (i < sizeof(partition_test_media))
            partition_test_media[i] = gpt_media[i];
    }
    enum pros_partition_type gpt_type = PROS_PARTITION_NONE;
    if (partition_probe(&gpt_parent, &gpt_type) != PROS_PARTITION_OK ||
        gpt_type != PROS_PARTITION_GPT)
        return -3;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_OK ||
        count != 1u || partitions[0].first_lba != 2u ||
        partitions[0].sector_count != 2u ||
        partitions[0].type_guid[0] != 0x11u)
        return -4;

    /* Reject a partition extent outside the GPT usable-LBA range. */
    partition_test_media[2u * 512u + 332u] = 1u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
        PROS_PARTITION_EINVAL)
        return -5;
    partition_test_media[2u * 512u + 332u] = 3u;

    /* Reject a partition-entry array that extends beyond the device. */
    partition_test_media[512u + 80u] = 0xFFu;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
        PROS_PARTITION_EINVAL)
        return -6;

    /* Restore a valid entry count, then reject an empty entry array. */
    partition_test_media[512u + 80u] = 0u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
        PROS_PARTITION_EINVAL)
        return -7;

    return PROS_PARTITION_OK;
}
