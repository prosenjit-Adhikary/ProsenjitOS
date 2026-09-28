#include "ntfs.h"

#define PROS_NTFS_RECORD_WORK_MAX 65536u

#define PROS_NTFS_INDEX_NODE_MAX 128u
#define PROS_NTFS_UPCASE_RECORD 10u
#define PROS_NTFS_UPCASE_BYTES 131072u

/* Shared NTFS UpCase table state used by index name comparison/insertion. */
static uint8_t ntfs_upcase_data[PROS_NTFS_UPCASE_BYTES];
static const uint8_t *ntfs_active_upcase = 0;
static uint8_t ntfs_name_key_matches(const uint8_t *key,
                                      uint16_t key_size,
                                      const uint8_t *name_utf16le,
                                      uint8_t name_length);
static int ntfs_name_key_compare(const uint8_t *key,
                                 uint16_t key_size,
                                 const uint8_t *name_utf16le,
                                 uint8_t name_length);
static int ntfs_upcase_load(const struct pros_ntfs *fs);

static int ntfs_vfs_mount(struct pros_ntfs *fs);
static const struct pros_vfs_vnode_ops ntfs_vfs_vnode_ops;

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

static uint32_t abs_u8(int8_t value)
{
    return value < 0 ? (uint32_t)(-(int32_t)value) : (uint32_t)value;
}

int ntfs_mount(struct pros_ntfs *fs, struct pros_block_device *device)
{
    uint8_t boot[4096];
    uint16_t sector_size;
    int8_t clusters_per_record;

    if (!fs || !device || device->sector_size < 512u ||
        device->sector_size > sizeof(boot))
        return PROS_NTFS_EINVAL;
    if (block_device_read(device, 0, 1, boot) != PROS_BLOCK_OK)
        return PROS_NTFS_EIO;

    if (boot[3] != 'N' || boot[4] != 'T' || boot[5] != 'F' ||
        boot[6] != 'S' || boot[7] != ' ' || boot[8] != ' ' ||
        boot[9] != ' ' || boot[10] != ' ')
        return PROS_NTFS_ENOTSUP;

    sector_size = le16(&boot[11]);
    clusters_per_record = (int8_t)boot[64];

    if (sector_size < 512u || sector_size > 4096u ||
        (sector_size & (sector_size - 1u)) != 0u ||
        sector_size != device->sector_size ||
        boot[13] == 0u || boot[13] > 128u ||
        le64(&boot[40]) == 0u ||
        le64(&boot[48]) == 0u || le64(&boot[56]) == 0u ||
        clusters_per_record == 0)
        return PROS_NTFS_ECORRUPT;

    if (clusters_per_record > 0) {
        if ((uint32_t)clusters_per_record > 128u)
            return PROS_NTFS_ECORRUPT;
        fs->clusters_per_file_record = (uint32_t)clusters_per_record;
        fs->bytes_per_file_record =
            (uint32_t)clusters_per_record * boot[13] * sector_size;
    } else {
        uint32_t shift = abs_u8(clusters_per_record);
        if (shift > 31u)
            return PROS_NTFS_ECORRUPT;
        fs->clusters_per_file_record = 0u;
        fs->bytes_per_file_record = 1u << shift;
        if (fs->bytes_per_file_record < 1024u ||
            fs->bytes_per_file_record > 1048576u)
            return PROS_NTFS_ECORRUPT;
    }

    if (le64(&boot[40]) > device->sector_count ||
        le64(&boot[48]) >= le64(&boot[40]) ||
        le64(&boot[56]) >= le64(&boot[40]))
        return PROS_NTFS_ECORRUPT;

    if (vfs_mount_init(&fs->mount, PROS_FS_NTFS, device, fs, device->write ? 0u : 1u) !=
        PROS_VFS_OK)
        return PROS_NTFS_EINVAL;

    fs->bytes_per_sector = sector_size;
    fs->sectors_per_cluster = boot[13];
    fs->total_sectors = le64(&boot[40]);
    fs->mft_lcn = le64(&boot[48]);
    fs->mftmirr_lcn = le64(&boot[56]);

    if (vfs_mount_activate(&fs->mount) != PROS_VFS_OK)
        return PROS_NTFS_EINVAL;
    {
        int status = ntfs_bootstrap_mft(fs);
        if (status != PROS_NTFS_OK)
            return status;
    }
    return ntfs_vfs_mount(fs);
}

int ntfs_file_record_fixup(void *record, uint32_t record_size,
                           uint32_t bytes_per_sector)
{
    uint8_t *p = (uint8_t *)record;
    uint16_t usa_offset, usa_count;
    if (!p || record_size < 48u || bytes_per_sector < 512u ||
        record_size % bytes_per_sector != 0u)
        return PROS_NTFS_EINVAL;
    if (p[0]!='F' || p[1]!='I' || p[2]!='L' || p[3]!='E')
        return PROS_NTFS_ECORRUPT;
    usa_offset = le16(p + 4);
    usa_count = le16(p + 6);
    if (usa_count < 2u ||
        (uint32_t)usa_offset + (uint32_t)usa_count * 2u > record_size)
        return PROS_NTFS_ECORRUPT;
    {
        uint16_t sequence = le16(p + usa_offset);
        for (uint32_t i=1u; i<usa_count; ++i) {
            uint32_t end = i * bytes_per_sector - 2u;
            uint16_t saved = le16(p + end);
            if (saved != sequence)
                return PROS_NTFS_ECORRUPT;
            p[end] = p[usa_offset + i*2u];
            p[end+1u] = p[usa_offset + i*2u + 1u];
        }
    }
    return PROS_NTFS_OK;
}

int ntfs_read_file_record(const struct pros_ntfs *fs, uint64_t record_number,
                          void *buffer, uint32_t buffer_size)
{
    uint64_t byte_offset;
    uint64_t cluster_bytes;
    uint32_t remaining;
    uint32_t copied = 0u;
    uint8_t sector[4096];

    if (!fs || !buffer || buffer_size < fs->bytes_per_file_record ||
        fs->bytes_per_file_record == 0u || fs->sectors_per_cluster == 0u ||
        fs->bytes_per_sector > sizeof(sector))
        return PROS_NTFS_EINVAL;
    if (record_number > 0x0000FFFFFFFFFFFFull)
        return PROS_NTFS_EINVAL;
    if (record_number >
        (0xFFFFFFFFFFFFFFFFull / fs->bytes_per_file_record))
        return PROS_NTFS_EOVERFLOW;

    byte_offset = record_number * (uint64_t)fs->bytes_per_file_record;
    cluster_bytes = (uint64_t)fs->bytes_per_sector * fs->sectors_per_cluster;
    remaining = fs->bytes_per_file_record;

    while (remaining != 0u) {
        uint64_t logical_cluster = byte_offset / cluster_bytes;
        uint64_t cluster_offset = byte_offset % cluster_bytes;
        uint64_t cumulative = 0u;
        uint64_t physical_lcn = 0u;
        uint8_t sparse = 0u;
        uint8_t found = 0u;

        if (fs->mft_run_count == 0u) {
            physical_lcn = fs->mft_lcn + logical_cluster;
            if (physical_lcn < fs->mft_lcn)
                return PROS_NTFS_EOVERFLOW;
            found = 1u;
        } else {
            for (uint32_t r = 0u; r < fs->mft_run_count; ++r) {
                const struct pros_ntfs_run *run = &fs->mft_runs[r];
                if (logical_cluster >= cumulative &&
                    logical_cluster - cumulative < run->clusters) {
                    physical_lcn = run->lcn + (logical_cluster - cumulative);
                    if (physical_lcn < run->lcn)
                        return PROS_NTFS_EOVERFLOW;
                    sparse = run->sparse;
                    found = 1u;
                    break;
                }
                if (run->clusters > 0xFFFFFFFFFFFFFFFFull - cumulative)
                    return PROS_NTFS_EOVERFLOW;
                cumulative += run->clusters;
            }
        }

        if (!found)
            return PROS_NTFS_EIO;

        {
            uint32_t sector_index =
                (uint32_t)(cluster_offset / fs->bytes_per_sector);
            uint32_t in_sector =
                (uint32_t)(cluster_offset % fs->bytes_per_sector);
            uint32_t take = fs->bytes_per_sector - in_sector;
            uint64_t lba;

            if (take > remaining)
                take = remaining;
            if (sparse) {
                for (uint32_t i = 0u; i < take; ++i)
                    ((uint8_t *)buffer)[copied + i] = 0u;
            } else {
                if (sector_index >= fs->sectors_per_cluster ||
                    ntfs_cluster_lba(fs, physical_lcn, &lba) != PROS_NTFS_OK)
                    return PROS_NTFS_EIO;
                lba += sector_index;
                if (block_device_read(fs->mount.device, lba, 1u, sector) !=
                    PROS_BLOCK_OK)
                    return PROS_NTFS_EIO;
                for (uint32_t i = 0u; i < take; ++i)
                    ((uint8_t *)buffer)[copied + i] = sector[in_sector + i];
            }
            copied += take;
            remaining -= take;
            byte_offset += take;
        }
    }

    return ntfs_file_record_fixup(buffer, fs->bytes_per_file_record,
                                   fs->bytes_per_sector);
}

int ntfs_cluster_lba(const struct pros_ntfs *fs, uint64_t lcn,
                     uint64_t *lba)
{
    uint64_t offset;
    if (!fs || !lba || !fs->mount.active || lcn > 0x0000FFFFFFFFFFFFull)
        return PROS_NTFS_EINVAL;
    if (fs->sectors_per_cluster == 0u)
        return PROS_NTFS_EINVAL;
    if (lcn > 0xFFFFFFFFFFFFFFFFull / fs->sectors_per_cluster)
        return PROS_NTFS_EOVERFLOW;
    offset = lcn * fs->sectors_per_cluster;
    if (offset > fs->total_sectors ||
        fs->sectors_per_cluster > fs->total_sectors - offset ||
        offset + fs->sectors_per_cluster > fs->mount.device->sector_count)
        return PROS_NTFS_EIO;
    *lba = offset;
    return PROS_NTFS_OK;
}

int ntfs_decode_runlist(const uint8_t *runlist, uint32_t length,
                        struct pros_ntfs_run *runs, uint32_t capacity,
                        uint32_t *run_count)
{
    uint32_t pos = 0u, count = 0u;
    int64_t current_lcn = 0;
    uint8_t terminated = 0u;
    if (!runlist || !runs || !run_count || length == 0u || capacity == 0u)
        return PROS_NTFS_EINVAL;
    while (pos < length) {
        uint8_t header = runlist[pos++];
        uint32_t len_bytes = header & 0x0Fu;
        uint32_t off_bytes = (header >> 4) & 0x0Fu;
        uint64_t run_len = 0u, raw = 0u;
        int64_t delta = 0;
        if (header == 0u) {
            terminated = 1u;
            break;
        }
        if (len_bytes == 0u || len_bytes > 8u || off_bytes > 8u ||
            pos + len_bytes + off_bytes > length)
            return PROS_NTFS_ECORRUPT;
        for (uint32_t i=0u;i<len_bytes;++i)
            run_len |= (uint64_t)runlist[pos+i] << (8u*i);
        pos += len_bytes;
        if (off_bytes) {
            for (uint32_t i=0u;i<off_bytes;++i)
                raw |= (uint64_t)runlist[pos+i] << (8u*i);
            if (raw & (1ull << (off_bytes*8u-1u)))
                raw |= ~0ull << (off_bytes*8u);
            delta = (int64_t)raw;
            current_lcn += delta;
            if (current_lcn < 0) return PROS_NTFS_ECORRUPT;
        }
        pos += off_bytes;
        if (run_len == 0u || count >= capacity) return PROS_NTFS_EOVERFLOW;
        runs[count].lcn = off_bytes ? (uint64_t)current_lcn : 0u;
        runs[count].clusters = run_len;
        runs[count].sparse = off_bytes == 0u ? 1u : 0u;
        ++count;
    }
    if (!terminated)
        return PROS_NTFS_ECORRUPT;
    if (count == 0u) return PROS_NTFS_ENOTSUP;
    *run_count = count;
    return PROS_NTFS_OK;
}

int ntfs_find_attribute(const void *record, uint32_t record_size,
                        uint32_t type, uint32_t *offset)
{
    const uint8_t *p = (const uint8_t *)record;
    uint16_t first;
    uint32_t pos;
    if (!p || !offset || record_size < 24u) return PROS_NTFS_EINVAL;
    if (p[0]!='F' || p[1]!='I' || p[2]!='L' || p[3]!='E')
        return PROS_NTFS_ECORRUPT;
    first = le16(p + 20);
    pos = first;
    while (pos + 8u <= record_size) {
        uint32_t len = le32(p + pos + 4u);
        uint32_t atype = le32(p + pos);
        if (atype == 0xFFFFFFFFu) return PROS_NTFS_ENOTSUP;
        if (len < 16u || pos + len > record_size) return PROS_NTFS_ECORRUPT;
        if (atype == type) { *offset = pos; return PROS_NTFS_OK; }
        pos += len;
    }
    return PROS_NTFS_ENOTSUP;
}

static int ntfs_find_unnamed_attribute(const void *record,
                                           uint32_t record_size,
                                           uint32_t type,
                                           uint32_t *offset)
{
    const uint8_t *p = (const uint8_t *)record;
    uint16_t first;
    uint32_t pos;

    if (!p || !offset || record_size < 24u)
        return PROS_NTFS_EINVAL;
    if (p[0] != 'F' || p[1] != 'I' || p[2] != 'L' || p[3] != 'E')
        return PROS_NTFS_ECORRUPT;
    first = le16(p + 20u);
    if (first < 24u || first >= record_size)
        return PROS_NTFS_ECORRUPT;
    pos = first;
    while (pos + 8u <= record_size) {
        uint32_t atype = le32(p + pos);
        uint32_t len = le32(p + pos + 4u);
        if (atype == 0xFFFFFFFFu)
            return PROS_NTFS_ENOTSUP;
        if (len < 16u || pos + len > record_size)
            return PROS_NTFS_ECORRUPT;
        if (atype == type && p[pos + 9u] == 0u) {
            *offset = pos;
            return PROS_NTFS_OK;
        }
        pos += len;
    }
    return PROS_NTFS_ENOTSUP;
}

int ntfs_attribute_data(const void *record, uint32_t record_size,
                        uint32_t attr_offset, const uint8_t **data,
                        uint32_t *data_size)
{
    const uint8_t *a = (const uint8_t *)record;
    uint8_t nonresident;
    uint16_t value_offset;
    uint32_t value_length;
    if (!a || !data || !data_size || attr_offset + 24u > record_size)
        return PROS_NTFS_EINVAL;
    if (le32(a + attr_offset) == 0xFFFFFFFFu) return PROS_NTFS_ENOTSUP;
    if (le32(a + attr_offset + 4u) < 24u) return PROS_NTFS_ECORRUPT;
    nonresident = a[attr_offset + 8u];
    if (nonresident) return PROS_NTFS_ENOTSUP;
    value_length = le32(a + attr_offset + 16u);
    value_offset = le16(a + attr_offset + 20u);
    if ((uint32_t)value_offset + value_length >
        le32(a + attr_offset + 4u) ||
        attr_offset + (uint32_t)value_offset + value_length > record_size)
        return PROS_NTFS_ECORRUPT;
    *data = a + attr_offset + value_offset;
    *data_size = value_length;
    return PROS_NTFS_OK;
}

int ntfs_attribute_runlist(const void *record, uint32_t record_size,
                           uint32_t attr_offset, const uint8_t **runlist,
                           uint32_t *runlist_size)
{
    const uint8_t *a = (const uint8_t *)record;
    uint32_t attr_len;
    uint16_t run_offset;
    uint64_t allocated;
    if (!a || !runlist || !runlist_size || attr_offset + 64u > record_size)
        return PROS_NTFS_EINVAL;
    attr_len = le32(a + attr_offset + 4u);
    if (attr_len < 64u ||
        attr_offset > record_size ||
        attr_len > record_size - attr_offset)
        return PROS_NTFS_ECORRUPT;
    if (a[attr_offset + 8u] == 0u)
        return PROS_NTFS_ENOTSUP;
    run_offset = le16(a + attr_offset + 32u);
    allocated = le64(a + attr_offset + 40u);
    if ((uint32_t)run_offset >= attr_len || allocated == 0u ||
        run_offset < 64u)
        return PROS_NTFS_ECORRUPT;
    *runlist = a + attr_offset + run_offset;
    *runlist_size = attr_len - run_offset;
    return PROS_NTFS_OK;
}

int ntfs_file_read(const struct pros_ntfs *fs, uint64_t record_number,
                   uint64_t offset, void *buffer, uint32_t size,
                   uint32_t *bytes_read)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    struct pros_ntfs_run runs[PROS_NTFS_MFT_RUN_MAX];
    const uint8_t *data;
    const uint8_t *runlist;
    uint32_t data_size, attr_offset, attr_length, runlist_size, run_count;
    uint64_t real_size;
    uint32_t wanted;
    uint32_t copied = 0u;

    if (!fs || !buffer || !bytes_read || size == 0u ||
        fs->bytes_per_file_record > sizeof(record))
        return PROS_NTFS_EINVAL;
    *bytes_read = 0u;

    if (ntfs_read_file_record(fs, record_number, record, sizeof(record)) !=
        PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    if (ntfs_find_unnamed_attribute(record, fs->bytes_per_file_record,
                                    0x80u, &attr_offset) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    attr_length = le32(record + attr_offset + 4u);
    if (attr_length < 24u ||
        attr_offset + attr_length > fs->bytes_per_file_record)
        return PROS_NTFS_ECORRUPT;

    /* The first DATA attribute is the unnamed/default data stream. */
    if (record[attr_offset + 9u] != 0u)
        return PROS_NTFS_ENOTSUP;

    if (record[attr_offset + 8u] == 0u) {
        if (ntfs_attribute_data(record, fs->bytes_per_file_record,
                                attr_offset, &data, &data_size) !=
            PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
        if (offset > data_size)
            return PROS_NTFS_EINVAL;
        wanted = data_size - (uint32_t)offset;
        if (wanted > size)
            wanted = size;
        for (uint32_t i = 0u; i < wanted; ++i)
            ((uint8_t *)buffer)[i] = data[(uint32_t)offset + i];
        *bytes_read = wanted;
        return PROS_NTFS_OK;
    }

    if (record[attr_offset + 8u] != 1u || attr_length < 64u)
        return PROS_NTFS_ECORRUPT;
    if (le64(record + attr_offset + 16u) != 0u)
        return PROS_NTFS_ENOTSUP;
    if (le16(record + attr_offset + 12u) != 0u)
        return PROS_NTFS_ENOTSUP;

    real_size = le64(record + attr_offset + 48u);
    if (offset > real_size)
        return PROS_NTFS_EINVAL;
    wanted = size;
    if ((uint64_t)wanted > real_size - offset)
        wanted = (uint32_t)(real_size - offset);
    if (wanted == 0u)
        return PROS_NTFS_OK;

    if (ntfs_attribute_runlist(record, fs->bytes_per_file_record,
                               attr_offset, &runlist, &runlist_size) !=
        PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (ntfs_decode_runlist(runlist, runlist_size, runs,
                            PROS_NTFS_MFT_RUN_MAX, &run_count) !=
        PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    {
        uint32_t cluster_bytes = fs->bytes_per_sector *
                                 fs->sectors_per_cluster;
        uint64_t file_offset = offset;

        if (cluster_bytes == 0u || fs->bytes_per_sector > 4096u)
            return PROS_NTFS_EINVAL;

        while (copied < wanted) {
            uint64_t logical_cluster = file_offset / cluster_bytes;
            uint64_t cluster_offset = file_offset % cluster_bytes;
            uint64_t cumulative = 0u;
            uint64_t physical_lcn = 0u;
            uint8_t sparse = 0u;
            uint8_t found = 0u;
            uint32_t sector_index;
            uint32_t in_sector;
            uint32_t take;
            uint64_t lba;
            uint8_t sector[4096];

            for (uint32_t r = 0u; r < run_count; ++r) {
                if (logical_cluster >= cumulative &&
                    logical_cluster - cumulative < runs[r].clusters) {
                    physical_lcn = runs[r].lcn +
                                   (logical_cluster - cumulative);
                    if (physical_lcn < runs[r].lcn)
                        return PROS_NTFS_EOVERFLOW;
                    sparse = runs[r].sparse;
                    found = 1u;
                    break;
                }
                if (runs[r].clusters >
                    0xFFFFFFFFFFFFFFFFull - cumulative)
                    return PROS_NTFS_EOVERFLOW;
                cumulative += runs[r].clusters;
            }
            if (!found)
                return PROS_NTFS_EIO;

            sector_index = (uint32_t)(cluster_offset / fs->bytes_per_sector);
            in_sector = (uint32_t)(cluster_offset % fs->bytes_per_sector);
            take = fs->bytes_per_sector - in_sector;
            if (take > wanted - copied)
                take = wanted - copied;
            if (sector_index >= fs->sectors_per_cluster)
                return PROS_NTFS_EIO;

            if (sparse) {
                for (uint32_t i = 0u; i < take; ++i)
                    ((uint8_t *)buffer)[copied + i] = 0u;
            } else {
                if (ntfs_cluster_lba(fs, physical_lcn, &lba) !=
                    PROS_NTFS_OK)
                    return PROS_NTFS_EIO;
                lba += sector_index;
                if (block_device_read(fs->mount.device, lba, 1u, sector) !=
                    PROS_BLOCK_OK)
                    return PROS_NTFS_EIO;
                for (uint32_t i = 0u; i < take; ++i)
                    ((uint8_t *)buffer)[copied + i] =
                        sector[in_sector + i];
            }
            copied += take;
            file_offset += take;
        }
    }

    *bytes_read = copied;
    return PROS_NTFS_OK;
}


/*
 * Allocate one contiguous extent from the volume bitmap ($Bitmap, MFT
 * record 6). NTFS records one allocation state bit per cluster, with bit 0
 * of byte 0 representing LCN 0. The helper deliberately returns one
 * contiguous extent; runlist expansion can then append that extent without
 * inventing sparse mappings.
 */
static int ntfs_bitmap_update_clusters(struct pros_ntfs *fs,
                                          uint64_t first_lcn,
                                          uint64_t cluster_count,
                                          uint8_t allocate)
{
    static uint8_t bitmap_chunk[4096];
    static uint8_t bitmap_sector[4096];
    /* Keep the large MFT record workspace out of the kernel stack. */
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    const uint8_t *runlist;
    uint32_t attr_offset, runlist_size, run_count;
    struct pros_ntfs_run runs[PROS_NTFS_MFT_RUN_MAX];
    uint64_t bitmap_size, total_clusters;
    uint64_t start_byte, end_byte;
    uint32_t got;

    if (!fs || cluster_count == 0u ||
        first_lcn > 0xFFFFFFFFFFFFFFFFull - (cluster_count - 1u) ||
        fs->bytes_per_file_record > sizeof(record))
        return PROS_NTFS_EINVAL;

    if (fs->sectors_per_cluster == 0u)
        return PROS_NTFS_EINVAL;
    total_clusters = fs->total_sectors / fs->sectors_per_cluster;
    if (total_clusters == 0u ||
        first_lcn >= total_clusters ||
        cluster_count > total_clusters - first_lcn)
        return PROS_NTFS_EOVERFLOW;

    if (ntfs_read_file_record(fs, 6u, record, sizeof(record)) !=
        PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    if (ntfs_find_unnamed_attribute(record, fs->bytes_per_file_record,
                                    0x80u, &attr_offset) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (record[attr_offset + 9u] != 0u)
        return PROS_NTFS_ENOTSUP;

    if (record[attr_offset + 8u] == 0u) {
        uint32_t resident_size;
        if (ntfs_attribute_data(record, fs->bytes_per_file_record,
                                (uint32_t)attr_offset,
                                (const uint8_t **)&runlist,
                                &resident_size) != PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
        bitmap_size = resident_size;
    } else {
        if (record[attr_offset + 8u] != 1u ||
            le64(record + attr_offset + 16u) != 0u ||
            le16(record + attr_offset + 12u) != 0u)
            return PROS_NTFS_ENOTSUP;
        bitmap_size = le64(record + attr_offset + 48u);
        if (ntfs_attribute_runlist(record, fs->bytes_per_file_record,
                                    attr_offset, &runlist,
                                    &runlist_size) != PROS_NTFS_OK ||
            ntfs_decode_runlist(runlist, runlist_size, runs,
                                PROS_NTFS_MFT_RUN_MAX, &run_count) !=
                PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
    }
    if (bitmap_size < total_clusters / 8u +
                      ((total_clusters % 8u) != 0u ? 1u : 0u))
        return PROS_NTFS_ECORRUPT;

    start_byte = first_lcn / 8u;
    end_byte = (first_lcn + cluster_count - 1u) / 8u;
    while (start_byte <= end_byte) {
        uint32_t byte_count =
            (uint32_t)(end_byte - start_byte + 1u);
        if (byte_count > sizeof(bitmap_chunk))
            return PROS_NTFS_EOVERFLOW;

        if (ntfs_file_read(fs, 6u, start_byte, bitmap_chunk,
                           byte_count, &got) != PROS_NTFS_OK ||
            got != byte_count)
            return PROS_NTFS_EIO;

        for (uint32_t i = 0u; i < byte_count; ++i) {
            uint64_t byte_lcn = (start_byte + i) * 8u;
            uint8_t mask = 0u;
            for (uint32_t j = 0u; j < 8u; ++j) {
                uint64_t bit_lcn = byte_lcn + j;
                if (bit_lcn >= first_lcn &&
                    bit_lcn < first_lcn + cluster_count)
                    mask |= (uint8_t)(1u << j);
            }
            if (allocate)
                bitmap_chunk[i] |= mask;
            else
                bitmap_chunk[i] &= (uint8_t)~mask;
        }

        if (record[attr_offset + 8u] == 0u) {
            uint32_t written = 0u;
            if (ntfs_file_write(fs, 6u, start_byte, bitmap_chunk,
                                byte_count, &written) != PROS_NTFS_OK ||
                written != byte_count)
                return PROS_NTFS_EIO;
        } else {
            /*
             * Do not route nonresident $Bitmap writes back through
             * ntfs_file_write(). Allocation invokes this helper from inside
             * ntfs_file_write(), so that recursive path would reuse the
             * caller's static record workspace. Write the already-updated
             * bitmap bytes directly through its decoded runlist instead.
             */
            uint64_t done = 0u;
            uint32_t cluster_bytes =
                fs->bytes_per_sector * fs->sectors_per_cluster;
            if (cluster_bytes == 0u || fs->bytes_per_sector > 4096u)
                return PROS_NTFS_EINVAL;
            while (done < byte_count) {
                uint64_t file_byte = start_byte + done;
                uint64_t logical_cluster = file_byte / cluster_bytes;
                uint32_t cluster_offset =
                    (uint32_t)(file_byte % cluster_bytes);
                uint64_t cumulative = 0u;
                uint64_t physical_lcn = 0u;
                uint8_t sparse = 0u, found = 0u;
                uint32_t sector_index;
                uint32_t in_sector;
                uint32_t take;
                uint64_t lba;

                for (uint32_t r = 0u; r < run_count; ++r) {
                    if (logical_cluster >= cumulative &&
                        logical_cluster - cumulative < runs[r].clusters) {
                        physical_lcn = runs[r].lcn +
                                       logical_cluster - cumulative;
                        if (physical_lcn < runs[r].lcn)
                            return PROS_NTFS_EOVERFLOW;
                        sparse = runs[r].sparse;
                        found = 1u;
                        break;
                    }
                    if (runs[r].clusters >
                        0xFFFFFFFFFFFFFFFFull - cumulative)
                        return PROS_NTFS_EOVERFLOW;
                    cumulative += runs[r].clusters;
                }
                if (!found || sparse)
                    return PROS_NTFS_EIO;

                sector_index = cluster_offset / fs->bytes_per_sector;
                in_sector = cluster_offset % fs->bytes_per_sector;
                take = fs->bytes_per_sector - in_sector;
                if ((uint64_t)take > (uint64_t)byte_count - done)
                    take = (uint32_t)((uint64_t)byte_count - done);
                if (sector_index >= fs->sectors_per_cluster ||
                    ntfs_cluster_lba(fs, physical_lcn, &lba) !=
                        PROS_NTFS_OK)
                    return PROS_NTFS_EIO;
                lba += sector_index;

                if (in_sector != 0u || take != fs->bytes_per_sector) {
                    if (block_device_read(fs->mount.device, lba, 1u,
                                          bitmap_sector) != PROS_BLOCK_OK)
                        return PROS_NTFS_EIO;
                }
                for (uint32_t i = 0u; i < take; ++i)
                    bitmap_sector[in_sector + i] =
                        bitmap_chunk[(uint32_t)done + i];
                if (block_device_write(fs->mount.device, lba, 1u,
                                       bitmap_sector) != PROS_BLOCK_OK)
                    return PROS_NTFS_EIO;
                done += take;
            }
        }
        start_byte += byte_count;
    }
    return PROS_NTFS_OK;
}

static int ntfs_bitmap_allocate_clusters_from(struct pros_ntfs *fs,
                                               uint64_t start_lcn,
                                               uint64_t cluster_count,
                                               uint64_t *first_lcn)
{
    static uint8_t bitmap_chunk[4096];
    /* Keep the large MFT record workspace out of the kernel stack. */
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    const uint8_t *runlist;
    uint32_t attr_offset, runlist_size, run_count;
    struct pros_ntfs_run runs[PROS_NTFS_MFT_RUN_MAX];
    uint64_t bitmap_size, total_clusters;
    uint64_t scan_lcn, candidate = 0u, candidate_len = 0u;
    uint32_t got;

    if (!fs || !first_lcn || cluster_count == 0u ||
        fs->bytes_per_file_record > sizeof(record))
        return PROS_NTFS_EINVAL;
    *first_lcn = 0u;

    if (fs->sectors_per_cluster == 0u)
        return PROS_NTFS_ECORRUPT;
    total_clusters = fs->total_sectors / fs->sectors_per_cluster;
    if (total_clusters == 0u)
        return PROS_NTFS_ECORRUPT;
    if (start_lcn >= total_clusters ||
        cluster_count > total_clusters - start_lcn)
        return PROS_NTFS_EOVERFLOW;

    if (ntfs_read_file_record(fs, 6u, record, sizeof(record)) !=
        PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    if (ntfs_find_unnamed_attribute(record, fs->bytes_per_file_record,
                                    0x80u, &attr_offset) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (record[attr_offset + 9u] != 0u)
        return PROS_NTFS_ENOTSUP;

    if (record[attr_offset + 8u] == 0u) {
        uint32_t resident_size;
        if (ntfs_attribute_data(record, fs->bytes_per_file_record,
                                attr_offset, (const uint8_t **)&runlist,
                                &resident_size) != PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
        bitmap_size = resident_size;
    } else {
        if (record[attr_offset + 8u] != 1u ||
            le64(record + attr_offset + 16u) != 0u ||
            le16(record + attr_offset + 12u) != 0u)
            return PROS_NTFS_ENOTSUP;
        bitmap_size = le64(record + attr_offset + 48u);
        if (ntfs_attribute_runlist(record, fs->bytes_per_file_record,
                                    attr_offset, &runlist,
                                    &runlist_size) != PROS_NTFS_OK ||
            ntfs_decode_runlist(runlist, runlist_size, runs,
                                PROS_NTFS_MFT_RUN_MAX, &run_count) !=
                PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
    }
    if (bitmap_size < total_clusters / 8u +
                      ((total_clusters % 8u) != 0u ? 1u : 0u))
        return PROS_NTFS_ECORRUPT;

    scan_lcn = start_lcn;
    while (scan_lcn < total_clusters) {
        uint64_t byte_offset = scan_lcn / 8u;
        uint64_t bitmap_remaining = bitmap_size - byte_offset;
        uint32_t want = bitmap_remaining > 0xFFFFFFFFull ?
                        0xFFFFFFFFu : (uint32_t)bitmap_remaining;
        if (want > sizeof(bitmap_chunk))
            want = sizeof(bitmap_chunk);
        if (want == 0u)
            return PROS_NTFS_ECORRUPT;

        got = 0u;
        if (ntfs_file_read(fs, 6u, byte_offset, bitmap_chunk, want,
                           &got) != PROS_NTFS_OK || got != want)
            return PROS_NTFS_EIO;

        for (uint32_t b = 0u; b < want && scan_lcn < total_clusters; ++b) {
            uint8_t value = bitmap_chunk[b];
            uint32_t bit_start = (uint32_t)(scan_lcn & 7u);
            uint32_t bits = 8u - bit_start;
            if ((uint64_t)bits > total_clusters - scan_lcn)
                bits = (uint32_t)(total_clusters - scan_lcn);

            for (uint32_t bit = 0u; bit < bits; ++bit) {
                uint64_t lcn = scan_lcn + bit;
                uint8_t used = (uint8_t)((value >> (bit + bit_start)) & 1u);
                if (!used) {
                    if (candidate_len == 0u)
                        candidate = lcn;
                    ++candidate_len;
                    if (candidate_len == cluster_count) {
                        if (ntfs_bitmap_update_clusters(fs, candidate,
                                                        cluster_count, 1u) !=
                            PROS_NTFS_OK)
                            return PROS_NTFS_EIO;
                        *first_lcn = candidate;
                        return PROS_NTFS_OK;
                    }
                } else {
                    candidate_len = 0u;
                }
            }
            scan_lcn += bits;
        }
    }
    return PROS_NTFS_EIO;
}

static int ntfs_bitmap_allocate_clusters_exact(struct pros_ntfs *fs,
                                                        uint64_t first_lcn,
                                                        uint64_t cluster_count)
{
    static uint8_t bitmap_chunk[4096];
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    const uint8_t *runlist;
    uint32_t attr_offset, runlist_size, run_count;
    struct pros_ntfs_run runs[PROS_NTFS_MFT_RUN_MAX];
    uint64_t bitmap_size, total_clusters;
    uint64_t start_byte, end_byte;
    uint32_t got;

    if (!fs || cluster_count == 0u ||
        first_lcn > 0xFFFFFFFFFFFFFFFFull - (cluster_count - 1u) ||
        fs->bytes_per_file_record > sizeof(record))
        return PROS_NTFS_EINVAL;

    if (fs->sectors_per_cluster == 0u)
        return PROS_NTFS_ECORRUPT;
    total_clusters = fs->total_sectors / fs->sectors_per_cluster;
    if (total_clusters == 0u || first_lcn >= total_clusters ||
        cluster_count > total_clusters - first_lcn)
        return PROS_NTFS_EOVERFLOW;

    if (ntfs_read_file_record(fs, 6u, record, sizeof(record)) !=
        PROS_NTFS_OK ||
        ntfs_find_unnamed_attribute(record, fs->bytes_per_file_record,
                                    0x80u, &attr_offset) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (record[attr_offset + 9u] != 0u)
        return PROS_NTFS_ENOTSUP;

    if (record[attr_offset + 8u] == 0u) {
        uint32_t resident_size;
        if (ntfs_attribute_data(record, fs->bytes_per_file_record,
                                attr_offset, (const uint8_t **)&runlist,
                                &resident_size) != PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
        bitmap_size = resident_size;
    } else {
        if (record[attr_offset + 8u] != 1u ||
            le64(record + attr_offset + 16u) != 0u ||
            le16(record + attr_offset + 12u) != 0u)
            return PROS_NTFS_ENOTSUP;
        bitmap_size = le64(record + attr_offset + 48u);
        if (ntfs_attribute_runlist(record, fs->bytes_per_file_record,
                                    attr_offset, &runlist,
                                    &runlist_size) != PROS_NTFS_OK ||
            ntfs_decode_runlist(runlist, runlist_size, runs,
                                PROS_NTFS_MFT_RUN_MAX, &run_count) !=
                PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
    }
    if (bitmap_size < total_clusters / 8u +
                      ((total_clusters % 8u) != 0u ? 1u : 0u))
        return PROS_NTFS_ECORRUPT;

    start_byte = first_lcn / 8u;
    end_byte = (first_lcn + cluster_count - 1u) / 8u;
    if (end_byte - start_byte + 1u > sizeof(bitmap_chunk))
        return PROS_NTFS_EOVERFLOW;

    got = 0u;
    if (ntfs_file_read(fs, 6u, start_byte, bitmap_chunk,
                       (uint32_t)(end_byte - start_byte + 1u), &got) !=
            PROS_NTFS_OK ||
        got != (uint32_t)(end_byte - start_byte + 1u))
        return PROS_NTFS_EIO;

    for (uint64_t lcn = first_lcn; lcn < first_lcn + cluster_count; ++lcn) {
        uint32_t byte = (uint32_t)((lcn - start_byte * 8u) / 8u);
        uint32_t bit = (uint32_t)(lcn & 7u);
        if ((bitmap_chunk[byte] & (uint8_t)(1u << bit)) != 0u)
            return PROS_NTFS_EIO;
    }
    return ntfs_bitmap_update_clusters(fs, first_lcn, cluster_count, 1u);
}

int ntfs_bitmap_allocate_clusters(struct pros_ntfs *fs,
                                   uint64_t cluster_count,
                                   uint64_t *first_lcn)
{
    return ntfs_bitmap_allocate_clusters_from(fs, 0u, cluster_count,
                                              first_lcn);
}

static int ntfs_bitmap_free_clusters(struct pros_ntfs *fs,
                                     uint64_t first_lcn,
                                     uint64_t cluster_count)
{
    return ntfs_bitmap_update_clusters(fs, first_lcn, cluster_count, 0u);
}

static int ntfs_write_file_record(struct pros_ntfs *fs,
                                   uint64_t record_number,
                                   uint8_t *record);

int ntfs_mft_free_record(struct pros_ntfs *fs, uint64_t record_number)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    uint32_t attr_offset;
    const uint8_t *data;
    uint32_t data_size;
    uint8_t *bitmap;
    uint32_t byte;

    if (!fs || record_number < 24u ||
        record_number > 0x0000FFFFFFFFFFFFull ||
        fs->bytes_per_file_record > sizeof(record))
        return PROS_NTFS_EINVAL;
    if (ntfs_read_file_record(fs, 0u, record, sizeof(record)) != PROS_NTFS_OK ||
        ntfs_find_attribute(record, fs->bytes_per_file_record, 0xB0u,
                            &attr_offset) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (record[attr_offset + 9u] != 0u || record[attr_offset + 8u] != 0u)
        return PROS_NTFS_ENOTSUP;
    if (ntfs_attribute_data(record, fs->bytes_per_file_record, attr_offset,
                            &data, &data_size) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    byte = (uint32_t)(record_number / 8u);
    if (byte >= data_size)
        return PROS_NTFS_EOVERFLOW;
    bitmap = (uint8_t *)data;
    bitmap[byte] &= (uint8_t)~(1u << (record_number & 7u));
    return ntfs_write_file_record(fs, 0u, record);
}

int ntfs_create_file(struct pros_ntfs *fs,
                     uint64_t parent_record_number,
                     uint64_t *record_number,
                     const uint16_t *name_utf16le,
                     uint8_t name_length)
{
    uint64_t allocated_record = 0u;
    uint64_t parent_reference;
    uint8_t parent_record[PROS_NTFS_RECORD_WORK_MAX];
    int rc;

    if (!fs || !record_number || !name_utf16le || name_length == 0u ||
        fs->bytes_per_file_record > sizeof(parent_record))
        return PROS_NTFS_EINVAL;
    if (!fs->mount.active || fs->mount.read_only ||
        !fs->mount.device || !fs->mount.device->write)
        return PROS_NTFS_ENOTSUP;
    *record_number = 0u;

    if (ntfs_read_file_record(fs, parent_record_number, parent_record,
                              sizeof(parent_record)) != PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    if ((le16(parent_record + 22u) & 0x03u) != 0x03u)
        return PROS_NTFS_ENOTSUP;

    parent_reference = (parent_record_number & 0x0000FFFFFFFFFFFFull) |
                       ((uint64_t)le16(parent_record + 16u) << 48);

    /*
     * Reject an existing case-insensitive name before consuming an MFT slot.
     * A missing resident entry is reported as ENOTSUP by the current lookup
     * path; that is the expected fall-through for a new name.
     */
    {
        uint64_t existing_reference = 0u;
        int lookup_status = ntfs_directory_lookup(fs, parent_record_number,
                                                   (const uint8_t *)
                                                   name_utf16le,
                                                   name_length,
                                                   &existing_reference);
        if (lookup_status == PROS_NTFS_OK)
            return PROS_NTFS_EINVAL;
        if (lookup_status != PROS_NTFS_ENOTSUP)
            return lookup_status;
    }

    rc = ntfs_mft_allocate_record(fs, &allocated_record);
    if (rc != PROS_NTFS_OK)
        return rc;

    rc = ntfs_mft_initialize_record(fs, allocated_record, 0u);
    if (rc == PROS_NTFS_OK)
        rc = ntfs_mft_add_standard_information(fs, allocated_record, 0x80u);
    if (rc == PROS_NTFS_OK)
        rc = ntfs_mft_add_file_name(fs, allocated_record, parent_reference,
                                    name_utf16le, name_length, 1u);
    if (rc == PROS_NTFS_OK)
        rc = ntfs_mft_add_empty_data(fs, allocated_record);
    if (rc == PROS_NTFS_OK)
        rc = ntfs_index_root_insert(fs, parent_record_number,
                                    allocated_record);

    if (rc != PROS_NTFS_OK) {
        if (ntfs_mft_free_record(fs, allocated_record) != PROS_NTFS_OK)
            return PROS_NTFS_EIO;
        return rc;
    }

    *record_number = allocated_record;
    return PROS_NTFS_OK;
}

int ntfs_mft_initialize_record(struct pros_ntfs *fs,
                                            uint64_t record_number,
                                            uint8_t directory)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    static uint8_t old_record[PROS_NTFS_RECORD_WORK_MAX];
    uint32_t first_attr;
    uint32_t used_size;
    uint32_t usa_count;
    uint16_t sequence = 1u;

    if (!fs || fs->bytes_per_file_record < 512u ||
        fs->bytes_per_file_record > sizeof(record) ||
        fs->bytes_per_sector < 512u ||
        fs->bytes_per_file_record % fs->bytes_per_sector != 0u ||
        record_number > 0x0000FFFFFFFFFFFFull)
        return PROS_NTFS_EINVAL;

    /*
     * Reusing an MFT slot must invalidate stale file references. Preserve the
     * previous FILE record sequence and advance it; an uninitialized/free
     * slot starts at sequence 1.
     */
    {
        int old_status = ntfs_read_file_record(fs, record_number,
                                               old_record,
                                               sizeof(old_record));
        if (old_status == PROS_NTFS_OK &&
            old_record[0] == 'F' && old_record[1] == 'I' &&
            old_record[2] == 'L' && old_record[3] == 'E') {
            sequence = le16(old_record + 16u);
            if (sequence == 0xFFFFu)
                sequence = 1u;
            else {
                ++sequence;
                if (sequence == 0u)
                    sequence = 1u;
            }
        } else if (old_status == PROS_NTFS_EIO) {
            return PROS_NTFS_EIO;
        }
    }

    usa_count = fs->bytes_per_file_record / fs->bytes_per_sector + 1u;
    first_attr = 56u;
    if ((uint32_t)first_attr + 8u > fs->bytes_per_file_record ||
        usa_count < 2u ||
        (uint32_t)48u + usa_count * 2u > first_attr)
        return PROS_NTFS_ECORRUPT;

    for (uint32_t i = 0u; i < fs->bytes_per_file_record; ++i)
        record[i] = 0u;

    record[0] = 'F';
    record[1] = 'I';
    record[2] = 'L';
    record[3] = 'E';

    record[4] = 48u;
    record[5] = 0u;
    record[6] = (uint8_t)usa_count;
    record[7] = (uint8_t)(usa_count >> 8);

    /* Advance the MFT sequence on reuse; sequence zero is never emitted. */
    record[16] = (uint8_t)sequence;
    record[17] = (uint8_t)(sequence >> 8);

    /* One hard-link/name reference is expected once $FILE_NAME is added. */
    record[18] = 1u;
    record[19] = 0u;

    /* Next attribute identifier starts at zero. */
    record[40] = 0u;
    record[41] = 0u;

    /* Store the MFT record number in the standard header field. */
    record[44] = (uint8_t)record_number;
    record[45] = (uint8_t)(record_number >> 8);
    record[46] = (uint8_t)(record_number >> 16);
    record[47] = (uint8_t)(record_number >> 24);

    record[20] = (uint8_t)first_attr;
    record[21] = (uint8_t)(first_attr >> 8);

    record[22] = (uint8_t)(directory ? 0x03u : 0x01u);
    record[23] = 0u;

    used_size = first_attr + 8u;
    record[24] = (uint8_t)used_size;
    record[25] = (uint8_t)(used_size >> 8);
    record[26] = (uint8_t)(used_size >> 16);
    record[27] = (uint8_t)(used_size >> 24);

    record[28] = (uint8_t)fs->bytes_per_file_record;
    record[29] = (uint8_t)(fs->bytes_per_file_record >> 8);
    record[30] = (uint8_t)(fs->bytes_per_file_record >> 16);
    record[31] = (uint8_t)(fs->bytes_per_file_record >> 24);

    /* No base record and no next attribute identifier yet. */
    record[first_attr] = 0xFFu;
    record[first_attr + 1u] = 0xFFu;
    record[first_attr + 2u] = 0xFFu;
    record[first_attr + 3u] = 0xFFu;

    return ntfs_write_file_record(fs, record_number, record);
}

int ntfs_mft_add_empty_data(struct pros_ntfs *fs,
                            uint64_t record_number)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    uint32_t first;
    uint32_t pos;
    uint32_t len;
    uint16_t attr_id = 0u;
    const uint32_t attr_len = 32u;
    uint32_t new_used;

    if (!fs || fs->bytes_per_file_record > sizeof(record) ||
        record_number > 0x0000FFFFFFFFFFFFull)
        return PROS_NTFS_EINVAL;
    if (ntfs_read_file_record(fs, record_number, record, sizeof(record)) !=
            PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    if (ntfs_find_attribute(record, fs->bytes_per_file_record, 0x80u,
                            &pos) == PROS_NTFS_OK)
        return PROS_NTFS_EINVAL;

    first = le16(record + 20u);
    if (first < 24u || first + 8u > fs->bytes_per_file_record)
        return PROS_NTFS_ECORRUPT;

    pos = first;
    while (pos + 8u <= fs->bytes_per_file_record) {
        uint32_t type = le32(record + pos);
        len = le32(record + pos + 4u);
        if (type == 0xFFFFFFFFu)
            break;
        if (len < 16u || pos + len > fs->bytes_per_file_record)
            return PROS_NTFS_ECORRUPT;
        {
            uint16_t id = le16(record + pos + 14u);
            if (id >= attr_id)
                attr_id = (uint16_t)(id + 1u);
        }
        pos += len;
    }

    if (pos + attr_len + 8u > fs->bytes_per_file_record)
        return PROS_NTFS_EOVERFLOW;

    for (uint32_t i = 0u; i < attr_len + 8u; ++i)
        record[pos + i] = 0u;

    record[pos + 0u] = 0x80u;
    record[pos + 4u] = (uint8_t)attr_len;
    record[pos + 8u] = 0u;
    record[pos + 16u] = 0u;
    record[pos + 20u] = 24u;
    record[pos + 14u] = (uint8_t)attr_id;
    record[pos + 15u] = (uint8_t)(attr_id >> 8);

    record[pos + attr_len] = 0xFFu;
    record[pos + attr_len + 1u] = 0xFFu;
    record[pos + attr_len + 2u] = 0xFFu;
    record[pos + attr_len + 3u] = 0xFFu;

    new_used = pos + attr_len + 8u;
    record[24] = (uint8_t)new_used;
    record[25] = (uint8_t)(new_used >> 8);
    record[26] = (uint8_t)(new_used >> 16);
    record[27] = (uint8_t)(new_used >> 24);

    return ntfs_write_file_record(fs, record_number, record);
}

int ntfs_mft_add_standard_information(struct pros_ntfs *fs,
                                             uint64_t record_number,
                                             uint32_t file_attributes)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    uint32_t first;
    uint32_t pos;
    uint32_t len;
    uint32_t attr_len = 72u;
    uint32_t value_len = 48u;
    uint16_t attr_id = 0u;
    uint32_t new_used;

    if (!fs || fs->bytes_per_file_record > sizeof(record) ||
        record_number > 0x0000FFFFFFFFFFFFull)
        return PROS_NTFS_EINVAL;

    if (ntfs_read_file_record(fs, record_number, record, sizeof(record)) !=
            PROS_NTFS_OK)
        return PROS_NTFS_EIO;

    if (ntfs_find_attribute(record, fs->bytes_per_file_record, 0x10u,
                            &pos) == PROS_NTFS_OK)
        return PROS_NTFS_EINVAL;

    first = le16(record + 20u);
    if (first < 24u || first + 8u > fs->bytes_per_file_record)
        return PROS_NTFS_ECORRUPT;

    pos = first;
    while (pos + 8u <= fs->bytes_per_file_record) {
        uint32_t type = le32(record + pos);
        len = le32(record + pos + 4u);
        if (type == 0xFFFFFFFFu)
            break;
        if (len < 16u || pos + len > fs->bytes_per_file_record)
            return PROS_NTFS_ECORRUPT;
        {
            uint16_t id = le16(record + pos + 14u);
            if (id >= attr_id)
                attr_id = (uint16_t)(id + 1u);
        }
        pos += len;
    }

    if (pos + attr_len + 8u > fs->bytes_per_file_record)
        return PROS_NTFS_EOVERFLOW;

    for (uint32_t i = 0u; i < attr_len + 8u; ++i)
        record[pos + i] = 0u;

    record[pos + 0u] = 0x10u;
    record[pos + 4u] = (uint8_t)attr_len;
    record[pos + 5u] = (uint8_t)(attr_len >> 8);
    record[pos + 8u] = 0u;
    record[pos + 16u] = (uint8_t)value_len;
    record[pos + 17u] = 0u;
    record[pos + 20u] = 24u;
    record[pos + 14u] = (uint8_t)attr_id;
    record[pos + 15u] = (uint8_t)(attr_id >> 8);

    /* Four NTFS timestamps remain zero until a clock source is wired in. */
    for (uint32_t i = 0u; i < 4u; ++i)
        record[pos + 24u + i * 8u] = 0u;

    record[pos + 24u + 32u] = (uint8_t)file_attributes;
    record[pos + 24u + 33u] = (uint8_t)(file_attributes >> 8);
    record[pos + 24u + 34u] = (uint8_t)(file_attributes >> 16);
    record[pos + 24u + 35u] = (uint8_t)(file_attributes >> 24);

    record[pos + attr_len] = 0xFFu;
    record[pos + attr_len + 1u] = 0xFFu;
    record[pos + attr_len + 2u] = 0xFFu;
    record[pos + attr_len + 3u] = 0xFFu;

    new_used = pos + attr_len + 8u;
    record[24] = (uint8_t)new_used;
    record[25] = (uint8_t)(new_used >> 8);
    record[26] = (uint8_t)(new_used >> 16);
    record[27] = (uint8_t)(new_used >> 24);

    return ntfs_write_file_record(fs, record_number, record);
}

int ntfs_mft_add_file_name(struct pros_ntfs *fs,
                           uint64_t record_number,
                           uint64_t parent_reference,
                           const uint16_t *name_utf16le,
                           uint8_t name_length,
                           uint8_t namespace_id)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    uint32_t first;
    uint32_t pos;
    uint32_t old_len;
    uint32_t attr_len;
    uint32_t value_len;
    uint32_t new_used;
    uint16_t attr_id;
    uint32_t flags;

    if (!fs || !name_utf16le || name_length == 0u ||
        namespace_id > 3u ||
        fs->bytes_per_file_record > sizeof(record))
        return PROS_NTFS_EINVAL;

    if (ntfs_read_file_record(fs, record_number, record, sizeof(record)) !=
            PROS_NTFS_OK)
        return PROS_NTFS_EIO;

    if (ntfs_find_attribute(record, fs->bytes_per_file_record, 0x30u,
                            &pos) == PROS_NTFS_OK)
        return PROS_NTFS_EINVAL;

    first = le16(record + 20u);
    if (first < 24u || first + 8u > fs->bytes_per_file_record)
        return PROS_NTFS_ECORRUPT;

    pos = first;
    while (pos + 8u <= fs->bytes_per_file_record) {
        uint32_t type = le32(record + pos);
        old_len = le32(record + pos + 4u);
        if (type == 0xFFFFFFFFu)
            break;
        if (old_len < 16u || pos + old_len > fs->bytes_per_file_record)
            return PROS_NTFS_ECORRUPT;
        pos += old_len;
    }

    if (pos + 8u > fs->bytes_per_file_record)
        return PROS_NTFS_EOVERFLOW;

    value_len = 66u + (uint32_t)name_length * 2u;
    attr_len = (value_len + 24u + 7u) & ~7u;
    new_used = pos + attr_len + 8u;
    if (attr_len < 32u || new_used > fs->bytes_per_file_record)
        return PROS_NTFS_EOVERFLOW;

    for (uint32_t i = 0u; i < attr_len + 8u; ++i)
        record[pos + i] = 0u;

    record[pos + 0u] = 0x30u;
    record[pos + 4u] = (uint8_t)attr_len;
    record[pos + 5u] = (uint8_t)(attr_len >> 8);
    record[pos + 6u] = (uint8_t)(attr_len >> 16);
    record[pos + 7u] = (uint8_t)(attr_len >> 24);
    record[pos + 8u] = 0u;

    /* $FILE_NAME is resident, unnamed, and has no attribute flags. */
    record[pos + 9u] = 0u;
    record[pos + 10u] = 0u;
    record[pos + 11u] = 0u;
    record[pos + 12u] = 0u;
    record[pos + 13u] = 0u;
    record[pos + 16u] = (uint8_t)value_len;
    record[pos + 17u] = (uint8_t)(value_len >> 8);
    record[pos + 18u] = (uint8_t)(value_len >> 16);
    record[pos + 19u] = (uint8_t)(value_len >> 24);
    record[pos + 20u] = 24u;

    /* Attribute identifier follows the highest existing identifier. */
    attr_id = 0u;
    {
        uint32_t scan = first;
        while (scan + 8u <= fs->bytes_per_file_record) {
            uint32_t type = le32(record + scan);
            uint32_t len = le32(record + scan + 4u);
            if (type == 0xFFFFFFFFu)
                break;
            if (len < 16u || scan + len > fs->bytes_per_file_record)
                return PROS_NTFS_ECORRUPT;
            {
                uint16_t id = le16(record + scan + 14u);
                if (id >= attr_id)
                    attr_id = (uint16_t)(id + 1u);
            }
            scan += len;
        }
    }
    record[pos + 14u] = (uint8_t)attr_id;
    record[pos + 15u] = (uint8_t)(attr_id >> 8);

    for (uint32_t i = 0u; i < 8u; ++i)
        record[pos + 24u + i] = (uint8_t)(parent_reference >> (8u * i));

    /* Timestamps are intentionally zero until the standard-information builder. */
    flags = (record[22u] & 0x03u) == 0x03u ? 0x10000000u : 0u;
    for (uint32_t i = 0u; i < 4u; ++i)
        record[pos + 24u + 56u + i] =
            (uint8_t)(flags >> (8u * i));
    record[pos + 24u + 64u] = name_length;
    record[pos + 24u + 65u] = namespace_id;

    for (uint32_t i = 0u; i < (uint32_t)name_length * 2u; ++i) {
        const uint8_t *src = (const uint8_t *)name_utf16le;
        record[pos + 24u + 66u + i] = src[i];
    }

    record[pos + attr_len + 0u] = 0xFFu;
    record[pos + attr_len + 1u] = 0xFFu;
    record[pos + attr_len + 2u] = 0xFFu;
    record[pos + attr_len + 3u] = 0xFFu;

    record[24] = (uint8_t)new_used;
    record[25] = (uint8_t)(new_used >> 8);
    record[26] = (uint8_t)(new_used >> 16);
    record[27] = (uint8_t)(new_used >> 24);

    if (ntfs_write_file_record(fs, record_number, record) != PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    return PROS_NTFS_OK;
}

int ntfs_mft_allocate_record(struct pros_ntfs *fs,
                                 uint64_t *record_number)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    static uint8_t bitmap[4096];
    uint32_t attr_offset;
    uint64_t bitmap_size;
    uint64_t total_records;
    uint64_t start_record = 24u;
    uint64_t record_count;
    uint64_t byte_offset;

    if (!fs || !record_number || fs->bytes_per_file_record == 0u ||
        fs->bytes_per_file_record > sizeof(record) ||
        !fs->mount.active || fs->mount.read_only ||
        !fs->mount.device || !fs->mount.device->write)
        return PROS_NTFS_EINVAL;
    *record_number = 0u;

    /*
     * $MFT (record 0) owns the $BITMAP attribute. Each bit describes one
     * MFT record; unlike $Bitmap, this bitmap is indexed by record number.
     * Records 0..23 are NTFS metadata/reserved records and are never
     * returned by the allocator.
     */
    if (ntfs_read_file_record(fs, 0u, record, sizeof(record)) !=
            PROS_NTFS_OK ||
        ntfs_find_attribute(record, fs->bytes_per_file_record, 0xB0u,
                            &attr_offset) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    if (record[attr_offset + 9u] != 0u ||
        record[attr_offset + 8u] != 0u)
        return PROS_NTFS_ENOTSUP;

    {
        const uint8_t *bitmap_data;
        uint32_t size;
        if (ntfs_attribute_data(record, fs->bytes_per_file_record,
                                attr_offset, &bitmap_data,
                                &size) != PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
        bitmap_size = size;
    }

    if (fs->bytes_per_file_record == 0u ||
        fs->bytes_per_sector == 0u ||
        fs->sectors_per_cluster == 0u ||
        fs->total_sectors == 0u)
        return PROS_NTFS_ECORRUPT;
    if (fs->total_sectors >
        0xFFFFFFFFFFFFFFFFull / fs->bytes_per_sector)
        return PROS_NTFS_EOVERFLOW;
    total_records = (fs->total_sectors * (uint64_t)fs->bytes_per_sector) /
                    fs->bytes_per_file_record;
    record_count = bitmap_size * 8u;
    if (record_count > total_records)
        record_count = total_records;
    if (record_count <= start_record)
        return PROS_NTFS_EIO;

    byte_offset = start_record / 8u;
    while (byte_offset < bitmap_size) {
        uint32_t want = (uint32_t)(bitmap_size - byte_offset);
        if (want > sizeof(bitmap))
            want = sizeof(bitmap);
        {
            const uint8_t *bitmap_data;
            uint32_t bitmap_data_size;
            if (ntfs_attribute_data(record, fs->bytes_per_file_record,
                                    attr_offset, &bitmap_data,
                                    &bitmap_data_size) != PROS_NTFS_OK ||
                byte_offset > bitmap_data_size ||
                want > bitmap_data_size - byte_offset)
                return PROS_NTFS_ECORRUPT;
            for (uint32_t i = 0u; i < want; ++i)
                bitmap[i] = bitmap_data[byte_offset + i];
        }

        for (uint32_t byte = 0u; byte < want; ++byte) {
            uint64_t base = (byte_offset + byte) * 8u;
            uint8_t value = bitmap[byte];

            for (uint32_t bit = 0u; bit < 8u; ++bit) {
                uint64_t candidate = base + bit;
                if (candidate < start_record || candidate >= record_count)
                    continue;
                if ((value & (uint8_t)(1u << bit)) != 0u)
                    continue;

                {
                    uint8_t updated = (uint8_t)(value | (uint8_t)(1u << bit));
                    uint32_t bitmap_byte = (uint32_t)(byte_offset + byte);
                    const uint8_t *bitmap_data;
                    uint32_t bitmap_data_size;
                    if (ntfs_attribute_data(record, fs->bytes_per_file_record,
                                            attr_offset, &bitmap_data,
                                            &bitmap_data_size) != PROS_NTFS_OK ||
                        bitmap_byte >= bitmap_data_size)
                        return PROS_NTFS_ECORRUPT;
                    ((uint8_t *)bitmap_data)[bitmap_byte] = updated;
                    if (ntfs_write_file_record(fs, 0u, record) !=
                        PROS_NTFS_OK)
                        return PROS_NTFS_EIO;
                }
                *record_number = candidate;
                return PROS_NTFS_OK;
            }
        }
        byte_offset += want;
    }
    return PROS_NTFS_EIO;
}

static int ntfs_write_file_record(struct pros_ntfs *fs,
                                      uint64_t record_number,
                                      uint8_t *record)
{
    uint16_t usa_offset, usa_count;
    uint16_t sequence;
    uint64_t byte_offset;
    uint64_t cluster_bytes;

    if (!fs || !record || fs->bytes_per_file_record == 0u ||
        fs->bytes_per_sector == 0u ||
        fs->bytes_per_file_record % fs->bytes_per_sector != 0u)
        return PROS_NTFS_EINVAL;

    if (record[0] != 'F' || record[1] != 'I' ||
        record[2] != 'L' || record[3] != 'E')
        return PROS_NTFS_ECORRUPT;

    usa_offset = le16(record + 4u);
    usa_count = le16(record + 6u);
    if (usa_count < 2u ||
        (uint32_t)usa_offset + (uint32_t)usa_count * 2u >
            fs->bytes_per_file_record)
        return PROS_NTFS_ECORRUPT;

    if ((uint32_t)usa_count !=
        fs->bytes_per_file_record / fs->bytes_per_sector + 1u)
        return PROS_NTFS_ECORRUPT;

    sequence = le16(record + usa_offset);
    if (sequence == 0xFFFFu)
        sequence = 1u;
    else
        ++sequence;
    record[usa_offset] = (uint8_t)sequence;
    record[usa_offset + 1u] = (uint8_t)(sequence >> 8);

    for (uint32_t i = 1u; i < usa_count; ++i) {
        uint32_t end = i * fs->bytes_per_sector - 2u;
        record[end] = (uint8_t)sequence;
        record[end + 1u] = (uint8_t)(sequence >> 8);
    }

    if (record_number > 0xFFFFFFFFFFFFFFFFull /
                        fs->bytes_per_file_record)
        return PROS_NTFS_EOVERFLOW;
    byte_offset = record_number * (uint64_t)fs->bytes_per_file_record;

    cluster_bytes = (uint64_t)fs->bytes_per_sector *
                    fs->sectors_per_cluster;
    if (cluster_bytes == 0u)
        return PROS_NTFS_EOVERFLOW;

    uint32_t written = 0u;
    while (written < fs->bytes_per_file_record) {
        uint64_t logical_cluster = byte_offset / cluster_bytes;
        uint64_t cluster_offset = byte_offset % cluster_bytes;
        uint64_t cumulative = 0u, physical_lcn = 0u;
        uint8_t found = 0u, sparse = 0u;
        uint32_t sector_index, in_sector, take;
        uint64_t lba;

        for (uint32_t r = 0u; r < fs->mft_run_count; ++r) {
            const struct pros_ntfs_run *run = &fs->mft_runs[r];
            if (logical_cluster < cumulative + run->clusters) {
                physical_lcn = run->lcn + logical_cluster - cumulative;
                if (physical_lcn < run->lcn)
                    return PROS_NTFS_EOVERFLOW;
                sparse = run->sparse;
                found = 1u;
                break;
            }
            if (run->clusters > 0xFFFFFFFFFFFFFFFFull - cumulative)
                return PROS_NTFS_EOVERFLOW;
            cumulative += run->clusters;
        }
        if (!found || sparse)
            return PROS_NTFS_EIO;

        sector_index = (uint32_t)(cluster_offset / fs->bytes_per_sector);
        in_sector = (uint32_t)(cluster_offset % fs->bytes_per_sector);
        take = fs->bytes_per_sector - in_sector;
        if (take > fs->bytes_per_file_record - written)
            take = fs->bytes_per_file_record - written;
        if (in_sector != 0u || take != fs->bytes_per_sector)
            return PROS_NTFS_ECORRUPT;

        if (ntfs_cluster_lba(fs, physical_lcn, &lba) != PROS_NTFS_OK ||
            sector_index >= fs->sectors_per_cluster)
            return PROS_NTFS_EIO;
        lba += sector_index;
        if (block_device_write(fs->mount.device, lba, 1u,
                               record + written) != PROS_BLOCK_OK)
            return PROS_NTFS_EIO;

        written += take;
        byte_offset += take;
    }

    return PROS_NTFS_OK;
}

static int ntfs_extend_last_run(uint8_t *record, uint32_t record_size,
                                    uint32_t attr_offset,
                                    uint64_t added_clusters,
                                    uint64_t new_allocated_clusters,
                                    struct pros_ntfs_run *last_run)
{
    const uint8_t *runlist;
    uint32_t runlist_size;
    uint32_t pos = 0u;
    uint32_t length_pos = 0u;
    uint32_t length_bytes = 0u;
    uint64_t run_vcn = 0u;
    uint8_t terminated = 0u;

    if (!record || !last_run || added_clusters == 0u ||
        new_allocated_clusters == 0u)
        return PROS_NTFS_EINVAL;
    if (ntfs_attribute_runlist(record, record_size, attr_offset,
                               &runlist, &runlist_size) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    while (pos < runlist_size) {
        uint32_t entry = pos;
        uint8_t header = runlist[pos++];
        uint32_t len_bytes = header & 0x0Fu;
        uint32_t off_bytes = (header >> 4) & 0x0Fu;
        uint64_t run_len = 0u;

        if (header == 0u) {
            terminated = 1u;
            break;
        }
        if (len_bytes == 0u || len_bytes > 8u || off_bytes > 8u ||
            pos + len_bytes + off_bytes > runlist_size)
            return PROS_NTFS_ECORRUPT;
        for (uint32_t i = 0u; i < len_bytes; ++i)
            run_len |= (uint64_t)runlist[pos + i] << (8u * i);
        pos += len_bytes + off_bytes;

        if (run_vcn > 0xFFFFFFFFFFFFFFFFull - run_len)
            return PROS_NTFS_EOVERFLOW;
        run_vcn += run_len;
        length_pos = entry + 1u;
        length_bytes = len_bytes;
    }

    if (!terminated || length_bytes == 0u ||
        run_vcn != new_allocated_clusters)
        return PROS_NTFS_ECORRUPT;
    if (last_run->sparse ||
        last_run->clusters > 0xFFFFFFFFFFFFFFFFull - added_clusters)
        return PROS_NTFS_EOVERFLOW;
    if (last_run->clusters + added_clusters >
        ((length_bytes == 8u) ? 0xFFFFFFFFFFFFFFFFull :
         ((1ull << (length_bytes * 8u)) - 1ull)))
        return PROS_NTFS_ENOTSUP;

    {
        uint8_t *mutable_runlist = record + attr_offset +
                                    le16(record + attr_offset + 32u);
        uint64_t new_run_length = last_run->clusters + added_clusters;
        for (uint32_t i = 0u; i < length_bytes; ++i)
            mutable_runlist[length_pos + i] =
                (uint8_t)(new_run_length >> (8u * i));
    }

    for (uint32_t i = 0u; i < 8u; ++i)
        record[attr_offset + 24u + i] =
            (uint8_t)((new_allocated_clusters - 1u) >> (8u * i));

    last_run->clusters += added_clusters;
    return PROS_NTFS_OK;
}
static uint32_t ntfs_signed_width(int64_t value)
{
    if (value >= -128ll && value <= 127ll) return 1u;
    if (value >= -32768ll && value <= 32767ll) return 2u;
    if (value >= -8388608ll && value <= 8388607ll) return 3u;
    if (value >= -2147483648ll && value <= 2147483647ll) return 4u;
    if (value >= -549755813888ll && value <= 549755813887ll) return 5u;
    if (value >= -140737488355328ll && value <= 140737488355327ll) return 6u;
    if (value >= -36028797018963968ll && value <= 36028797018963967ll) return 7u;
    return 8u;
}

static int ntfs_append_run(uint8_t *record, uint32_t record_size,
                           uint32_t attr_offset, uint64_t vcn,
                           uint64_t lcn, uint64_t clusters,
                           const struct pros_ntfs_run *last_run)
{
    const uint8_t *runlist;
    uint32_t runlist_size;
    uint32_t pos = 0u;
    uint32_t required;
    int64_t lcn_delta;

    if (!record || !last_run || clusters == 0u ||
        vcn > 0x7FFFFFFFFFFFFFFFull ||
        clusters > 0x7FFFFFFFFFFFFFFFull ||
        clusters > 0xFFFFFFFFFFFFFFFFull - vcn)
        return PROS_NTFS_EINVAL;
    if (ntfs_attribute_runlist(record, record_size, attr_offset,
                               &runlist, &runlist_size) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    while (pos < runlist_size && runlist[pos] != 0u) {
        uint8_t h = runlist[pos++];
        uint32_t vbytes = h & 0x0Fu;
        uint32_t lbytes = (h >> 4) & 0x0Fu;
        if (vbytes == 0u || vbytes > 8u || lbytes > 8u ||
            pos + vbytes + lbytes > runlist_size)
            return PROS_NTFS_ECORRUPT;
        pos += vbytes + lbytes;
    }
    if (pos >= runlist_size || runlist[pos] != 0u)
        return PROS_NTFS_ECORRUPT;

    if (lcn < last_run->lcn)
        lcn_delta = -(int64_t)(last_run->lcn - lcn);
    else if (lcn - last_run->lcn > 0x7FFFFFFFFFFFFFFFull)
        return PROS_NTFS_EOVERFLOW;
    else
        lcn_delta = (int64_t)(lcn - last_run->lcn);

    {
        uint32_t vbytes = ntfs_signed_width((int64_t)clusters);
        uint32_t lbytes = ntfs_signed_width(lcn_delta);
        required = 1u + vbytes + lbytes + 1u;
        {
            uint32_t attr_len = le32(record + attr_offset + 4u);
            uint16_t run_offset = le16(record + attr_offset + 32u);
            if (attr_offset > record_size ||
                attr_len < 64u ||
                attr_len > record_size - attr_offset ||
                run_offset < 64u ||
                run_offset >= attr_len ||
                pos > attr_len - run_offset ||
                required > attr_len - run_offset - pos)
                return PROS_NTFS_ENOTSUP;
        }

        uint8_t *dst = record + attr_offset +
                       le16(record + attr_offset + 32u) + pos;
        dst[0] = (uint8_t)(vbytes | (lbytes << 4));
        for (uint32_t i = 0u; i < vbytes; ++i)
            dst[1u + i] = (uint8_t)(clusters >> (8u * i));
        for (uint32_t i = 0u; i < lbytes; ++i)
            dst[1u + vbytes + i] =
                (uint8_t)(((uint64_t)lcn_delta) >> (8u * i));
        dst[1u + vbytes + lbytes] = 0u;
    }

    for (uint32_t i = 0u; i < 8u; ++i)
        record[attr_offset + 24u + i] =
            (uint8_t)((vcn + clusters - 1u) >> (8u * i));
    return PROS_NTFS_OK;
}


int ntfs_file_write(struct pros_ntfs *fs, uint64_t record_number,
                    uint64_t offset, const void *buffer, uint32_t size,
                    uint32_t *bytes_written)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    const uint8_t *input = (const uint8_t *)buffer;
    const uint8_t *runlist;
    uint32_t runlist_size, run_count, attr_offset, attr_length;
    uint64_t real_size;
    uint64_t allocated_size;
    uint64_t new_real_size;
    struct pros_ntfs_run runs[PROS_NTFS_MFT_RUN_MAX];
    static uint8_t growth_sector[4096];
    static uint8_t data_sector[4096];

    if (!fs || !buffer || !bytes_written || size == 0u ||
        fs->bytes_per_file_record > sizeof(record) ||
        !fs->mount.active || fs->mount.read_only ||
        !fs->mount.device->write)
        return PROS_NTFS_EINVAL;
    *bytes_written = 0u;

    if (ntfs_read_file_record(fs, record_number, record, sizeof(record)) !=
        PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    if (ntfs_find_unnamed_attribute(record, fs->bytes_per_file_record,
                                    0x80u, &attr_offset) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    attr_length = le32(record + attr_offset + 4u);
    if (attr_length < 24u ||
        attr_offset + attr_length > fs->bytes_per_file_record ||
        record[attr_offset + 9u] != 0u)
        return PROS_NTFS_ECORRUPT;

    /*
     * Resident data is overwrite-only for now. Nonresident data may grow
     * through the explicitly supported bitmap/runlist path below.
     */
    if (record[attr_offset + 12u] != 0u)
        return PROS_NTFS_ENOTSUP;

    if (record[attr_offset + 8u] == 0u) {
        const uint8_t *data;
        uint32_t data_size;
        if (ntfs_attribute_data(record, fs->bytes_per_file_record,
                                attr_offset, &data, &data_size) !=
            PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;
        if (offset > data_size ||
            (uint64_t)size > (uint64_t)data_size - offset)
            return PROS_NTFS_EOVERFLOW;

        for (uint32_t i = 0u; i < size; ++i)
            ((uint8_t *)data)[(uint32_t)offset + i] = input[i];

        if (ntfs_write_file_record(fs, record_number, record) !=
            PROS_NTFS_OK)
            return PROS_NTFS_EIO;
        *bytes_written = size;
        return PROS_NTFS_OK;
    }

    if (record[attr_offset + 8u] != 1u || attr_length < 64u)
        return PROS_NTFS_ECORRUPT;
    if (le64(record + attr_offset + 16u) != 0u ||
        le16(record + attr_offset + 12u) != 0u)
        return PROS_NTFS_ENOTSUP;

    real_size = le64(record + attr_offset + 48u);
    allocated_size = le64(record + attr_offset + 40u);
    if (real_size > allocated_size)
        return PROS_NTFS_ECORRUPT;
    if (offset > 0xFFFFFFFFFFFFFFFFull - (uint64_t)size)
        return PROS_NTFS_EOVERFLOW;
    new_real_size = offset + (uint64_t)size;

    if (ntfs_attribute_runlist(record, fs->bytes_per_file_record,
                               attr_offset, &runlist, &runlist_size) !=
        PROS_NTFS_OK ||
        ntfs_decode_runlist(runlist, runlist_size, runs,
                            PROS_NTFS_MFT_RUN_MAX, &run_count) !=
        PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    if (fs->sectors_per_cluster != 0u &&
        fs->bytes_per_sector > 0u &&
        fs->sectors_per_cluster > 0xFFFFFFFFu / fs->bytes_per_sector)
        return PROS_NTFS_EOVERFLOW;
    uint32_t cluster_bytes = fs->bytes_per_sector *
                             fs->sectors_per_cluster;
    uint64_t growth_lcn = 0u;
    uint64_t growth_clusters = 0u;
    int write_status = PROS_NTFS_OK;
    if (cluster_bytes == 0u || fs->bytes_per_sector > 4096u)
        return PROS_NTFS_EINVAL;

    /*
     * If EOF crosses the current allocation boundary, extend only the final
     * non-sparse run. The bitmap allocator starts at that run's next LCN, so
     * the runlist can grow without adding a new mapping-pairs entry.
     */
    if (new_real_size > allocated_size) {
        uint64_t old_alloc_clusters;
        uint64_t new_alloc_clusters;
        uint64_t required_clusters;

        if (allocated_size == 0u ||
            (allocated_size % cluster_bytes) != 0u ||
            run_count == 0u)
            return PROS_NTFS_ENOTSUP;

        old_alloc_clusters = allocated_size / cluster_bytes;
        new_alloc_clusters =
            new_real_size / cluster_bytes +
            ((new_real_size % cluster_bytes) != 0u ? 1u : 0u);
        if (new_alloc_clusters < old_alloc_clusters)
            return PROS_NTFS_ECORRUPT;
        required_clusters = new_alloc_clusters - old_alloc_clusters;
        if (required_clusters == 0u)
            return PROS_NTFS_ECORRUPT;

        if (runs[run_count - 1u].sparse ||
            runs[run_count - 1u].clusters >
                0xFFFFFFFFFFFFFFFFull - runs[run_count - 1u].lcn)
            return PROS_NTFS_ENOTSUP;
        growth_lcn = runs[run_count - 1u].lcn +
                     runs[run_count - 1u].clusters;
        if (growth_lcn < runs[run_count - 1u].lcn)
            return PROS_NTFS_EOVERFLOW;

        if (ntfs_bitmap_allocate_clusters_exact(fs, growth_lcn,
                                                 required_clusters) !=
            PROS_NTFS_OK) {
            /*
             * If the preferred contiguous extension is unavailable, keep the
             * fallback allocation at or after the old run's end. Starting the
             * global scan at LCN 0 could consume an unrelated low-numbered
             * free cluster and would produce a non-local fragmented run.
             */
            uint64_t fallback_start =
                runs[run_count - 1u].lcn + runs[run_count - 1u].clusters;
            if (fallback_start < runs[run_count - 1u].lcn ||
                ntfs_bitmap_allocate_clusters_from(fs, fallback_start,
                                                    required_clusters,
                                                    &growth_lcn) !=
                    PROS_NTFS_OK)
                return PROS_NTFS_EIO;
        }
        growth_clusters = required_clusters;

        if (growth_lcn == runs[run_count - 1u].lcn +
                         runs[run_count - 1u].clusters) {
            if (ntfs_extend_last_run(record, fs->bytes_per_file_record,
                                     attr_offset, required_clusters,
                                     new_alloc_clusters,
                                     &runs[run_count - 1u]) != PROS_NTFS_OK) {
                (void)ntfs_bitmap_free_clusters(fs, growth_lcn,
                                                 growth_clusters);
                return PROS_NTFS_ENOTSUP;
            }
        } else {
            uint64_t new_vcn = old_alloc_clusters;
            if (run_count >= PROS_NTFS_MFT_RUN_MAX ||
                ntfs_append_run(record, fs->bytes_per_file_record,
                                attr_offset, new_vcn, growth_lcn,
                                required_clusters,
                                &runs[run_count - 1u]) != PROS_NTFS_OK) {
                (void)ntfs_bitmap_free_clusters(fs, growth_lcn,
                                                 growth_clusters);
                return PROS_NTFS_ENOTSUP;
            }
            runs[run_count].lcn = growth_lcn;
            runs[run_count].clusters = required_clusters;
            runs[run_count].sparse = 0u;
            ++run_count;
        }
        if (new_alloc_clusters > 0xFFFFFFFFFFFFFFFFull /
                                (uint64_t)cluster_bytes) {
            write_status = PROS_NTFS_EOVERFLOW;
            goto write_fail;
        }
        allocated_size = new_alloc_clusters * (uint64_t)cluster_bytes;
        for (uint32_t b = 0u; b < 8u; ++b)
            record[attr_offset + 40u + b] =
                (uint8_t)(allocated_size >> (8u * b));
    }

    /*
     * Growth is allowed only inside already allocated, non-sparse clusters.
     * If the caller writes beyond EOF, the gap is explicitly zero-filled so
     * the newly exposed bytes are initialized before the metadata length is
     * committed.
     */
    uint64_t file_offset = real_size;
    uint32_t copied = 0u;
    while (file_offset < offset) {
        uint64_t logical_cluster = file_offset / cluster_bytes;
        uint64_t cluster_offset = file_offset % cluster_bytes;
        uint64_t cumulative = 0u, physical_lcn = 0u;
        uint8_t found = 0u, sparse = 0u;
        uint32_t sector_index, in_sector, take;
        uint64_t lba;
        for (uint32_t r = 0u; r < run_count; ++r) {
            if (logical_cluster >= cumulative &&
                logical_cluster - cumulative < runs[r].clusters) {
                physical_lcn = runs[r].lcn +
                               logical_cluster - cumulative;
                if (physical_lcn < runs[r].lcn) {
                    write_status = PROS_NTFS_EOVERFLOW;
                    goto write_fail;
                }
                sparse = runs[r].sparse;
                found = 1u;
                break;
            }
            if (runs[r].clusters > 0xFFFFFFFFFFFFFFFFull - cumulative) {
                write_status = PROS_NTFS_EOVERFLOW;
                goto write_fail;
            }
            cumulative += runs[r].clusters;
        }
        if (!found || sparse) {
            write_status = PROS_NTFS_EIO;
            goto write_fail;
        }

        sector_index = (uint32_t)(cluster_offset / fs->bytes_per_sector);
        in_sector = (uint32_t)(cluster_offset % fs->bytes_per_sector);
        take = fs->bytes_per_sector - in_sector;
        if (take > offset - file_offset)
            take = (uint32_t)(offset - file_offset);
        if (sector_index >= fs->sectors_per_cluster ||
            ntfs_cluster_lba(fs, physical_lcn, &lba) != PROS_NTFS_OK) {
            write_status = PROS_NTFS_EIO;
            goto write_fail;
        }
        lba += sector_index;

        if (block_device_read(fs->mount.device, lba, 1u, growth_sector) !=
            PROS_BLOCK_OK) {
            write_status = PROS_NTFS_EIO;
            goto write_fail;
        }
        for (uint32_t i = 0u; i < take; ++i)
            growth_sector[in_sector + i] = 0u;
        if (block_device_write(fs->mount.device, lba, 1u, growth_sector) !=
            PROS_BLOCK_OK) {
            write_status = PROS_NTFS_EIO;
            goto write_fail;
        }

        file_offset += take;
    }

    file_offset = offset;
    copied = 0u;
    while (copied < size) {
        uint64_t logical_cluster = file_offset / cluster_bytes;
        uint64_t cluster_offset = file_offset % cluster_bytes;
        uint64_t cumulative = 0u, physical_lcn = 0u;
        uint8_t found = 0u, sparse = 0u;
        uint32_t sector_index, in_sector, take;
        uint64_t lba;

        for (uint32_t r = 0u; r < run_count; ++r) {
            if (logical_cluster < cumulative + runs[r].clusters) {
                physical_lcn = runs[r].lcn +
                               logical_cluster - cumulative;
                if (physical_lcn < runs[r].lcn) {
                    write_status = PROS_NTFS_EOVERFLOW;
                    goto write_fail;
                }
                sparse = runs[r].sparse;
                found = 1u;
                break;
            }
            if (runs[r].clusters > 0xFFFFFFFFFFFFFFFFull - cumulative) {
                write_status = PROS_NTFS_EOVERFLOW;
                goto write_fail;
            }
            cumulative += runs[r].clusters;
        }
        if (!found || sparse) {
            write_status = PROS_NTFS_EIO;
            goto write_fail;
        }

        sector_index = (uint32_t)(cluster_offset / fs->bytes_per_sector);
        in_sector = (uint32_t)(cluster_offset % fs->bytes_per_sector);
        take = fs->bytes_per_sector - in_sector;
        if (take > size - copied)
            take = size - copied;
        if (sector_index >= fs->sectors_per_cluster ||
            ntfs_cluster_lba(fs, physical_lcn, &lba) != PROS_NTFS_OK) {
            write_status = PROS_NTFS_EIO;
            goto write_fail;
        }
        lba += sector_index;

        if (in_sector != 0u || take != fs->bytes_per_sector) {
            if (block_device_read(fs->mount.device, lba, 1u, data_sector) !=
                PROS_BLOCK_OK) {
                write_status = PROS_NTFS_EIO;
                goto write_fail;
            }
        }
        for (uint32_t i = 0u; i < take; ++i)
            data_sector[in_sector + i] = input[copied + i];

        if (block_device_write(fs->mount.device, lba, 1u, data_sector) !=
            PROS_BLOCK_OK) {
            write_status = PROS_NTFS_EIO;
            goto write_fail;
        }

        copied += take;
        file_offset += take;
    }

    /*
     * Commit the stream length only after all data and zero-fill writes have
     * succeeded. The initialized size follows the new EOF because the newly
     * exposed range has been explicitly initialized to zero.
     */
    if (new_real_size > real_size) {
        record[attr_offset + 48u] = (uint8_t)new_real_size;
        record[attr_offset + 49u] = (uint8_t)(new_real_size >> 8);
        record[attr_offset + 50u] = (uint8_t)(new_real_size >> 16);
        record[attr_offset + 51u] = (uint8_t)(new_real_size >> 24);
        record[attr_offset + 52u] = (uint8_t)(new_real_size >> 32);
        record[attr_offset + 53u] = (uint8_t)(new_real_size >> 40);
        record[attr_offset + 54u] = (uint8_t)(new_real_size >> 48);
        record[attr_offset + 55u] = (uint8_t)(new_real_size >> 56);
        record[attr_offset + 56u] = (uint8_t)new_real_size;
        record[attr_offset + 57u] = (uint8_t)(new_real_size >> 8);
        record[attr_offset + 58u] = (uint8_t)(new_real_size >> 16);
        record[attr_offset + 59u] = (uint8_t)(new_real_size >> 24);
        record[attr_offset + 60u] = (uint8_t)(new_real_size >> 32);
        record[attr_offset + 61u] = (uint8_t)(new_real_size >> 40);
        record[attr_offset + 62u] = (uint8_t)(new_real_size >> 48);
        record[attr_offset + 63u] = (uint8_t)(new_real_size >> 56);

        /*
         * Every resident $FILE_NAME copy stores the stream data size. Keep
         * those MFT copies synchronized. Directory index keys are a separate
         * metadata transaction and remain a later milestone.
         */
        uint16_t first = le16(record + 20u);
        uint32_t pos = first;
        while (pos + 8u <= fs->bytes_per_file_record) {
            uint32_t len = le32(record + pos + 4u);
            uint32_t type = le32(record + pos);
            if (type == 0xFFFFFFFFu)
                break;
            if (len < 16u || pos + len > fs->bytes_per_file_record) {
                write_status = PROS_NTFS_ECORRUPT;
                goto write_fail;
            }
            if (type == 0x30u && record[pos + 8u] == 0u) {
                uint32_t value_offset = le16(record + pos + 20u);
                uint32_t value_length = le32(record + pos + 16u);
                if (value_offset < 66u ||
                    value_offset + value_length > len ||
                    value_offset + 56u > len) {
                    write_status = PROS_NTFS_ECORRUPT;
                    goto write_fail;
                }
                for (uint32_t b = 0u; b < 8u; ++b)
                    record[pos + value_offset + 48u + b] =
                        (uint8_t)(new_real_size >> (8u * b));
            }
            pos += len;
        }

        if (ntfs_write_file_record(fs, record_number, record) !=
            PROS_NTFS_OK) {
            write_status = PROS_NTFS_EIO;
            goto write_fail;
        }
    }

    *bytes_written = copied;
    return PROS_NTFS_OK;

write_fail:
    if (growth_clusters != 0u)
        (void)ntfs_bitmap_free_clusters(fs, growth_lcn, growth_clusters);
    *bytes_written = 0u;
    return write_status;
}


int ntfs_file_name_attribute(const void *record, uint32_t record_size,
                             uint32_t attr_offset,
                             struct pros_ntfs_file_name *info)
{
    const uint8_t *data;
    uint32_t data_size;
    uint32_t name_bytes;

    if (!record || !info)
        return PROS_NTFS_EINVAL;
    if (ntfs_attribute_data(record, record_size, attr_offset,
                            &data, &data_size) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (data_size < 66u)
        return PROS_NTFS_ECORRUPT;

    name_bytes = (uint32_t)data[0x40u] * 2u;
    if (data[0x41u] > 3u || data[0x40u] == 0u ||
        66u + name_bytes > data_size)
        return PROS_NTFS_ECORRUPT;

    info->parent_reference = le64(data + 0x00u);
    info->allocated_size = le64(data + 0x28u);
    info->data_size = le64(data + 0x30u);
    info->flags = le32(data + 0x38u);
    info->reparse_tag = le32(data + 0x3Cu);
    info->name_length = data[0x40u];
    info->namespace_id = data[0x41u];
    info->name_utf16le = data + 0x42u;
    return PROS_NTFS_OK;
}

int ntfs_index_root_header(const void *record, uint32_t record_size,
                           uint32_t attr_offset,
                           struct pros_ntfs_index_root *info)
{
    const uint8_t *data;
    uint32_t data_size;
    uint32_t first;
    uint32_t entries;
    uint32_t allocated;

    if (!record || !info)
        return PROS_NTFS_EINVAL;
    if (ntfs_attribute_data(record, record_size, attr_offset,
                            &data, &data_size) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (data_size < 32u)
        return PROS_NTFS_ECORRUPT;

    first = le32(data + 16u);
    entries = le32(data + 20u);
    allocated = le32(data + 24u);
    if (first < 32u || entries < first || entries > data_size ||
        allocated < entries || allocated > data_size ||
        (data[28u] & 0xFEu) != 0u ||
        le32(data + 8u) == 0u || data[12u] == 0u)
        return PROS_NTFS_ECORRUPT;

    info->bytes_per_index_record = le32(data + 8u);
    info->clusters_per_index_record = data[12u];
    info->first_entry_offset = first;
    info->entries_size = entries;
    info->allocated_size = allocated;
    info->flags = data[28u];
    return PROS_NTFS_OK;
}

int ntfs_index_root_entry(const void *record, uint32_t record_size,
                          uint32_t attr_offset, uint32_t entry_offset,
                          uint64_t *file_reference,
                          const uint8_t **key, uint16_t *key_size,
                          uint8_t *entry_flags, uint32_t *next_entry_offset)
{
    const uint8_t *data;
    uint32_t data_size;
    struct pros_ntfs_index_root root;
    uint32_t remaining;
    uint16_t entry_size;
    uint16_t stream_size;
    uint8_t flags;
    uint32_t key_limit;

    if (!record || !file_reference || !key || !key_size ||
        !entry_flags || !next_entry_offset)
        return PROS_NTFS_EINVAL;
    if (ntfs_attribute_data(record, record_size, attr_offset,
                            &data, &data_size) != PROS_NTFS_OK ||
        ntfs_index_root_header(record, record_size, attr_offset, &root) !=
            PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    if (entry_offset < root.first_entry_offset ||
        entry_offset >= root.entries_size ||
        entry_offset >= data_size)
        return PROS_NTFS_EINVAL;

    remaining = root.entries_size - entry_offset;
    if (remaining < 16u)
        return PROS_NTFS_ECORRUPT;

    entry_size = le16(data + entry_offset + 8u);
    stream_size = le16(data + entry_offset + 10u);
    flags = data[entry_offset + 12u];
    if (entry_size < 16u || entry_size > remaining ||
        (entry_size & 7u) != 0u || (flags & ~3u) != 0u)
        return PROS_NTFS_ECORRUPT;

    key_limit = (flags & 1u) ? 8u : 0u;
    if (entry_size < 16u + key_limit ||
        stream_size > entry_size - 16u - key_limit)
        return PROS_NTFS_ECORRUPT;

    *file_reference = le64(data + entry_offset);
    *key = data + entry_offset + 16u;
    *key_size = stream_size;
    *entry_flags = flags;
    *next_entry_offset = (flags & 2u) ? 0u : entry_offset + entry_size;
    return PROS_NTFS_OK;
}


int ntfs_index_root_insert(struct pros_ntfs *fs,
                           uint64_t directory_record_number,
                           uint64_t child_record_number)
{
    static uint8_t directory_record[PROS_NTFS_RECORD_WORK_MAX];
    static uint8_t child_record[PROS_NTFS_RECORD_WORK_MAX];
    static uint8_t key_copy[0x10000];
    uint32_t index_root_offset;
    uint32_t file_name_offset;
    const uint8_t *root_data;
    const uint8_t *child_name;
    uint32_t root_size;
    uint32_t child_name_size;
    struct pros_ntfs_index_root root;
    uint32_t offset;
    uint32_t terminal = 0u;
    uint32_t insert_offset = 0u;
    uint32_t entry_size;
    uint32_t required_size;
    uint32_t new_entries_size;
    uint64_t file_reference;

    if (!fs || directory_record_number > 0x0000FFFFFFFFFFFFull ||
        child_record_number > 0x0000FFFFFFFFFFFFull ||
        fs->bytes_per_file_record > sizeof(directory_record))
        return PROS_NTFS_EINVAL;

    if (ntfs_read_file_record(fs, directory_record_number, directory_record,
                              sizeof(directory_record)) != PROS_NTFS_OK ||
        ntfs_read_file_record(fs, child_record_number, child_record,
                              sizeof(child_record)) != PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    if (ntfs_upcase_load(fs) != PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    ntfs_active_upcase = ntfs_upcase_data;

    if (ntfs_find_attribute(directory_record, fs->bytes_per_file_record,
                            0x90u, &index_root_offset) != PROS_NTFS_OK) {
        ntfs_active_upcase = 0;
        return PROS_NTFS_ENOTSUP;
    }
    if (ntfs_index_root_header(directory_record, fs->bytes_per_file_record,
                               index_root_offset, &root) != PROS_NTFS_OK) {
        ntfs_active_upcase = 0;
        return PROS_NTFS_ECORRUPT;
    }
    if (ntfs_attribute_data(directory_record, fs->bytes_per_file_record,
                            index_root_offset, &root_data, &root_size) !=
        PROS_NTFS_OK || root.allocated_size > root_size) {
        ntfs_active_upcase = 0;
        return PROS_NTFS_ECORRUPT;
    }

    if (ntfs_find_attribute(child_record, fs->bytes_per_file_record,
                            0x30u, &file_name_offset) != PROS_NTFS_OK ||
        ntfs_attribute_data(child_record, fs->bytes_per_file_record,
                            file_name_offset, &child_name,
                            &child_name_size) != PROS_NTFS_OK ||
        child_name_size < 66u || child_name_size > sizeof(key_copy) ||
        child_name[64u] == 0u || child_name[65u] > 3u ||
        66u + (uint32_t)child_name[64u] * 2u > child_name_size) {
        ntfs_active_upcase = 0;
        return PROS_NTFS_ECORRUPT;
    }

    for (uint32_t i = 0u; i < child_name_size; ++i)
        key_copy[i] = child_name[i];

    offset = root.first_entry_offset;
    while (offset < root.entries_size) {
        uint64_t reference;
        const uint8_t *key;
        uint16_t key_size;
        uint8_t flags;
        uint32_t next;

        if (ntfs_index_root_entry(directory_record, fs->bytes_per_file_record,
                                  index_root_offset, offset, &reference, &key,
                                  &key_size, &flags, &next) != PROS_NTFS_OK) {
            ntfs_active_upcase = 0;
            return PROS_NTFS_ECORRUPT;
        }

        if ((flags & 2u) != 0u) {
            terminal = offset;
            if (insert_offset == 0u)
                insert_offset = offset;
            break;
        }

        if (ntfs_name_key_matches(key, key_size, key_copy,
                                  key_copy[64u])) {
            ntfs_active_upcase = 0;
            return PROS_NTFS_EINVAL;
        }

        if (insert_offset == 0u &&
            ntfs_name_key_compare(key, key_size, key_copy,
                                  key_copy[64u]) > 0)
            insert_offset = offset;

        if (next == 0u || next <= offset || next >= root.entries_size) {
            ntfs_active_upcase = 0;
            return PROS_NTFS_ECORRUPT;
        }
        offset = next;
    }

    if (terminal == 0u || insert_offset == 0u) {
        ntfs_active_upcase = 0;
        return PROS_NTFS_ECORRUPT;
    }

    required_size = 16u + child_name_size;
    if (required_size > 0xFFFFu) {
        ntfs_active_upcase = 0;
        return PROS_NTFS_EOVERFLOW;
    }
    entry_size = (required_size + 7u) & ~7u;
    if (entry_size > 0xFFFFu ||
        root.allocated_size < root.entries_size ||
        entry_size > root.allocated_size - root.entries_size) {
        ntfs_active_upcase = 0;
        return PROS_NTFS_ENOTSUP;
    }

    for (uint32_t i = root.entries_size; i > insert_offset; --i)
        ((uint8_t *)root_data)[i + entry_size - 1u] =
            root_data[i - 1u];
    for (uint32_t i = 0u; i < entry_size; ++i)
        ((uint8_t *)root_data)[insert_offset + i] = 0u;

    file_reference = ((uint64_t)le16(child_record + 16u) << 48) |
                     (child_record_number & 0x0000FFFFFFFFFFFFull);
    for (uint32_t i = 0u; i < 8u; ++i)
        ((uint8_t *)root_data)[insert_offset + i] =
            (uint8_t)(file_reference >> (8u * i));
    ((uint8_t *)root_data)[insert_offset + 8u] = (uint8_t)entry_size;
    ((uint8_t *)root_data)[insert_offset + 9u] = (uint8_t)(entry_size >> 8);
    ((uint8_t *)root_data)[insert_offset + 10u] = (uint8_t)child_name_size;
    ((uint8_t *)root_data)[insert_offset + 11u] =
        (uint8_t)(child_name_size >> 8);
    ((uint8_t *)root_data)[insert_offset + 12u] = 0u;
    for (uint32_t i = 0u; i < child_name_size; ++i)
        ((uint8_t *)root_data)[insert_offset + 16u + i] = key_copy[i];

    new_entries_size = root.entries_size + entry_size;
    ((uint8_t *)root_data)[20u] = (uint8_t)new_entries_size;
    ((uint8_t *)root_data)[21u] = (uint8_t)(new_entries_size >> 8);
    ((uint8_t *)root_data)[22u] = (uint8_t)(new_entries_size >> 16);
    ((uint8_t *)root_data)[23u] = (uint8_t)(new_entries_size >> 24);

    {
        int status = ntfs_write_file_record(fs, directory_record_number,
                                             directory_record);
        ntfs_active_upcase = 0;
        return status;
    }
}



int ntfs_index_root_lookup(const void *record, uint32_t record_size,
                           uint32_t attr_offset,
                           const uint8_t *name_utf16le, uint8_t name_length,
                           uint64_t *file_reference)
{
    struct pros_ntfs_index_root root;
    uint32_t offset;
    uint8_t want_bytes;
    if (!record || !name_utf16le || !file_reference || name_length == 0u)
        return PROS_NTFS_EINVAL;
    want_bytes = (uint8_t)(name_length * 2u);
    if (ntfs_index_root_header(record, record_size, attr_offset, &root) !=
        PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    offset = root.first_entry_offset;
    while (offset < root.entries_size) {
        uint64_t reference;
        const uint8_t *key;
        uint16_t key_size;
        uint8_t flags;
        uint32_t next;

        if (ntfs_index_root_entry(record, record_size, attr_offset, offset,
                                  &reference, &key, &key_size, &flags,
                                  &next) != PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;

        if ((flags & 2u) != 0u) {
            /* End marker: no key and no child node to inspect here. */
            return PROS_NTFS_ENOTSUP;
        }

        /*
         * An INDEX_ROOT filename key contains the $FILE_NAME value. Its
         * filename length is at byte 64 and UTF-16LE starts at byte 66.
         */
        if (key_size >= 66u) {
            uint32_t key_name_bytes = (uint32_t)key[64u] * 2u;
            if (key[65u] != 2u && key[65u] <= 3u && key_name_bytes == want_bytes &&
                66u + key_name_bytes <= key_size) {
                uint8_t equal = ntfs_active_upcase ?
                    ntfs_name_key_matches(key, key_size, name_utf16le,
                                          name_length) : 1u;
                if (!ntfs_active_upcase) {
                    for (uint32_t i = 0u; i < key_name_bytes; ++i) {
                        if (key[66u + i] != name_utf16le[i]) {
                            equal = 0u;
                            break;
                        }
                    }
                }
                if (equal) {
                    *file_reference = reference & 0x0000FFFFFFFFFFFFull;
                    return PROS_NTFS_OK;
                }
            }
        }

        if (next == 0u)
            break;
        if (next <= offset || next >= root.entries_size)
            return PROS_NTFS_ECORRUPT;
        offset = next;
    }
    return PROS_NTFS_ENOTSUP;
}



static int ntfs_upcase_load(const struct pros_ntfs *fs)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    struct pros_ntfs_run runs[PROS_NTFS_MFT_RUN_MAX];
    const uint8_t *runlist;
    uint32_t runlist_size, attr_offset, attr_length, run_count;
    uint64_t real_size, byte_offset = 0u;
    uint32_t copied = 0u, cluster_bytes;

    if (!fs || fs->bytes_per_file_record > sizeof(record) ||
        fs->bytes_per_sector < 512u || fs->bytes_per_sector > 4096u ||
        fs->sectors_per_cluster == 0u)
        return PROS_NTFS_EINVAL;
    if (ntfs_read_file_record(fs, PROS_NTFS_UPCASE_RECORD, record,
                              sizeof(record)) != PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    if (ntfs_find_unnamed_attribute(record, fs->bytes_per_file_record,
                                    0x80u, &attr_offset) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    attr_length = le32(record + attr_offset + 4u);
    if (attr_length < 64u ||
        attr_offset + attr_length > fs->bytes_per_file_record ||
        record[attr_offset + 8u] == 0u)
        return PROS_NTFS_ECORRUPT;
    real_size = le64(record + attr_offset + 48u);
    if (real_size != PROS_NTFS_UPCASE_BYTES)
        return PROS_NTFS_ECORRUPT;
    if (ntfs_attribute_runlist(record, fs->bytes_per_file_record,
                               attr_offset, &runlist, &runlist_size) !=
        PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (ntfs_decode_runlist(runlist, runlist_size, runs,
                            PROS_NTFS_MFT_RUN_MAX, &run_count) !=
        PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
    if (cluster_bytes == 0u)
        return PROS_NTFS_EOVERFLOW;

    while (copied < PROS_NTFS_UPCASE_BYTES) {
        uint64_t logical_cluster = byte_offset / cluster_bytes;
        uint64_t cluster_offset = byte_offset % cluster_bytes;
        uint64_t cumulative = 0u, physical_lcn = 0u, lba;
        uint8_t sparse = 0u, found = 0u;
        uint32_t sector_index, in_sector, take;
        uint8_t sector[4096];

        for (uint32_t r = 0u; r < run_count; ++r) {
            if (logical_cluster < cumulative + runs[r].clusters) {
                physical_lcn = runs[r].lcn +
                               logical_cluster - cumulative;
                if (physical_lcn < runs[r].lcn)
                    return PROS_NTFS_EOVERFLOW;
                sparse = runs[r].sparse;
                found = 1u;
                break;
            }
            if (runs[r].clusters > 0xFFFFFFFFFFFFFFFFull - cumulative)
                return PROS_NTFS_EOVERFLOW;
            cumulative += runs[r].clusters;
        }
        if (!found)
            return PROS_NTFS_EIO;

        sector_index = (uint32_t)(cluster_offset / fs->bytes_per_sector);
        in_sector = (uint32_t)(cluster_offset % fs->bytes_per_sector);
        take = fs->bytes_per_sector - in_sector;
        if (take > PROS_NTFS_UPCASE_BYTES - copied)
            take = PROS_NTFS_UPCASE_BYTES - copied;
        if (sector_index >= fs->sectors_per_cluster)
            return PROS_NTFS_EIO;

        if (sparse) {
            for (uint32_t i = 0u; i < take; ++i)
                ntfs_upcase_data[copied + i] = 0u;
        } else {
            if (ntfs_cluster_lba(fs, physical_lcn, &lba) != PROS_NTFS_OK)
                return PROS_NTFS_EIO;
            lba += sector_index;
            if (block_device_read(fs->mount.device, lba, 1u, sector) !=
                PROS_BLOCK_OK)
                return PROS_NTFS_EIO;
            for (uint32_t i = 0u; i < take; ++i)
                ntfs_upcase_data[copied + i] = sector[in_sector + i];
        }
        copied += take;
        byte_offset += take;
    }
    return PROS_NTFS_OK;
}


static int ntfs_name_key_compare(const uint8_t *key,
                                 uint16_t key_size,
                                 const uint8_t *name_utf16le,
                                 uint8_t name_length)
{
    uint32_t key_name_length;
    uint32_t limit;

    if (!key || !name_utf16le || key_size < 66u || name_length == 0u)
        return 0;

    key_name_length = key[64u];
    if ((key[65u] != 1u && key[65u] != 3u) ||
        key_name_length == 0u ||
        66u + key_name_length * 2u > key_size)
        return 0;

    limit = key_name_length < name_length ? key_name_length : name_length;
    for (uint32_t i = 0u; i < limit; ++i) {
        uint16_t stored = le16(key + 66u + i * 2u);
        uint16_t wanted = le16(name_utf16le + i * 2u);
        uint16_t a = stored;
        uint16_t b = wanted;

        if (ntfs_active_upcase) {
            a = le16(ntfs_active_upcase + (uint32_t)stored * 2u);
            b = le16(ntfs_active_upcase + (uint32_t)wanted * 2u);
        }

        if (a < b)
            return -1;
        if (a > b)
            return 1;
    }

    if (key_name_length < name_length)
        return -1;
    if (key_name_length > name_length)
        return 1;
    return 0;
}

static uint8_t ntfs_name_key_matches(const uint8_t *key, uint16_t key_size,
                                      const uint8_t *name_utf16le,
                                      uint8_t name_length)
{
    uint32_t name_bytes;
    if (!key || !name_utf16le || key_size < 66u || name_length == 0u)
        return 0u;
    name_bytes = (uint32_t)key[64u] * 2u;
    /*
     * $FILE_NAME namespace: 1 = Win32, 3 = Win32+DOS. Normal Windows
     * path lookup must not resolve POSIX-only or DOS-only aliases.
     */
    if ((key[65u] != 1u && key[65u] != 3u) ||
        name_bytes != (uint32_t)name_length * 2u ||
        66u + name_bytes > key_size)
        return 0u;
    if (ntfs_active_upcase) {
        for (uint32_t i = 0u; i < name_length; ++i) {
            uint16_t stored = le16(key + 66u + i * 2u);
            uint16_t wanted = le16(name_utf16le + i * 2u);
            uint16_t a = le16(ntfs_active_upcase + (uint32_t)stored * 2u);
            uint16_t b = le16(ntfs_active_upcase + (uint32_t)wanted * 2u);
            if (a != b)
                return 0u;
        }
    } else {
        for (uint32_t i = 0u; i < name_bytes; ++i)
            if (key[66u + i] != name_utf16le[i])
                return 0u;
    }
    return 1u;
}

int ntfs_index_record_fixup(void *record, uint32_t record_size,
                            uint32_t bytes_per_sector)
{
    uint8_t *p = (uint8_t *)record;
    uint16_t usa_offset;
    uint16_t usa_count;

    if (!p || record_size < 48u || bytes_per_sector < 512u ||
        record_size % bytes_per_sector != 0u)
        return PROS_NTFS_EINVAL;
    if (p[0] != 'I' || p[1] != 'N' || p[2] != 'D' || p[3] != 'X')
        return PROS_NTFS_ECORRUPT;

    usa_offset = le16(p + 4u);
    usa_count = le16(p + 6u);
    if (usa_count < 2u ||
        (uint32_t)usa_offset + (uint32_t)usa_count * 2u > record_size)
        return PROS_NTFS_ECORRUPT;

    {
        uint16_t sequence = le16(p + usa_offset);
        for (uint32_t i = 1u; i < usa_count; ++i) {
            uint32_t end = i * bytes_per_sector - 2u;
            uint16_t saved;
            if (end + 2u > record_size)
                return PROS_NTFS_ECORRUPT;
            saved = le16(p + end);
            if (saved != sequence)
                return PROS_NTFS_ECORRUPT;
            p[end] = p[usa_offset + i * 2u];
            p[end + 1u] = p[usa_offset + i * 2u + 1u];
        }
    }
    return PROS_NTFS_OK;
}

int ntfs_index_record_header(const void *record, uint32_t record_size,
                             uint32_t bytes_per_sector, uint64_t expected_vcn,
                             struct pros_ntfs_index_record *info)
{
    const uint8_t *p = (const uint8_t *)record;
    uint32_t first_relative;
    uint32_t entries_size;
    uint32_t allocated_size;
    uint8_t flags;

    if (!p || !info)
        return PROS_NTFS_EINVAL;
    if (record_size < 40u || bytes_per_sector < 512u ||
        record_size % bytes_per_sector != 0u)
        return PROS_NTFS_EINVAL;
    if (p[0] != 'I' || p[1] != 'N' || p[2] != 'D' || p[3] != 'X')
        return PROS_NTFS_ECORRUPT;
    if (ntfs_index_record_fixup((void *)record, record_size,
                                bytes_per_sector) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    if (le64(p + 16u) != expected_vcn)
        return PROS_NTFS_ECORRUPT;

    first_relative = le32(p + 24u);
    entries_size = le32(p + 28u);
    allocated_size = le32(p + 32u);
    flags = p[36u];

    if (first_relative < 16u ||
        entries_size < first_relative ||
        allocated_size < entries_size ||
        (flags & ~1u) != 0u ||
        24u + allocated_size > record_size ||
        24u + entries_size > record_size ||
        24u + first_relative > 24u + entries_size)
        return PROS_NTFS_ECORRUPT;

    info->vcn = expected_vcn;
    info->first_entry_offset = 24u + first_relative;
    info->entries_size = 24u + entries_size;
    info->allocated_size = 24u + allocated_size;
    info->flags = flags;
    return PROS_NTFS_OK;
}

static int ntfs_index_allocation_read(const struct pros_ntfs *fs,
                                      const struct pros_ntfs_run *runs,
                                      uint32_t run_count,
                                      uint64_t byte_offset,
                                      void *buffer,
                                      uint32_t size)
{
    uint8_t sector[4096];
    uint32_t copied = 0u;
    uint64_t cluster_bytes;

    if (!fs || !runs || run_count == 0u || !buffer || size == 0u ||
        fs->bytes_per_sector < 512u || fs->bytes_per_sector > sizeof(sector) ||
        fs->sectors_per_cluster == 0u)
        return PROS_NTFS_EINVAL;

    cluster_bytes = (uint64_t)fs->bytes_per_sector * fs->sectors_per_cluster;
    while (copied < size) {
        uint64_t logical_cluster = byte_offset / cluster_bytes;
        uint64_t cluster_offset = byte_offset % cluster_bytes;
        uint64_t cumulative = 0u;
        uint64_t physical_lcn = 0u;
        uint8_t sparse = 0u;
        uint8_t found = 0u;

        for (uint32_t r = 0u; r < run_count; ++r) {
            const struct pros_ntfs_run *run = &runs[r];
            if (logical_cluster < cumulative + run->clusters) {
                physical_lcn = run->lcn + (logical_cluster - cumulative);
                if (physical_lcn < run->lcn)
                    return PROS_NTFS_EOVERFLOW;
                sparse = run->sparse;
                found = 1u;
                break;
            }
            if (run->clusters > 0xFFFFFFFFFFFFFFFFull - cumulative)
                return PROS_NTFS_EOVERFLOW;
            cumulative += run->clusters;
        }
        if (!found)
            return PROS_NTFS_EIO;

        {
            uint32_t sector_index =
                (uint32_t)(cluster_offset / fs->bytes_per_sector);
            uint32_t in_sector =
                (uint32_t)(cluster_offset % fs->bytes_per_sector);
            uint32_t take = fs->bytes_per_sector - in_sector;
            uint64_t lba;

            if (take > size - copied)
                take = size - copied;
            if (sparse) {
                for (uint32_t i = 0u; i < take; ++i)
                    ((uint8_t *)buffer)[copied + i] = 0u;
            } else {
                if (sector_index >= fs->sectors_per_cluster ||
                    ntfs_cluster_lba(fs, physical_lcn, &lba) != PROS_NTFS_OK)
                    return PROS_NTFS_EIO;
                lba += sector_index;
                if (block_device_read(fs->mount.device, lba, 1u, sector) !=
                    PROS_BLOCK_OK)
                    return PROS_NTFS_EIO;
                for (uint32_t i = 0u; i < take; ++i)
                    ((uint8_t *)buffer)[copied + i] = sector[in_sector + i];
            }
            copied += take;
            byte_offset += take;
        }
    }
    return PROS_NTFS_OK;
}

static int ntfs_index_record_geometry(const struct pros_ntfs *fs,
                                      const struct pros_ntfs_index_root *root,
                                      uint32_t *record_clusters)
{
    uint32_t cluster_bytes;
    int8_t encoded;
    uint32_t expected_bytes;

    if (!fs || !root || !record_clusters || fs->bytes_per_sector == 0u ||
        fs->sectors_per_cluster == 0u)
        return PROS_NTFS_EINVAL;

    cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
    if (cluster_bytes == 0u)
        return PROS_NTFS_EOVERFLOW;

    encoded = (int8_t)root->clusters_per_index_record;
    if (encoded > 0) {
        if ((uint32_t)encoded > 128u ||
            (uint32_t)encoded > 0xFFFFFFFFu / cluster_bytes)
            return PROS_NTFS_ECORRUPT;
        expected_bytes = (uint32_t)encoded * cluster_bytes;
        *record_clusters = (uint32_t)encoded;
    } else if (encoded < 0) {
        uint32_t shift = abs_u8(encoded);
        if (shift > 16u)
            return PROS_NTFS_ECORRUPT;
        expected_bytes = 1u << shift;
        if (expected_bytes % cluster_bytes != 0u)
            return PROS_NTFS_ECORRUPT;
        *record_clusters = expected_bytes / cluster_bytes;
        if (*record_clusters == 0u)
            return PROS_NTFS_ECORRUPT;
    } else {
        return PROS_NTFS_ECORRUPT;
    }

    if (expected_bytes != root->bytes_per_index_record ||
        expected_bytes > PROS_NTFS_RECORD_WORK_MAX ||
        expected_bytes < fs->bytes_per_sector ||
        (expected_bytes % fs->bytes_per_sector) != 0u)
        return PROS_NTFS_ECORRUPT;
    return PROS_NTFS_OK;
}

int ntfs_index_allocation_lookup(const struct pros_ntfs *fs,
                                 const void *record, uint32_t record_size,
                                 uint32_t index_root_attr_offset,
                                 uint32_t index_allocation_attr_offset,
                                 const uint8_t *name_utf16le,
                                 uint8_t name_length,
                                 uint64_t *file_reference)
{
    struct pros_ntfs_index_root root;
    struct pros_ntfs_run runs[PROS_NTFS_MFT_RUN_MAX];
    uint32_t run_count = 0u;
    uint32_t record_clusters;
    const uint8_t *root_data;
    uint32_t root_size;
    uint64_t pending[PROS_NTFS_INDEX_NODE_MAX];
    uint64_t visited[PROS_NTFS_INDEX_NODE_MAX];
    uint32_t pending_count = 0u;
    uint32_t visited_count = 0u;
    static uint8_t index_record[PROS_NTFS_RECORD_WORK_MAX];

    if (!fs || !record || !name_utf16le || !file_reference ||
        name_length == 0u)
        return PROS_NTFS_EINVAL;

    if (ntfs_index_root_header(record, record_size, index_root_attr_offset,
                               &root) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (ntfs_index_record_geometry(fs, &root, &record_clusters) !=
        PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    if (ntfs_attribute_runlist(record, record_size,
                               index_allocation_attr_offset, &root_data,
                               &root_size) != PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;
    if (ntfs_decode_runlist(root_data, root_size, runs,
                            PROS_NTFS_MFT_RUN_MAX, &run_count) !=
        PROS_NTFS_OK)
        return PROS_NTFS_ECORRUPT;

    /*
     * Scan the resident root first. Every sub-node pointer is queued, so
     * exact lookup does not depend on implementing NTFS collation yet.
     */
    {
        const uint8_t *data;
        uint32_t data_size;
        uint32_t offset;

        if (ntfs_attribute_data(record, record_size, index_root_attr_offset,
                                 &data, &data_size) != PROS_NTFS_OK)
            return PROS_NTFS_ECORRUPT;

        offset = root.first_entry_offset;
        while (offset < root.entries_size) {
            uint16_t entry_size;
            uint16_t stream_size;
            uint8_t flags;
            uint32_t remaining;

            if (root.entries_size - offset < 16u)
                return PROS_NTFS_ECORRUPT;
            remaining = root.entries_size - offset;
            entry_size = le16(data + offset + 8u);
            stream_size = le16(data + offset + 10u);
            flags = data[offset + 12u];

            if (entry_size < 16u || entry_size > remaining ||
                (entry_size & 7u) != 0u || (flags & ~3u) != 0u)
                return PROS_NTFS_ECORRUPT;

            if ((flags & 2u) == 0u && stream_size != 0u &&
                stream_size <= entry_size - 16u -
                ((flags & 1u) ? 8u : 0u) &&
                ntfs_name_key_matches(data + offset + 16u, stream_size,
                                      name_utf16le, name_length)) {
                *file_reference = le64(data + offset) &
                    0x0000FFFFFFFFFFFFull;
                return PROS_NTFS_OK;
            }

            if (flags & 1u) {
                uint64_t child_vcn;
                if (entry_size < 24u)
                    return PROS_NTFS_ECORRUPT;
                child_vcn = le64(data + offset + entry_size - 8u);
                if (child_vcn > 0x0000FFFFFFFFFFFFull ||
                    child_vcn % record_clusters != 0u)
                    return PROS_NTFS_ECORRUPT;
                if (pending_count >= PROS_NTFS_INDEX_NODE_MAX)
                    return PROS_NTFS_EOVERFLOW;
                pending[pending_count++] = child_vcn;
            }

            if (flags & 2u)
                break;
            if (offset + entry_size <= offset)
                return PROS_NTFS_ECORRUPT;
            offset += entry_size;
        }
    }

    while (pending_count != 0u) {
        uint64_t vcn = pending[--pending_count];
        uint8_t duplicate = 0u;

        for (uint32_t i = 0u; i < visited_count; ++i)
            if (visited[i] == vcn) {
                duplicate = 1u;
                break;
            }
        if (duplicate)
            continue;
        if (visited_count >= PROS_NTFS_INDEX_NODE_MAX)
            return PROS_NTFS_EOVERFLOW;
        visited[visited_count++] = vcn;

        if (vcn > 0xFFFFFFFFFFFFFFFFull / (uint64_t)fs->bytes_per_sector /
                  fs->sectors_per_cluster / record_clusters)
            return PROS_NTFS_EOVERFLOW;

        {
            uint64_t byte_offset =
                vcn * (uint64_t)fs->bytes_per_sector *
                fs->sectors_per_cluster;
            struct pros_ntfs_index_record info;

            if (ntfs_index_allocation_read(fs, runs, run_count,
                                           byte_offset, index_record,
                                           root.bytes_per_index_record) !=
                PROS_NTFS_OK)
                return PROS_NTFS_EIO;

            if (ntfs_index_record_header(index_record,
                                         root.bytes_per_index_record,
                                         fs->bytes_per_sector, vcn, &info) !=
                PROS_NTFS_OK)
                return PROS_NTFS_ECORRUPT;

            {
                uint32_t offset = info.first_entry_offset;
                while (offset < info.entries_size) {
                    uint16_t entry_size;
                    uint16_t stream_size;
                    uint8_t flags;
                    uint32_t remaining;

                    if (info.entries_size - offset < 16u)
                        return PROS_NTFS_ECORRUPT;
                    remaining = info.entries_size - offset;
                    entry_size = le16(index_record + offset + 8u);
                    stream_size = le16(index_record + offset + 10u);
                    flags = index_record[offset + 12u];

                    if (entry_size < 16u || entry_size > remaining ||
                        (entry_size & 7u) != 0u || (flags & ~3u) != 0u)
                        return PROS_NTFS_ECORRUPT;

                    if ((flags & 2u) == 0u && stream_size != 0u &&
                        stream_size <= entry_size - 16u -
                        ((flags & 1u) ? 8u : 0u) &&
                        ntfs_name_key_matches(index_record + offset + 16u,
                                              stream_size, name_utf16le,
                                              name_length)) {
                        *file_reference = le64(index_record + offset) &
                            0x0000FFFFFFFFFFFFull;
                        return PROS_NTFS_OK;
                    }

                    if (flags & 1u) {
                        uint64_t child_vcn;
                        if (entry_size < 24u)
                            return PROS_NTFS_ECORRUPT;
                        child_vcn = le64(index_record + offset +
                                         entry_size - 8u);
                        if (child_vcn > 0x0000FFFFFFFFFFFFull ||
                            child_vcn % record_clusters != 0u)
                            return PROS_NTFS_ECORRUPT;
                        if (pending_count >= PROS_NTFS_INDEX_NODE_MAX)
                            return PROS_NTFS_EOVERFLOW;
                        pending[pending_count++] = child_vcn;
                    }

                    if (flags & 2u)
                        break;
                    if (offset + entry_size <= offset)
                        return PROS_NTFS_ECORRUPT;
                    offset += entry_size;
                }
            }
        }
    }

    return PROS_NTFS_ENOTSUP;
}


int ntfs_bootstrap_mft(struct pros_ntfs *fs)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    uint32_t attr_offset;
    const uint8_t *runlist;
    uint32_t runlist_size;
    uint32_t count;
    int rc;

    if (!fs || fs->bytes_per_file_record == 0u ||
        fs->bytes_per_file_record > PROS_NTFS_RECORD_WORK_MAX)
        return PROS_NTFS_EINVAL;
    rc = ntfs_read_file_record(fs, 0u, record, sizeof(record));
    if (rc != PROS_NTFS_OK)
        return rc;
    rc = ntfs_find_attribute(record, fs->bytes_per_file_record, 0x80u,
                             &attr_offset);
    if (rc != PROS_NTFS_OK)
        return rc;
    rc = ntfs_attribute_runlist(record, fs->bytes_per_file_record,
                                attr_offset, &runlist, &runlist_size);
    if (rc != PROS_NTFS_OK)
        return rc;
    rc = ntfs_decode_runlist(runlist, runlist_size, fs->mft_runs,
                             PROS_NTFS_MFT_RUN_MAX, &count);
    if (rc != PROS_NTFS_OK)
        return rc;
    if (count == 0u || fs->mft_runs[0].sparse)
        return PROS_NTFS_ECORRUPT;
    fs->mft_run_count = count;
    return PROS_NTFS_OK;
}

struct ntfs_test_disk {
    uint8_t boot[512];
    uint8_t mft_record[1024];
    uint8_t file_record[1024];
    uint8_t nonresident_record[1024];
    uint8_t nonresident_data[16384];
    uint8_t upcase_record[1024];
    uint8_t upcase_data[PROS_NTFS_UPCASE_BYTES];
    uint8_t index_record[4096];
    uint8_t root_record[1024];
    uint8_t bitmap_record[1024];
    uint8_t bitmap_data[512];
};

static int ntfs_test_read(struct pros_block_device *device, uint64_t lba,
                          uint32_t count, void *buffer)
{
    struct ntfs_test_disk *disk =
        (struct ntfs_test_disk *)device->private_data;
    if (!disk || !buffer || count == 0u)
        return PROS_BLOCK_EIO;
    for (uint32_t s = 0; s < count; ++s) {
        uint8_t *dst = (uint8_t *)buffer + s * 512u;
        for (uint32_t i = 0; i < 512u; ++i) dst[i] = 0;
        if (lba + s == 0u)
            for (uint32_t i = 0; i < 512u; ++i) dst[i] = disk->boot[i];
        else if (lba + s == 32u || lba + s == 33u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->mft_record[(lba + s - 32u) * 512u + i];
        else if (lba + s == 54u || lba + s == 55u)
            for (uint32_t i = 0u; i < 512u; ++i)
                dst[i] = disk->file_record[(lba + s - 54u) * 512u + i];
        else if (lba + s == 56u || lba + s == 57u)
            for (uint32_t i = 0u; i < 512u; ++i)
                dst[i] = disk->nonresident_record[(lba + s - 56u) * 512u + i];
        else if (lba + s == 42u || lba + s == 43u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->root_record[(lba + s - 42u) * 512u + i];
        else if (lba + s == 44u || lba + s == 45u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->bitmap_record[(lba + s - 44u) * 512u + i];
        else if (lba + s >= 336u && lba + s < 368u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->nonresident_data[(lba + s - 336u) * 512u + i];
        else if (lba + s >= 368u && lba + s < 369u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->bitmap_data[(lba + s - 368u) * 512u + i];
        else if (lba + s == 52u || lba + s == 53u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->upcase_record[(lba + s - 52u) * 512u + i];
        else if (lba + s >= 80u && lba + s < 336u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->upcase_data[(lba + s - 80u) * 512u + i];
        else if (lba + s >= 64u && lba + s < 72u)
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->index_record[(lba + s - 64u) * 512u + i];
    }
    return PROS_BLOCK_OK;
}



static int ntfs_test_write(struct pros_block_device *device, uint64_t lba,
                           uint32_t count, const void *buffer)
{
    struct ntfs_test_disk *disk =
        (struct ntfs_test_disk *)device->private_data;
    if (!disk || !buffer || count == 0u)
        return PROS_BLOCK_EIO;

    for (uint32_t s = 0u; s < count; ++s) {
        const uint8_t *src = (const uint8_t *)buffer + s * 512u;
        uint64_t sector = lba + s;
        if (sector == 0u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->boot[i] = src[i];
        } else if (sector == 32u || sector == 33u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->mft_record[(sector - 32u) * 512u + i] = src[i];
        } else if (sector == 54u || sector == 55u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->file_record[(sector - 54u) * 512u + i] = src[i];
        } else if (sector == 56u || sector == 57u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->nonresident_record[(sector - 56u) * 512u + i] = src[i];
        } else if (sector >= 336u && sector < 368u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->nonresident_data[(sector - 336u) * 512u + i] = src[i];
        } else if (sector == 44u || sector == 45u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->bitmap_record[(sector - 44u) * 512u + i] = src[i];
        } else if (sector == 368u) {
            for (uint32_t i = 0u; i < 512u; ++i)
                disk->bitmap_data[i] = src[i];
        } else {
            return PROS_BLOCK_EIO;
        }
    }
    return PROS_BLOCK_OK;
}


int ntfs_directory_lookup(const struct pros_ntfs *fs,
                          uint64_t directory_record_number,
                          const uint8_t *name_utf16le, uint8_t name_length,
                          uint64_t *file_reference)
{
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    uint32_t index_root_offset;
    uint32_t index_allocation_offset;
    int status;

    if (!fs || !name_utf16le || name_length == 0u || !file_reference ||
        fs->bytes_per_file_record > sizeof(record))
        return PROS_NTFS_EINVAL;

    status = ntfs_upcase_load(fs);
    if (status != PROS_NTFS_OK)
        return status;
    ntfs_active_upcase = ntfs_upcase_data;

    status = ntfs_read_file_record(fs, directory_record_number,
                                   record, sizeof(record));
    if (status != PROS_NTFS_OK) {
        ntfs_active_upcase = 0;
        return status;
    }

    status = ntfs_find_attribute(record, fs->bytes_per_file_record,
                                 0x90u, &index_root_offset);
    if (status != PROS_NTFS_OK) {
        ntfs_active_upcase = 0;
        return status;
    }

    /*
     * Search the resident root first. This permits small directories that
     * have no $INDEX_ALLOCATION stream.
     */
    status = ntfs_index_root_lookup(record, fs->bytes_per_file_record,
                                     index_root_offset, name_utf16le,
                                     name_length, file_reference);
    if (status == PROS_NTFS_OK) {
        ntfs_active_upcase = 0;
        return PROS_NTFS_OK;
    }
    if (status != PROS_NTFS_ENOTSUP) {
        ntfs_active_upcase = 0;
        return status;
    }

    status = ntfs_find_attribute(record, fs->bytes_per_file_record,
                                 0xA0u, &index_allocation_offset);
    if (status != PROS_NTFS_OK) {
        ntfs_active_upcase = 0;
        return status;
    }

    status = ntfs_index_allocation_lookup(fs, record,
                                          fs->bytes_per_file_record,
                                          index_root_offset,
                                          index_allocation_offset,
                                          name_utf16le, name_length,
                                          file_reference);
    ntfs_active_upcase = 0;
    return status;
}

int ntfs_path_lookup(const struct pros_ntfs *fs,
                     const uint8_t *const *components,
                     const uint8_t *component_lengths,
                     uint32_t component_count,
                     uint64_t *file_reference)
{
    uint64_t current = 5u; /* NTFS $Root directory MFT record. */

    if (!fs || !components || !component_lengths || !file_reference ||
        component_count == 0u)
        return PROS_NTFS_EINVAL;
    if (component_count > 256u)
        return PROS_NTFS_EOVERFLOW;

    for (uint32_t i = 0u; i < component_count; ++i) {
        uint64_t next;
        if (!components[i] || component_lengths[i] == 0u)
            return PROS_NTFS_EINVAL;

        {
            int status = ntfs_directory_lookup(fs, current, components[i],
                                               component_lengths[i], &next);
            if (status != PROS_NTFS_OK)
                return status;
        }

        current = next;
    }

    *file_reference = current;
    return PROS_NTFS_OK;
}


static int ntfs_vfs_close(struct pros_vfs_file *file)
{
    (void)file;
    return PROS_VFS_OK;
}

static int ntfs_vfs_read(struct pros_vfs_file *file, uint32_t offset,
                         uint32_t length, void *buffer)
{
    struct pros_ntfs_vfs_node *node;
    uint32_t available;
    uint32_t got = 0u;
    int rc;

    if (!file || !file->mount || !buffer)
        return PROS_VFS_EINVAL;
    node = (struct pros_ntfs_vfs_node *)file->private_data;
    if (!node || node->vnode.type != PROS_VFS_NODE_FILE ||
        (uint64_t)offset > node->data_size)
        return PROS_VFS_EINVAL;

    available = (uint32_t)(node->data_size - (uint64_t)offset);
    if (length > available)
        length = available;
    rc = ntfs_file_read((const struct pros_ntfs *)file->mount->private_data,
                        node->record_number, offset, buffer, length, &got);
    if (rc != PROS_NTFS_OK)
        return rc == PROS_NTFS_EINVAL ? PROS_VFS_EINVAL : PROS_VFS_EIO;
    return (int)got;
}

static int ntfs_vfs_write(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, const void *buffer)
{
    struct pros_ntfs_vfs_node *node;
    uint32_t written = 0u;
    int rc;

    if (!file || !file->mount || !buffer)
        return PROS_VFS_EINVAL;
    node = (struct pros_ntfs_vfs_node *)file->private_data;
    if (!node || node->vnode.type != PROS_VFS_NODE_FILE)
        return PROS_VFS_EINVAL;
    rc = ntfs_file_write((struct pros_ntfs *)file->mount->private_data,
                         node->record_number, offset, buffer, length,
                         &written);
    if (rc != PROS_NTFS_OK)
        return rc == PROS_NTFS_EINVAL ? PROS_VFS_EINVAL :
               (rc == PROS_NTFS_EOVERFLOW ? PROS_VFS_EOVERFLOW :
                rc == PROS_NTFS_ENOTSUP ? PROS_VFS_ENOSYS :
                PROS_VFS_EIO);

    /*
     * NTFS growth is committed to the on-disk DATA/FILE_NAME metadata by
     * ntfs_file_write(). Keep the cached VFS vnode size synchronized after a
     * successful write that extends EOF.
     */
    if ((uint64_t)offset + written > node->data_size) {
        node->data_size = (uint64_t)offset + written;
        node->vnode.size = node->data_size;
    }
    return (int)written;
}

static const struct pros_vfs_file_ops ntfs_vfs_file_ops = {
    .read = ntfs_vfs_read,
    .write = ntfs_vfs_write,
    .close = ntfs_vfs_close
};

static int ntfs_vfs_readdir(struct pros_vfs_vnode *vnode, uint64_t *cookie,
                            struct pros_vfs_dirent *entry)
{
    struct pros_ntfs_vfs_node *parent_node;
    struct pros_ntfs *fs;
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    uint32_t mft_attr;
    uint64_t mft_size;
    uint64_t max_records;
    uint64_t record_number;

    if (!vnode || !cookie || !entry ||
        vnode->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    if (!vnode->mount)
        return PROS_VFS_EINVAL;

    parent_node = (struct pros_ntfs_vfs_node *)vnode->private_data;
    fs = parent_node ? (struct pros_ntfs *)vnode->mount->private_data : 0;
    if (!fs || fs->bytes_per_file_record == 0u ||
        fs->bytes_per_file_record > sizeof(record))
        return PROS_VFS_EINVAL;

    if (ntfs_read_file_record(fs, 0u, record, sizeof(record)) !=
            PROS_NTFS_OK ||
        ntfs_find_attribute(record, fs->bytes_per_file_record, 0x80u,
                            &mft_attr) != PROS_NTFS_OK)
        return PROS_VFS_EIO;

    if (le32(record + mft_attr + 4u) < 64u ||
        record[mft_attr + 8u] != 1u)
        return PROS_VFS_ENOSYS;

    mft_size = le64(record + mft_attr + 48u);
    max_records = mft_size / fs->bytes_per_file_record;
    if (max_records == 0u)
        return PROS_VFS_EIO;

    record_number = *cookie;
    while (record_number < max_records) {
        uint16_t record_flags;
        uint32_t attr_pos;
        uint64_t parent_reference;
        uint64_t data_size = 0u;
        const uint8_t *name = 0;
        uint32_t name_bytes = 0u;
        uint8_t found_name = 0u;

        if (ntfs_read_file_record(fs, record_number, record,
                                  sizeof(record)) == PROS_NTFS_OK) {
            record_flags = le16(record + 22u);
            if ((record_flags & 0x0001u) != 0u) {
                uint16_t first_attr = le16(record + 20u);
                attr_pos = first_attr;

                while (attr_pos + 24u <= fs->bytes_per_file_record) {
                    uint32_t attr_type = le32(record + attr_pos);
                    uint32_t attr_len = le32(record + attr_pos + 4u);

                    if (attr_type == 0xFFFFFFFFu)
                        break;
                    if (attr_len < 24u ||
                        attr_pos > fs->bytes_per_file_record - attr_len)
                        break;

                    if (attr_type == 0x30u && record[attr_pos + 8u] == 0u) {
                        uint16_t value_offset = le16(record + attr_pos + 20u);
                        uint32_t value_length = le32(record + attr_pos + 16u);

                        if ((uint32_t)value_offset <= attr_len &&
                            value_length <= attr_len - (uint32_t)value_offset &&
                            value_length >= 66u) {
                            const uint8_t *data =
                                record + attr_pos + value_offset;
                            uint8_t candidate_length = data[64u];
                            uint8_t candidate_namespace = data[65u];

                            if ((candidate_namespace == 1u ||
                                 candidate_namespace == 3u) &&
                                candidate_length != 0u &&
                                66u + (uint32_t)candidate_length * 2u <=
                                    value_length) {
                                parent_reference = le64(data);
                                if ((parent_reference &
                                     0x0000FFFFFFFFFFFFull) ==
                                    (parent_node->record_number &
                                     0x0000FFFFFFFFFFFFull)) {
                                    name = data + 66u;
                                    name_bytes =
                                        (uint32_t)candidate_length * 2u;
                                    data_size = le64(data + 48u);
                                    found_name = 1u;
                                    break;
                                }
                            }
                        }
                    }

                    attr_pos += attr_len;
                }
            }
        }

        ++record_number;
        *cookie = record_number;

        if (!found_name)
            continue;

        {
            uint32_t out = 0u;
            uint32_t units = name_bytes / 2u;

            for (uint32_t i = 0u; i < units; ++i) {
                uint32_t cp = le16(name + i * 2u);

                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    uint32_t low;
                    if (i + 1u >= units)
                        return PROS_VFS_EIO;
                    low = le16(name + (i + 1u) * 2u);
                    if (low < 0xDC00u || low > 0xDFFFu)
                        return PROS_VFS_EIO;
                    cp = 0x10000u + ((cp - 0xD800u) << 10u) +
                         (low - 0xDC00u);
                    ++i;
                } else if (cp >= 0xDC00u) {
                    return PROS_VFS_EIO;
                }

                if (cp <= 0x7Fu) {
                    if (out + 1u > PROS_VFS_NAME_MAX)
                        return PROS_VFS_EOVERFLOW;
                    entry->name[out++] = (char)cp;
                } else if (cp <= 0x7FFu) {
                    if (out + 2u > PROS_VFS_NAME_MAX)
                        return PROS_VFS_EOVERFLOW;
                    entry->name[out++] = (char)(0xC0u | (cp >> 6u));
                    entry->name[out++] = (char)(0x80u | (cp & 0x3Fu));
                } else if (cp <= 0xFFFFu) {
                    if (out + 3u > PROS_VFS_NAME_MAX)
                        return PROS_VFS_EOVERFLOW;
                    entry->name[out++] = (char)(0xE0u | (cp >> 12u));
                    entry->name[out++] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
                    entry->name[out++] = (char)(0x80u | (cp & 0x3Fu));
                } else {
                    if (out + 4u > PROS_VFS_NAME_MAX)
                        return PROS_VFS_EOVERFLOW;
                    entry->name[out++] = (char)(0xF0u | (cp >> 18u));
                    entry->name[out++] = (char)(0x80u | ((cp >> 12u) & 0x3Fu));
                    entry->name[out++] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
                    entry->name[out++] = (char)(0x80u | (cp & 0x3Fu));
                }
            }

            entry->name[out] = '\0';
            entry->type = (record_flags & 0x0002u) != 0u ?
                          PROS_VFS_NODE_DIRECTORY :
                          PROS_VFS_NODE_FILE;
            entry->size = data_size;
        }
        return PROS_VFS_OK;
    }

    return PROS_VFS_ENOENT;
}

static int ntfs_vfs_lookup(struct pros_vfs_vnode *parent, const char *name,
                           struct pros_vfs_vnode **result)
{
    struct pros_ntfs_vfs_node *parent_node;
    struct pros_ntfs *fs;
    uint8_t name_utf16le[PROS_VFS_NAME_MAX * 2u];
    uint32_t name_length = 0u;
    uint32_t pos = 0u;
    uint64_t record_number = 0u;
    static uint8_t record[PROS_NTFS_RECORD_WORK_MAX];
    struct pros_ntfs_file_name info;
    int status;

    if (!parent || !name || !result ||
        parent->type != PROS_VFS_NODE_DIRECTORY || !parent->mount)
        return PROS_VFS_EINVAL;

    parent_node = (struct pros_ntfs_vfs_node *)parent->private_data;
    fs = parent_node ? (struct pros_ntfs *)parent->mount->private_data : 0;
    if (!fs)
        return PROS_VFS_EINVAL;

    /*
     * VFS paths are UTF-8. NTFS directory lookup operates on UTF-16LE.
     * Decode strictly here so Unicode filenames exposed by readdir() can
     * also be opened through the normal VFS path API. Invalid UTF-8,
     * overlong encodings, surrogate code points and out-of-range scalars
     * are rejected rather than being silently substituted.
     */
    while (name[pos] != '\0') {
        uint32_t cp;
        uint32_t extra;
        unsigned char ch = (unsigned char)name[pos++];

        if (ch <= 0x7Fu) {
            cp = ch;
            extra = 0u;
        } else if (ch >= 0xC2u && ch <= 0xDFu) {
            cp = ch & 0x1Fu;
            extra = 1u;
        } else if (ch >= 0xE0u && ch <= 0xEFu) {
            cp = ch & 0x0Fu;
            extra = 2u;
        } else if (ch >= 0xF0u && ch <= 0xF4u) {
            cp = ch & 0x07u;
            extra = 3u;
        } else {
            return PROS_VFS_EINVAL;
        }

        if (pos + extra > PROS_VFS_NAME_MAX * 4u)
            return PROS_VFS_EOVERFLOW;

        for (uint32_t i = 0u; i < extra; ++i) {
            unsigned char tail;
            if (name[pos] == '\0')
                return PROS_VFS_EINVAL;
            tail = (unsigned char)name[pos++];
            if ((tail & 0xC0u) != 0x80u)
                return PROS_VFS_EINVAL;
            cp = (cp << 6u) | (tail & 0x3Fu);
        }

        if ((extra == 2u && cp < 0x800u) ||
            (extra == 3u && cp < 0x10000u) ||
            cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
            return PROS_VFS_EINVAL;

        /*
         * NTFS filename components follow the Windows filename character
         * restrictions. Reject controls and the reserved punctuation here
         * before converting the scalar to UTF-16.
         */
        if (cp < 0x20u || cp == (uint32_t)'"' ||
            cp == (uint32_t)'/' || cp == (uint32_t)'\\' ||
            cp == (uint32_t)':' || cp == (uint32_t)'|' ||
            cp == (uint32_t)'<' || cp == (uint32_t)'>' ||
            cp == (uint32_t)'*' || cp == (uint32_t)'?')
            return PROS_VFS_EINVAL;

        if (cp <= 0xFFFFu) {
            if (name_length >= PROS_VFS_NAME_MAX)
                return PROS_VFS_EOVERFLOW;
            name_utf16le[name_length * 2u] = (uint8_t)cp;
            name_utf16le[name_length * 2u + 1u] = (uint8_t)(cp >> 8u);
            ++name_length;
        } else {
            uint32_t scalar = cp - 0x10000u;
            uint16_t high = (uint16_t)(0xD800u + (scalar >> 10u));
            uint16_t low = (uint16_t)(0xDC00u + (scalar & 0x3FFu));
            if (name_length + 2u > PROS_VFS_NAME_MAX)
                return PROS_VFS_EOVERFLOW;
            name_utf16le[name_length * 2u] = (uint8_t)high;
            name_utf16le[name_length * 2u + 1u] = (uint8_t)(high >> 8u);
            ++name_length;
            name_utf16le[name_length * 2u] = (uint8_t)low;
            name_utf16le[name_length * 2u + 1u] = (uint8_t)(low >> 8u);
            ++name_length;
        }
    }

    if (name_length == 0u)
        return PROS_VFS_EINVAL;

    status = ntfs_directory_lookup(fs, parent_node->record_number,
                                    name_utf16le, (uint8_t)name_length,
                                    &record_number);
    if (status != PROS_NTFS_OK)
        return status == PROS_NTFS_ENOTSUP ? PROS_VFS_ENOENT :
               (status == PROS_NTFS_EINVAL ? PROS_VFS_EINVAL :
                PROS_VFS_EIO);

    for (uint32_t i = 0u; i < PROS_NTFS_VFS_NODE_MAX; ++i) {
        struct pros_ntfs_vfs_node *node = &fs->vfs_nodes[i];
        if (node->used && node->vnode.parent == parent &&
            node->record_number == record_number) {
            *result = &node->vnode;
            return PROS_VFS_OK;
        }
    }

    status = ntfs_read_file_record(fs, record_number, record, sizeof(record));
    if (status != PROS_NTFS_OK)
        return PROS_VFS_EIO;
    {
        uint32_t attr_offset = 0u;
        if (ntfs_find_attribute(record, fs->bytes_per_file_record, 0x30u,
                                &attr_offset) != PROS_NTFS_OK ||
            ntfs_file_name_attribute(record, fs->bytes_per_file_record,
                                     attr_offset, &info) != PROS_NTFS_OK)
            return PROS_VFS_EIO;
    }

    for (uint32_t i = 1u; i < PROS_NTFS_VFS_NODE_MAX; ++i) {
        struct pros_ntfs_vfs_node *node = &fs->vfs_nodes[i];
        if (node->used)
            continue;

        node->used = 1u;
        node->record_number = record_number;
        node->data_size = info.data_size;
        if (vfs_vnode_init(
                &node->vnode, &fs->mount, parent,
                (info.flags & 0x10000000u) != 0u ?
                    PROS_VFS_NODE_DIRECTORY : PROS_VFS_NODE_FILE,
                &ntfs_vfs_vnode_ops, &ntfs_vfs_file_ops,
                node, info.data_size) != PROS_VFS_OK) {
            node->used = 0u;
            return PROS_VFS_EIO;
        }
        *result = &node->vnode;
        return PROS_VFS_OK;
    }
    return PROS_VFS_EBUSY;
}


static int ntfs_vfs_create(struct pros_vfs_vnode *parent, const char *name,
                           struct pros_vfs_vnode **result)
{
    struct pros_ntfs_vfs_node *parent_node;
    struct pros_ntfs *fs;
    uint8_t utf16le[PROS_VFS_NAME_MAX * 2u];
    uint32_t units = 0u, pos = 0u;
    uint64_t record_number;
    int rc;

    if (!parent || !name || !result || !parent->mount ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    parent_node = (struct pros_ntfs_vfs_node *)parent->private_data;
    fs = parent_node ? (struct pros_ntfs *)parent->mount->private_data : 0;
    if (!fs || parent_node->record_number > 0x0000FFFFFFFFFFFFull)
        return PROS_VFS_EINVAL;

    while (name[pos] != '\0') {
        uint32_t cp, extra;
        unsigned char ch = (unsigned char)name[pos++];

        if (ch <= 0x7Fu) { cp = ch; extra = 0u; }
        else if (ch >= 0xC2u && ch <= 0xDFu) { cp = ch & 0x1Fu; extra = 1u; }
        else if (ch >= 0xE0u && ch <= 0xEFu) { cp = ch & 0x0Fu; extra = 2u; }
        else if (ch >= 0xF0u && ch <= 0xF4u) { cp = ch & 0x07u; extra = 3u; }
        else return PROS_VFS_EINVAL;

        for (uint32_t i = 0u; i < extra; ++i) {
            unsigned char tail;
            if (name[pos] == '\0')
                return PROS_VFS_EINVAL;
            tail = (unsigned char)name[pos++];
            if ((tail & 0xC0u) != 0x80u)
                return PROS_VFS_EINVAL;
            cp = (cp << 6u) | (tail & 0x3Fu);
        }
        if ((extra == 2u && cp < 0x800u) ||
            (extra == 3u && cp < 0x10000u) ||
            cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
            return PROS_VFS_EINVAL;

        /* Keep VFS create consistent with VFS lookup/NTFS filename rules. */
        if (cp < 0x20u || cp == (uint32_t)'"' ||
            cp == (uint32_t)'/' || cp == (uint32_t)'\\' ||
            cp == (uint32_t)':' || cp == (uint32_t)'|' ||
            cp == (uint32_t)'<' || cp == (uint32_t)'>' ||
            cp == (uint32_t)'*' || cp == (uint32_t)'?')
            return PROS_VFS_EINVAL;

        if (cp <= 0xFFFFu) {
            if (units >= PROS_VFS_NAME_MAX) return PROS_VFS_EOVERFLOW;
            utf16le[units * 2u] = (uint8_t)cp;
            utf16le[units * 2u + 1u] = (uint8_t)(cp >> 8u);
            ++units;
        } else {
            uint32_t scalar = cp - 0x10000u;
            uint16_t high = (uint16_t)(0xD800u + (scalar >> 10u));
            uint16_t low = (uint16_t)(0xDC00u + (scalar & 0x3FFu));
            if (units + 2u > PROS_VFS_NAME_MAX) return PROS_VFS_EOVERFLOW;
            utf16le[units * 2u] = (uint8_t)high;
            utf16le[units * 2u + 1u] = (uint8_t)(high >> 8u);
            ++units;
            utf16le[units * 2u] = (uint8_t)low;
            utf16le[units * 2u + 1u] = (uint8_t)(low >> 8u);
            ++units;
        }
    }
    if (units == 0u)
        return PROS_VFS_EINVAL;

    struct pros_ntfs_vfs_node *node = 0;

    for (uint32_t i = 1u; i < PROS_NTFS_VFS_NODE_MAX; ++i) {
        if (!fs->vfs_nodes[i].used) {
            node = &fs->vfs_nodes[i];
            break;
        }
    }
    if (!node)
        return PROS_VFS_EBUSY;

    rc = ntfs_create_file(fs, parent_node->record_number, &record_number,
                          (const uint16_t *)utf16le, (uint8_t)units);
    if (rc != PROS_NTFS_OK)
        return rc == PROS_NTFS_ENOTSUP ? PROS_VFS_ENOSYS :
               (rc == PROS_NTFS_EOVERFLOW ? PROS_VFS_EOVERFLOW :
                rc == PROS_NTFS_EINVAL ? PROS_VFS_EINVAL :
                rc == PROS_NTFS_EIO ? PROS_VFS_EIO : PROS_VFS_EIO);

    node->used = 1u;
    node->record_number = record_number;
    node->data_size = 0u;
    if (vfs_vnode_init(&node->vnode, &fs->mount, parent,
                       PROS_VFS_NODE_FILE, &ntfs_vfs_vnode_ops,
                       &ntfs_vfs_file_ops, node, 0u) != PROS_VFS_OK) {
        node->used = 0u;
        return PROS_VFS_EIO;
    }
    *result = &node->vnode;
    return PROS_VFS_OK;
}

static const struct pros_vfs_vnode_ops ntfs_vfs_vnode_ops = {
    .lookup = ntfs_vfs_lookup,
    .readdir = ntfs_vfs_readdir,
    .create = ntfs_vfs_create
};

static int ntfs_vfs_mount(struct pros_ntfs *fs)
{
    struct pros_ntfs_vfs_node *root;

    if (!fs || !fs->mount.active)
        return PROS_NTFS_EINVAL;

    for (uint32_t i = 0u; i < PROS_NTFS_VFS_NODE_MAX; ++i)
        fs->vfs_nodes[i].used = 0u;

    root = &fs->vfs_nodes[0];
    root->used = 1u;
    root->record_number = 5u;
    root->data_size = 0u;
    if (vfs_vnode_init(&root->vnode, &fs->mount, 0,
                       PROS_VFS_NODE_DIRECTORY, &ntfs_vfs_vnode_ops,
                       0, root, 0u) != PROS_VFS_OK) {
        root->used = 0u;
        return PROS_NTFS_EINVAL;
    }
    return vfs_mount_set_root(&fs->mount, &root->vnode) == PROS_VFS_OK ?
        PROS_NTFS_OK : PROS_NTFS_EINVAL;
}

int ntfs_selftest(void)
{
    static struct ntfs_test_disk disk;
    struct pros_block_device device;
    struct pros_ntfs fs = {0};
    uint64_t lba = 0;
    struct pros_ntfs_run runs[3] = {{0}};
    uint32_t run_count = 0u;
    static uint8_t record[1024];
    static uint8_t attr_record[128];
    const uint8_t runlist[] = { 0x11u, 3u, 4u, 0x11u, 1u, 0xFEu,
                                0x01u, 2u, 0x00u };
    const uint8_t unterminated[] = { 0x21u, 1u, 1u };
    uint32_t attr_offset = 0u;
    const uint8_t *data = 0;
    uint32_t data_size = 0u;

    disk.boot[3]='N'; disk.boot[4]='T'; disk.boot[5]='F'; disk.boot[6]='S';
    for (uint32_t i=7;i<11u;++i) disk.boot[i]=' ';

    for (uint32_t i = 0u; i < sizeof(disk.file_record); ++i)
        disk.file_record[i] = 0u;
    disk.file_record[0]='F'; disk.file_record[1]='I';
    disk.file_record[2]='L'; disk.file_record[3]='E';
    disk.file_record[4]=8u; disk.file_record[6]=3u;
    disk.file_record[8]=0x56u; disk.file_record[9]=0x34u;
    disk.file_record[20]=48u;
    disk.file_record[48]=0x80u;
    disk.file_record[52]=72u;
    disk.file_record[56]=0u;
    disk.file_record[64]=5u; disk.file_record[65]=0u;
    disk.file_record[68]=24u; disk.file_record[69]=0u;
    disk.file_record[72]='H'; disk.file_record[73]='e';
    disk.file_record[74]='l'; disk.file_record[75]='l';
    disk.file_record[76]='o';
    disk.file_record[120]=0xFFu; disk.file_record[121]=0xFFu;
    disk.file_record[122]=0xFFu; disk.file_record[123]=0xFFu;
    disk.file_record[510]=0x56u; disk.file_record[511]=0x34u;
    disk.file_record[1022]=0x56u; disk.file_record[1023]=0x34u;
    disk.boot[11]=0x00; disk.boot[12]=0x02;
    disk.boot[13]=8;
    disk.boot[40]=0x00; disk.boot[41]=0x10;
    disk.boot[48]=4; disk.boot[56]=8;
    disk.boot[64]=0xF6;
    disk.boot[510]=0x55; disk.boot[511]=0xAA;

    for (uint32_t i=0u; i<sizeof(disk.mft_record); ++i) disk.mft_record[i]=0u;
    disk.mft_record[0]='F'; disk.mft_record[1]='I';
    disk.mft_record[2]='L'; disk.mft_record[3]='E';
    disk.mft_record[4]=8u; disk.mft_record[6]=3u;
    disk.mft_record[8]=0x34u; disk.mft_record[9]=0x12u;
    disk.mft_record[10]=0u; disk.mft_record[11]=0u;
    disk.mft_record[12]=0u; disk.mft_record[13]=0u;
    disk.mft_record[510]=0x34u; disk.mft_record[511]=0x12u;
    disk.mft_record[1022]=0x34u; disk.mft_record[1023]=0x12u;
    disk.mft_record[20]=48u;
    disk.mft_record[48]=0x80u;
    disk.mft_record[52]=72u;
    disk.mft_record[56]=1u;
    disk.mft_record[80]=64u;
    disk.mft_record[81]=0u;
    disk.mft_record[88]=0u;
    disk.mft_record[89]=4u;
    disk.mft_record[96]=32u;
    disk.mft_record[100]=0u;
    disk.mft_record[104]=0u;
    disk.mft_record[105]=0u;
    disk.mft_record[106]=0u;
    disk.mft_record[107]=0u;
    disk.mft_record[108]=0u;
    disk.mft_record[109]=0u;
    disk.mft_record[110]=0u;
    disk.mft_record[111]=0u;
    disk.mft_record[112]=0x11u;
    disk.mft_record[113]=4u;
    disk.mft_record[114]=4u;
    disk.mft_record[115]=0x00u;

    for (uint32_t i = 0u; i < sizeof(disk.nonresident_record); ++i)
        disk.nonresident_record[i] = 0u;
    disk.nonresident_record[0]='F'; disk.nonresident_record[1]='I';
    disk.nonresident_record[2]='L'; disk.nonresident_record[3]='E';
    disk.nonresident_record[4]=8u; disk.nonresident_record[6]=3u;
    disk.nonresident_record[8]=0x11u; disk.nonresident_record[9]=0x11u;
    disk.nonresident_record[20]=48u;
    disk.nonresident_record[48]=0x80u;
    disk.nonresident_record[52]=72u;
    disk.nonresident_record[56]=1u;
    disk.nonresident_record[64]=0u;
    disk.nonresident_record[65]=0u;
    disk.nonresident_record[80]=0x40u;
    disk.nonresident_record[88]=0x00u; disk.nonresident_record[89]=0x20u;
    disk.nonresident_record[96]=0x00u; disk.nonresident_record[97]=0x04u;
    disk.nonresident_record[104]=0x00u; disk.nonresident_record[105]=0x04u;
    disk.nonresident_record[112]=0x11u;
    disk.nonresident_record[113]=2u;
    disk.nonresident_record[114]=42u;
    disk.nonresident_record[115]=0u;
    disk.nonresident_record[120]=0xFFu; disk.nonresident_record[121]=0xFFu;
    disk.nonresident_record[122]=0xFFu; disk.nonresident_record[123]=0xFFu;
    disk.nonresident_record[510]=0x11u; disk.nonresident_record[511]=0x11u;
    disk.nonresident_record[1022]=0x11u; disk.nonresident_record[1023]=0x11u;
    for (uint32_t i = 0u; i < sizeof(disk.nonresident_data); ++i)
        disk.nonresident_data[i] = (uint8_t)('a' + (i % 26u));

    /*
     * Synthetic $Bitmap (MFT record 6): 64-byte allocation bitmap covering
     * the 512 test clusters. Its data is stored at LCN 46 (LBA 368).
     */
    for (uint32_t i = 0u; i < sizeof(disk.bitmap_record); ++i)
        disk.bitmap_record[i] = 0u;
    for (uint32_t i = 0u; i < sizeof(disk.bitmap_data); ++i)
        disk.bitmap_data[i] = 0u;
    disk.bitmap_record[0]='F'; disk.bitmap_record[1]='I';
    disk.bitmap_record[2]='L'; disk.bitmap_record[3]='E';
    disk.bitmap_record[4]=8u; disk.bitmap_record[6]=3u;
    disk.bitmap_record[8]=0x21u; disk.bitmap_record[9]=0x43u;
    disk.bitmap_record[20]=48u;
    disk.bitmap_record[48]=0x80u;
    disk.bitmap_record[52]=72u;
    disk.bitmap_record[56]=1u;
    disk.bitmap_record[64]=0u;
    disk.bitmap_record[80]=0x40u;
    disk.bitmap_record[88]=64u;
    disk.bitmap_record[96]=64u;
    disk.bitmap_record[104]=64u;
    disk.bitmap_record[112]=0x11u;
    disk.bitmap_record[113]=1u;
    disk.bitmap_record[114]=46u;
    disk.bitmap_record[115]=0u;
    disk.bitmap_record[120]=0xFFu; disk.bitmap_record[121]=0xFFu;
    disk.bitmap_record[122]=0xFFu; disk.bitmap_record[123]=0xFFu;
    disk.bitmap_record[510]=0x21u; disk.bitmap_record[511]=0x43u;
    disk.bitmap_record[1022]=0x21u; disk.bitmap_record[1023]=0x43u;

    /* Mark clusters used by the synthetic metadata/data mappings. */
    for (uint32_t lcn = 0u; lcn <= 7u; ++lcn)
        disk.bitmap_data[lcn / 8u] |= (uint8_t)(1u << (lcn % 8u));
    for (uint32_t lcn = 10u; lcn <= 43u; ++lcn)
        disk.bitmap_data[lcn / 8u] |= (uint8_t)(1u << (lcn % 8u));
    disk.bitmap_data[46u / 8u] |= (uint8_t)(1u << (46u % 8u));

    for (uint32_t i = 0u; i < sizeof(disk.upcase_record); ++i)
        disk.upcase_record[i] = 0u;
    disk.upcase_record[0]='F'; disk.upcase_record[1]='I';
    disk.upcase_record[2]='L'; disk.upcase_record[3]='E';
    disk.upcase_record[4]=8u; disk.upcase_record[6]=3u;
    disk.upcase_record[8]=0x34u; disk.upcase_record[9]=0x12u;
    disk.upcase_record[20]=24u; disk.upcase_record[24]=0x80u;
    disk.upcase_record[28]=88u; disk.upcase_record[32]=1u;
    disk.upcase_record[48]=31u; disk.upcase_record[56]=64u;
    disk.upcase_record[64]=0x00u; disk.upcase_record[66]=0x02u;
    disk.upcase_record[72]=0x00u; disk.upcase_record[74]=0x02u;
    disk.upcase_record[80]=0x00u; disk.upcase_record[82]=0x02u;
    disk.upcase_record[88]=0x11u; disk.upcase_record[89]=32u;
    disk.upcase_record[90]=10u; disk.upcase_record[92]=0u;
    disk.upcase_record[510]=0x34u; disk.upcase_record[511]=0x12u;
    disk.upcase_record[1022]=0x34u; disk.upcase_record[1023]=0x12u;

    for (uint32_t i = 0u; i < 65536u; ++i) {
        uint16_t folded = (i >= 'a' && i <= 'z') ?
            (uint16_t)(i - ('a' - 'A')) : (uint16_t)i;
        disk.upcase_data[i * 2u] = (uint8_t)folded;
        disk.upcase_data[i * 2u + 1u] = (uint8_t)(folded >> 8);
    }

    if (block_device_init(&device,512,4096,ntfs_test_read,ntfs_test_write,&disk) != PROS_BLOCK_OK ||
        ntfs_mount(&fs,&device) != PROS_NTFS_OK ||
        fs.bytes_per_sector != 512u || fs.sectors_per_cluster != 8u ||
        fs.bytes_per_file_record != 1024u ||
        fs.mft_run_count != 1u || fs.mft_runs[0].lcn != 4u ||
        fs.mft_runs[0].clusters != 4u || fs.mft_runs[0].sparse != 0u ||
        ntfs_cluster_lba(&fs,4u,&lba) != PROS_NTFS_OK || lba != 32u)
        return -101;

    if (ntfs_decode_runlist(runlist, sizeof(runlist), runs, 3u, &run_count) !=
            PROS_NTFS_OK || run_count != 3u ||
        runs[0].lcn != 4u || runs[0].clusters != 3u || runs[0].sparse != 0u ||
        runs[1].lcn != 2u || runs[1].clusters != 1u || runs[1].sparse != 0u ||
        runs[2].lcn != 0u || runs[2].clusters != 2u || runs[2].sparse != 1u)
        return -102;
    if (ntfs_decode_runlist(unterminated, sizeof(unterminated), runs, 3u,
                            &run_count) != PROS_NTFS_ECORRUPT)
        return -103;
    if (ntfs_decode_runlist(runlist, sizeof(runlist), runs, 2u, &run_count) !=
            PROS_NTFS_EOVERFLOW)
        return -104;

    /*
     * Safe NTFS write regression: overwrite an existing resident $DATA
     * stream without changing its length or allocating clusters. The write
     * path must re-seal the MFT record USA sequence before committing it.
     */
    {
        static const uint8_t replacement[] = { 'B', 'y', 'e' };
        static uint8_t verify[8];
        uint32_t written = 0u;
        uint32_t read_back = 0u;

        if (ntfs_file_write(&fs, 11u, 0u, replacement,
                            sizeof(replacement), &written) != PROS_NTFS_OK)
            return -1131;
        if (written != sizeof(replacement))
            return -1132;
        if (ntfs_file_read(&fs, 11u, 0u, verify, sizeof(verify),
                           &read_back) != PROS_NTFS_OK)
            return -1133;
        if (read_back != 5u ||
            verify[0] != 'B' || verify[1] != 'y' || verify[2] != 'e')
            return -1134;
        if (disk.file_record[510u] != disk.file_record[8u] ||
            disk.file_record[511u] != disk.file_record[9u])
            return -1135;
        if (disk.file_record[1022u] != disk.file_record[8u] ||
            disk.file_record[1023u] != disk.file_record[9u])
            return -1136;

        if (ntfs_file_write(&fs, 11u, 0u, replacement,
                            sizeof(replacement), &written) != PROS_NTFS_OK ||
            ntfs_file_write(&fs, 11u, 4u, replacement, 2u, &written) !=
                PROS_NTFS_EOVERFLOW)
            return -121;
    }

    /*
     * Nonresident overwrite regression. The write crosses a 512-byte sector
     * boundary, so both read-modify-write and direct-sector paths are tested.
     */
    {
        static const uint8_t replacement[] = { 'X', 'Y', 'Z', '!' };
        static uint8_t verify[8];
        uint32_t written = 0u;
        uint32_t read_back = 0u;

        if (ntfs_file_write(&fs, 12u, 510u, replacement, 2u,
                            &written) != PROS_NTFS_OK ||
            written != 2u)
            return -1221;
        if (ntfs_file_write(&fs, 12u, 512u, replacement + 2u, 2u,
                            &written) != PROS_NTFS_OK ||
            written != 2u)
            return -1222;
        if (ntfs_file_read(&fs, 12u, 510u, verify, sizeof(replacement),
                           &read_back) != PROS_NTFS_OK)
            return -1223;
        if (read_back != sizeof(replacement) ||
            verify[0] != 'X' || verify[1] != 'Y' ||
            verify[2] != 'Z' || verify[3] != '!')
            return -1224;
        if (disk.nonresident_data[509u] != 'p' ||
            disk.nonresident_data[514u] != 'u')
            return -1225;
    }


    /*
     * Preallocated-growth regression: extend EOF inside the already
     * allocated non-sparse DATA extent. The synthetic file has two 4 KiB
     * clusters allocated (8 KiB total) but only 1 KiB of real data, so the
     * write at 1 KiB must grow EOF without touching $Bitmap. The gap before
     * the write must be zero-filled and both nonresident DATA size fields
     * must move with the new EOF.
     */
    {
        static const uint8_t growth[] = { 'G', 'R', 'O', 'W' };
        static const uint8_t second[] = { 'O', 'K' };
        static uint8_t verify[16];
        uint32_t written = 0u;
        uint32_t read_back = 0u;

        if (ntfs_file_write(&fs, 12u, 1024u, growth,
                            sizeof(growth), &written) != PROS_NTFS_OK ||
            written != sizeof(growth) ||
            le64(disk.nonresident_record + 88u) != 8192u ||
            le64(disk.nonresident_record + 96u) != 1028u ||
            le64(disk.nonresident_record + 104u) != 1028u)
            return -123;
        if (ntfs_file_read(&fs, 12u, 1024u, verify, sizeof(verify),
                           &read_back) != PROS_NTFS_OK ||
            read_back != sizeof(growth) ||
            verify[0] != 'G' || verify[1] != 'R' ||
            verify[2] != 'O' || verify[3] != 'W')
            return -124;

        if (ntfs_file_write(&fs, 12u, 1030u, second,
                            sizeof(second), &written) != PROS_NTFS_OK ||
            written != sizeof(second) ||
            ntfs_file_read(&fs, 12u, 1024u, verify, 8u,
                           &read_back) != PROS_NTFS_OK ||
            read_back != 8u ||
            verify[0] != 'G' || verify[1] != 'R' ||
            verify[2] != 'O' || verify[3] != 'W' ||
            verify[4] != 0u || verify[5] != 0u ||
            verify[6] != 'O' || verify[7] != 'K')
            return -125;
        if (le64(disk.nonresident_record + 88u) != 8192u ||
            le64(disk.nonresident_record + 96u) != 1032u ||
            le64(disk.nonresident_record + 104u) != 1032u)
            return -126;
    }

    /*
     * Fragmented-growth regression: the existing DATA allocation is two
     * clusters at LCN 42-43. Make the preferred next cluster (LCN 44) busy;
     * the allocator must select LCN 45 and append a mapping-pairs run.
     * The new write crosses the two-cluster allocation boundary, requiring
     * exactly one additional cluster.
     */
    {
        static const uint8_t fragmented[] = { 'F', 'R', 'A', 'G' };
        uint32_t written = 0u;

        disk.bitmap_data[44u / 8u] |= (uint8_t)(1u << (44u % 8u));
        {
            int write_rc = ntfs_file_write(&fs, 12u, 8192u, fragmented,
                                           sizeof(fragmented), &written);
            if (write_rc != PROS_NTFS_OK)
                return -1271;
        }
        if (written != sizeof(fragmented))
            return -1272;
        if (le64(disk.nonresident_record + 88u) != 12288u)
            return -1273;
        if (le64(disk.nonresident_record + 96u) != 8196u)
            return -1274;
        if (le64(disk.nonresident_record + 104u) != 8196u)
            return -1275;
        if (disk.nonresident_record[112u] != 0x11u ||
            disk.nonresident_record[113u] != 2u ||
            disk.nonresident_record[114u] != 42u)
            return -1276;
        if (disk.nonresident_record[115u] != 0x11u ||
            disk.nonresident_record[116u] != 1u ||
            disk.nonresident_record[117u] != 3u)
            return -1277;
        if ((disk.bitmap_data[45u / 8u] &
             (uint8_t)(1u << (45u % 8u))) == 0u)
            return -1278;
    }

    /*
     * Write-boundary regression: an overflowing offset+size must be rejected
     * before any allocation or metadata commit, and the byte count must stay
     * zero.
     */
    {
        static const uint8_t one[] = { 'X' };
        uint32_t written = 99u;
        if (ntfs_file_write(&fs, 12u, 0xFFFFFFFFFFFFFFFFull, one,
                            sizeof(one), &written) !=
                PROS_NTFS_EOVERFLOW ||
            written != 0u)
            return -128;
    }


    for (uint32_t i=0u; i<sizeof(record); ++i) record[i]=0u;
    record[0]='F'; record[1]='I'; record[2]='L'; record[3]='E';
    record[4]=32u; record[6]=3u;
    record[32]=0x34u; record[33]=0x12u;
    record[34]=0x78u; record[35]=0x56u;
    record[36]=0x00u; record[37]=0x00u;
    record[510]=0x34u; record[511]=0x12u;
    record[1022]=0x34u; record[1023]=0x12u;
    if (ntfs_file_record_fixup(record, sizeof(record), 512u) != PROS_NTFS_OK ||
        record[510] != 0x78u || record[511] != 0x56u ||
        record[1022] != 0x00u || record[1023] != 0x00u)
        return -105;

    for (uint32_t i=0u; i<sizeof(attr_record); ++i) attr_record[i]=0u;
    attr_record[0]='F'; attr_record[1]='I'; attr_record[2]='L'; attr_record[3]='E';
    attr_record[20]=24u;
    attr_record[24]=0x80u;
    attr_record[28]=48u;
    attr_record[40]=3u;
    attr_record[44]=40u;
    attr_record[64]='a'; attr_record[65]='b'; attr_record[66]='c';
    if (ntfs_find_attribute(attr_record, sizeof(attr_record), 0x80u,
                            &attr_offset) != PROS_NTFS_OK || attr_offset != 24u ||
        ntfs_attribute_data(attr_record, sizeof(attr_record), attr_offset,
                            &data, &data_size) != PROS_NTFS_OK ||
        data_size != 3u || data[0] != 'a' || data[1] != 'b' || data[2] != 'c')
        return -106;


    /*
     * Directory metadata regression:
     * $FILE_NAME is resident and carries the Unicode name used by the
     * directory index. $INDEX_ROOT is resident and contains aligned index
     * entries whose keys are $FILE_NAME bodies.
     */
    {
        uint8_t directory_record[320] = {0};
        uint32_t file_name_offset = 24u;
        uint32_t index_root_offset = 128u;
        uint32_t file_name_value_size = 74u; /* 66-byte header + "test" */
        uint32_t index_value_size = 144u;
        struct pros_ntfs_file_name name_info;
        struct pros_ntfs_index_root index_info;
        uint64_t reference;
        const uint8_t *key;
        uint16_t key_size;
        uint8_t entry_flags;
        uint32_t next_entry;
        uint32_t i;

        directory_record[0] = 'F'; directory_record[1] = 'I';
        directory_record[2] = 'L'; directory_record[3] = 'E';
        directory_record[20] = 24u;

        /* Resident $FILE_NAME attribute. */
        directory_record[file_name_offset + 0u] = 0x30u;
        directory_record[file_name_offset + 4u] = 98u;
        directory_record[file_name_offset + 16u] = (uint8_t)file_name_value_size;
        directory_record[file_name_offset + 17u] = 0u;
        directory_record[file_name_offset + 20u] = 24u;
        directory_record[file_name_offset + 24u + 0u] = 5u;
        directory_record[file_name_offset + 24u + 1u] = 0u;
        directory_record[file_name_offset + 24u + 40u] = 0x00u;
        directory_record[file_name_offset + 24u + 41u] = 0x10u;
        directory_record[file_name_offset + 24u + 48u] = 0x00u;
        directory_record[file_name_offset + 24u + 49u] = 0x10u;
        directory_record[file_name_offset + 24u + 56u] = 0x00u;
        directory_record[file_name_offset + 24u + 57u] = 0x10u;
        directory_record[file_name_offset + 24u + 64u] = 4u;
        directory_record[file_name_offset + 24u + 65u] = 1u;
        directory_record[file_name_offset + 24u + 66u] = 't';
        directory_record[file_name_offset + 24u + 67u] = 0u;
        directory_record[file_name_offset + 24u + 68u] = 'e';
        directory_record[file_name_offset + 24u + 69u] = 0u;
        directory_record[file_name_offset + 24u + 70u] = 's';
        directory_record[file_name_offset + 24u + 71u] = 0u;
        directory_record[file_name_offset + 24u + 72u] = 't';
        directory_record[file_name_offset + 24u + 73u] = 0u;

        if (ntfs_file_name_attribute(directory_record, sizeof(directory_record),
                                     file_name_offset, &name_info) !=
                PROS_NTFS_OK ||
            name_info.parent_reference != 5u ||
            name_info.allocated_size != 0x1000u ||
            name_info.data_size != 0x1000u ||
            name_info.name_length != 4u ||
            name_info.namespace_id != 1u ||
            le16(name_info.name_utf16le) != 't' ||
            le16(name_info.name_utf16le + 2u) != 'e' ||
            le16(name_info.name_utf16le + 6u) != 't')
            return -107;

        /* Resident $INDEX_ROOT with one filename entry and a terminator. */
        directory_record[index_root_offset + 0u] = 0x90u;
        directory_record[index_root_offset + 4u] = 168u;
        directory_record[index_root_offset + 16u] = (uint8_t)index_value_size;
        directory_record[index_root_offset + 17u] = 0u;
        directory_record[index_root_offset + 20u] = 24u;

        {
            uint8_t *v = directory_record + index_root_offset + 24u;
            v[0] = 0x30u; /* indexed attribute: $FILE_NAME */
            v[4] = 1u;    /* filename collation */
            v[8] = 0x00u; v[9] = 0x10u;
            v[12] = 1u;   /* clusters per index record */
            v[16] = 32u; v[20] = 144u; v[24] = 144u;
            v[28] = 0u;
            for (i = 0u; i < 74u; ++i)
                v[32u + 16u + i] =
                    directory_record[file_name_offset + 24u + i];
            v[32u + 0u] = 0x00u; v[32u + 1u] = 0x10u;
            v[32u + 8u] = 96u;
            v[32u + 10u] = 74u;
            v[32u + 12u] = 0u;
            v[128u + 8u] = 16u;
            v[128u + 10u] = 0u;
            v[128u + 12u] = 2u;
        }

        if (ntfs_index_root_header(directory_record, sizeof(directory_record),
                                   index_root_offset, &index_info) !=
                PROS_NTFS_OK ||
            index_info.first_entry_offset != 32u ||
            index_info.entries_size != 144u ||
            index_info.allocated_size != 144u ||
            index_info.flags != 0u)
            return -108;

        if (ntfs_index_root_entry(directory_record, sizeof(directory_record),
                                  index_root_offset, 32u, &reference, &key,
                                  &key_size, &entry_flags, &next_entry) !=
                PROS_NTFS_OK ||
            reference != 0x1000u || key_size != 74u || entry_flags != 0u ||
            next_entry != 128u ||
            le16(key + 66u) != 't' || le16(key + 72u) != 't')
            return -109;

        if (ntfs_index_root_entry(directory_record, sizeof(directory_record),
                                  index_root_offset, 128u, &reference, &key,
                                  &key_size, &entry_flags, &next_entry) !=
                PROS_NTFS_OK ||
            entry_flags != 2u || key_size != 0u || next_entry != 0u)
            return -110;

        if (ntfs_index_root_entry(directory_record, sizeof(directory_record),
                                  index_root_offset, 129u, &reference, &key,
                                  &key_size, &entry_flags, &next_entry) !=
                PROS_NTFS_ECORRUPT)
            return -111;

        {
            static const uint8_t lookup_name[] = {
                't', 0u, 'e', 0u, 's', 0u, 't', 0u
            };
            if (ntfs_index_root_lookup(directory_record, sizeof(directory_record),
                                       index_root_offset, lookup_name, 4u,
                                       &reference) != PROS_NTFS_OK ||
                reference != 0x1000u)
                return -112;

        }
    }


    /*
     * INDEX_ALLOCATION regression:
     * the resident root points to VCN 0, and the child INDX leaf contains
     * the same exact UTF-16LE filename. This exercises runlist mapping,
     * INDX USA validation, VCN validation, child traversal and lookup.
     */
    {
        uint8_t lookup_record[384] = {0};
        uint8_t *root_attr = lookup_record + 24u;
        uint8_t *alloc_attr = lookup_record + 200u;
        static const uint8_t lookup_name[] = {
            't', 0u, 'e', 0u, 's', 0u, 't', 0u
        };
        uint8_t file_name_key[74] = {0};
        uint64_t child_reference = 0u;
        file_name_key[64u] = 4u;
        file_name_key[65u] = 1u;
        file_name_key[66u] = 't'; file_name_key[67u] = 0u;
        file_name_key[68u] = 'e'; file_name_key[69u] = 0u;
        file_name_key[70u] = 's'; file_name_key[71u] = 0u;
        file_name_key[72u] = 't'; file_name_key[73u] = 0u;
        struct pros_ntfs_index_record index_info;

        lookup_record[0] = 'F'; lookup_record[1] = 'I';
        lookup_record[2] = 'L'; lookup_record[3] = 'E';
        lookup_record[20] = 24u;

        /* Resident $INDEX_ROOT: 4 KiB child records, one VCN-0 child. */
        root_attr[0] = 0x90u;
        root_attr[4] = 176u;
        root_attr[16] = 152u;
        root_attr[20] = 24u;
        root_attr[24u + 8u] = 0x00u;
        root_attr[24u + 9u] = 0x10u;
        root_attr[24u + 12u] = 1u;
        root_attr[24u + 16u] = 32u;
        root_attr[24u + 20u] = 152u;
        root_attr[24u + 24u] = 152u;

        {
            uint32_t entry = 24u + 32u;
            uint32_t terminal = entry + 104u;
            root_attr[entry + 0u] = 0x00u;
            root_attr[entry + 1u] = 0x20u;
            root_attr[entry + 8u] = 104u;
            root_attr[entry + 10u] = 74u;
            root_attr[entry + 12u] = 1u;
            for (uint32_t i = 0u; i < 74u; ++i)
                root_attr[entry + 16u + i] =
                    file_name_key[i];
            /* Root separator key differs from the child target name. */
            root_attr[entry + 16u + 66u] = 'r';
            root_attr[entry + 16u + 68u] = 'o';
            root_attr[entry + 16u + 70u] = 'o';
            root_attr[entry + 16u + 72u] = 't';
            /* Child VCN 0 occupies the final eight bytes. */
            root_attr[entry + 104u - 8u] = 0u;

            root_attr[terminal + 8u] = 16u;
            root_attr[terminal + 10u] = 0u;
            root_attr[terminal + 12u] = 2u;
        }

        /* Nonresident $INDEX_ALLOCATION: one cluster at LCN 8 (LBA 64). */
        alloc_attr[0] = 0xA0u;
        alloc_attr[4] = 68u;
        alloc_attr[8] = 1u;
        alloc_attr[32u] = 64u;
        alloc_attr[40u] = 0x00u;
        alloc_attr[41u] = 0x10u;
        alloc_attr[48u] = 0x00u;
        alloc_attr[49u] = 0x10u;
        alloc_attr[56u] = 0x00u;
        alloc_attr[57u] = 0x10u;
        alloc_attr[64u] = 0x11u;
        alloc_attr[65u] = 1u;
        alloc_attr[66u] = 8u;
        alloc_attr[67u] = 0u;

        /* 4096-byte INDX page at VCN 0. */
        for (uint32_t i = 0u; i < sizeof(disk.index_record); ++i)
            disk.index_record[i] = 0u;
        disk.index_record[0] = 'I'; disk.index_record[1] = 'N';
        disk.index_record[2] = 'D'; disk.index_record[3] = 'X';
        disk.index_record[4] = 40u;
        disk.index_record[6] = 9u;
        disk.index_record[40u] = 0x34u;
        disk.index_record[41u] = 0x12u;
        for (uint32_t i = 0u; i < 8u; ++i) {
            disk.index_record[42u + i * 2u] = (uint8_t)(0xA0u + i);
            disk.index_record[43u + i * 2u] = 0u;
            disk.index_record[510u + i * 512u] = 0x34u;
            disk.index_record[511u + i * 512u] = 0x12u;
        }
        disk.index_record[16u] = 0u;
        disk.index_record[24u] = 40u;
        disk.index_record[28u] = 152u;
        disk.index_record[32u] = 152u;
        disk.index_record[36u] = 0u;

        {
            uint32_t entry = 64u;
            uint32_t terminal = entry + 96u;
            disk.index_record[entry + 0u] = 0x00u;
            disk.index_record[entry + 1u] = 0x30u;
            disk.index_record[entry + 8u] = 96u;
            disk.index_record[entry + 10u] = 74u;
            disk.index_record[entry + 12u] = 0u;
            for (uint32_t i = 0u; i < 74u; ++i)
                disk.index_record[entry + 16u + i] =
                    file_name_key[i];
            disk.index_record[terminal + 8u] = 16u;
            disk.index_record[terminal + 10u] = 0u;
            disk.index_record[terminal + 12u] = 2u;
        }

        {
            struct pros_ntfs_run index_runs[2] = {{0}};
            uint32_t index_run_count = 0u;
            static uint8_t read_index_record[4096];
            const uint8_t *index_runlist_data = 0;
            uint32_t index_runlist_size = 0u;

            if (ntfs_attribute_runlist(lookup_record, sizeof(lookup_record),
                                        200u, &index_runlist_data,
                                        &index_runlist_size) !=
                    PROS_NTFS_OK)
                return -116;
            if (ntfs_decode_runlist(index_runlist_data, index_runlist_size, index_runs, 2u,
                                    &index_run_count) != PROS_NTFS_OK ||
                index_run_count != 1u || index_runs[0].lcn != 8u ||
                index_runs[0].clusters != 1u)
                return -117;
            if (ntfs_index_allocation_read(&fs, index_runs, index_run_count,
                                           0u, read_index_record,
                                           sizeof(read_index_record)) !=
                    PROS_NTFS_OK)
                return -118;
            if (ntfs_index_record_header(read_index_record,
                                         sizeof(read_index_record), 512u, 0u,
                                         &index_info) != PROS_NTFS_OK)
                return -119;

            if (ntfs_index_allocation_lookup(
                    &fs, lookup_record, sizeof(lookup_record), 24u, 200u,
                    lookup_name, 4u, &child_reference) != PROS_NTFS_OK)
                return -114;
            if (child_reference != 0x3000u)
                return -115;
        }

        /* USA corruption must reject the child node rather than guess. */
        disk.index_record[510u] = 0xFFu;
        if (ntfs_index_record_header(disk.index_record,
                                     sizeof(disk.index_record), 512u, 0u,
                                     &index_info) != PROS_NTFS_ECORRUPT)
            return -120;
    }

    /*
     * Directory/path regression: MFT record 5 ($Root) contains a resident
     * $INDEX_ROOT entry, so the public path API must begin at record 5 and
     * return the entry's MFT reference.
     */
    {
        /*
         * ntfs_index_root_lookup() is the low-level record-only primitive;
         * case folding is supplied by ntfs_directory_lookup(), which loads
         * the volume UpCase table. Keep this direct probe case-exact, then
         * exercise case-insensitive lookup through the public directory/path
         * APIs below.
         */
        static const uint8_t path_name[] = {
            'T', 0u, 'e', 0u, 'S', 0u, 't', 0u
        };
        static const uint8_t folded_path_name[] = {
            't', 0u, 'e', 0u, 's', 0u, 't', 0u
        };
        uint8_t path_key[74] = {0};
        const uint8_t *components[] = { folded_path_name };
        path_key[64u] = 4u;
        path_key[65u] = 1u;
        path_key[66u] = 'T'; path_key[67u] = 0u;
        path_key[68u] = 'e'; path_key[69u] = 0u;
        path_key[70u] = 'S'; path_key[71u] = 0u;
        path_key[72u] = 't'; path_key[73u] = 0u;
        const uint8_t lengths[] = { 4u };
        uint64_t path_reference = 0u;

        for (uint32_t i = 0u; i < sizeof(disk.root_record); ++i)
            disk.root_record[i] = 0u;
        disk.root_record[0] = 'F'; disk.root_record[1] = 'I';
        disk.root_record[2] = 'L'; disk.root_record[3] = 'E';
        disk.root_record[4] = 8u; disk.root_record[6] = 3u;
        disk.root_record[8] = 0x34u; disk.root_record[9] = 0x12u;
        disk.root_record[20] = 24u;
        disk.root_record[24u] = 0x90u;
        disk.root_record[28u] = 168u;
        disk.root_record[40u] = 144u;
        disk.root_record[44u] = 24u;
        disk.root_record[48u] = 0x30u;
        disk.root_record[52u] = 1u;
        disk.root_record[56u] = 0x00u;
        disk.root_record[57u] = 0x10u;
        disk.root_record[60u] = 1u;
        disk.root_record[64u] = 32u;
        disk.root_record[68u] = 144u;
        disk.root_record[72u] = 144u;
        disk.root_record[80u] = 0x00u;
        disk.root_record[81u] = 0x20u;
        disk.root_record[88u] = 96u;
        disk.root_record[90u] = 74u;
        disk.root_record[92u] = 0u;
        for (uint32_t i = 0u; i < 74u; ++i)
            disk.root_record[96u + i] = path_key[i];
        /*
         * The index END entry starts at the first-entry offset (80) plus
         * the first entry's 96-byte length: absolute record offset 176.
         * Keep its header fields at +8/+10/+12.  The MFT attribute list then
         * ends at offset 192 with the mandatory 0xFFFFFFFF terminator;
         * without that terminator, a later INDEX_ALLOCATION probe treats
         * the zero-filled bytes as a malformed attribute.
         */
        disk.root_record[184u] = 16u;
        disk.root_record[186u] = 0u;
        disk.root_record[188u] = 2u;
        disk.root_record[192u] = 0xFFu;
        disk.root_record[193u] = 0xFFu;
        disk.root_record[194u] = 0xFFu;
        disk.root_record[195u] = 0xFFu;
        disk.root_record[510u] = 0x34u; disk.root_record[511u] = 0x12u;
        disk.root_record[1022u] = 0x34u; disk.root_record[1023u] = 0x12u;

        {
            uint8_t root_probe[1024];
            uint32_t root_attr_probe = 0u;
            for (uint32_t i = 0u; i < sizeof(root_probe); ++i)
                root_probe[i] = disk.root_record[i];
            if (ntfs_file_record_fixup(root_probe, sizeof(root_probe), 512u) !=
                    PROS_NTFS_OK)
                return -180;
            if (ntfs_find_attribute(root_probe, sizeof(root_probe), 0x90u,
                                     &root_attr_probe) != PROS_NTFS_OK)
                return -181;
            if (ntfs_index_root_lookup(root_probe, sizeof(root_probe),
                                       root_attr_probe, path_name, 4u,
                                       &path_reference) != PROS_NTFS_OK)
                return -182;

            {
                int directory_status = ntfs_directory_lookup(
                    &fs, 5u, path_name, 4u, &path_reference);
                if (directory_status != PROS_NTFS_OK)
                    return -140 + directory_status;
            if (path_reference != 0x2000u)
                return -150;
            {
                int path_status = ntfs_path_lookup(
                    &fs, components, lengths, 1u, &path_reference);
                if (path_status != PROS_NTFS_OK)
                    return -160 + path_status;
                if (path_reference != 0x2000u)
                    return -170;
            }
            }
        }

        /* DOS-only aliases must not satisfy a normal path lookup. */
        disk.root_record[96u + 65u] = 2u;
        if (ntfs_path_lookup(&fs, components, lengths, 1u,
                             &path_reference) != PROS_NTFS_ENOTSUP)
            return -183;
        disk.root_record[96u + 65u] = 1u;

        {
            uint64_t allocated_lcn = 0u;
            if (ntfs_bitmap_allocate_clusters(&fs, 2u, &allocated_lcn) !=
                    PROS_NTFS_OK ||
                allocated_lcn != 8u ||
                (disk.bitmap_data[1u] & 0x03u) != 0x03u)
                return -194;
        }

        {
            uint8_t file_data[8] = {0};
            uint32_t file_bytes = 0u;
            if (ntfs_file_read(&fs, 11u, 1u, file_data, 3u,
                               &file_bytes) != PROS_NTFS_OK ||
                file_bytes != 3u ||
                file_data[0] != 'e' || file_data[1] != 'l' ||
                file_data[2] != 'l')
                return -184;
            if (ntfs_file_read(&fs, 11u, 99u, file_data, 1u,
                               &file_bytes) != PROS_NTFS_EINVAL)
                return -185;
        }

        /*
         * End-to-end VFS regression: expose the synthetic root entry as
         * record 11 and give that record a resident $FILE_NAME. The VFS
         * must resolve /test through NTFS and stream the same "Hello" data.
         */
        disk.root_record[80u] = 11u;
        disk.root_record[81u] = 0u;
        disk.root_record[82u] = 0u;
        disk.root_record[83u] = 0u;
        disk.root_record[84u] = 0u;
        disk.root_record[85u] = 0u;
        disk.root_record[86u] = 0u;
        disk.root_record[87u] = 0u;

        disk.file_record[120u] = 0xFFu;
        disk.file_record[121u] = 0xFFu;
        disk.file_record[122u] = 0xFFu;
        disk.file_record[123u] = 0xFFu;
        {
            uint32_t fn = 128u;
            uint32_t value = fn + 24u;
            disk.file_record[fn + 0u] = 0x30u;
            disk.file_record[fn + 4u] = 128u;
            disk.file_record[fn + 16u] = 74u;
            disk.file_record[fn + 20u] = 24u;
            disk.file_record[value + 0u] = 5u;
            disk.file_record[value + 48u] = 5u;
            disk.file_record[value + 56u] = 0u;
            disk.file_record[value + 64u] = 4u;
            disk.file_record[value + 65u] = 1u;
            disk.file_record[value + 66u] = 't';
            disk.file_record[value + 68u] = 'e';
            disk.file_record[value + 70u] = 's';
            disk.file_record[value + 72u] = 't';
        }
        disk.file_record[256u] = 0xFFu;
        disk.file_record[257u] = 0xFFu;
        disk.file_record[258u] = 0xFFu;
        disk.file_record[259u] = 0xFFu;

        {
            struct pros_vfs_vnode *node = 0;
            struct pros_vfs_file file = {0};
            uint8_t data[8] = {0};
            uint32_t got = 0u;

            if (vfs_path_lookup(&fs.mount, "/test", &node) != PROS_VFS_OK ||
                !node || node->type != PROS_VFS_NODE_FILE ||
                node->size != 5u)
                return -186;
            if (vfs_file_open(&fs.mount, "/test", PROS_VFS_O_READ, &file) !=
                    PROS_VFS_OK)
                return -187;
            if (vfs_file_read(&file, data, 5u, &got) != PROS_VFS_OK ||
                got != 5u || data[0] != 'B' || data[1] != 'y' ||
                data[2] != 'e' || data[3] != 'l' || data[4] != 'o')
                return -188;
            if (vfs_file_read(&file, data, 1u, &got) != PROS_VFS_OK ||
                got != 0u)
                return -189;
            if (vfs_file_seek(&file, 1u) != PROS_VFS_OK ||
                vfs_file_read(&file, data, 2u, &got) != PROS_VFS_OK ||
                got != 2u || data[0] != 'y' || data[1] != 'e')
                return -190;
            if (vfs_file_close(&file) != PROS_VFS_OK ||
                vfs_file_open(&fs.mount, "/test",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
                return -191;
            {
                static const uint8_t replacement[] = { 'O', 'K' };
                uint32_t written = 0u;
                if (vfs_file_write(&file, replacement,
                                   sizeof(replacement), &written) !=
                        PROS_VFS_OK ||
                    written != sizeof(replacement) ||
                    vfs_file_close(&file) != PROS_VFS_OK)
                    return -192;
            }
            if (vfs_file_open(&fs.mount, "/test",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, data, 3u, &got) != PROS_VFS_OK ||
                got != 3u || data[0] != 'O' || data[1] != 'K' ||
                data[2] != 'e' ||
                vfs_file_close(&file) != PROS_VFS_OK ||
                vfs_vnode_put(node) != PROS_VFS_OK)
                return -193;
        }

        /* End-to-end VFS directory enumeration regression. */
        {
            uint64_t cookie = 0u;
            struct pros_vfs_dirent entry = {0};

            /* Extend the synthetic MFT data-size so record 11 is within the
             * range that readdir scans. */
            disk.mft_record[96u] = 0x00u;
            disk.mft_record[97u] = 0x30u;
            disk.mft_record[98u] = 0x00u;
            disk.mft_record[99u] = 0x00u;

            if (!fs.mount.root || !fs.mount.root->ops ||
                !fs.mount.root->ops->readdir)
                return -195;
            if (fs.mount.root->ops->readdir(fs.mount.root, &cookie, &entry) !=
                    PROS_VFS_OK || cookie != 12u ||
                entry.type != PROS_VFS_NODE_FILE || entry.size != 5u ||
                entry.name[0] != 't' || entry.name[1] != 'e' ||
                entry.name[2] != 's' || entry.name[3] != 't' ||
                entry.name[4] != '\0')
                return -196;
            if (fs.mount.root->ops->readdir(fs.mount.root, &cookie, &entry) !=
                    PROS_VFS_ENOENT || cookie != 12u)
                return -197;
        }

        /*
         * VFS UTF-8 decoder regression: malformed sequences must be
         * rejected before they reach the NTFS UTF-16LE lookup layer, while
         * valid multi-byte sequences must be accepted by the decoder.
         */
        {
            static const char valid_utf8_3[] = "\xE2\x82\xAC";
            static const char valid_utf8_4[] = "\xF0\x9F\x92\xA9";
            static const char bad_continuation[] = "\xE2\x28\xA1";
            static const char truncated[] = "\xF0\x9F\x92";
            static const char overlong[] = "\xC0\xAF";
            static const char surrogate[] = "\xED\xA0\x80";
            static const char out_of_range[] = "\xF4\x90\x80\x80";
            struct pros_vfs_vnode *unicode_node = 0;

            if (!fs.mount.root || !fs.mount.root->ops ||
                !fs.mount.root->ops->lookup)
                return -198;

            if (fs.mount.root->ops->lookup(fs.mount.root, valid_utf8_3,
                                           &unicode_node) != PROS_VFS_ENOENT ||
                fs.mount.root->ops->lookup(fs.mount.root, valid_utf8_4,
                                           &unicode_node) != PROS_VFS_ENOENT)
                return -199;

            if (fs.mount.root->ops->lookup(fs.mount.root, bad_continuation,
                                           &unicode_node) != PROS_VFS_EINVAL ||
                fs.mount.root->ops->lookup(fs.mount.root, truncated,
                                           &unicode_node) != PROS_VFS_EINVAL ||
                fs.mount.root->ops->lookup(fs.mount.root, overlong,
                                           &unicode_node) != PROS_VFS_EINVAL ||
                fs.mount.root->ops->lookup(fs.mount.root, surrogate,
                                           &unicode_node) != PROS_VFS_EINVAL ||
                fs.mount.root->ops->lookup(fs.mount.root, out_of_range,
                                           &unicode_node) != PROS_VFS_EINVAL)
                return -200;
        }

        if (ntfs_path_lookup(&fs, components, lengths, 0u,
                             &path_reference) != PROS_NTFS_EINVAL)
            return -122;
    }

    /*
     * Attribute-order regression: an ADS may precede the unnamed/default
     * $DATA stream. Default file I/O must select the unnamed stream rather
     * than treating the first named stream as the file contents.
     */
    {
        uint8_t stream_record[128] = {0};
        uint32_t stream_attr = 0u;
        const uint8_t *stream_data = 0;
        uint32_t stream_size = 0u;

        stream_record[0] = 'F'; stream_record[1] = 'I';
        stream_record[2] = 'L'; stream_record[3] = 'E';
        stream_record[20] = 24u;

        /* Named DATA stream: name length 3, no resident value. */
        stream_record[24u] = 0x80u;
        stream_record[28u] = 40u;
        stream_record[33u] = 3u;
        stream_record[34u] = 24u;
        stream_record[64u] = 0x80u;
        stream_record[68u] = 32u;
        stream_record[80u] = 3u;
        stream_record[84u] = 24u;
        stream_record[88u] = 'O';
        stream_record[89u] = 'K';
        stream_record[92u] = 0xFFu;
        stream_record[93u] = 0xFFu;
        stream_record[94u] = 0xFFu;
        stream_record[95u] = 0xFFu;

        if (ntfs_find_unnamed_attribute(stream_record, sizeof(stream_record),
                                        0x80u, &stream_attr) !=
                PROS_NTFS_OK || stream_attr != 64u ||
            ntfs_attribute_data(stream_record, sizeof(stream_record),
                                stream_attr, &stream_data,
                                &stream_size) != PROS_NTFS_OK ||
            stream_size != 3u || stream_data[0] != 'O' ||
            stream_data[1] != 'K' || stream_data[2] != 0u)
            return -201;
    }

    return 0;
}