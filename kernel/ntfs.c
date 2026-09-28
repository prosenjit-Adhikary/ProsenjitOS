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

    return vfs_mount_activate(&fs->mount) == PROS_VFS_OK
        ? PROS_NTFS_OK : PROS_NTFS_EINVAL;
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
    uint64_t byte_offset, lcn, cluster_offset, lba;
    uint32_t count, offset, first;
    if (!fs || !buffer || buffer_size < fs->bytes_per_file_record ||
        fs->bytes_per_file_record == 0u)
        return PROS_NTFS_EINVAL;
    if (record_number > 0x0000FFFFFFFFFFFFull)
        return PROS_NTFS_EINVAL;
    byte_offset = record_number * (uint64_t)fs->bytes_per_file_record;
    lcn = fs->mft_lcn + byte_offset / ((uint64_t)fs->bytes_per_sector *
                                       fs->sectors_per_cluster);
    cluster_offset = byte_offset % ((uint64_t)fs->bytes_per_sector *
                                    fs->sectors_per_cluster);
    if (ntfs_cluster_lba(fs, lcn, &lba) != PROS_NTFS_OK)
        return PROS_NTFS_EIO;
    offset = (uint32_t)cluster_offset;
    count = fs->bytes_per_file_record;
    first = fs->sectors_per_cluster - (offset / fs->bytes_per_sector);
    if (count + offset > first * fs->bytes_per_sector) {
        uint8_t sector[4096];
        uint32_t copied = 0u;
        while (copied < count) {
            uint64_t current_lba = lba + (offset + copied) / fs->bytes_per_sector;
            uint32_t in_sector = (offset + copied) % fs->bytes_per_sector;
            uint32_t take = fs->bytes_per_sector - in_sector;
            if (take > count - copied) take = count - copied;
            if (block_device_read(fs->mount.device, current_lba, 1u, sector) != PROS_BLOCK_OK)
                return PROS_NTFS_EIO;
            for (uint32_t i=0;i<take;++i) ((uint8_t *)buffer)[copied+i]=sector[in_sector+i];
            copied += take;
        }
    } else {
        uint32_t sectors = (count + offset + fs->bytes_per_sector - 1u) /
                           fs->bytes_per_sector;
        uint8_t sector[4096];
        if (sectors > 8u) return PROS_NTFS_EINVAL;
        if (block_device_read(fs->mount.device, lba, sectors, sector) != PROS_BLOCK_OK)
            return PROS_NTFS_EIO;
        for (uint32_t i=0;i<count;++i) ((uint8_t *)buffer)[i]=sector[offset+i];
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

struct ntfs_test_disk { uint8_t boot[512]; };

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
    }
    return PROS_BLOCK_OK;
}

int ntfs_selftest(void)
{
    struct ntfs_test_disk disk = {{0}};
    struct pros_block_device device;
    struct pros_ntfs fs = {0};
    uint64_t lba = 0;
    struct pros_ntfs_run runs[3] = {{0}};
    uint32_t run_count = 0u;
    static uint8_t record[1024];
    static uint8_t attr_record[128];
    const uint8_t runlist[] = { 0x21u, 3u, 4u, 0x11u, 1u, 0xFEu,
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

    if (block_device_init(&device,512,4096,ntfs_test_read,0,&disk) != PROS_BLOCK_OK ||
        ntfs_mount(&fs,&device) != PROS_NTFS_OK ||
        fs.bytes_per_sector != 512u || fs.sectors_per_cluster != 8u ||
        fs.bytes_per_file_record != 1024u ||
        ntfs_cluster_lba(&fs,4u,&lba) != PROS_NTFS_OK || lba != 32u)
        return -1;

    if (ntfs_decode_runlist(runlist, sizeof(runlist), runs, 3u, &run_count) !=
            PROS_NTFS_OK || run_count != 3u ||
        runs[0].lcn != 4u || runs[0].clusters != 3u || runs[0].sparse != 0u ||
        runs[1].lcn != 2u || runs[1].clusters != 1u || runs[1].sparse != 0u ||
        runs[2].lcn != 0u || runs[2].clusters != 2u || runs[2].sparse != 1u)
        return -2;
    if (ntfs_decode_runlist(unterminated, sizeof(unterminated), runs, 3u,
                            &run_count) != PROS_NTFS_ECORRUPT)
        return -3;
    if (ntfs_decode_runlist(runlist, sizeof(runlist), runs, 2u, &run_count) !=
            PROS_NTFS_EOVERFLOW)
        return -4;

    for (uint32_t i=0u; i<sizeof(record); ++i) record[i]=0u;
    record[0]='F'; record[1]='I'; record[2]='L'; record[3]='E';
    record[4]=48u; record[6]=3u;
    record[48]=0x34u; record[49]=0x12u;
    record[50]=0x78u; record[51]=0x56u;
    record[52]=0x00u; record[53]=0x00u;
    record[510]=0x34u; record[511]=0x12u;
    record[1022]=0x34u; record[1023]=0x12u;
    if (ntfs_file_record_fixup(record, sizeof(record), 512u) != PROS_NTFS_OK ||
        record[510] != 0x78u || record[511] != 0x56u ||
        record[1022] != 0x00u || record[1023] != 0x00u)
        return -5;

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
        return -6;

    return 0;
}
