#include "exfat.h"
#include "block.h"
#include "heap.h"

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

struct exfat_vfs_node {
    struct pros_vfs_vnode vnode;
    struct pros_exfat *fs;
    struct pros_exfat_dirent entry;
};

static int exfat_vfs_lookup(struct pros_vfs_vnode *, const char *,
                            struct pros_vfs_vnode **);
static int exfat_load_upcase(struct pros_exfat *fs);
static int exfat_vfs_readdir(struct pros_vfs_vnode *, uint64_t *,
                             struct pros_vfs_dirent *);
static int exfat_vfs_read(struct pros_vfs_file *, uint32_t, uint32_t, void *);
static int exfat_vfs_close(struct pros_vfs_file *);
static void exfat_vfs_release(struct pros_vfs_vnode *);

static const struct pros_vfs_vnode_ops exfat_vfs_vnode_ops = {
    .lookup = exfat_vfs_lookup,
    .readdir = exfat_vfs_readdir,
    .release = exfat_vfs_release
};

static const struct pros_vfs_file_ops exfat_vfs_file_ops = {
    .read = exfat_vfs_read,
    .close = exfat_vfs_close
};

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
    fs->upcase_data = 0;
    fs->upcase_length = 0u;

    if (vfs_vnode_init(&fs->root_vnode, &fs->mount, 0,
                       PROS_VFS_NODE_DIRECTORY, &exfat_vfs_vnode_ops, 0,
                       fs, 0) != PROS_VFS_OK ||
        vfs_mount_set_root(&fs->mount, &fs->root_vnode) != PROS_VFS_OK)
        return PROS_EXFAT_EINVAL;

    if (vfs_mount_activate(&fs->mount) != PROS_VFS_OK)
        return PROS_EXFAT_EINVAL;
    if (exfat_load_upcase(fs) != PROS_EXFAT_OK) {
        vfs_mount_deactivate(&fs->mount);
        return PROS_EXFAT_ECORRUPT;
    }

    return PROS_EXFAT_OK;
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
                                uint8_t name_length, char *name,
                                uint16_t *unicode_name, uint32_t capacity,
                                uint16_t *unicode_length)
{
    uint32_t out = 0u;
    uint32_t name_entries = 0u;
    uint32_t units = 0u;

    if (name_length == 0u || capacity <= 1u || !unicode_name ||
        !unicode_length)
        return PROS_EXFAT_EINVAL;

    for (uint32_t i = 1u; i <= secondary_count; ++i) {
        const uint8_t *entry = set + i * 32u;
        if (entry[0] != 0xC1u)
            continue;
        ++name_entries;
        for (uint32_t j = 0u; j < 15u && units < name_length; ++j) {
            uint16_t ch = exfat_le16(entry + 2u + j * 2u);
            if (ch == 0u)
                return PROS_EXFAT_ECORRUPT;
            if (units >= capacity - 1u)
                return PROS_EXFAT_EOVERFLOW;
            unicode_name[units++] = ch;
        }
    }

    if (name_entries == 0u || units != name_length)
        return PROS_EXFAT_ECORRUPT;

    for (uint32_t i = 0u; i < units; ++i) {
        uint16_t ch = unicode_name[i];
        if (ch >= 0xD800u && ch <= 0xDBFFu) {
            if (i + 1u >= units ||
                unicode_name[i + 1u] < 0xDC00u ||
                unicode_name[i + 1u] > 0xDFFFu)
                return PROS_EXFAT_ECORRUPT;
            ++i;
        } else if (ch >= 0xDC00u && ch <= 0xDFFFu) {
            return PROS_EXFAT_ECORRUPT;
        }
    }

    if (name) {
        uint32_t pos = 0u;
        for (uint32_t i = 0u; i < units; ++i) {
            uint32_t cp = unicode_name[i];
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                uint32_t low = unicode_name[++i];
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
            }
            if (cp <= 0x7Fu) {
                if (pos + 1u >= capacity) return PROS_EXFAT_EOVERFLOW;
                name[pos++] = (char)cp;
            } else if (cp <= 0x7FFu) {
                if (pos + 2u >= capacity) return PROS_EXFAT_EOVERFLOW;
                name[pos++] = (char)(0xC0u | (cp >> 6));
                name[pos++] = (char)(0x80u | (cp & 0x3Fu));
            } else if (cp <= 0xFFFFu) {
                if (pos + 3u >= capacity) return PROS_EXFAT_EOVERFLOW;
                name[pos++] = (char)(0xE0u | (cp >> 12));
                name[pos++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                name[pos++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                if (pos + 4u >= capacity) return PROS_EXFAT_EOVERFLOW;
                name[pos++] = (char)(0xF0u | (cp >> 18));
                name[pos++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                name[pos++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                name[pos++] = (char)(0x80u | (cp & 0x3Fu));
            }
        }
        name[pos] = 0;
        out = pos;
    }

    (void)out;
    *unicode_length = (uint16_t)units;
    return PROS_EXFAT_OK;
}

static uint16_t exfat_upcase_ascii(uint16_t ch)
{
    if (ch >= (uint16_t)'a' && ch <= (uint16_t)'z')
        return (uint16_t)(ch - (uint16_t)'a' + (uint16_t)'A');
    return ch;
}

static int exfat_utf8_next(const char **input, uint16_t *units,
                           uint32_t capacity, uint32_t *count)
{
    const uint8_t *s = (const uint8_t *)*input;
    uint32_t cp;
    uint32_t needed;

    if (!s || !units || !count || capacity < 2u)
        return PROS_EXFAT_EINVAL;
    if (*s == 0u)
        return 0;

    if (*s < 0x80u) {
        cp = *s;
        needed = 1u;
    } else if (*s >= 0xC2u && *s <= 0xDFu) {
        if (s[1] < 0x80u || s[1] > 0xBFu) return PROS_EXFAT_EINVAL;
        cp = ((uint32_t)(s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
        needed = 2u;
    } else if (*s >= 0xE0u && *s <= 0xEFu) {
        if (s[1] < 0x80u || s[1] > 0xBFu ||
            s[2] < 0x80u || s[2] > 0xBFu ||
            (s[0] == 0xE0u && s[1] < 0xA0u) ||
            (s[0] == 0xEDu && s[1] >= 0xA0u))
            return PROS_EXFAT_EINVAL;
        cp = ((uint32_t)(s[0] & 0x0Fu) << 12) |
             ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
        needed = 3u;
    } else if (*s >= 0xF0u && *s <= 0xF4u) {
        if (s[1] < 0x80u || s[1] > 0xBFu ||
            s[2] < 0x80u || s[2] > 0xBFu ||
            s[3] < 0x80u || s[3] > 0xBFu ||
            (*s == 0xF0u && s[1] < 0x90u) ||
            (*s == 0xF4u && s[1] > 0x8Fu))
            return PROS_EXFAT_EINVAL;
        cp = ((uint32_t)(s[0] & 0x07u) << 18) |
             ((uint32_t)(s[1] & 0x3Fu) << 12) |
             ((uint32_t)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
        needed = 4u;
    } else {
        return PROS_EXFAT_EINVAL;
    }

    if (cp <= 0xFFFFu) {
        if (*count >= capacity) return PROS_EXFAT_EOVERFLOW;
        units[(*count)++] = (uint16_t)cp;
    } else {
        if (*count + 2u > capacity) return PROS_EXFAT_EOVERFLOW;
        cp -= 0x10000u;
        units[(*count)++] = (uint16_t)(0xD800u + (cp >> 10));
        units[(*count)++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
    }
    *input += needed;
    return 1;
}

static int exfat_name_equal_ci(const struct pros_exfat *fs,
                               const struct pros_exfat_dirent *entry,
                               const char *input)
{
    uint16_t input_units[256];
    uint32_t input_count = 0u;
    uint32_t i = 0u;

    if (!fs || !entry || !input)
        return PROS_EXFAT_EINVAL;

    while (*input) {
        int rc = exfat_utf8_next(&input, input_units,
                                 sizeof(input_units) / sizeof(input_units[0]),
                                 &input_count);
        if (rc <= 0)
            return rc == 0 ? PROS_EXFAT_EINVAL : rc;
    }

    if (input_count != entry->name_length)
        return 0;

    while (i < input_count) {
        uint16_t lhs, rhs;
        if (exfat_upcase_lookup(&fs->upcase_table,
                                entry->unicode_name[i], &lhs) !=
                PROS_EXFAT_UPCASE_OK ||
            exfat_upcase_lookup(&fs->upcase_table,
                                input_units[i], &rhs) !=
                PROS_EXFAT_UPCASE_OK)
            return PROS_EXFAT_ECORRUPT;
        if (lhs != rhs)
            return 0;
        ++i;
    }
    return 1;
}

static int exfat_name_equal_ascii_ci(const char *a, const char *b)
{
    uint32_t i = 0u;
    while (a[i] && b[i]) {
        uint16_t ca = exfat_upcase_ascii((uint16_t)(uint8_t)a[i]);
        uint16_t cb = exfat_upcase_ascii((uint16_t)(uint8_t)b[i]);
        if (ca != cb) return 0;
        ++i;
    }
    return a[i] == b[i];
}


static int exfat_directory_read(const struct pros_exfat *fs,
                                      uint32_t directory_cluster,
                                      uint64_t offset, void *buffer,
                                      uint32_t length)
{
    uint64_t cluster_bytes;
    uint8_t sector[4096];

    if (!fs || !buffer || !fs->mount.active ||
        directory_cluster < 2u ||
        (uint64_t)directory_cluster > (uint64_t)fs->cluster_count + 1u ||
        fs->bytes_per_sector > sizeof(sector))
        return PROS_EXFAT_EINVAL;

    cluster_bytes = (uint64_t)fs->bytes_per_sector *
                    fs->sectors_per_cluster;
    if (cluster_bytes == 0u || offset > UINT64_MAX - length)
        return PROS_EXFAT_ECORRUPT;

    while (length != 0u) {
        uint64_t cluster_index = offset / cluster_bytes;
        uint64_t cluster_offset = offset % cluster_bytes;
        uint32_t cluster;
        uint32_t sector_index;
        uint32_t in_sector;
        uint32_t take;
        uint64_t lba;

        if (exfat_file_cluster(fs, directory_cluster, 0u,
                               cluster_index, &cluster) != PROS_EXFAT_OK)
            return PROS_EXFAT_EIO;
        if (exfat_cluster_lba(fs, cluster, &lba) != PROS_EXFAT_OK)
            return PROS_EXFAT_EIO;

        sector_index = (uint32_t)(cluster_offset / fs->bytes_per_sector);
        in_sector = (uint32_t)(cluster_offset % fs->bytes_per_sector);
        if (sector_index >= fs->sectors_per_cluster)
            return PROS_EXFAT_ECORRUPT;
        take = fs->bytes_per_sector - in_sector;
        if (take > length)
            take = length;

        lba += sector_index;
        if (lba >= fs->volume_length ||
            block_device_read(fs->mount.device, lba, 1u, sector) !=
                PROS_BLOCK_OK)
            return PROS_EXFAT_EIO;

        for (uint32_t i = 0u; i < take; ++i)
            ((uint8_t *)buffer)[i] = sector[in_sector + i];

        buffer = (uint8_t *)buffer + take;
        offset += take;
        length -= take;
    }
    return PROS_EXFAT_OK;
}

static int exfat_load_upcase(struct pros_exfat *fs)
{
    uint8_t entry[32];
    uint64_t offset = 0u;
    uint8_t *data = 0;
    uint32_t length = 0u;
    uint32_t first_cluster = 0u;
    uint32_t expected_checksum = 0u;
    uint8_t compressed;

    if (!fs || !fs->mount.active)
        return PROS_EXFAT_EINVAL;

    for (;;) {
        if (exfat_directory_read(fs, fs->root_cluster, offset,
                                 entry, sizeof(entry)) != PROS_EXFAT_OK)
            return PROS_EXFAT_EIO;
        if (entry[0] == 0x00u)
            return PROS_EXFAT_ECORRUPT;
        if (entry[0] == 0x82u) {
            first_cluster = le32(entry + 20u);
            if (le64(entry + 24u) > 0xFFFFFFFFu)
                return PROS_EXFAT_ECORRUPT;
            length = (uint32_t)le64(entry + 24u);
            expected_checksum = le32(entry + 4u);
            break;
        }
        if (offset > UINT64_MAX - 32u)
            return PROS_EXFAT_ECORRUPT;
        offset += 32u;
    }

    if (length == 0u || length > 0x00020000u ||
        first_cluster < 2u ||
        (uint64_t)first_cluster > (uint64_t)fs->cluster_count + 1u)
        return PROS_EXFAT_ECORRUPT;

    compressed = (uint8_t)(length != 0x00020000u);
    data = (uint8_t *)kmalloc(length);
    if (!data)
        return PROS_EXFAT_EIO;

    {
        uint32_t done = 0u;
        int rc = exfat_file_read(fs, first_cluster, 0u, length, length,
                                 0u, data, length, &done);
        if (rc != PROS_EXFAT_OK || done != length) {
            kfree(data);
            return rc == PROS_EXFAT_EIO ? PROS_EXFAT_EIO :
                   PROS_EXFAT_ECORRUPT;
        }
    }

    if (exfat_upcase_checksum(data, length) != expected_checksum) {
        kfree(data);
        return PROS_EXFAT_ECORRUPT;
    }

    if (exfat_upcase_parse(data, length, compressed,
                           &fs->upcase_table) != PROS_EXFAT_OK) {
        kfree(data);
        return PROS_EXFAT_ECORRUPT;
    }

    fs->upcase_data = data;
    fs->upcase_length = length;
    return PROS_EXFAT_OK;
}

static int exfat_directory_entry(const struct pros_exfat *fs,
                                 uint32_t directory_cluster,
                                 uint64_t offset,
                                 uint8_t *set,
                                 uint32_t set_capacity,
                                 uint32_t *set_bytes,
                                 struct pros_exfat_dirent *result)
{
    uint8_t primary[32];
    uint32_t secondary_count, entry_count, bytes;
    const uint8_t *stream = 0;
    int rc;

    if (!set || !set_bytes || !result)
        return PROS_EXFAT_EINVAL;
    if (exfat_directory_read(fs, directory_cluster, offset,
                             primary, sizeof(primary)) != PROS_EXFAT_OK)
        return PROS_EXFAT_EIO;
    if (primary[0] == 0x00u)
        return PROS_EXFAT_ENOENT;
    if (primary[0] != 0x85u) {
        *set_bytes = 32u;
        return PROS_EXFAT_ENOTSUP;
    }

    secondary_count = primary[1];
    if (secondary_count == 0u || secondary_count > 31u)
        return PROS_EXFAT_ECORRUPT;
    entry_count = secondary_count + 1u;
    bytes = entry_count * 32u;
    if (bytes > set_capacity)
        return PROS_EXFAT_EINVAL;

    if (exfat_directory_read(fs, directory_cluster, offset,
                             set, bytes) != PROS_EXFAT_OK)
        return PROS_EXFAT_EIO;

    if (exfat_directory_checksum(set, entry_count) != exfat_le16(set + 2u))
        return PROS_EXFAT_ECORRUPT;

    for (uint32_t i = 1u; i <= secondary_count; ++i) {
        const uint8_t *secondary = set + i * 32u;
        if (secondary[0] == 0xC0u) {
            if (stream)
                return PROS_EXFAT_ECORRUPT;
            stream = secondary;
        } else if (secondary[0] != 0xC1u) {
            return PROS_EXFAT_ECORRUPT;
        }
    }
    if (!stream)
        return PROS_EXFAT_ECORRUPT;

    rc = exfat_directory_name(set, secondary_count, stream[3],
                              result->name, result->unicode_name,
                              sizeof(result->name), &result->name_length);
    if (rc != PROS_EXFAT_OK)
        return rc;

    result->attributes = set[4];
    result->no_fat_chain = (uint8_t)((stream[1] & 0x02u) != 0u);
    result->valid_data_length = le64(stream + 8u);
    result->first_cluster = le32(stream + 20u);
    result->size = le64(stream + 24u);
    if (result->valid_data_length > result->size)
        return PROS_EXFAT_ECORRUPT;
    if (result->size != 0u &&
        (result->first_cluster < 2u ||
         (uint64_t)result->first_cluster >
             (uint64_t)fs->cluster_count + 1u))
        return PROS_EXFAT_ECORRUPT;

    *set_bytes = bytes;
    return PROS_EXFAT_OK;
}

int exfat_find(const struct pros_exfat *fs, uint32_t directory_cluster,
               const char *name, struct pros_exfat_dirent *result)
{
    static uint8_t set[32u * 32u];
    uint64_t offset = 0u;

    if (!fs || !name || !result || !fs->mount.active ||
        !fs->mount.device || directory_cluster < 2u ||
        (uint64_t)directory_cluster > (uint64_t)fs->cluster_count + 1u)
        return PROS_EXFAT_EINVAL;

    for (;;) {
        uint8_t first;
        uint32_t set_bytes = 32u;
        struct pros_exfat_dirent parsed;
        int rc;

        if (exfat_directory_read(fs, directory_cluster, offset,
                                 &first, 1u) != PROS_EXFAT_OK)
            return PROS_EXFAT_EIO;
        if (first == 0x00u)
            return PROS_EXFAT_ENOENT;

        rc = exfat_directory_entry(fs, directory_cluster, offset, set,
                                    sizeof(set), &set_bytes, &parsed);
        if (rc == PROS_EXFAT_ENOTSUP) {
            offset += 32u;
            continue;
        }
        if (rc != PROS_EXFAT_OK)
            return rc;

        {
            int match = exfat_name_equal_ci(fs, &parsed, name);
            if (match == PROS_EXFAT_ECORRUPT)
                return PROS_EXFAT_ECORRUPT;
            if (match == 1) {
                *result = parsed;
                return PROS_EXFAT_OK;
            }
        }
        if (offset > UINT64_MAX - set_bytes)
            return PROS_EXFAT_ECORRUPT;
        offset += set_bytes;
    }
}

static struct pros_exfat *exfat_vfs_get_fs(struct pros_vfs_vnode *vnode)
{
    if (!vnode || !vnode->mount || vnode->mount->type != PROS_FS_EXFAT)
        return 0;
    if (vnode->private_data == vnode->mount->private_data)
        return (struct pros_exfat *)vnode->private_data;
    {
        struct exfat_vfs_node *node =
            (struct exfat_vfs_node *)vnode->private_data;
        return node ? node->fs : 0;
    }
}

static int exfat_vfs_lookup(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result)
{
    struct pros_exfat *fs = exfat_vfs_get_fs(parent);
    struct pros_exfat_dirent entry;
    struct exfat_vfs_node *node;
    int rc;

    if (!fs || !name || !result || parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    rc = exfat_find(fs,
                    parent == &fs->root_vnode ? fs->root_cluster :
                    ((struct exfat_vfs_node *)parent->private_data)->entry.first_cluster,
                    name, &entry);
    if (rc == PROS_EXFAT_ENOENT)
        return PROS_VFS_ENOENT;
    if (rc != PROS_EXFAT_OK)
        return rc == PROS_EXFAT_ENOTSUP ? PROS_VFS_ENOSYS : PROS_VFS_EIO;

    node = (struct exfat_vfs_node *)kmalloc(sizeof(*node));
    if (!node)
        return PROS_VFS_EIO;
    if (vfs_vnode_get(parent) != PROS_VFS_OK) {
        kfree(node);
        return PROS_VFS_EINVAL;
    }
    node->fs = fs;
    node->entry = entry;
    rc = vfs_vnode_init(&node->vnode, &fs->mount, parent,
                        (entry.attributes & 0x10u) != 0u
                            ? PROS_VFS_NODE_DIRECTORY
                            : PROS_VFS_NODE_FILE,
                        &exfat_vfs_vnode_ops,
                        (entry.attributes & 0x10u) != 0u
                            ? 0 : &exfat_vfs_file_ops,
                        node, entry.size);
    if (rc != PROS_VFS_OK) {
        vfs_vnode_put(parent);
        kfree(node);
        return rc;
    }
    *result = &node->vnode;
    return PROS_VFS_OK;
}

static int exfat_vfs_readdir(struct pros_vfs_vnode *vnode, uint64_t *cookie,
                             struct pros_vfs_dirent *entry)
{
    struct pros_exfat *fs = exfat_vfs_get_fs(vnode);
    static uint8_t set[32u * 32u];
    uint32_t directory_cluster;
    uint64_t offset;

    if (!fs || !cookie || !entry || vnode->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;

    directory_cluster = vnode == &fs->root_vnode ? fs->root_cluster :
        ((struct exfat_vfs_node *)vnode->private_data)->entry.first_cluster;
    if (directory_cluster < 2u ||
        (uint64_t)directory_cluster > (uint64_t)fs->cluster_count + 1u)
        return PROS_VFS_EIO;

    offset = *cookie;
    for (;;) {
        uint8_t first;
        uint32_t consumed = 32u;
        struct pros_exfat_dirent parsed;
        int rc;

        if (exfat_directory_read(fs, directory_cluster, offset,
                                 &first, 1u) != PROS_EXFAT_OK)
            return PROS_VFS_EIO;
        if (first == 0x00u)
            return PROS_VFS_ENOENT;

        rc = exfat_directory_entry(fs, directory_cluster, offset, set,
                                    sizeof(set), &consumed, &parsed);
        if (rc == PROS_EXFAT_ENOTSUP) {
            if (offset > UINT64_MAX - 32u)
                return PROS_VFS_EIO;
            offset += 32u;
            continue;
        }
        if (rc != PROS_EXFAT_OK)
            return rc == PROS_EXFAT_ENOTSUP ? PROS_VFS_ENOSYS :
                   PROS_VFS_EIO;

        for (uint32_t i = 0u; i < sizeof(entry->name); ++i) {
            entry->name[i] = parsed.name[i];
            if (parsed.name[i] == 0)
                break;
        }
        entry->type = (parsed.attributes & 0x10u) != 0u ?
            PROS_VFS_NODE_DIRECTORY : PROS_VFS_NODE_FILE;
        entry->size = parsed.size;
        if (offset > UINT64_MAX - consumed)
            return PROS_VFS_EIO;
        *cookie = offset + consumed;
        return PROS_VFS_OK;
    }
}

static int exfat_vfs_read(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, void *buffer)
{
    struct exfat_vfs_node *node;
    uint32_t done = 0u;
    int rc;
    if (!file || !file->private_data || !buffer)
        return PROS_VFS_EINVAL;
    node = (struct exfat_vfs_node *)file->private_data;
    if (node->entry.attributes & 0x10u)
        return PROS_VFS_EISDIR;
    rc = exfat_file_read(node->fs, node->entry.first_cluster,
                         node->entry.no_fat_chain, node->entry.size,
                         node->entry.valid_data_length, offset,
                         buffer, length, &done);
    if (rc != PROS_EXFAT_OK)
        return rc == PROS_EXFAT_EIO ? PROS_VFS_EIO : PROS_VFS_EINVAL;
    return (int)done;
}

static int exfat_vfs_close(struct pros_vfs_file *file)
{
    (void)file;
    return PROS_VFS_OK;
}

static void exfat_vfs_release(struct pros_vfs_vnode *vnode)
{
    struct exfat_vfs_node *node;
    struct pros_vfs_vnode *parent;
    if (!vnode || vnode->private_data == vnode->mount->private_data)
        return;
    node = (struct exfat_vfs_node *)vnode->private_data;
    parent = vnode->parent;
    vnode->initialized = 0u;
    if (parent)
        vfs_vnode_put(parent);
    kfree(node);
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

    /* FAT[2] -> 3, FAT[3] -> EOC, FAT[4]/[5] -> EOC. */
    exfat_test_le32(disk.sectors + 24u * 512u + 8u, 3u);
    exfat_test_le32(disk.sectors + 24u * 512u + 12u, 0xFFFFFFFFu);
    exfat_test_le32(disk.sectors + 24u * 512u + 16u, 0xFFFFFFFFu);
    exfat_test_le32(disk.sectors + 24u * 512u + 20u, 0xFFFFFFFFu);

    /* A compact, complete Unicode-range Up-case table:
     * identity 0000..0060, A-Z mappings for 0061..007A,
     * identity 007B..FFFF. */
    {
        uint8_t *table = disk.sectors + 28u * 512u;
        uint32_t p = 0u;
        table[p++] = 0xFFu; table[p++] = 0xFFu;
        table[p++] = 0x61u; table[p++] = 0x00u;
        for (uint16_t ch = 0x61u; ch <= 0x7Au; ++ch) {
            uint16_t mapped = (uint16_t)(ch - 0x20u);
            table[p++] = (uint8_t)mapped;
            table[p++] = (uint8_t)(mapped >> 8);
        }
        table[p++] = 0xFFu; table[p++] = 0xFFu;
        table[p++] = 0x85u; table[p++] = 0xFFu;

        disk.sectors[25u * 512u + 0u] = 0x82u;
        exfat_test_le32(disk.sectors + 25u * 512u + 4u,
                        exfat_upcase_checksum(table, p));
        exfat_test_le32(disk.sectors + 25u * 512u + 20u, 5u);
        exfat_test_le64(disk.sectors + 25u * 512u + 24u, p);
    }

    for (uint32_t i = 0u; i < 512u; ++i) {
        disk.sectors[25u * 512u + i] = (uint8_t)('A' + (i % 26u));
        disk.sectors[26u * 512u + i] = (uint8_t)('a' + (i % 26u));
        disk.sectors[27u * 512u + i] = (uint8_t)('0' + (i % 10u));
    }

    /* Restore the root-directory Up-case entry after initializing the
     * root cluster's backing sectors with test payload data. */
    disk.sectors[25u * 512u + 0u] = 0x82u;
    exfat_test_le32(disk.sectors + 25u * 512u + 4u,
                    exfat_upcase_checksum(table, p));
    exfat_test_le32(disk.sectors + 25u * 512u + 20u, 5u);
    exfat_test_le64(disk.sectors + 25u * 512u + 24u, p);

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
        uint8_t *set = disk.sectors + 25u * 512u + 32u;
        const char *filename = "HELLO.TXT";
        set[0] = 0x85u;
        set[1] = 2u;
        set[4] = 0x20u;
        set[32] = 0xC0u;
        set[33] = 1u;
        set[35] = 9u;
        exfat_test_le64(set + 40u, 700u);
        exfat_test_le32(set + 52u, 4u);
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
        }
        {
            struct pros_exfat_dirent found;
            if (exfat_find(&fs, 2u, "hello.txt", &found) != PROS_EXFAT_OK ||
                found.size != 700u ||
                found.valid_data_length != 700u ||
                found.first_cluster != 4u ||
                found.no_fat_chain != 0u ||
                !exfat_name_equal_ascii_ci(found.name, filename))
                return -12;
        }
        set[2] ^= 1u;
        {
            struct pros_exfat_dirent found;
            if (exfat_find(&fs, 2u, filename, &found) != PROS_EXFAT_ECORRUPT)
                return -13;
        }
        set[2] ^= 1u;
    }

    /* Exercise a non-ASCII UTF-16 filename through the real Up-case lookup
     * path. U+00E9 is identity-mapped by this valid test table. */
    {
        uint8_t *unicode_set = disk.sectors + 25u * 512u + 128u;
        static const uint16_t filename[] = {
            0x0063u, 0x0061u, 0x0066u, 0x00E9u,
            0x002Eu, 0x0074u, 0x0078u, 0x0074u
        };
        struct pros_exfat_dirent found;

        unicode_set[0] = 0x85u;
        unicode_set[1] = 2u;
        unicode_set[4] = 0x20u;
        unicode_set[32] = 0xC0u;
        unicode_set[33] = 1u;
        unicode_set[35] = 8u;
        exfat_test_le64(unicode_set + 40u, 700u);
        exfat_test_le32(unicode_set + 52u, 4u);
        exfat_test_le64(unicode_set + 56u, 700u);
        unicode_set[64] = 0xC1u;
        for (uint32_t i = 0u; i < 8u; ++i) {
            unicode_set[66u + i * 2u] = (uint8_t)filename[i];
            unicode_set[67u + i * 2u] = (uint8_t)(filename[i] >> 8);
        }
        {
            uint16_t checksum = exfat_directory_checksum(unicode_set, 3u);
            unicode_set[2] = (uint8_t)checksum;
            unicode_set[3] = (uint8_t)(checksum >> 8);
        }

        if (exfat_find(&fs, 2u, "CAF\xC3\x89.TXT", &found) !=
                PROS_EXFAT_OK ||
            found.name_length != 8u ||
            found.unicode_name[3] != 0x00E9u)
            return -19;
    }

    /* Directory entry sets may legally cross a cluster boundary. */
    {
        uint8_t boundary_set[96];
        struct pros_exfat_dirent found;
        const char *filename = "CROSS.TXT";
        for (uint32_t i = 0u; i < sizeof(boundary_set); ++i)
            boundary_set[i] = 0u;
        boundary_set[0] = 0x85u;
        boundary_set[1] = 2u;
        boundary_set[4] = 0x20u;
        boundary_set[32] = 0xC0u;
        boundary_set[33] = 1u;
        boundary_set[35] = 9u;
        exfat_test_le64(boundary_set + 40u, 700u);
        exfat_test_le32(boundary_set + 52u, 4u);
        exfat_test_le64(boundary_set + 56u, 700u);
        boundary_set[64] = 0xC1u;
        for (uint32_t i = 0u; i < 9u; ++i) {
            boundary_set[66u + i * 2u] = (uint8_t)filename[i];
            boundary_set[67u + i * 2u] = 0u;
        }
        {
            uint16_t checksum = exfat_directory_checksum(boundary_set, 3u);
            boundary_set[2] = (uint8_t)checksum;
            boundary_set[3] = (uint8_t)(checksum >> 8);
        }
        /* Keep the unused slots before the boundary entry non-zero so
         * the directory end marker does not terminate the scan early. */
        for (uint32_t off = 224u; off < 480u; off += 32u)
            disk.sectors[25u * 512u + off] = 0x01u;
        for (uint32_t i = 0u; i < 32u; ++i)
            disk.sectors[25u * 512u + 480u + i] = boundary_set[i];
        for (uint32_t i = 0u; i < 64u; ++i)
            disk.sectors[26u * 512u + i] = boundary_set[32u + i];
        for (uint32_t i = 64u; i < 512u; ++i)
            disk.sectors[26u * 512u + i] = 0u;

        if (exfat_find(&fs, 2u, "cross.txt", &found) != PROS_EXFAT_OK ||
            found.size != 700u || found.first_cluster != 4u ||
            !exfat_name_equal_ci(&fs, &found, filename))
            return -18;
    }

    /* NoFatChain must read a contiguous allocation without consulting FAT. */
    if (exfat_file_read(&fs, 4u, 1u, 100u, 50u, 0u,
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

    /* VFS integration: root lookup, case-insensitive file open/read,
     * directory enumeration, and vnode lifetime must all work on the same
     * read-only exFAT mount. */
    {
        struct pros_vfs_vnode *found_vnode = 0;
        struct pros_vfs_file file;
        struct pros_vfs_dir dir;
        struct pros_vfs_dirent dirent;
        uint32_t bytes_read = 0u;
        uint8_t file_data[8];

        if (vfs_path_lookup(&fs.mount, "/hello.txt", &found_vnode) !=
                PROS_VFS_OK || !found_vnode ||
            found_vnode->type != PROS_VFS_NODE_FILE ||
            found_vnode->size != 700u)
            return -14;
        vfs_vnode_put(found_vnode);

        if (vfs_path_lookup(&fs.mount, "/CAF\xC3\x89.TXT", &found_vnode) !=
                PROS_VFS_OK || !found_vnode ||
            found_vnode->type != PROS_VFS_NODE_FILE ||
            found_vnode->size != 700u)
            return -20;
        vfs_vnode_put(found_vnode);

        if (vfs_file_open(&fs.mount, "/HELLO.TXT", PROS_VFS_O_READ,
                          &file) != PROS_VFS_OK ||
            vfs_file_read(&file, file_data, sizeof(file_data), &bytes_read) !=
                PROS_VFS_OK || bytes_read != sizeof(file_data) ||
            file_data[0] != '0' || file_data[7] != '7' ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return -15;

        if (vfs_dir_open(&fs.mount, "/", &dir) != PROS_VFS_OK ||
            vfs_dir_read(&dir, &dirent) != PROS_VFS_OK ||
            dirent.type != PROS_VFS_NODE_FILE ||
            !exfat_name_equal_ascii_ci(dirent.name, "HELLO.TXT") ||
            dirent.size != 700u ||
            vfs_dir_read(&dir, &dirent) != PROS_VFS_OK ||
            !exfat_name_equal_ascii_ci(dirent.name, "caf\xC3\xA9.txt") ||
            dirent.size != 700u ||
            vfs_dir_read(&dir, &dirent) != PROS_VFS_OK ||
            !exfat_name_equal_ascii_ci(dirent.name, "CROSS.TXT") ||
            dirent.size != 700u ||
            vfs_dir_read(&dir, &dirent) != PROS_VFS_ENOENT ||
            vfs_dir_close(&dir) != PROS_VFS_OK)
            return -16;

        if (vfs_mount_deactivate(&fs.mount) != PROS_VFS_OK)
            return -17;
    }

    return 0;
}
