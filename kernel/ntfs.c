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
        boot[9] != ' ' || boot[10] != ' ' || boot[11] != ' ' ||
        boot[12] != ' ' || boot[13] != ' ' || boot[14] != ' ' ||
        boot[15] != ' ')
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
    struct pros_ntfs fs;
    uint64_t lba = 0;

    disk.boot[3]='N'; disk.boot[4]='T'; disk.boot[5]='F'; disk.boot[6]='S';
    for (uint32_t i=7;i<11u;++i) disk.boot[i]=' ';
    disk.boot[11]=0x00; disk.boot[12]=0x02;
    disk.boot[13]=8; /* 4 KiB cluster */
    disk.boot[40]=0x00; disk.boot[41]=0x10; /* 4096 sectors */
    disk.boot[48]=4; disk.boot[56]=8;
    disk.boot[64]=0xF6; /* 1024-byte file record */
    disk.boot[510]=0x55; disk.boot[511]=0xAA;

    if (block_device_init(&device,512,4096,ntfs_test_read,0,&disk) != PROS_BLOCK_OK ||
        ntfs_mount(&fs,&device) != PROS_NTFS_OK ||
        fs.bytes_per_sector != 512u || fs.sectors_per_cluster != 8u ||
        fs.bytes_per_file_record != 1024u ||
        ntfs_cluster_lba(&fs,4u,&lba) != PROS_NTFS_OK || lba != 32u)
        return -1;
    return 0;
}
