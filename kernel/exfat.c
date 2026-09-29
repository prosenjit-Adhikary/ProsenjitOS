#include "exfat.h"
#include "block.h"

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

    if (!fs || !value || !fs->mount.active ||
        !fs->mount.device || !fs->mount.device->read || cluster < 2u ||
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
    if (data_length > UINT64_MAX - (cluster_bytes - 1u))
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

struct exfat_test_disk {
    uint8_t sectors[32u * 512u];
};

static void exfat_test_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void exfat_test_le64(uint8_t *p, uint64_t value)
{
    exfat_test_le32(p, (uint32_t)value);
    exfat_test_le32(p + 4u, (uint32_t)(value >> 32));
}

static int exfat_test_read(struct pros_block_device *device, uint64_t lba,
                            uint32_t count, void *buffer)
{
    struct exfat_test_disk *disk =
        (struct exfat_test_disk *)device->private_data;
    if (!disk || !buffer || count == 0u ||
        lba >= 32u || count > 32u - lba)
        return PROS_BLOCK_EIO;
    {
        volatile uint8_t *dst = (volatile uint8_t *)buffer;
        volatile const uint8_t *src = disk->sectors + lba * 512u;
        for (uint32_t i = 0u; i < count * 512u; ++i)
            dst[i] = src[i];
    }
    return PROS_BLOCK_OK;
}


static uint16_t exfat_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t exfat_directory_checksum(const uint8_t *set,
                                         uint32_t entry_count)
{
    uint16_t sum = 0u;
    for (uint32_t i = 0u; i < entry_count * 32u; ++i) {
        uint8_t value = set[i];
        if (i == 2u || i == 3u) value = 0u;
        sum = (uint16_t)(((sum << 15) | (sum >> 1)) + value);
    }
    return sum;
}

static int exfat_directory_name(const uint8_t *set, uint32_t secondary_count,
                                uint8_t name_length, char *name, uint32_t capacity)
{
    uint32_t out = 0u;
    uint32_t name_entries = 0u;

    if (name_length == 0u || capacity <= (uint32_t)name_length)
        return PROS_EXFAT_EINVAL;

    for (uint32_t i = 1u; i <= secondary_count; ++i) {
        const uint8_t *entry = set + i * 32u;
        if (entry[0] != 0xC1u) continue;
        ++name_entries;
        for (uint32_t j = 0u; j < 15u && out < name_length; ++j) {
            uint16_t ch = exfat_le16(entry + 2u + j * 2u);
            if (ch == 0u)
                return PROS_EXFAT_ECORRUPT;
            if (ch > 0x7Fu)
                return PROS_EXFAT_ENOTSUP;
            name[out++] = (char)ch;
        }
    }

    if (name_entries == 0u || out != name_length)
        return PROS_EXFAT_ECORRUPT;
    name[out] = 0;
    return PROS_EXFAT_OK;
}

static int exfat_name_equal_ci(const char *a, const char *b)
{
    uint32_t i = 0u;
    while (a[i] && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
        ++i;
    }
    return a[i] == b[i];
}

int exfat_find(const struct pros_exfat *fs, uint32_t directory_cluster,
               const char *name, struct pros_exfat_dirent *result)
{
    static uint8_t cluster[32768];
    static uint8_t set[32u * 32u];
    uint32_t cluster_bytes, current, guard = 0u;

    if (!fs || !name || !result || !fs->mount.active ||
        !fs->mount.device || !directory_cluster ||
        directory_cluster < 2u ||
        (uint64_t)directory_cluster > (uint64_t)fs->cluster_count + 1u)
        return PROS_EXFAT_EINVAL;

    cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
    if (cluster_bytes == 0u || cluster_bytes > sizeof(cluster))
        return PROS_EXFAT_ENOTSUP;

    current = directory_cluster;
    while (guard++ <= fs->cluster_count) {
        if (exfat_read_cluster(fs, current, cluster) != PROS_EXFAT_OK)
            return PROS_EXFAT_EIO;
        if (current == directory_cluster && cluster[0] != 0x85u)
            return -225;

        for (uint32_t off = 0u; off + 32u <= cluster_bytes; off += 32u) {
            const uint8_t *entry = cluster + off;
            if (entry[0] == 0x00u)
                return PROS_EXFAT_ENOTSUP;
            if (entry[0] != 0x85u)
                continue;

            uint32_t secondary_count = entry[1];
            uint32_t entry_count = secondary_count + 1u;
            uint32_t set_bytes = entry_count * 32u;
            if (secondary_count == 0u || secondary_count > 31u)
                return PROS_EXFAT_ECORRUPT;
            if (off + set_bytes > cluster_bytes)
                return PROS_EXFAT_ENOTSUP;

            {
                volatile uint8_t *dst = set;
                volatile const uint8_t *src = cluster + off;
                for (uint32_t i = 0u; i < set_bytes; ++i)
                    dst[i] = src[i];
            }

            uint16_t stored = exfat_le16(set + 2u);
            if (exfat_directory_checksum(set, entry_count) != stored)
                return -220;

            const uint8_t *stream = 0;
            for (uint32_t i = 1u; i <= secondary_count; ++i) {
                const uint8_t *secondary = set + i * 32u;
                if (secondary[0] == 0xC0u) {
                    if (stream) return -221;
                    stream = secondary;
                } else if (secondary[0] != 0xC1u) {
                    return -222;
                }
            }
            if (!stream)
                return -223;

            char parsed[256];
            if (exfat_directory_name(set, secondary_count, stream[3],
                                     parsed, sizeof(parsed)) != PROS_EXFAT_OK)
                return -224;

            if (exfat_name_equal_ci(parsed, name)) {
                result->attributes = set[4];
                result->no_fat_chain = (uint8_t)((stream[1] & 0x02u) != 0u);
                result->valid_data_length = le64(stream + 8u);
                result->first_cluster = le32(stream + 20u);
                result->size = le64(stream + 24u);
                if (result->valid_data_length > result->size)
                    return -210;
                if (result->size != 0u &&
                    (result->first_cluster < 2u ||
                     (uint64_t)result->first_cluster >
                         (uint64_t)fs->cluster_count + 1u))
                    return -211;
                for (uint32_t i = 0u; i < sizeof(result->name); ++i)
                    result->name[i] = 0u;
                for (uint32_t i = 0u; i < sizeof(result->name) - 1u &&
                                     parsed[i]; ++i)
                    result->name[i] = parsed[i];
                return PROS_EXFAT_OK;
            }
            if (parsed[0] != 0)
                return -300 - (int)(unsigned char)parsed[0];
            off += set_bytes - 32u;
        }

        {
            uint32_t next;
            int rc = exfat_read_fat_entry(fs, current, &next);
            if (rc != PROS_EXFAT_OK) return rc;
            if (next == 0xFFFFFFFFu) return PROS_EXFAT_ENOENT;
            current = next;
        }
    }
    return PROS_EXFAT_ECORRUPT;
}

int exfat_selftest(void)
{
    static struct exfat_test_disk disk;
    struct pros_block_device device;
    struct pros_exfat fs;
    uint64_t lba;
    uint8_t output[800];
    uint32_t done;

    uint8_t *boot = disk.sectors;
    boot[3]='E'; boot[4]='X'; boot[5]='F'; boot[6]='A';
    boot[7]='T'; boot[8]=' '; boot[9]=' '; boot[10]=' ';
    exfat_test_le32(boot + 72, 29u);  /* volume length */
    exfat_test_le32(boot + 80, 24u);  /* FAT starts at sector 24 */
    exfat_test_le32(boot + 84, 1u);   /* one FAT sector */
    exfat_test_le32(boot + 88, 25u);  /* cluster heap starts at 25 */
    exfat_test_le32(boot + 92, 4u);   /* clusters 2..5 */
    exfat_test_le32(boot + 96, 2u);   /* root cluster */
    boot[108]=9u; boot[109]=0u; boot[110]=1u;
    boot[510]=0x55u; boot[511]=0xAAu;

    /* FAT[2] -> 3, FAT[3] -> EOC. */
    exfat_test_le32(disk.sectors + 24u * 512u + 8u, 3u);
    exfat_test_le32(disk.sectors + 24u * 512u + 12u, 0xFFFFFFFFu);

    for (uint32_t i = 0u; i < 512u; ++i) {
        disk.sectors[25u * 512u + i] = (uint8_t)('A' + (i % 26u));
        disk.sectors[26u * 512u + i] = (uint8_t)('a' + (i % 26u));
        disk.sectors[27u * 512u + i] = (uint8_t)('0' + (i % 10u));
    }

    if (block_device_init(&device,512,32,exfat_test_read,0,&disk) != PROS_BLOCK_OK)
        return -101;
    if (exfat_mount(&fs, &device) != PROS_EXFAT_OK)
        return -1;
    if (exfat_cluster_lba(&fs,2,&lba) != PROS_EXFAT_OK || lba != 25u)
        return -102;

    if (exfat_read_fat_entry(&fs, 2u, &done) != PROS_EXFAT_OK ||
        done != 3u ||
        exfat_read_fat_entry(&fs, 3u, &done) != PROS_EXFAT_OK ||
        done != 0xFFFFFFFFu)
        return -2;

    if (exfat_file_read(&fs, 2u, 0u, 700u, 700u, 0u,
                        output, sizeof(output), &done) != PROS_EXFAT_OK ||
        done != 700u ||
        output[0] != 'A' ||
        output[511] != 'R' ||
        output[512] != 'a' ||
        output[699] != 'f')
        return -3;

    /* Validate a complete file directory-entry set and case-insensitive lookup. */
    {
        uint8_t *set = disk.sectors + 25u * 512u;
        const char *filename = "HELLO.TXT";
        set[0] = 0x85u;
        set[1] = 2u;
        set[4] = 0x20u;
        set[32] = 0xC0u;
        set[33] = 1u;
        set[35] = 9u;
        exfat_test_le64(set + 40u, 700u);
        exfat_test_le32(set + 52u, 2u);
        exfat_test_le64(set + 56u, 700u);
        set[64] = 0xC1u;
        for (uint32_t i = 0u; i < 9u; ++i) {
            set[66u + i * 2u] = (uint8_t)filename[i];
            set[67u + i * 2u] = 0u;
        }
        {
            uint16_t checksum = exfat_directory_checksum(set, 3u);
            set[2] = (uint8_t)checksum;
            set[3] = (uint8_t)(checksum >> 8);
            if (exfat_directory_checksum(set, 3u) != exfat_le16(set + 2u))
                return -130;
        }
        {
            char decoded_name[256];
            if (exfat_directory_name(set, 2u, set[35], decoded_name,
                                     sizeof(decoded_name)) != PROS_EXFAT_OK)
                return -131;
            if (!exfat_name_equal_ci(decoded_name, filename))
                return -132;
        }
        {
            if (exfat_read_cluster(&fs, 2u, output) != PROS_EXFAT_OK ||
                output[0] != 0x85u ||
                output[32] != 0xC0u ||
                output[64] != 0xC1u)
                return -133;
            struct pros_exfat_dirent found;
            int find_status = exfat_find(&fs, 2u, "hello.txt", &found);
            if (find_status != PROS_EXFAT_OK)
                return -150 + find_status;
            if (found.size != 700u)
                return -121;
            if (found.valid_data_length != 700u)
                return -122;
            if (found.first_cluster != 2u)
                return -123;
            if (found.no_fat_chain != 0u)
                return -124;
            if (!exfat_name_equal_ci(found.name, filename))
                return -125;
        }
        set[2] ^= 1u;
        {
            struct pros_exfat_dirent found;
            if (exfat_find(&fs, 2u, filename, &found) != PROS_EXFAT_ECORRUPT)
                return -13;
        }
        set[2] ^= 1u;
    }

    /* NoFatChain must read a contiguous allocation without consulting FAT. */
    if (exfat_file_read(&fs, 5u, 1u, 100u, 50u, 0u,
                        output, sizeof(output), &done) != PROS_EXFAT_OK ||
        done != 100u ||
        output[0] != '0' ||
        output[49] != '9' ||
        output[50] != 0u ||
        output[99] != 0u)
        return -4;

    /* Reject a malformed FAT chain that terminates before DataLength. */
    if (exfat_file_read(&fs, 3u, 0u, 600u, 600u, 0u,
                        output, sizeof(output), &done) != PROS_EXFAT_ECORRUPT)
        return -5;

    /* Reject a volume whose declared length exceeds the physical device. */
    exfat_test_le32(boot + 72, 33u);
    device.sector_count = 32u;
    {
        int physical_volume_status = exfat_mount(&fs, &device);
        if (physical_volume_status != PROS_EXFAT_ECORRUPT)
            return -6;
    }
    exfat_test_le32(boot + 72, 29u);

    /* Restore a valid volume and reject an overlapping cluster heap. */
    exfat_test_le32(boot + 88, 1u);
    if (exfat_mount(&fs, &device) != PROS_EXFAT_ECORRUPT)
        return -11;

    /* Restore heap layout and reject a root cluster outside the heap. */
    exfat_test_le32(boot + 88, 2u);
    exfat_test_le32(boot + 96, 6u);
    if (exfat_mount(&fs, &device) != PROS_EXFAT_ECORRUPT)
        return -10;

    /* Restore root cluster and reject an impossible cluster size. */
    exfat_test_le32(boot + 96, 2u);
    boot[109] = 17u;
    if (exfat_mount(&fs, &device) != PROS_EXFAT_ENOTSUP)
        return -9;

    return 0;
}
