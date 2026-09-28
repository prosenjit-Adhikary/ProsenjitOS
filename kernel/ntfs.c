#include "ntfs.h"

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

    if (vfs_mount_init(&fs->mount, PROS_FS_NTFS, device, fs, 1u) !=
        PROS_VFS_OK)
        return PROS_NTFS_EINVAL;

    fs->bytes_per_sector = sector_size;
    fs->sectors_per_cluster = boot[13];
    fs->total_sectors = le64(&boot[40]);
    fs->mft_lcn = le64(&boot[48]);
    fs->mftmirr_lcn = le64(&boot[56]);

    if (vfs_mount_activate(&fs->mount) != PROS_VFS_OK)
        return PROS_NTFS_EINVAL;
    return ntfs_bootstrap_mft(fs);
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
    if (attr_len < 64u || attr_offset + attr_len > record_size)
        return PROS_NTFS_ECORRUPT;
    if (a[attr_offset + 8u] == 0u)
        return PROS_NTFS_ENOTSUP;
    run_offset = le16(a + attr_offset + 32u);
    allocated = le64(a + attr_offset + 40u);
    if ((uint32_t)run_offset >= attr_len || allocated == 0u)
        return PROS_NTFS_ECORRUPT;
    *runlist = a + attr_offset + run_offset;
    *runlist_size = attr_len - run_offset;
    return PROS_NTFS_OK;
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


#define PROS_NTFS_RECORD_WORK_MAX 65536u

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

struct ntfs_test_disk { uint8_t boot[512]; uint8_t mft_record[1024]; };

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
    }
    return PROS_BLOCK_OK;
}

int ntfs_selftest(void)
{
    struct ntfs_test_disk disk = {0};
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

    if (block_device_init(&device,512,4096,ntfs_test_read,0,&disk) != PROS_BLOCK_OK ||
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
                PROS_NTFS_EINVAL)
            return -111;
    }

    return 0;
}
