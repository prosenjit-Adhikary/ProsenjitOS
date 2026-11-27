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

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* GPT specifies the reflected IEEE CRC-32 polynomial, not CRC32C. */
static uint32_t partition_crc32_update(uint32_t crc,
                                       const uint8_t *data,
                                       uint32_t length)
{
    for (uint32_t i = 0u; i < length; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc;
}

static uint32_t partition_crc32(const uint8_t *data, uint32_t length)
{
    return partition_crc32_update(0xFFFFFFFFu, data, length) ^ 0xFFFFFFFFu;
}

static void partition_test_gpt_update_crcs(uint8_t *media,
                                            uint32_t entry_count,
                                            uint32_t entry_size)
{
    uint32_t entry_bytes = entry_count * entry_size;
    uint8_t *header = media + 512u;
    put_le32(header + 88u, partition_crc32(media + 1024u, entry_bytes));
    put_le32(header + 16u, 0u);
    put_le32(header + 16u, partition_crc32(header, le32(header + 12u)));
}

int partition_probe(struct pros_block_device *device,
                    enum pros_partition_type *table_type)
{
    uint8_t sector[512];
    uint8_t primary_header[512];
    uint8_t backup_header[512];
    enum pros_partition_type detected = PROS_PARTITION_MBR;
    uint32_t protective_entries = 0u;
    int primary_gpt;
    int backup_gpt = 0;
    if (!device || !table_type || device->sector_size != sizeof(sector) ||
        device->sector_count < 2u)
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 0, 1, sector) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (le16(sector + 510) != 0xAA55)
        return PROS_PARTITION_ENOTSUP;

    /* Inspect both GPT header locations even when the protective MBR is
     * missing or malformed. Unsupported GPT layouts must never fall through
     * to the native MBR reservation path. */
    if (block_device_read(device, 1u, 1u, primary_header) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    primary_gpt =
        primary_header[0] == 'E' && primary_header[1] == 'F' &&
        primary_header[2] == 'I' && primary_header[3] == ' ' &&
        primary_header[4] == 'P' && primary_header[5] == 'A' &&
        primary_header[6] == 'R' && primary_header[7] == 'T';
    if (device->sector_count - 1u != 1u) {
        if (block_device_read(device, device->sector_count - 1u, 1u,
                              backup_header) != PROS_BLOCK_OK)
            return PROS_PARTITION_EIO;
        backup_gpt =
            backup_header[0] == 'E' && backup_header[1] == 'F' &&
            backup_header[2] == 'I' && backup_header[3] == ' ' &&
            backup_header[4] == 'P' && backup_header[5] == 'A' &&
            backup_header[6] == 'R' && backup_header[7] == 'T';
    }
    if (primary_gpt || backup_gpt)
        detected = PROS_PARTITION_GPT;

    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t *entry = sector + 446 + i * 16;
        uint8_t type = entry[4];
        uint32_t first_lba = le32(entry + 8u);
        uint32_t sector_count = le32(entry + 12u);
        if (type == 0xEEu) {
            uint64_t expected_count;
            ++protective_entries;
            expected_count = device->sector_count - 1u;
            if (expected_count > UINT32_MAX)
                expected_count = UINT32_MAX;
            /* A protective MBR is a single, inactive entry covering the disk. */
            if (entry[0] != 0u || first_lba != 1u ||
                sector_count != (uint32_t)expected_count || !primary_gpt)
                return PROS_PARTITION_EINVAL;
        }
    }
    if (protective_entries > 1u)
        return PROS_PARTITION_EINVAL;
    if (protective_entries == 1u) {
        for (uint32_t i = 0u; i < 4u; ++i) {
            const uint8_t *entry = sector + 446u + i * 16u;
            if (entry[4] != 0xEEu &&
                (entry[0] != 0u || entry[4] != 0u ||
                 le32(entry + 8u) != 0u ||
                 le32(entry + 12u) != 0u))
                return PROS_PARTITION_EINVAL;
        }
    }
    *table_type = detected;
    return PROS_PARTITION_OK;
}

static void partition_clear_results(struct pros_partition *partitions,
                                     uint32_t count)
{
    if (!partitions)
        return;
    for (uint32_t i = 0u; i < count; ++i) {
        partitions[i].first_lba = 0u;
        partitions[i].sector_count = 0u;
        partitions[i].mbr_type = 0u;
        partitions[i].table_type = PROS_PARTITION_NONE;
        for (uint32_t b = 0u; b < 16u; ++b)
            partitions[i].type_guid[b] = 0u;
    }
}

int partition_read_mbr(struct pros_block_device *device,
                       struct pros_partition *partitions,
                       uint32_t capacity, uint32_t *count)
{
    uint8_t sector[512];
    uint32_t found = 0;
    if (!device || !partitions || !count || capacity == 0 || device->sector_size != 512u)
        return PROS_PARTITION_EINVAL;
    *count = 0u;
    /* Keep every failed scan from exposing stale entries from an older scan. */
    partition_clear_results(partitions, capacity);
    /* MBR-only callers must reject GPT even if its protective MBR is absent. */
    {
        enum pros_partition_type detected = PROS_PARTITION_NONE;
        int probe_rc = partition_probe(device, &detected);
        if (probe_rc != PROS_PARTITION_OK)
            return probe_rc;
        if (detected != PROS_PARTITION_MBR)
            return PROS_PARTITION_ENOTSUP;
    }
    if (block_device_read(device, 0, 1, sector) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (le16(sector + 510) != 0xAA55)
        return PROS_PARTITION_ENOTSUP;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t *entry = sector + 446 + i * 16;
        uint32_t first = le32(entry + 8);
        uint32_t sectors = le32(entry + 12);
        uint8_t type = entry[4];

        /* The MBR status byte is exactly 0x00 (inactive) or 0x80 (bootable). */
        if (entry[0] != 0u && entry[0] != 0x80u) {
            partition_clear_results(partitions, found);
            return PROS_PARTITION_EINVAL;
        }
        if (type == 0u) {
            /* An unused entry may not carry an LBA range. */
            if (first != 0u || sectors != 0u) {
                partition_clear_results(partitions, found);
                return PROS_PARTITION_EINVAL;
            }
            continue;
        }
        if (sectors == 0u || first == 0u ||
            first >= device->sector_count ||
            (uint64_t)sectors > device->sector_count - first) {
            partition_clear_results(partitions, found);
            return PROS_PARTITION_EINVAL;
        }
        /* Primary MBR extents must not overlap one another. */
        for (uint32_t previous = 0u; previous < found; ++previous) {
            uint64_t previous_end =
                partitions[previous].first_lba +
                partitions[previous].sector_count;
            uint64_t current_end = (uint64_t)first + sectors;
            if ((uint64_t)first < previous_end &&
                partitions[previous].first_lba < current_end) {
                partition_clear_results(partitions, found);
                return PROS_PARTITION_EINVAL;
            }
        }
        if (found == capacity) {
            partition_clear_results(partitions, found);
            return PROS_PARTITION_EINVAL;
        }
        partitions[found].first_lba = first;
        partitions[found].sector_count = sectors;
        partitions[found].mbr_type = type;
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
    uint32_t revision;
    uint32_t header_size;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t found = 0;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint32_t expected_header_crc;
    uint32_t expected_entries_crc;
    uint8_t disk_guid_nonzero = 0u;
    uint32_t crc_state;
    uint8_t crc_sector[512];
    if (!device || !partitions || !count || capacity == 0 || device->sector_size != 512u)
        return PROS_PARTITION_EINVAL;
    *count = 0u;
    /*
     * Never leave entries from a previous successful scan visible after a
     * later failed scan. Clear the full caller-provided output capacity
     * before any I/O or validation that can fail.
     */
    partition_clear_results(partitions, capacity);
    if (block_device_read(device, 1, 1, header) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (header[0] != 'E' || header[1] != 'F' || header[2] != 'I' || header[3] != ' ' ||
        header[4] != 'P' || header[5] != 'A' || header[6] != 'R' || header[7] != 'T')
        return PROS_PARTITION_ENOTSUP;
    revision = le32(header + 8u);
    header_size = le32(header + 12u);
    current_lba = le64(header + 24);
    backup_lba = le64(header + 32);
    first_usable_lba = le64(header + 40);
    last_usable_lba = le64(header + 48);
    entries_lba = le64(header + 72);
    entry_count = le32(header + 80);
    entry_size = le32(header + 84);
    if (revision < 0x00010000u || header_size < 92u ||
        header_size > device->sector_size || le32(header + 20u) != 0u ||
        current_lba != 1u || device->sector_count < 3u ||
        backup_lba != device->sector_count - 1u ||
        first_usable_lba < 2u || first_usable_lba > last_usable_lba ||
        last_usable_lba >= backup_lba ||
        entry_count == 0u || entry_size < 128u ||
        entry_size > device->sector_size || entry_size > 1024u ||
        (entry_size & 7u) != 0u || entries_lba < 2u ||
        entries_lba >= device->sector_count)
        return PROS_PARTITION_EINVAL;

    /* Validate the GPT header CRC with its checksum field treated as zero. */
    expected_header_crc = le32(header + 16u);
    put_le32(header + 16u, 0u);
    if (partition_crc32(header, header_size) != expected_header_crc)
        return PROS_PARTITION_EINVAL;
    put_le32(header + 16u, expected_header_crc);

    /* GPT disk GUID is required and must not be the all-zero nil GUID. */
    for (uint32_t i = 0u; i < 16u; ++i)
        disk_guid_nonzero |= header[56u + i];
    if (disk_guid_nonzero == 0u)
        return PROS_PARTITION_EINVAL;

    uint64_t entry_bytes = (uint64_t)entry_count * entry_size;
    uint64_t entry_sectors = entry_bytes / device->sector_size;
    if (entry_bytes % device->sector_size != 0u)
        ++entry_sectors;
    if (entry_sectors == 0u ||
        entry_sectors > device->sector_count - entries_lba ||
        entries_lba >= first_usable_lba ||
        entry_sectors > first_usable_lba - entries_lba)
        return PROS_PARTITION_EINVAL;

    /*
     * Check the complete entry-array CRC before publishing any parsed entries.
     * Stream one sector at a time so large, otherwise valid arrays do not
     * require an unbounded allocation.
     */
    expected_entries_crc = le32(header + 88u);
    crc_state = 0xFFFFFFFFu;
    for (uint64_t offset = 0u; offset < entry_bytes;) {
        uint64_t lba;
        uint32_t bytes = (uint32_t)(entry_bytes - offset);
        if (bytes > device->sector_size)
            bytes = device->sector_size;
        if (block_device_lba_add(device, entries_lba,
                                 offset / device->sector_size,
                                 &lba) != PROS_BLOCK_OK ||
            block_device_read(device, lba, 1u, crc_sector) != PROS_BLOCK_OK)
            return PROS_PARTITION_EIO;
        crc_state = partition_crc32_update(crc_state, crc_sector, bytes);
        offset += bytes;
    }
    if ((crc_state ^ 0xFFFFFFFFu) != expected_entries_crc)
        return PROS_PARTITION_EINVAL;
    for (uint32_t i = 0; i < entry_count && found < capacity; ++i) {
        uint64_t byte_offset = (uint64_t)i * entry_size;
        uint64_t lba;
        uint32_t offset = (uint32_t)(byte_offset % device->sector_size);
        if (block_device_lba_add(device, entries_lba,
                                 byte_offset / device->sector_size,
                                 &lba) != PROS_BLOCK_OK) {
            partition_clear_results(partitions, found);
            return PROS_PARTITION_EINVAL;
        }
        uint32_t sectors_to_read =
            (offset + entry_size > device->sector_size) ? 2u : 1u;
        if (!block_device_lba_range_valid(device, lba, sectors_to_read)) {
            partition_clear_results(partitions, found);
            return PROS_PARTITION_EINVAL;
        }
        if (block_device_read(device, lba, sectors_to_read, entry) != PROS_BLOCK_OK) {
            partition_clear_results(partitions, found);
            return PROS_PARTITION_EIO;
        }
        uint8_t empty = 1;
        for (uint32_t b = 0; b < 16; ++b)
            if (entry[offset + b] != 0) { empty = 0; break; }
        if (empty)
            continue;
        uint64_t first_lba = le64(entry + offset + 32);
        uint64_t last_lba = le64(entry + offset + 40);
        if (last_lba < first_lba || first_lba < first_usable_lba ||
            last_lba > last_usable_lba || last_lba >= device->sector_count) {
            partition_clear_results(partitions, found);
            return PROS_PARTITION_EINVAL;
        }
        /* Distinct GPT entries may not describe overlapping disk extents. */
        {
            uint64_t current_end = last_lba + 1u;
            for (uint32_t previous = 0u; previous < found; ++previous) {
                uint64_t previous_end =
                    partitions[previous].first_lba +
                    partitions[previous].sector_count;
                if (first_lba < previous_end &&
                    partitions[previous].first_lba < current_end) {
                    partition_clear_results(partitions, found);
                    return PROS_PARTITION_EINVAL;
                }
            }
        }
        if (found == capacity) {
            partition_clear_results(partitions, found);
            return PROS_PARTITION_EINVAL;
        }
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

static void partition_trace_record(struct pros_partition_device *view,
                                    uint64_t local_lba, uint64_t parent_lba,
                                    uint32_t count, uint8_t write)
{
    struct pros_partition_trace_event *event;
    if (!view)
        return;
    event = &view->trace[view->trace_head];
    event->local_lba = local_lba;
    event->parent_lba = parent_lba;
    event->count = count;
    event->write = write;
    view->trace_head = (view->trace_head + 1u) % PROS_PARTITION_TRACE_DEPTH;
    if (view->trace_count < PROS_PARTITION_TRACE_DEPTH)
        ++view->trace_count;
}

void partition_trace_reset(struct pros_partition_device *view)
{
    if (!view)
        return;
    view->trace_count = 0u;
    view->trace_head = 0u;
}

int partition_trace_find(const struct pros_partition_device *view, uint64_t lba,
                         uint8_t write, uint64_t *parent_lba,
                         uint32_t *trace_count)
{
    if (!view || !parent_lba || !trace_count)
        return PROS_PARTITION_EINVAL;
    for (uint32_t n = 0u; n < view->trace_count; ++n) {
        uint32_t index = (view->trace_head + PROS_PARTITION_TRACE_DEPTH - 1u - n) %
                         PROS_PARTITION_TRACE_DEPTH;
        const struct pros_partition_trace_event *event = &view->trace[index];
        if (event->write == write && lba >= event->local_lba &&
            lba - event->local_lba < event->count) {
            if (block_device_lba_add(view->parent, event->parent_lba,
                                     lba - event->local_lba,
                                     parent_lba) != PROS_BLOCK_OK)
                return PROS_PARTITION_EINVAL;
            *trace_count = event->count;
            return PROS_PARTITION_OK;
        }
    }
    return PROS_PARTITION_ENOTSUP;
}

static int partition_device_read(struct pros_block_device *device,
                                 uint64_t lba, uint32_t count, void *buffer)
{
    struct pros_partition_device *view;
    if (!device || !buffer || !device->private_data)
        return PROS_BLOCK_EINVAL;
    view = (struct pros_partition_device *)device->private_data;
    if (!view->parent || !block_device_lba_range_valid(device, lba, count))
        return PROS_BLOCK_EINVAL;
    uint64_t parent_lba;
    if (block_device_lba_add(view->parent, view->first_lba, lba,
                             &parent_lba) != PROS_BLOCK_OK ||
        !block_device_lba_range_valid(view->parent, parent_lba, count))
        return PROS_BLOCK_EINVAL;
    partition_trace_record(view, lba, parent_lba, count, 0u);
    return block_device_read(view->parent, parent_lba, count, buffer);
}

static int partition_device_flush(struct pros_block_device *device)
{
    struct pros_partition_device *view;
    if (!device || !device->private_data)
        return PROS_BLOCK_EINVAL;
    view = (struct pros_partition_device *)device->private_data;
    if (!view->parent)
        return PROS_BLOCK_EINVAL;
    return block_device_flush(view->parent);
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
        !block_device_lba_range_valid(device, lba, count))
        return PROS_BLOCK_EINVAL;
    uint64_t parent_lba;
    if (block_device_lba_add(view->parent, view->first_lba, lba,
                             &parent_lba) != PROS_BLOCK_OK ||
        !block_device_lba_range_valid(view->parent, parent_lba, count))
        return PROS_BLOCK_EINVAL;
    partition_trace_record(view, lba, parent_lba, count, 1u);
    return block_device_write(view->parent, parent_lba, count, buffer);
}

int partition_device_init(struct pros_partition_device *view,
                          struct pros_block_device *parent,
                          const struct pros_partition *partition)
{
    /*
     * Validate every condition that can make block_device_init() fail before
     * touching the caller-owned view. A missing parent read callback would
     * otherwise create a view that can never service reads.
     */
    if (!view || !parent || !partition || !parent->read ||
        parent->sector_size == 0u || partition->sector_count == 0u ||
        partition->first_lba >= parent->sector_count ||
        partition->sector_count > parent->sector_count - partition->first_lba)
        return PROS_PARTITION_EINVAL;
    view->parent = parent;
    view->first_lba = partition->first_lba;
    partition_trace_reset(view);
    if (block_device_init(&view->device, parent->sector_size,
                          partition->sector_count, partition_device_read,
                          parent->write ? partition_device_write : 0, view) !=
        PROS_BLOCK_OK)
        return PROS_PARTITION_EINVAL;
    if (parent->flush &&
        block_device_set_flush(&view->device, partition_device_flush) != PROS_BLOCK_OK)
        return PROS_PARTITION_EINVAL;
    return PROS_PARTITION_OK;
}

static uint8_t partition_test_media[8 * 512];
static uint64_t partition_test_fail_lba = UINT64_MAX;

static int partition_test_read(struct pros_block_device *device,
                               uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    if (!buffer || lba > 8u || count > 8u - lba)
        return PROS_BLOCK_EINVAL;
    if (partition_test_fail_lba >= lba &&
        partition_test_fail_lba - lba < count)
        return PROS_BLOCK_EIO;
    for (uint32_t i = 0; i < count * 512u; ++i)
        ((uint8_t *)buffer)[i] = partition_test_media[lba * 512u + i];
    return PROS_BLOCK_OK;
}

static int partition_test_write(struct pros_block_device *device,
                                uint64_t lba, uint32_t count,
                                const void *buffer)
{
    (void)device;
    if (!buffer || lba > 8u || count > 8u - lba)
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

    /* Failed reads must not leave stale partition counts or probe results. */
    {
        struct pros_partition failed_partitions[2];
        uint32_t failed_count = 0xA5A5A5A5u;
        enum pros_partition_type failed_type = PROS_PARTITION_GPT;
        partition_test_fail_lba = 0u;
        if (partition_read_mbr(&parent, failed_partitions, 2u,
                               &failed_count) != PROS_PARTITION_EIO ||
            failed_count != 0u)
            return -10;
        partition_test_fail_lba = UINT64_MAX;
        failed_count = 0xA5A5A5A5u;
        partition_test_media[446u + 4u] = 0xEEu;
        /* Keep the protective extent valid so the injected LBA 1 read fails. */
        partition_test_media[446u + 12u] = 3u;
        partition_test_fail_lba = 1u;
        if (partition_probe(&parent, &failed_type) != PROS_PARTITION_EIO ||
            failed_type != PROS_PARTITION_GPT)
            return -11;
        partition_test_fail_lba = UINT64_MAX;
        partition_test_media[446u + 4u] = 0x0Bu;
        partition_test_media[446u + 12u] = 2u;
    }

    /*
     * Failed partition-view initialization must not mutate caller state.
     * Exercise both failure conditions that block_device_init() would
     * otherwise discover only after the view has already been modified.
     */
    {
        struct pros_partition_device rejected_view;
        struct pros_block_device invalid_parent = parent;
        struct pros_partition candidate = {0};
        candidate.first_lba = 1u;
        candidate.sector_count = 2u;
        for (uint32_t i = 0u; i < sizeof(rejected_view); ++i)
            ((uint8_t *)&rejected_view)[i] = 0xA5u;
        rejected_view.parent = &parent;
        rejected_view.first_lba = UINT64_MAX;
        rejected_view.trace_count = 17u;
        rejected_view.trace_head = 9u;
        rejected_view.device.sector_size = 0xA5A5A5A5u;
        rejected_view.device.sector_count = UINT64_MAX;
        rejected_view.device.read = partition_test_read;
        rejected_view.device.write = partition_test_write;
        rejected_view.device.flush = 0;
        rejected_view.device.private_data = &rejected_view;

        invalid_parent.sector_size = 0u;
        if (partition_device_init(&rejected_view, &invalid_parent, &candidate) !=
                PROS_PARTITION_EINVAL ||
            rejected_view.parent != &parent ||
            rejected_view.first_lba != UINT64_MAX ||
            rejected_view.trace_count != 17u ||
            rejected_view.trace_head != 9u ||
            rejected_view.device.sector_size != 0xA5A5A5A5u ||
            rejected_view.device.sector_count != UINT64_MAX ||
            rejected_view.device.read != partition_test_read ||
            rejected_view.device.write != partition_test_write ||
            rejected_view.device.flush != 0 ||
            rejected_view.device.private_data != &rejected_view)
            return -8;

        invalid_parent = parent;
        invalid_parent.read = 0;
        if (partition_device_init(&rejected_view, &invalid_parent, &candidate) !=
                PROS_PARTITION_EINVAL ||
            rejected_view.parent != &parent ||
            rejected_view.first_lba != UINT64_MAX ||
            rejected_view.trace_count != 17u ||
            rejected_view.trace_head != 9u ||
            rejected_view.device.sector_size != 0xA5A5A5A5u ||
            rejected_view.device.sector_count != UINT64_MAX ||
            rejected_view.device.read != partition_test_read ||
            rejected_view.device.write != partition_test_write ||
            rejected_view.device.flush != 0 ||
            rejected_view.device.private_data != &rejected_view)
            return -9;
    }
    struct pros_partition partitions[4];
    uint32_t count = 0;
    if (partition_read_mbr(&parent, partitions, 4, &count) !=
            PROS_PARTITION_OK ||
        count != 1u || partitions[0].first_lba != 1u ||
        partitions[0].sector_count != 2u)
        return -1;
    /* A later invalid MBR entry must not expose earlier partial results. */
    partition_test_media[446u + 16u + 4u] = 0x83u;
    partition_test_media[446u + 16u + 8u] = 0xFFu;
    partition_test_media[446u + 16u + 12u] = 0xFFu;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u ||
        partitions[0].first_lba != 0u ||
        partitions[0].sector_count != 0u ||
        partitions[0].table_type != PROS_PARTITION_NONE)
        return -13;
    partition_test_media[446u + 16u + 4u] = 0u;
    partition_test_media[446u + 16u + 8u] = 0u;
    partition_test_media[446u + 16u + 12u] = 0u;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_OK || count != 1u)
        return -14;

    /* Reject invalid boot status without publishing partial partition data. */
    partition_test_media[446u] = 0x7Fu;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u ||
        partitions[0].table_type != PROS_PARTITION_NONE)
        return -16;
    partition_test_media[446u] = 0u;

    /* A type-zero entry carrying an extent is malformed, not an empty slot. */
    partition_test_media[446u + 4u] = 0u;
    partition_test_media[446u + 8u] = 1u;
    partition_test_media[446u + 12u] = 2u;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -17;
    partition_test_media[446u + 4u] = 0x0Bu;

    /* Reject overlapping primary entries, even when both ranges are in-bounds. */
    partition_test_media[446u + 16u + 4u] = 0x83u;
    partition_test_media[446u + 16u + 8u] = 2u;
    partition_test_media[446u + 16u + 12u] = 1u;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -18;
    partition_test_media[446u + 16u + 4u] = 0u;
    partition_test_media[446u + 16u + 8u] = 0u;
    partition_test_media[446u + 16u + 12u] = 0u;

    /* Valid active/inactive boot flags remain accepted. */
    partition_test_media[446u] = 0x80u;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_OK || count != 1u)
        return -19;
    partition_test_media[446u] = 0u;
    part = partitions[0];
    if (partition_device_init(&view, &parent, &part) != PROS_PARTITION_OK)
        return -1;
    if (block_device_read(&view.device, 0, 1, buffer) != PROS_BLOCK_OK)
        return -1;
    if (buffer[0] != partition_test_media[512])
        return -1;
    {
        uint64_t translated = 0u;
        uint32_t trace_count = 0u;
        if (partition_trace_find(&view, 0u, 0u, &translated,
                                 &trace_count) != PROS_PARTITION_OK ||
            translated != 1u || trace_count != 1u)
            return -2;
        if (block_device_read(&view.device, 1u, 1u, buffer) != PROS_BLOCK_OK ||
            partition_trace_find(&view, 1u, 0u, &translated,
                                 &trace_count) != PROS_PARTITION_OK ||
            translated != 2u)
            return -3;
    }
    if (block_device_read(&view.device, 2, 1, buffer) != PROS_BLOCK_EINVAL)
        return -40;

    /*
     * GPT regression: exercise an entry that crosses a sector boundary.
     * A 304-byte entry starts at offset 304, so its tail is in the next
     * sector and must be read as a two-sector span.
     */
    static uint8_t gpt_media[8u * 512u];
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
    put_le32(gpt_media + 446u + 8u, 1u);
    put_le32(gpt_media + 446u + 12u, 7u);
    gpt_media[510u] = 0x55u;
    gpt_media[511u] = 0xAAu;
    put_le32(gpt_media + 512u + 8u, 0x00010000u);
    gpt_media[512u + 12u] = 92u;
    gpt_media[512u + 24u] = 1u;
    gpt_media[512u + 32u] = 7u;
    gpt_media[512u + 40u] = 4u;
    gpt_media[512u + 48u] = 6u;
    gpt_media[512u + 56u] = 0xA5u;
    gpt_media[512u + 72u] = 2u;
    gpt_media[512u + 80u] = 2u;
    put_le32(gpt_media + 512u + 84u, 304u);
    gpt_media[2u * 512u + 304u] = 0x11u;
    /* Device has 8 sectors: usable LBAs 4..6, partition spans 4..5. */
    gpt_media[2u * 512u + 336u] = 4u;
    gpt_media[2u * 512u + 344u] = 5u;
    partition_test_gpt_update_crcs(gpt_media, 2u, 304u);

    struct pros_block_device gpt_parent;
    if (block_device_init(&gpt_parent, 512u, 8u, partition_test_read, 0, 0) !=
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

    /*
     * GPT detection must not depend on a protective MBR being present.
     * The MBR-only native path must reject both primary-header-only and
     * backup-header-only GPT layouts instead of treating either as MBR.
     */
    {
        uint8_t saved_protective[16], saved_primary[8], saved_backup[8];
        static const uint8_t signature[8] = {
            'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
        };
        for (uint32_t i = 0u; i < 16u; ++i)
            saved_protective[i] = partition_test_media[446u + i];
        for (uint32_t i = 0u; i < 8u; ++i) {
            saved_primary[i] = partition_test_media[512u + i];
            saved_backup[i] = partition_test_media[7u * 512u + i];
        }
        for (uint32_t i = 0u; i < 16u; ++i)
            partition_test_media[446u + i] = 0u;
        if (partition_probe(&gpt_parent, &gpt_type) != PROS_PARTITION_OK ||
            gpt_type != PROS_PARTITION_GPT)
            return -46;
        count = 0xA5A5A5A5u;
        if (partition_read_mbr(&gpt_parent, partitions, 4u, &count) !=
                PROS_PARTITION_ENOTSUP || count != 0u)
            return -47;

        for (uint32_t i = 0u; i < 8u; ++i) {
            partition_test_media[512u + i] = 0u;
            partition_test_media[7u * 512u + i] = signature[i];
        }
        if (partition_probe(&gpt_parent, &gpt_type) != PROS_PARTITION_OK ||
            gpt_type != PROS_PARTITION_GPT)
            return -48;
        count = 0xA5A5A5A5u;
        if (partition_read_mbr(&gpt_parent, partitions, 4u, &count) !=
                PROS_PARTITION_ENOTSUP || count != 0u)
            return -49;

        for (uint32_t i = 0u; i < 16u; ++i)
            partition_test_media[446u + i] = saved_protective[i];
        for (uint32_t i = 0u; i < 8u; ++i) {
            partition_test_media[512u + i] = saved_primary[i];
            partition_test_media[7u * 512u + i] = saved_backup[i];
        }
    }

    /* Protective MBR must start at LBA 1 and cover the device extent. */
    put_le32(partition_test_media + 446u + 8u, 2u);
    if (partition_probe(&gpt_parent, &gpt_type) != PROS_PARTITION_EINVAL)
        return -42;
    put_le32(partition_test_media + 446u + 8u, 1u);
    put_le32(partition_test_media + 446u + 12u, 6u);
    if (partition_probe(&gpt_parent, &gpt_type) != PROS_PARTITION_EINVAL)
        return -43;
    put_le32(partition_test_media + 446u + 12u, 7u);
    partition_test_media[446u + 16u + 4u] = 0x83u;
    put_le32(partition_test_media + 446u + 16u + 8u, 4u);
    put_le32(partition_test_media + 446u + 16u + 12u, 2u);
    if (partition_probe(&gpt_parent, &gpt_type) != PROS_PARTITION_EINVAL)
        return -44;
    partition_test_media[446u + 16u + 4u] = 0u;
    put_le32(partition_test_media + 446u + 16u + 8u, 0u);
    put_le32(partition_test_media + 446u + 16u + 12u, 0u);
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_OK ||
        count != 1u || partitions[0].first_lba != 4u ||
        partitions[0].sector_count != 2u ||
        partitions[0].type_guid[0] != 0x11u)
        return -41;

    /* A checksum-valid GPT header with a nil disk GUID is invalid. */
    partition_test_media[512u + 56u] = 0u;
    put_le32(partition_test_media + 512u + 16u, 0u);
    put_le32(partition_test_media + 512u + 16u,
             partition_crc32(partition_test_media + 512u, 92u));
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -45;
    partition_test_media[512u + 56u] = 0xA5u;
    put_le32(partition_test_media + 512u + 16u, 0u);
    put_le32(partition_test_media + 512u + 16u,
             partition_crc32(partition_test_media + 512u, 92u));

    /* Reject non-primary geometry, reserved bits, and misaligned entry sizes. */
    put_le32(partition_test_media + 512u + 32u, 6u);
    put_le32(partition_test_media + 512u + 16u, 0u);
    put_le32(partition_test_media + 512u + 16u,
             partition_crc32(partition_test_media + 512u, 92u));
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -22;
    put_le32(partition_test_media + 512u + 32u, 7u);
    put_le32(partition_test_media + 512u + 20u, 1u);
    put_le32(partition_test_media + 512u + 16u, 0u);
    put_le32(partition_test_media + 512u + 16u,
             partition_crc32(partition_test_media + 512u, 92u));
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -23;
    put_le32(partition_test_media + 512u + 20u, 0u);
    put_le32(partition_test_media + 512u + 84u, 129u);
    put_le32(partition_test_media + 512u + 16u, 0u);
    put_le32(partition_test_media + 512u + 16u,
             partition_crc32(partition_test_media + 512u, 92u));
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -24;
    put_le32(partition_test_media + 512u + 84u, 304u);
    put_le32(partition_test_media + 512u + 16u, 0u);
    put_le32(partition_test_media + 512u + 16u,
             partition_crc32(partition_test_media + 512u, 92u));

    /*
     * Two checksum-valid, individually in-range entries that overlap must be
     * rejected, and the first parsed entry must not leak through on failure.
     */
    partition_test_media[2u * 512u] = 0x22u;
    partition_test_media[2u * 512u + 32u] = 4u;
    partition_test_media[2u * 512u + 40u] = 5u;
    partition_test_gpt_update_crcs(partition_test_media, 2u, 304u);
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u ||
        partitions[0].table_type != PROS_PARTITION_NONE)
        return -25;
    partition_test_media[2u * 512u] = 0u;
    partition_test_media[2u * 512u + 32u] = 0u;
    partition_test_media[2u * 512u + 40u] = 0u;
    partition_test_gpt_update_crcs(partition_test_media, 2u, 304u);

    /* Header and entry-array checksums are independently enforced. */
    partition_test_media[512u + 20u] ^= 0x01u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -20;
    partition_test_media[512u + 20u] ^= 0x01u;
    partition_test_media[2u * 512u + 304u + 16u] ^= 0x01u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -21;
    partition_test_media[2u * 512u + 304u + 16u] ^= 0x01u;

    /* Failure while reading a later GPT entry clears earlier output slots. */
    partition_test_fail_lba = 3u;
    count = 0xA5A5A5A5u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EIO || count != 0u ||
        partitions[0].first_lba != 0u ||
        partitions[0].sector_count != 0u ||
        partitions[0].table_type != PROS_PARTITION_NONE)
        return -15;
    partition_test_fail_lba = UINT64_MAX;

    /* A GPT header read failure must clear the caller's previous count. */
    count = 0xA5A5A5A5u;
    partition_test_fail_lba = 1u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EIO || count != 0u)
        return -12;
    partition_test_fail_lba = UINT64_MAX;

    /* Reject a partition extent outside the GPT usable-LBA range. */
    partition_test_media[2u * 512u + 336u] = 1u;
    partition_test_gpt_update_crcs(partition_test_media, 2u, 304u);
    count = 0xA5A5A5A5u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -5;
    partition_test_media[2u * 512u + 336u] = 2u;
    partition_test_gpt_update_crcs(partition_test_media, 2u, 304u);

    /* Reject an entry array extending beyond the device with a valid header CRC. */
    put_le32(partition_test_media + 512u + 80u, 0xFFu);
    put_le32(partition_test_media + 512u + 16u, 0u);
    put_le32(partition_test_media + 512u + 16u,
             partition_crc32(partition_test_media + 512u, 92u));
    count = 0xA5A5A5A5u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -6;

    /* Restore a valid entry count, then reject an empty entry array. */
    put_le32(partition_test_media + 512u + 80u, 0u);
    put_le32(partition_test_media + 512u + 16u, 0u);
    put_le32(partition_test_media + 512u + 16u,
             partition_crc32(partition_test_media + 512u, 92u));
    count = 0xA5A5A5A5u;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -7;

    return PROS_PARTITION_OK;
}
