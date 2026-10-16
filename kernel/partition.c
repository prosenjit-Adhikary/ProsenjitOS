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

/* Partition ranges are half-open [first, first + count), already device-bounded. */
static int partition_ranges_overlap(uint64_t first_a, uint64_t count_a,
                                   uint64_t first_b, uint64_t count_b)
{
    return first_a < first_b + count_b &&
           first_b < first_a + count_a;
}

static uint32_t partition_crc32_ieee_update(uint32_t crc,
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

static uint32_t partition_gpt_header_crc(const uint8_t *header,
                                         uint32_t header_size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < header_size; ++i) {
        uint8_t value = (i >= 16u && i < 20u) ? 0u : header[i];
        crc = partition_crc32_ieee_update(crc, &value, 1u);
    }
    return ~crc;
}

int partition_probe(struct pros_block_device *device,
                    enum pros_partition_type *table_type)
{
    uint8_t sector[512];
    if (!table_type)
        return PROS_PARTITION_EINVAL;
    *table_type = PROS_PARTITION_NONE;
    if (!device || device->sector_size != sizeof(sector))
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
            if (block_device_read(device, 1, 1, header) != PROS_BLOCK_OK) {
                *table_type = PROS_PARTITION_NONE;
                return PROS_PARTITION_EIO;
            }
            if (header[0] != 'E' || header[1] != 'F' ||
                header[2] != 'I' || header[3] != ' ' ||
                header[4] != 'P' || header[5] != 'A' ||
                header[6] != 'R' || header[7] != 'T') {
                *table_type = PROS_PARTITION_NONE;
                return PROS_PARTITION_ENOTSUP;
            }
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
    /*
     * A caller may reuse a previous successful count. Clear it whenever the
     * output pointer is valid, even if another input argument is invalid.
     */
    if (count)
        *count = 0u;
    if (!device || !partitions || !count || capacity == 0 ||
        device->sector_size != 512u)
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 0, 1, sector) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (le16(sector + 510) != 0xAA55)
        return PROS_PARTITION_ENOTSUP;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t *entry = sector + 446 + i * 16;
        uint32_t first = le32(entry + 8);
        uint32_t sectors = le32(entry + 12);
        if (entry[4] == 0 || sectors == 0)
            continue;
        /* LBA 0 contains the MBR itself and cannot be partition payload. */
        if (first == 0u || first >= device->sector_count ||
            (uint64_t)sectors > device->sector_count - first)
            return PROS_PARTITION_EINVAL;
        if (found >= capacity)
            return PROS_PARTITION_EINVAL;
        for (uint32_t previous = 0u; previous < found; ++previous) {
            if (partition_ranges_overlap(first, sectors,
                                         partitions[previous].first_lba,
                                         partitions[previous].sector_count))
                return PROS_PARTITION_EINVAL;
        }
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
    uint32_t header_size;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t found = 0;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    /*
     * Preserve the output contract on invalid arguments as well as media
     * errors: a valid count pointer must never retain a stale partition count.
     */
    if (count)
        *count = 0u;
    if (!device || !partitions || !count || capacity == 0 ||
        device->sector_size != 512u)
        return PROS_PARTITION_EINVAL;
    if (block_device_read(device, 1, 1, header) != PROS_BLOCK_OK)
        return PROS_PARTITION_EIO;
    if (header[0] != 'E' || header[1] != 'F' || header[2] != 'I' || header[3] != ' ' ||
        header[4] != 'P' || header[5] != 'A' || header[6] != 'R' || header[7] != 'T')
        return PROS_PARTITION_ENOTSUP;
    /* GPT 1.x reserves this field; non-zero values are malformed metadata. */
    if (le32(header + 20u) != 0u)
        return PROS_PARTITION_EINVAL;
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
        backup_lba <= current_lba || first_usable_lba <= current_lba ||
        first_usable_lba > last_usable_lba || backup_lba <= last_usable_lba ||
        last_usable_lba >= device->sector_count ||
        entry_count == 0u || entry_size < 128u || (entry_size % 8u) != 0u ||
        entry_size > device->sector_size || entry_size > 1024u ||
        entries_lba <= current_lba || entries_lba >= device->sector_count)
        return PROS_PARTITION_EINVAL;
    uint64_t entry_bytes = (uint64_t)entry_count * entry_size;
    uint64_t entry_sectors = entry_bytes / device->sector_size;
    if (entry_bytes % device->sector_size != 0u)
        ++entry_sectors;
    if (entry_sectors == 0u ||
        entry_sectors > device->sector_count - entries_lba)
        return PROS_PARTITION_EINVAL;

    /*
     * The primary GPT entry array is reserved metadata. It must not overlap
     * either GPT header or any LBA advertised as usable partition payload.
     * Otherwise a crafted table could expose metadata sectors as a volume.
     */
    if (partition_ranges_overlap(entries_lba, entry_sectors,
                                 current_lba, 1u) ||
        partition_ranges_overlap(entries_lba, entry_sectors,
                                 backup_lba, 1u) ||
        partition_ranges_overlap(entries_lba, entry_sectors,
                                 first_usable_lba,
                                 last_usable_lba - first_usable_lba + 1u))
        return PROS_PARTITION_EINVAL;
    /* Reject torn or modified GPT metadata before exposing any volume. */
    if (le32(header + 16u) != partition_gpt_header_crc(header, header_size))
        return PROS_PARTITION_EINVAL;
    {
        uint32_t crc = 0xFFFFFFFFu;
        uint64_t remaining = entry_bytes;
        uint64_t lba = entries_lba;
        uint8_t crc_sector[512];
        while (remaining != 0u) {
            uint32_t chunk = remaining > sizeof(crc_sector)
                           ? (uint32_t)sizeof(crc_sector)
                           : (uint32_t)remaining;
            if (block_device_read(device, lba, 1u, crc_sector) != PROS_BLOCK_OK)
                return PROS_PARTITION_EIO;
            crc = partition_crc32_ieee_update(crc, crc_sector, chunk);
            remaining -= chunk;
            ++lba;
        }
        if (~crc != le32(header + 88u))
            return PROS_PARTITION_EINVAL;
    }
    for (uint32_t i = 0; i < entry_count; ++i) {
        uint64_t byte_offset = (uint64_t)i * entry_size;
        uint64_t lba;
        uint32_t offset = (uint32_t)(byte_offset % device->sector_size);
        if (block_device_lba_add(device, entries_lba,
                                 byte_offset / device->sector_size,
                                 &lba) != PROS_BLOCK_OK)
            return PROS_PARTITION_EINVAL;
        uint32_t sectors_to_read =
            (offset + entry_size > device->sector_size) ? 2u : 1u;
        if (!block_device_lba_range_valid(device, lba, sectors_to_read))
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
        uint64_t partition_sectors = last_lba - first_lba + 1u;
        if (found >= capacity)
            return PROS_PARTITION_EINVAL;
        for (uint32_t previous = 0u; previous < found; ++previous) {
            if (partition_ranges_overlap(first_lba, partition_sectors,
                                         partitions[previous].first_lba,
                                         partitions[previous].sector_count))
                return PROS_PARTITION_EINVAL;
        }
        partitions[found].first_lba = first_lba;
        partitions[found].sector_count = partition_sectors;
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
    if (!view || !parent || !partition || partition->sector_count == 0u ||
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

/* Keep the synthetic GPT fixture spec-valid after each deliberate mutation.
 * Invalid geometry is still rejected by the parser's earlier bounds checks. */
static void partition_test_gpt_refresh_crc(void)
{
    uint8_t *header = partition_test_media + 512u;
    uint32_t header_size = le32(header + 12u);
    uint64_t entries_lba = le64(header + 72u);
    uint32_t entry_count = le32(header + 80u);
    uint32_t entry_size = le32(header + 84u);
    uint64_t bytes = (uint64_t)entry_count * entry_size;
    uint32_t entry_crc = 0u;

    if (entry_count != 0u && entry_size != 0u &&
        entries_lba < (sizeof(partition_test_media) / 512u) &&
        bytes <= sizeof(partition_test_media) - entries_lba * 512u) {
        entry_crc = ~partition_crc32_ieee_update(
            0xFFFFFFFFu,
            partition_test_media + entries_lba * 512u, (uint32_t)bytes);
    }
    header[88] = (uint8_t)entry_crc;
    header[89] = (uint8_t)(entry_crc >> 8);
    header[90] = (uint8_t)(entry_crc >> 16);
    header[91] = (uint8_t)(entry_crc >> 24);
    if (header_size >= 92u && header_size <= 512u) {
        uint32_t header_crc = partition_gpt_header_crc(header, header_size);
        header[16] = (uint8_t)header_crc;
        header[17] = (uint8_t)(header_crc >> 8);
        header[18] = (uint8_t)(header_crc >> 16);
        header[19] = (uint8_t)(header_crc >> 24);
    }
}

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
    if (!buffer || lba > 4u || count > 4u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t i = 0; i < count * 512u; ++i)
        partition_test_media[lba * 512u + i] = ((const uint8_t *)buffer)[i];
    return PROS_BLOCK_OK;
}

static int partition_test_read_fail(struct pros_block_device *device,
                                     uint64_t lba, uint32_t count,
                                     void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    return PROS_BLOCK_EIO;
}

int partition_selftest(void)
{
    static const uint8_t crc32_known_vector[] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9'
    };
    struct pros_block_device parent;
    /* Independent IEEE CRC-32 known vector: fixture CRCs use this same helper. */
    if (~partition_crc32_ieee_update(0xFFFFFFFFu, crc32_known_vector,
                                     sizeof(crc32_known_vector)) != 0xCBF43926u)
        return -26;
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

    /*
     * partition_probe() must clear stale caller output for malformed media,
     * invalid device arguments and I/O errors, rather than leaving a previous
     * successful type behind.
     */
    enum pros_partition_type probed_type = PROS_PARTITION_GPT;
    if (partition_probe(0, &probed_type) != PROS_PARTITION_EINVAL ||
        probed_type != PROS_PARTITION_NONE)
        return -23;
    struct pros_block_device wrong_sector_device;
    if (block_device_init(&wrong_sector_device, 256u, 4u,
                          partition_test_read, 0, 0) != PROS_BLOCK_OK)
        return -24;
    probed_type = PROS_PARTITION_GPT;
    if (partition_probe(&wrong_sector_device, &probed_type) !=
            PROS_PARTITION_EINVAL || probed_type != PROS_PARTITION_NONE)
        return -25;
    if (partition_probe(&parent, &probed_type) != PROS_PARTITION_OK ||
        probed_type != PROS_PARTITION_MBR)
        return -18;
    partition_test_media[510] = 0u;
    probed_type = PROS_PARTITION_GPT;
    if (partition_probe(&parent, &probed_type) != PROS_PARTITION_ENOTSUP ||
        probed_type != PROS_PARTITION_NONE)
        return -19;
    partition_test_media[510] = 0x55u;
    struct pros_block_device failed_probe_device;
    if (block_device_init(&failed_probe_device, 512u, 4u,
                          partition_test_read_fail, 0, 0) != PROS_BLOCK_OK)
        return -20;
    probed_type = PROS_PARTITION_GPT;
    if (partition_probe(&failed_probe_device, &probed_type) != PROS_PARTITION_EIO ||
        probed_type != PROS_PARTITION_NONE)
        return -21;

    /* A protective MBR whose GPT-header read fails must also clear the type. */
    partition_test_media[446u + 4u] = 0xEEu;
    partition_test_fail_lba = 1u;
    probed_type = PROS_PARTITION_GPT;
    if (partition_probe(&parent, &probed_type) != PROS_PARTITION_EIO ||
        probed_type != PROS_PARTITION_NONE)
        return -22;
    partition_test_fail_lba = UINT64_MAX;
    partition_test_media[446u + 4u] = 0x0Bu;

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
        return -4;

    /* Failed MBR parsing must clear a caller's stale output count. */
    count = 0xFFFFFFFFu;
    partition_test_media[510] = 0u;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_ENOTSUP ||
        count != 0u)
        return -8;

    /* Overlapping MBR extents must not be exposed as independent volumes. */
    partition_test_media[510] = 0x55u;
    partition_test_media[511] = 0xAAu;
    partition_test_media[446u + 16u + 4u] = 0x83u;
    partition_test_media[446u + 16u + 8u] = 2u;
    partition_test_media[446u + 16u + 12u] = 1u;
    count = 0xFFFFFFFFu;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -9;
    for (uint32_t i = 446u + 16u; i < 446u + 32u; ++i)
        partition_test_media[i] = 0u;

    /* A too-small output array must not silently hide a valid MBR partition. */
    partition_test_media[446u + 16u + 4u] = 0x83u;
    partition_test_media[446u + 16u + 8u] = 3u;
    partition_test_media[446u + 16u + 12u] = 1u;
    count = 0xFFFFFFFFu;
    if (partition_read_mbr(&parent, partitions, 1u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -11;
    for (uint32_t i = 446u + 16u; i < 446u + 32u; ++i)
        partition_test_media[i] = 0u;

    /* An MBR partition may not claim LBA 0, which contains the partition table. */
    partition_test_media[446u + 8u] = 0u;
    count = 0xFFFFFFFFu;
    if (partition_read_mbr(&parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -12;
    partition_test_media[446u + 8u] = 1u;

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
    gpt_media[510u] = 0x55u;
    gpt_media[511u] = 0xAAu;
    gpt_media[512u + 12u] = 92u;
    gpt_media[512u + 24u] = 1u;
    gpt_media[512u + 32u] = 7u;
    gpt_media[512u + 40u] = 4u;
    gpt_media[512u + 48u] = 6u;
    gpt_media[512u + 72u] = 2u;
    gpt_media[512u + 80u] = 2u;
    gpt_media[512u + 84u] = 48u;
    gpt_media[512u + 85u] = 1u;
    gpt_media[2u * 512u + 304u] = 0x11u;
    /* Entry array occupies LBAs 2..3; usable payload is disjoint at 4..6. */
    gpt_media[2u * 512u + 336u] = 4u;
    gpt_media[2u * 512u + 337u] = 0u;
    gpt_media[2u * 512u + 338u] = 0u;
    gpt_media[2u * 512u + 339u] = 0u;
    gpt_media[2u * 512u + 340u] = 0u;
    gpt_media[2u * 512u + 341u] = 0u;
    gpt_media[2u * 512u + 342u] = 0u;
    gpt_media[2u * 512u + 343u] = 0u;
    gpt_media[2u * 512u + 344u] = 5u;
    gpt_media[2u * 512u + 345u] = 0u;
    gpt_media[2u * 512u + 346u] = 0u;
    gpt_media[2u * 512u + 347u] = 0u;
    gpt_media[2u * 512u + 348u] = 0u;
    gpt_media[2u * 512u + 349u] = 0u;
    gpt_media[2u * 512u + 350u] = 0u;
    gpt_media[2u * 512u + 351u] = 0u;

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
     * A protective MBR without a valid GPT signature must not fall back to
     * exposing its 0xEE entry as an ordinary MBR volume.
     */
    partition_test_media[512u] = 0u;
    gpt_type = PROS_PARTITION_GPT;
    if (partition_probe(&gpt_parent, &gpt_type) != PROS_PARTITION_ENOTSUP ||
        gpt_type != PROS_PARTITION_NONE)
        return -17;
    partition_test_media[512u] = 'E';
    if (partition_probe(&gpt_parent, &gpt_type) != PROS_PARTITION_OK ||
        gpt_type != PROS_PARTITION_GPT)
        return -18;

    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_OK ||
        count != 1u || partitions[0].first_lba != 4u ||
        partitions[0].sector_count != 2u ||
        partitions[0].type_guid[0] != 0x11u)
        return -4;
    /* Reserved GPT header bytes must remain zero, even with a valid CRC. */
    partition_test_media[512u + 20u] = 1u;
    partition_test_gpt_refresh_crc();
    count = 0xFFFFFFFFu;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -24;
    partition_test_media[512u + 20u] = 0u;
    partition_test_gpt_refresh_crc();

    /* GPT header and entry-array CRC mismatches must fail closed. */
    partition_test_media[512u + 56u] ^= 1u;
    count = 0xFFFFFFFFu;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -22;
    partition_test_media[512u + 56u] ^= 1u;
    partition_test_gpt_refresh_crc();

    partition_test_media[2u * 512u + 10u] ^= 1u;
    count = 0xFFFFFFFFu;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL || count != 0u)
        return -23;
    partition_test_media[2u * 512u + 10u] ^= 1u;
    partition_test_gpt_refresh_crc();

    /* Entry-array read I/O failure must not expose a stale output count. */
    partition_test_fail_lba = 2u;
    count = 0xFFFFFFFFu;
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EIO || count != 0u)
        return -27;
    partition_test_fail_lba = UINT64_MAX;

    /* GPT entry metadata must not start at LBA 0 or the primary header. */
    partition_test_media[512u + 72u] = 1u;
    partition_test_media[512u + 80u] = 1u;
    count = 0xFFFFFFFFu;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -14;
    partition_test_media[512u + 72u] = 2u;
    partition_test_media[512u + 80u] = 2u;

    /* Usable payload must not include the primary GPT header sector. */
    partition_test_media[512u + 40u] = 1u;
    count = 0xFFFFFFFFu;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -15;
    partition_test_media[512u + 40u] = 4u;

    /* Reject an entry array that overlaps usable partition payload. */
    partition_test_media[512u + 72u] = 4u;
    count = 0xFFFFFFFFu;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -12;
    partition_test_media[512u + 72u] = 2u;

    /* Reject a primary entry array that reaches the backup-header LBA. */
    partition_test_media[512u + 72u] = 7u;
    partition_test_media[512u + 80u] = 1u;
    count = 0xFFFFFFFFu;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -13;
    partition_test_media[512u + 72u] = 2u;
    partition_test_media[512u + 80u] = 2u;

    /* GPT entry sizes must follow the 8-byte alignment requirement. */
    partition_test_media[512u + 84u] = 129u;
    count = 0xFFFFFFFFu;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -16;
    partition_test_media[512u + 84u] = 48u;

    /* Reject a second GPT extent that overlaps the first. */
    partition_test_media[2u * 512u] = 0x22u;
    partition_test_media[2u * 512u + 32u] = 4u;
    partition_test_media[2u * 512u + 40u] = 5u;
    count = 0xFFFFFFFFu;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -10;

    /*
     * Capacity rejection must be tested with two adjacent, non-overlapping
     * extents; otherwise an overlap error could mask a capacity regression.
     * Entry 0 is [6, 7), and entry 1 is [4, 6).
     */
    partition_test_media[2u * 512u + 32u] = 6u;
    partition_test_media[2u * 512u + 40u] = 6u;
    partition_test_media[2u * 512u + 336u] = 4u;
    partition_test_media[2u * 512u + 344u] = 5u;
    count = 0xFFFFFFFFu;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 1u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -11;
    partition_test_media[2u * 512u] = 0u;

    /*
     * Failed reads must never leave a stale successful partition count for
     * callers that reuse the output variable.
     */
    count = 0xFFFFFFFFu;
    partition_test_media[2u * 512u + 336u] = 1u;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
            PROS_PARTITION_EINVAL ||
        count != 0u)
        return -5;
    partition_test_media[2u * 512u + 336u] = 4u;

    /* Reject a partition-entry array that extends beyond the device. */
    partition_test_media[512u + 80u] = 0xFFu;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
        PROS_PARTITION_EINVAL)
        return -6;

    /* Restore a valid entry count, then reject an empty entry array. */
    partition_test_media[512u + 80u] = 0u;
    partition_test_gpt_refresh_crc();
    if (partition_read_gpt(&gpt_parent, partitions, 4u, &count) !=
        PROS_PARTITION_EINVAL)
        return -7;

    return PROS_PARTITION_OK;
}
