#include "exfat.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

int exfat_mount(struct pros_exfat *fs, struct pros_block_device *device)
{
    uint8_t boot[4096];
    uint64_t volume_length, fat_end, heap_end;
    uint32_t bytes_per_sector, sectors_per_cluster;
    uint32_t fat_offset, fat_length, heap_offset, cluster_count, root_cluster;
    uint8_t sector_shift, cluster_shift, fat_count;

    if (!fs || !device || device->sector_size < 512u ||
        device->sector_size > sizeof(boot))
        return PROS_EXFAT_EINVAL;
    if (block_device_read(device, 0, 1, boot) != PROS_BLOCK_OK)
        return PROS_EXFAT_EIO;
    if (boot[510] != 0x55u || boot[511] != 0xAAu ||
        boot[3] != 'E' || boot[4] != 'X' || boot[5] != 'F' ||
        boot[6] != 'A' || boot[7] != 'T' || boot[8] != ' ' ||
        boot[9] != ' ' || boot[10] != ' ')
        return PROS_EXFAT_ENOTSUP;

    volume_length = le64(&boot[72]);
    fat_offset = le32(&boot[80]);
    fat_length = le32(&boot[84]);
    heap_offset = le32(&boot[88]);
    cluster_count = le32(&boot[92]);
    root_cluster = le32(&boot[96]);
    sector_shift = boot[108];
    cluster_shift = boot[109];
    fat_count = boot[110];

    if (sector_shift < 9u || sector_shift > 12u ||
        (1u << sector_shift) != device->sector_size ||
        cluster_shift > 25u || fat_count == 0u ||
        volume_length == 0u || fat_length == 0u ||
        cluster_count == 0u || root_cluster < 2u)
        return PROS_EXFAT_ECORRUPT;

    bytes_per_sector = 1u << sector_shift;
    if ((uint32_t)sector_shift + cluster_shift > 25u)
        return PROS_EXFAT_ENOTSUP;
    sectors_per_cluster = 1u << cluster_shift;
    if (!power_of_two(sectors_per_cluster))
        return PROS_EXFAT_ECORRUPT;

    fat_end = (uint64_t)fat_offset + (uint64_t)fat_length * fat_count;
    heap_end = (uint64_t)heap_offset +
               (uint64_t)cluster_count * sectors_per_cluster;
    if (fat_offset < 24u || fat_end > volume_length ||
        heap_offset < fat_end || heap_end > volume_length ||
        volume_length > device->sector_count ||
        (uint64_t)root_cluster > (uint64_t)cluster_count + 1u)
        return PROS_EXFAT_ECORRUPT;

    if (vfs_mount_init(&fs->mount, PROS_FS_EXFAT, device, fs, 1u) !=
        PROS_VFS_OK)
        return PROS_EXFAT_EINVAL;

    fs->bytes_per_sector = bytes_per_sector;
    fs->sectors_per_cluster = sectors_per_cluster;
    fs->fat_offset = fat_offset;
    fs->fat_length = fat_length;
    fs->cluster_heap_offset = heap_offset;
    fs->cluster_count = cluster_count;
    fs->root_cluster = root_cluster;
    fs->volume_length = volume_length;

    return vfs_mount_activate(&fs->mount) == PROS_VFS_OK
        ? PROS_EXFAT_OK : PROS_EXFAT_EINVAL;
}

int exfat_cluster_lba(const struct pros_exfat *fs, uint32_t cluster,
                      uint64_t *lba)
{
    uint64_t offset;
    if (!fs || !lba || !fs->mount.active || cluster < 2u ||
        (uint64_t)cluster > (uint64_t)fs->cluster_count + 1u)
        return PROS_EXFAT_EINVAL;
    offset = (uint64_t)(cluster - 2u) * fs->sectors_per_cluster;
    if ((uint64_t)fs->cluster_heap_offset + offset +
            fs->sectors_per_cluster > fs->volume_length ||
        (uint64_t)fs->cluster_heap_offset + offset +
            fs->sectors_per_cluster > fs->mount.device->sector_count)
        return PROS_EXFAT_EIO;
    *lba = (uint64_t)fs->cluster_heap_offset + offset;
    return PROS_EXFAT_OK;
}

int exfat_read_cluster(const struct pros_exfat *fs, uint32_t cluster,
                       void *buffer)
{
    uint64_t lba;
    if (!fs || !buffer || !fs->mount.active)
        return PROS_EXFAT_EINVAL;
    if (exfat_cluster_lba(fs, cluster, &lba) != PROS_EXFAT_OK)
        return PROS_EXFAT_EINVAL;
    return block_device_read(fs->mount.device, lba, fs->sectors_per_cluster,
                             buffer) == PROS_BLOCK_OK
        ? PROS_EXFAT_OK : PROS_EXFAT_EIO;
}


int exfat_read_fat_entry(const struct pros_exfat *fs, uint32_t cluster,
                         uint32_t *value)
{
    uint64_t byte_offset, sector_offset, lba;
    uint32_t in_sector;
    uint8_t sector[4096];

    if (!fs || !value || !fs->mount.active || cluster < 2u ||
        (uint64_t)cluster > (uint64_t)fs->cluster_count + 1u ||
        fs->bytes_per_sector > sizeof(sector))
        return PROS_EXFAT_EINVAL;

    byte_offset = (uint64_t)cluster * 4u;
    sector_offset = byte_offset / fs->bytes_per_sector;
    in_sector = (uint32_t)(byte_offset % fs->bytes_per_sector);
    if (sector_offset >= fs->fat_length ||
        in_sector + 4u > fs->bytes_per_sector)
        return PROS_EXFAT_ECORRUPT;

    lba = (uint64_t)fs->fat_offset + sector_offset;
    if (lba >= fs->volume_length ||
        lba >= fs->mount.device->sector_count)
        return PROS_EXFAT_EIO;
    if (block_device_read(fs->mount.device, lba, 1u, sector) !=
        PROS_BLOCK_OK)
        return PROS_EXFAT_EIO;

    *value = le32(sector + in_sector);
    if (*value == 0xFFFFFFF7u)
        return PROS_EXFAT_ECORRUPT;
    if (*value == 0xFFFFFFFFu)
        return PROS_EXFAT_OK;
    if (*value < 2u ||
        (uint64_t)*value > (uint64_t)fs->cluster_count + 1u)
        return PROS_EXFAT_ECORRUPT;
    return PROS_EXFAT_OK;
}

static int exfat_file_cluster(const struct pros_exfat *fs,
                              uint32_t first_cluster, uint8_t no_fat_chain,
                              uint64_t cluster_index, uint32_t *cluster)
{
    uint64_t max_clusters;
    uint32_t current;

    if (!fs || !cluster || first_cluster < 2u ||
        (uint64_t)first_cluster > (uint64_t)fs->cluster_count + 1u)
        return PROS_EXFAT_EINVAL;

    max_clusters = (uint64_t)fs->cluster_count;
    if (cluster_index >= max_clusters)
        return PROS_EXFAT_ECORRUPT;

    if (no_fat_chain) {
        uint64_t contiguous = (uint64_t)first_cluster + cluster_index;
        if (contiguous > (uint64_t)fs->cluster_count + 1u)
            return PROS_EXFAT_ECORRUPT;
        *cluster = (uint32_t)contiguous;
        return PROS_EXFAT_OK;
    }

    current = first_cluster;
    for (uint64_t i = 0u; i < cluster_index; ++i) {
        uint32_t next;
        int rc = exfat_read_fat_entry(fs, current, &next);
        if (rc != PROS_EXFAT_OK)
            return rc;
        if (next == 0xFFFFFFFFu)
            return PROS_EXFAT_ECORRUPT;
        current = next;
    }
    *cluster = current;
    return PROS_EXFAT_OK;
}

int exfat_file_read(const struct pros_exfat *fs, uint32_t first_cluster,
                    uint8_t no_fat_chain, uint64_t data_length,
                    uint64_t valid_data_length, uint64_t offset,
                    void *buffer, uint32_t length, uint32_t *done)
{
    uint64_t cluster_bytes, clusters_required, position, remaining;
    uint8_t sector[4096];

    if (!fs || !buffer || !done || !fs->mount.active ||
        fs->bytes_per_sector > sizeof(sector) ||
        fs->sectors_per_cluster == 0u ||
        valid_data_length > data_length)
        return PROS_EXFAT_EINVAL;

    *done = 0u;
    if (offset >= data_length || length == 0u)
        return PROS_EXFAT_OK;
    if (first_cluster < 2u)
        return PROS_EXFAT_ECORRUPT;

    cluster_bytes = (uint64_t)fs->bytes_per_sector *
                    fs->sectors_per_cluster;
    if (data_length > 0u &&
        data_length > 0xFFFFFFFFFFFFFFFFull - cluster_bytes + 1u)
        return PROS_EXFAT_ECORRUPT;
    clusters_required = (data_length + cluster_bytes - 1u) / cluster_bytes;
    if (clusters_required == 0u ||
        clusters_required > (uint64_t)fs->cluster_count)
        return PROS_EXFAT_ECORRUPT;

    remaining = data_length - offset;
    if (remaining > length)
        remaining = length;
    position = offset;

    while (remaining != 0u) {
        uint64_t cluster_index = position / cluster_bytes;
        uint64_t cluster_offset = position % cluster_bytes;
        uint32_t cluster;
        uint32_t sector_index;
        uint32_t in_sector;
        uint32_t take;
        uint64_t lba;

        if (exfat_file_cluster(fs, first_cluster, no_fat_chain,
                               cluster_index, &cluster) != PROS_EXFAT_OK)
            return PROS_EXFAT_ECORRUPT;
        if (exfat_cluster_lba(fs, cluster, &lba) != PROS_EXFAT_OK)
            return PROS_EXFAT_EIO;

        sector_index = (uint32_t)(cluster_offset / fs->bytes_per_sector);
        in_sector = (uint32_t)(cluster_offset % fs->bytes_per_sector);
        take = fs->bytes_per_sector - in_sector;
        if (take > remaining)
            take = (uint32_t)remaining;

        if (sector_index >= fs->sectors_per_cluster)
            return PROS_EXFAT_ECORRUPT;
        lba += sector_index;
        if (lba >= fs->volume_length ||
            block_device_read(fs->mount.device, lba, 1u, sector) !=
                PROS_BLOCK_OK)
            return PROS_EXFAT_EIO;

        for (uint32_t i = 0u; i < take; ++i) {
            uint64_t absolute = position + i;
            ((uint8_t *)buffer)[*done + i] =
                absolute < valid_data_length ? sector[in_sector + i] : 0u;
        }

        *done += take;
        position += take;
        remaining -= take;
    }

    return PROS_EXFAT_OK;
}

struct exfat_test_disk { uint8_t boot[512]; };

static int exfat_test_read(struct pros_block_device *device, uint64_t lba,
                            uint32_t count, void *buffer)
{
    struct exfat_test_disk *disk =
        (struct exfat_test_disk *)device->private_data;
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

int exfat_selftest(void)
{
    struct exfat_test_disk disk = {{0}};
    struct pros_block_device device;
    struct pros_exfat fs;
    uint64_t lba;

    disk.boot[3]='E'; disk.boot[4]='X'; disk.boot[5]='F'; disk.boot[6]='A';
    disk.boot[7]='T'; disk.boot[8]=' '; disk.boot[9]=' '; disk.boot[10]=' ';
    disk.boot[72]=0x00; disk.boot[73]=0x10; /* 4096 sectors */
    disk.boot[80]=24; disk.boot[84]=8; disk.boot[88]=32;
    disk.boot[92]=64; disk.boot[96]=2; disk.boot[108]=9; disk.boot[109]=0;
    disk.boot[110]=1; disk.boot[510]=0x55; disk.boot[511]=0xAA;

    if (block_device_init(&device,512,4096,exfat_test_read,0,&disk) != PROS_BLOCK_OK ||
        exfat_mount(&fs,&device) != PROS_EXFAT_OK ||
        exfat_cluster_lba(&fs,2,&lba) != PROS_EXFAT_OK || lba != 32u)
        return -1;

    /* Reject geometry that extends beyond the physical device. */
    disk.boot[72] = 0x00; disk.boot[73] = 0x20; /* 8192 sectors */
    if (exfat_mount(&fs, &device) != PROS_EXFAT_ECORRUPT)
        return -2;

    /* Restore a valid volume and reject an overlapping cluster heap. */
    disk.boot[72] = 0x00; disk.boot[73] = 0x10; /* 4096 sectors */
    disk.boot[88] = 31u; /* heap starts before FAT end (24 + 8). */
    if (exfat_mount(&fs, &device) != PROS_EXFAT_ECORRUPT)
        return -3;

    /* Restore heap layout and reject a root cluster outside the heap. */
    disk.boot[88] = 32u;
    disk.boot[96] = 66u; /* cluster_count is 64; valid max is 65. */
    if (exfat_mount(&fs, &device) != PROS_EXFAT_ECORRUPT)
        return -4;

    /* Restore root cluster and reject an impossible cluster size. */
    disk.boot[96] = 2u;
    disk.boot[109] = 17u; /* 512-byte sectors would yield 128 KiB clusters. */
    if (exfat_mount(&fs, &device) != PROS_EXFAT_ENOTSUP)
        return -5;

    return 0;
}
