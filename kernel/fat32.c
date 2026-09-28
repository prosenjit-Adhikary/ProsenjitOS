#include "fat32.h"

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

int fat32_mount(struct pros_fat32 *fs, struct pros_block_device *device)
{
    uint8_t bpb[4096];
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved;
    uint32_t fat_count;
    uint32_t fat_sectors;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint64_t data_lba;
    uint64_t data_sectors;
    uint64_t cluster_count;
    uint64_t fat_entries;

    if (!fs || !device || device->sector_size < 512u ||
        device->sector_size > sizeof(bpb))
        return PROS_FAT32_EINVAL;

    if (block_device_read(device, 0, 1, bpb) != PROS_BLOCK_OK)
        return PROS_FAT32_EIO;

    if (le16(&bpb[510]) != 0xAA55u)
        return PROS_FAT32_ENOTSUP;

    bytes_per_sector = le16(&bpb[11]);
    sectors_per_cluster = bpb[13];
    reserved = le16(&bpb[14]);
    fat_count = bpb[16];
    total_sectors = le32(&bpb[32]);
    fat_sectors = le32(&bpb[36]);
    root_cluster = le32(&bpb[44]);

    if (bytes_per_sector != device->sector_size ||
        !power_of_two(bytes_per_sector) ||
        bytes_per_sector > 4096u ||
        !power_of_two(sectors_per_cluster) ||
        reserved == 0u || fat_count == 0u || total_sectors == 0u ||
        fat_sectors == 0u || root_cluster < 2u)
        return PROS_FAT32_ECORRUPT;

    data_lba = (uint64_t)reserved +
               (uint64_t)fat_count * fat_sectors;
    if (data_lba >= total_sectors ||
        (uint64_t)total_sectors > device->sector_count)
        return PROS_FAT32_ECORRUPT;

    data_sectors = (uint64_t)total_sectors - data_lba;
    cluster_count = data_sectors / sectors_per_cluster;

    /* FAT32 is defined for 65,525 through 0x0FFFFFF5 data clusters. */
    if (cluster_count < 65525u || cluster_count > 0x0FFFFFF5ULL ||
        (uint64_t)root_cluster >= cluster_count + 2u)
        return PROS_FAT32_ENOTSUP;

    fat_entries = ((uint64_t)fat_sectors * bytes_per_sector) / 4u;
    if (fat_entries < cluster_count + 2u)
        return PROS_FAT32_ECORRUPT;

    if (vfs_mount_init(&fs->mount, PROS_FS_FAT32, device, fs, 1u) !=
        PROS_VFS_OK)
        return PROS_FAT32_EINVAL;

    fs->bytes_per_sector = bytes_per_sector;
    fs->sectors_per_cluster = sectors_per_cluster;
    fs->reserved_sectors = reserved;
    fs->fat_sectors = fat_sectors;
    fs->root_cluster = root_cluster;
    fs->fat_lba = reserved;
    fs->data_lba = data_lba;
    fs->cluster_count = (uint32_t)cluster_count;

    if (vfs_mount_activate(&fs->mount) != PROS_VFS_OK)
        return PROS_FAT32_EINVAL;
    return fat32_vfs_mount(fs);
}

int fat32_cluster_lba(const struct pros_fat32 *fs,
                      uint32_t cluster,
                      uint64_t *lba)
{
    uint64_t offset;
    uint64_t available;

    if (!fs || !lba || !fs->mount.device || !fs->mount.active ||
        cluster < 2u || (uint64_t)cluster >= (uint64_t)fs->cluster_count + 2u)
        return PROS_FAT32_EINVAL;

    if (fs->data_lba >= fs->mount.device->sector_count)
        return PROS_FAT32_EIO;

    offset = (uint64_t)(cluster - 2u) * fs->sectors_per_cluster;
    available = fs->mount.device->sector_count - fs->data_lba;
    if (offset >= available ||
        fs->sectors_per_cluster > available - offset)
        return PROS_FAT32_EIO;

    *lba = fs->data_lba + offset;
    return PROS_FAT32_OK;
}

int fat32_read_cluster(const struct pros_fat32 *fs,
                       uint32_t cluster,
                       void *buffer)
{
    uint64_t lba;

    if (!fs || !buffer || !fs->mount.active)
        return PROS_FAT32_EINVAL;
    if (fat32_cluster_lba(fs, cluster, &lba) != PROS_FAT32_OK)
        return PROS_FAT32_EINVAL;

    return block_device_read(fs->mount.device, lba,
                             fs->sectors_per_cluster, buffer) ==
                   PROS_BLOCK_OK
        ? PROS_FAT32_OK : PROS_FAT32_EIO;
}

struct fat32_test_disk {
    uint8_t boot[512];
    uint8_t root[512];
};

static int fat32_test_read(struct pros_block_device *device,
                           uint64_t lba,
                           uint32_t count,
                           void *buffer)
{
    struct fat32_test_disk *disk =
        (struct fat32_test_disk *)device->private_data;

    if (!disk || !buffer || count == 0u)
        return PROS_BLOCK_EIO;

    for (uint32_t sector = 0; sector < count; ++sector) {
        uint8_t *dst = (uint8_t *)buffer + sector * 512u;
        uint64_t current = lba + sector;

        for (uint32_t i = 0; i < 512u; ++i)
            dst[i] = 0;

        if (current == 0u) {
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->boot[i];
        } else if (current == 16032u) {
            for (uint32_t i = 0; i < 512u; ++i)
                dst[i] = disk->root[i];
        }
    }

    return PROS_BLOCK_OK;
}

int fat32_selftest(void)
{
    struct fat32_test_disk disk = { {0}, {0} };
    struct pros_block_device device;
    struct pros_fat32 fs;
    uint8_t root[512];
    uint64_t lba = 0;

    /* 1,000,000-sector synthetic FAT32 geometry. */
    disk.boot[11] = 0x00; disk.boot[12] = 0x02; /* 512 bytes/sector */
    disk.boot[13] = 0x01;                         /* 1 sector/cluster */
    disk.boot[14] = 0x20; disk.boot[15] = 0x00; /* 32 reserved sectors */
    disk.boot[16] = 0x02;                         /* two FATs */
    disk.boot[32] = 0x40; disk.boot[33] = 0x42;
    disk.boot[34] = 0x0F; disk.boot[35] = 0x00; /* 1,000,000 sectors */
    disk.boot[36] = 0x40; disk.boot[37] = 0x1F; /* 8,000 sectors/FAT */
    disk.boot[44] = 0x02;                         /* root cluster */
    disk.boot[510] = 0x55; disk.boot[511] = 0xAA;
    disk.root[0] = 'P'; disk.root[1] = 'R';
    disk.root[2] = 'O'; disk.root[3] = 'S';

    if (block_device_init(&device, 512, 1000000u,
                          fat32_test_read, 0, &disk) != PROS_BLOCK_OK ||
        fat32_mount(&fs, &device) != PROS_FAT32_OK ||
        fs.fat_lba != 32u || fs.data_lba != 16032u ||
        fs.root_cluster != 2u ||
        fat32_cluster_lba(&fs, 2u, &lba) != PROS_FAT32_OK ||
        lba != 16032u ||
        fat32_read_cluster(&fs, 2u, root) != PROS_FAT32_OK ||
        root[0] != 'P' || root[1] != 'R' ||
        root[2] != 'O' || root[3] != 'S')
        return -1;

    if (fat32_cluster_lba(&fs, 0u, &lba) == PROS_FAT32_OK ||
        fat32_cluster_lba(&fs, fs.cluster_count + 2u, &lba) ==
            PROS_FAT32_OK)
        return -1;

    return 0;
}

int fat32_next_cluster(const struct pros_fat32 *fs, uint32_t cluster,
                       uint32_t *next)
{
    uint8_t sector[4096];
    uint64_t fat_offset;
    uint64_t lba;
    uint32_t offset;
    uint32_t value;

    if (!fs || !next || !fs->mount.active || cluster < 2u ||
        (uint64_t)cluster >= (uint64_t)fs->cluster_count + 2u)
        return PROS_FAT32_EINVAL;

    fat_offset = (uint64_t)cluster * 4u;
    lba = fs->fat_lba + fat_offset / fs->bytes_per_sector;
    offset = (uint32_t)(fat_offset % fs->bytes_per_sector);
    if (offset + 4u > fs->bytes_per_sector)
        return PROS_FAT32_EIO;
    if (block_device_read(fs->mount.device, lba, 1, sector) != PROS_BLOCK_OK)
        return PROS_FAT32_EIO;

    value = le32(&sector[offset]) & 0x0FFFFFFFu;
    if (value >= 0x0FFFFFF8u) {
        *next = 0x0FFFFFFFu;
        return PROS_FAT32_OK;
    }
    if (value < 2u || value >= (uint32_t)fs->cluster_count + 2u)
        return PROS_FAT32_ECORRUPT;

    *next = value;
    return PROS_FAT32_OK;
}

static void fat32_name83(const uint8_t *entry, char *name)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8u && entry[i] != ' '; ++i)
        name[n++] = (char)entry[i];
    if (entry[8] != ' ') {
        name[n++] = '.';
        for (uint32_t i = 8u; i < 11u && entry[i] != ' '; ++i)
            name[n++] = (char)entry[i];
    }
    name[n] = 0;
}

int fat32_find_83(const struct pros_fat32 *fs, uint32_t directory_cluster,
                  const char *name, struct pros_fat32_dirent *result)
{
    uint8_t cluster[32768];
    uint32_t cluster_size;
    uint32_t current;
    uint32_t guard = 0;

    if (!fs || !name || !result || !fs->mount.active ||
        fs->bytes_per_sector * fs->sectors_per_cluster > sizeof(cluster))
        return PROS_FAT32_EINVAL;

    cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    current = directory_cluster;
    while (current >= 2u && current < fs->cluster_count + 2u &&
           guard++ <= fs->cluster_count) {
        if (fat32_read_cluster(fs, current, cluster) != PROS_FAT32_OK)
            return PROS_FAT32_EIO;

        for (uint32_t off = 0; off + 32u <= cluster_size; off += 32u) {
            const uint8_t *entry = &cluster[off];
            char parsed[12];

            if (entry[0] == 0x00)
                return PROS_FAT32_ENOENT;
            if (entry[0] == 0xE5 || entry[11] == 0x0Fu)
                continue;
            if ((entry[11] & 0x08u) != 0u)
                continue;

            fat32_name83(entry, parsed);
            uint32_t i = 0;
            while (parsed[i] && name[i] && parsed[i] == name[i]) ++i;
            if (parsed[i] != name[i])
                continue;

            for (i = 0; i < sizeof(result->name); ++i)
                result->name[i] = 0;
            for (i = 0; i < sizeof(result->name) - 1u && parsed[i]; ++i)
                result->name[i] = parsed[i];
            result->attributes = entry[11];
            result->first_cluster = ((uint32_t)le16(&entry[20]) << 16) |
                                    le16(&entry[26]);
            result->size = le32(&entry[28]);
            if (result->first_cluster < 2u ||
                (uint64_t)result->first_cluster >=
                    (uint64_t)fs->cluster_count + 2u)
                return PROS_FAT32_ECORRUPT;
            return PROS_FAT32_OK;
        }

        uint32_t next;
        if (fat32_next_cluster(fs, current, &next) != PROS_FAT32_OK)
            return PROS_FAT32_EIO;
        if (next >= 0x0FFFFFF8u)
            return PROS_FAT32_ENOENT;
        current = next;
    }
    return PROS_FAT32_ECORRUPT;
}

static uint8_t fat32_lfn_checksum(const uint8_t *entry)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11u; ++i)
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + entry[i]);
    return sum;
}

static uint16_t fat32_lfn_char(const uint8_t *entry, uint32_t index)
{
    if (index < 5u)
        return le16(&entry[1u + index * 2u]);
    if (index < 11u)
        return le16(&entry[14u + (index - 5u) * 2u]);
    return le16(&entry[28u + (index - 11u) * 2u]);
}

static int fat32_name_equal(const char *a, const char *b)
{
    uint32_t i = 0;
    while (a[i] && b[i]) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb)
            return 0;
        ++i;
    }
    return a[i] == b[i];
}

static int fat32_lfn_build(const uint8_t *entries, uint32_t count,
                           char *name, uint32_t capacity, uint8_t checksum)
{
    uint32_t max_chars = (capacity - 1u < 260u) ? capacity - 1u : 260u;
    uint32_t total = 0;
    uint32_t expected = 0;
    uint32_t ordinal_seen[20] = {0};

    if (!entries || !name || capacity < 2u || count == 0u ||
        count > 20u || max_chars == 0u)
        return PROS_FAT32_EINVAL;

    /* entries are stored from highest ordinal down to 1. */
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = entries + i * 32u;
        uint8_t ord = e[0] & 0x1Fu;
        if (ord == 0u || ord > count || ordinal_seen[ord] != 0u ||
            e[11] != 0x0Fu || e[13] != checksum)
            return PROS_FAT32_ECORRUPT;
        ordinal_seen[ord] = 1u;
        if (i == 0u) {
            if ((e[0] & 0x40u) == 0u)
                return PROS_FAT32_ECORRUPT;
            expected = ord;
        } else if ((e[0] & 0x40u) != 0u || ord + i != expected)
            return PROS_FAT32_ECORRUPT;
        total += 13u;
    }
    if (expected != count || total > 260u)
        return PROS_FAT32_ECORRUPT;

    uint32_t out = 0;
    for (uint32_t ordinal = 1u; ordinal <= count; ++ordinal) {
        const uint8_t *e = entries + (count - ordinal) * 32u;
        for (uint32_t j = 0; j < 13u; ++j) {
            uint16_t ch = fat32_lfn_char(e, j);
            if (ch == 0x0000u || ch == 0xFFFFu)
                break;
            if (ch > 0x007Fu || out + 1u >= capacity)
                return PROS_FAT32_ENOTSUP;
            name[out++] = (char)ch;
        }
    }
    name[out] = 0;
    return out != 0u ? PROS_FAT32_OK : PROS_FAT32_ECORRUPT;
}

int fat32_find(const struct pros_fat32 *fs, uint32_t directory_cluster,
               const char *name, struct pros_fat32_dirent *result)
{
    uint8_t cluster[32768];
    uint8_t lfn[20u * 32u];
    char parsed[261];
    uint32_t cluster_size;
    uint32_t current;
    uint32_t guard = 0;
    uint32_t lfn_count = 0;
    uint8_t lfn_checksum = 0;

    if (!fs || !name || !result || !fs->mount.active ||
        fs->sectors_per_cluster == 0u ||
        (uint64_t)fs->bytes_per_sector * fs->sectors_per_cluster >
            sizeof(cluster))
        return PROS_FAT32_EINVAL;

    cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    current = directory_cluster;

    while (current >= 2u && (uint64_t)current < (uint64_t)fs->cluster_count + 2u &&
           guard++ <= fs->cluster_count) {
        if (fat32_read_cluster(fs, current, cluster) != PROS_FAT32_OK)
            return PROS_FAT32_EIO;

        for (uint32_t off = 0; off + 32u <= cluster_size; off += 32u) {
            const uint8_t *entry = &cluster[off];

            if (entry[0] == 0x00)
                return PROS_FAT32_ENOENT;
            if (entry[0] == 0xE5) {
                lfn_count = 0;
                continue;
            }

            if (entry[11] == 0x0Fu) {
                if (lfn_count < 20u) {
                    for (uint32_t j = 0; j < 32u; ++j)
                        lfn[lfn_count * 32u + j] = entry[j];
                    if (lfn_count == 0u)
                        lfn_checksum = entry[13];
                    ++lfn_count;
                } else {
                    lfn_count = 0;
                }
                continue;
            }

            if ((entry[11] & 0x08u) != 0u) {
                lfn_count = 0;
                continue;
            }

            int matched = 0;
            if (lfn_count != 0u &&
                fat32_lfn_build(lfn, lfn_count, parsed, sizeof(parsed),
                                lfn_checksum) == PROS_FAT32_OK) {
                if (fat32_lfn_checksum(entry) == lfn_checksum &&
                    fat32_name_equal(parsed, name))
                    matched = 1;
            }
            if (!matched) {
                char short_name[12];
                fat32_name83(entry, short_name);
                if (fat32_name_equal(short_name, name))
                    matched = 1;
            }
            lfn_count = 0;

            if (!matched)
                continue;

            for (uint32_t i = 0; i < sizeof(result->name); ++i)
                result->name[i] = 0;
            const char *source = parsed[0] != 0 ? parsed : "";
            if (!fat32_name_equal(source, name)) {
                fat32_name83(entry, parsed);
                source = parsed;
            }
            uint32_t i = 0;
            while (source[i] && i + 1u < sizeof(result->name)) {
                result->name[i] = source[i];
                ++i;
            }
            result->attributes = entry[11];
            result->first_cluster = ((uint32_t)le16(&entry[20]) << 16) |
                                    le16(&entry[26]);
            result->size = le32(&entry[28]);
            if ((result->attributes & 0x10u) == 0u &&
                (result->size != 0u &&
                 (result->first_cluster < 2u ||
                  (uint64_t)result->first_cluster >=
                      (uint64_t)fs->cluster_count + 2u)))
                return PROS_FAT32_ECORRUPT;
            return PROS_FAT32_OK;
        }

        uint32_t next;
        if (fat32_next_cluster(fs, current, &next) != PROS_FAT32_OK)
            return PROS_FAT32_EIO;
        if (next >= 0x0FFFFFF8u)
            return PROS_FAT32_ENOENT;
        current = next;
    }
    return PROS_FAT32_ECORRUPT;
}

int fat32_read_file(const struct pros_fat32 *fs,
                    const struct pros_fat32_dirent *file,
                    uint32_t offset, uint32_t length, void *buffer)
{
    uint8_t cluster[32768];
    uint32_t cluster_size;
    uint32_t current;
    uint32_t skip;
    uint32_t remaining;
    uint8_t *out;

    if (!fs || !file || !buffer || !fs->mount.active ||
        (uint64_t)fs->bytes_per_sector * fs->sectors_per_cluster >
            sizeof(cluster) || offset > file->size)
        return PROS_FAT32_EINVAL;
    if ((uint64_t)offset + length > file->size)
        return PROS_FAT32_EINVAL;
    if (length == 0u)
        return PROS_FAT32_OK;
    if (file->first_cluster < 2u ||
        (uint64_t)file->first_cluster >= (uint64_t)fs->cluster_count + 2u)
        return PROS_FAT32_ECORRUPT;

    cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    current = file->first_cluster;
    skip = offset / cluster_size;
    remaining = length;
    out = (uint8_t *)buffer;

    while (skip != 0u) {
        uint32_t next;
        if (fat32_next_cluster(fs, current, &next) != PROS_FAT32_OK ||
            next >= 0x0FFFFFF8u)
            return PROS_FAT32_ECORRUPT;
        current = next;
        --skip;
    }

    while (remaining != 0u) {
        uint32_t in_cluster = offset % cluster_size;
        uint32_t take = cluster_size - in_cluster;
        if (take > remaining)
            take = remaining;

        if (fat32_read_cluster(fs, current, cluster) != PROS_FAT32_OK)
            return PROS_FAT32_EIO;
        for (uint32_t i = 0; i < take; ++i)
            out[i] = cluster[in_cluster + i];

        out += take;
        remaining -= take;
        offset += take;
        if (remaining != 0u) {
            uint32_t next;
            if (fat32_next_cluster(fs, current, &next) != PROS_FAT32_OK ||
                next >= 0x0FFFFFF8u)
                return PROS_FAT32_ECORRUPT;
            current = next;
        }
    }
    return PROS_FAT32_OK;
}

static int fat32_vfs_read(struct pros_vfs_file *, uint32_t, uint32_t, void *);
static int fat32_vfs_close(struct pros_vfs_file *);
static const struct pros_vfs_file_ops fat32_vfs_ops = { .read = fat32_vfs_read, .close = fat32_vfs_close };

static int fat32_vfs_lookup(struct pros_vfs_vnode *parent, const char *name, struct pros_vfs_vnode **result);
static int fat32_vfs_readdir(struct pros_vfs_vnode *vnode, uint64_t *cookie, struct pros_vfs_dirent *entry);
static const struct pros_vfs_vnode_ops fat32_vfs_vnode_ops = { .lookup = fat32_vfs_lookup, .readdir = fat32_vfs_readdir };

static int fat32_vfs_node_create(struct pros_fat32 *fs, struct pros_fat32_vfs_node *parent, const struct pros_fat32_dirent *entry, struct pros_vfs_vnode **result)
{
    if (!fs || !parent || !entry || !result) return PROS_VFS_EINVAL;
    for (uint32_t i=0;i<PROS_FAT32_VFS_NODE_MAX;++i) {
        struct pros_fat32_vfs_node *n=&fs->vfs_nodes[i];
        if (n->used && n->directory_cluster==parent->entry.first_cluster && fat32_name_equal(n->entry.name,entry->name)) { *result=&n->vnode; return PROS_VFS_OK; }
    }
    for (uint32_t i=0;i<PROS_FAT32_VFS_NODE_MAX;++i) {
        struct pros_fat32_vfs_node *n=&fs->vfs_nodes[i];
        if (n->used) continue;
        n->entry=*entry; n->directory_cluster=parent->entry.first_cluster; n->used=1u;
        enum pros_vfs_node_type type=(entry->attributes&0x10u)?PROS_VFS_NODE_DIRECTORY:PROS_VFS_NODE_FILE;
        int rc=vfs_vnode_init(&n->vnode,&fs->mount,&parent->vnode,type,&fat32_vfs_vnode_ops,type==PROS_VFS_NODE_FILE?&fat32_vfs_ops:0,n,entry->size);
        if (rc!=PROS_VFS_OK) { n->used=0u; return rc; }
        *result=&n->vnode; return PROS_VFS_OK;
    }
    return PROS_VFS_EBUSY;
}

static int fat32_vfs_lookup(struct pros_vfs_vnode *parent, const char *name, struct pros_vfs_vnode **result)
{
    struct pros_fat32_vfs_node *n; struct pros_fat32 *fs; struct pros_fat32_dirent found;
    if (!parent || !name || !result || parent->type!=PROS_VFS_NODE_DIRECTORY) return PROS_VFS_EINVAL;
    n=(struct pros_fat32_vfs_node *)parent->private_data; fs=n?(struct pros_fat32 *)parent->mount->private_data:0;
    if (!n || !fs) return PROS_VFS_EINVAL;
    int rc=fat32_find(fs,n->entry.first_cluster,name,&found);
    if (rc == PROS_FAT32_ENOENT) return PROS_VFS_ENOENT;
    if (rc != PROS_FAT32_OK) return PROS_VFS_EIO;
    return fat32_vfs_node_create(fs,n,&found,result);
}

static int fat32_vfs_readdir(struct pros_vfs_vnode *vnode, uint64_t *cookie, struct pros_vfs_dirent *entry)
{
    struct pros_fat32_vfs_node *dir; struct pros_fat32 *fs; uint8_t cluster[32768];
    uint32_t cluster_size,current,guard=0; uint64_t index=0;
    if (!vnode || !cookie || !entry || vnode->type!=PROS_VFS_NODE_DIRECTORY) return PROS_VFS_EINVAL;
    dir=(struct pros_fat32_vfs_node *)vnode->private_data; fs=dir?(struct pros_fat32 *)vnode->mount->private_data:0;
    if (!fs || (uint64_t)fs->bytes_per_sector*fs->sectors_per_cluster>sizeof(cluster)) return PROS_VFS_EINVAL;
    cluster_size=fs->bytes_per_sector*fs->sectors_per_cluster; current=dir->entry.first_cluster;
    while(current>=2u && (uint64_t)current<(uint64_t)fs->cluster_count+2u && guard++<=fs->cluster_count) {
        if(fat32_read_cluster(fs,current,cluster)!=PROS_FAT32_OK) return PROS_VFS_EIO;
        for(uint32_t off=0;off+32u<=cluster_size;off+=32u) {
            const uint8_t *raw=&cluster[off]; if(raw[0]==0x00) return 0;
            if(raw[0]==0xE5 || raw[11]==0x0Fu || (raw[11]&0x08u)) continue;
            if(index++<*cookie) continue;
            char short_name[12]; fat32_name83(raw,short_name); uint32_t i=0;
            while(short_name[i] && i<PROS_VFS_NAME_MAX){entry->name[i]=short_name[i];++i;} entry->name[i]=0;
            entry->type=(raw[11]&0x10u)?PROS_VFS_NODE_DIRECTORY:PROS_VFS_NODE_FILE; entry->size=le32(&raw[28]); *cookie=index; return 1;
        }
        uint32_t next; if(fat32_next_cluster(fs,current,&next)!=PROS_FAT32_OK) return PROS_VFS_EIO; if(next>=0x0FFFFFF8u) return 0; current=next;
    } return PROS_VFS_EIO;
}

int fat32_vfs_mount(struct pros_fat32 *fs)
{
    if(!fs || !fs->mount.active) return PROS_FAT32_EINVAL;
    for(uint32_t i=0;i<PROS_FAT32_VFS_NODE_MAX;++i) fs->vfs_nodes[i].used=0u;
    struct pros_fat32_vfs_node *root=&fs->vfs_nodes[0];
    root->used=1u; root->entry.name[0]='/'; root->entry.name[1]=0; root->entry.attributes=0x10u; root->entry.first_cluster=fs->root_cluster; root->entry.size=0; root->directory_cluster=fs->root_cluster;
    if(vfs_vnode_init(&root->vnode,&fs->mount,0,PROS_VFS_NODE_DIRECTORY,&fat32_vfs_vnode_ops,0,root,0)!=PROS_VFS_OK) return PROS_FAT32_EINVAL;
    return vfs_mount_set_root(&fs->mount,&root->vnode)==PROS_VFS_OK?PROS_FAT32_OK:PROS_FAT32_EINVAL;
}

static int fat32_vfs_read(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, void *buffer)
{
    struct pros_fat32_dirent *entry =
        (struct pros_fat32_dirent *)file->private_data;
    uint32_t available;
    if (!entry || offset > entry->size)
        return PROS_VFS_EINVAL;
    available = entry->size - offset;
    if (length > available)
        length = available;
    if (fat32_read_file(file->mount->private_data, entry, offset, length,
                         buffer) != PROS_FAT32_OK)
        return PROS_VFS_EIO;
    return (int)length;
}

static int fat32_vfs_close(struct pros_vfs_file *file)
{
    (void)file;
    return PROS_VFS_OK;
}

int fat32_open_83(struct pros_fat32 *fs, uint32_t directory_cluster,
                  const char *name, struct pros_vfs_file *file,
                  struct pros_fat32_dirent *entry)
{
    if (!fs || !file || !entry || !name || !fs->mount.active)
        return PROS_FAT32_EINVAL;
    if (fat32_find(fs, directory_cluster, name, entry) != PROS_FAT32_OK)
        return PROS_FAT32_ENOENT;
    if ((entry->attributes & 0x10u) != 0u)
        return PROS_FAT32_EINVAL;
    if (vfs_file_init(file, &fs->mount, &fat32_vfs_ops, entry, 0u) !=
        PROS_VFS_OK)
        return PROS_FAT32_EINVAL;
    return PROS_FAT32_OK;
}
