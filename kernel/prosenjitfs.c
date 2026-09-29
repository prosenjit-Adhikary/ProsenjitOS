#include "prosenjitfs.h"
#include "vfs.h"
#include "heap.h"
#include <stddef.h>

#define PFS_MIN_BLOCKS 256u
#define PFS_MIN_JOURNAL 16u
#define PFS_MAX_JOURNAL 64u
#define PFS_MAX_INODES 1024u
#define PFS_MIN_INODES 64u
#define PFS_UUID_SEED 0x50524F53454E4A49ULL

static void pfs_zero(void *buffer, uint32_t size)
{
    uint8_t *p = (uint8_t *)buffer;
    for (uint32_t i = 0; i < size; ++i)
        p[i] = 0;
}

static void pfs_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}


static int pfs_mount_debug_stage;
static int pfs_journal_debug_stage;

static int pfs_mem_equal(const void *a, const void *b, uint32_t size)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint32_t i = 0u; i < size; ++i)
        if (pa[i] != pb[i])
            return (pa[i] < pb[i]) ? -1 : 1;
    return 0;
}

static void pfs_copy(void *dst, const void *src, uint32_t size)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < size; ++i)
        d[i] = s[i];
}

static uint32_t pfs_crc_update(uint32_t crc, const uint8_t *data,
                               uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    return crc;
}

uint32_t prosenjitfs_crc32c(const void *data, uint32_t length)
{
    return ~pfs_crc_update(0xFFFFFFFFu, (const uint8_t *)data, length);
}

static uint32_t pfs_crc_with_zero32(const uint8_t *data, uint32_t length,
                                    uint32_t checksum_offset)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t value = (i >= checksum_offset && i < checksum_offset + 4u)
                      ? 0u : data[i];
        crc ^= value;
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    return ~crc;
}

static uint32_t pfs_region_end(uint64_t start, uint32_t blocks,
                               uint64_t *end)
{
    if (!end || start > UINT64_MAX - (uint64_t)blocks)
        return PROSENJITFS_EOVERFLOW;
    *end = start + blocks;
    return PROSENJITFS_OK;
}

static int pfs_region_valid(const struct pros_prosenjitfs_superblock *sb,
                            uint64_t start, uint32_t count)
{
    uint64_t end;
    if (!sb || count == 0u ||
        pfs_region_end(start, count, &end) != PROSENJITFS_OK ||
        start < 2u || end > sb->total_blocks)
        return 0;
    return 1;
}

static uint64_t pfs_data_start(const struct pros_prosenjitfs_superblock *sb)
{
    return sb ? sb->data_start : UINT64_MAX;
}

static int pfs_read_block(struct pros_block_device *device, uint64_t block,
                          void *buffer)
{
    if (!device || !buffer || block >= device->sector_count)
        return PROSENJITFS_EINVAL;
    return block_device_read(device, block, 1u, buffer) == PROS_BLOCK_OK
         ? PROSENJITFS_OK : PROSENJITFS_EIO;
}

static int pfs_write_block(struct pros_block_device *device, uint64_t block,
                           const void *buffer)
{
    if (!device || !buffer || block >= device->sector_count)
        return PROSENJITFS_EINVAL;
    return block_device_write(device, block, 1u, buffer) == PROS_BLOCK_OK
         ? PROSENJITFS_OK : PROSENJITFS_EIO;
}

static int pfs_flush(struct pros_block_device *device)
{
    return block_device_flush(device) == PROS_BLOCK_OK
         ? PROSENJITFS_OK : PROSENJITFS_EIO;
}

static int pfs_superblock_decode(const uint8_t *block,
                                 struct pros_prosenjitfs_superblock *sb)
{
    if (!block || !sb)
        return PROSENJITFS_EINVAL;
    pfs_zero(sb, sizeof(*sb));
    pfs_copy(sb, block, sizeof(*sb));
    return PROSENJITFS_OK;
}

static int pfs_superblock_valid(const uint8_t *block,
                                uint32_t block_size,
                                struct pros_prosenjitfs_superblock *sb)
{
    uint32_t checksum;
    if (!block || !sb || block_size < sizeof(*sb))
        return PROSENJITFS_ECORRUPT;
    if (pfs_superblock_decode(block, sb) != PROSENJITFS_OK)
        return PROSENJITFS_ECORRUPT;
    if (sb->magic[0] != 'P' || sb->magic[1] != 'J' ||
        sb->magic[2] != 'S' || sb->magic[3] != 'F' ||
        sb->magic[4] != 'S' || sb->magic[5] != '0' ||
        sb->magic[6] != '1' || sb->magic[7] != 0)
        return PROSENJITFS_ECORRUPT;
    if (sb->version != PROSENJITFS_VERSION ||
        sb->header_size != sizeof(*sb) ||
        sb->header_size > block_size ||
        sb->block_size != block_size ||
        sb->block_size < 512u || sb->block_size > 4096u ||
        (sb->block_size & (sb->block_size - 1u)) != 0u ||
        sb->total_blocks < PFS_MIN_BLOCKS ||
        sb->root_inode != 0u ||
        sb->inode_size != PROSENJITFS_INODE_SIZE ||
        sb->inode_count < PFS_MIN_INODES ||
        sb->inode_count > PFS_MAX_INODES ||
        sb->max_name != PROSENJITFS_MAX_NAME ||
        (sb->features & (PROSENJITFS_FEATURE_CRC32C |
                         PROSENJITFS_FEATURE_JOURNAL |
                         PROSENJITFS_FEATURE_EXTENTS |
                         PROSENJITFS_FEATURE_UTF8)) !=
                         (PROSENJITFS_FEATURE_CRC32C |
                          PROSENJITFS_FEATURE_JOURNAL |
                          PROSENJITFS_FEATURE_EXTENTS |
                          PROSENJITFS_FEATURE_UTF8))
        return PROSENJITFS_ECORRUPT;
    checksum = pfs_crc_with_zero32(block, sb->header_size,
                                   (uint32_t)offsetof(
                                       struct pros_prosenjitfs_superblock,
                                       checksum));
    if (checksum != sb->checksum)
        return PROSENJITFS_ECORRUPT;
    if (!pfs_region_valid(sb, sb->journal_start, sb->journal_blocks) ||
        !pfs_region_valid(sb, sb->block_bitmap_start, sb->block_bitmap_blocks) ||
        !pfs_region_valid(sb, sb->inode_bitmap_start, sb->inode_bitmap_blocks) ||
        !pfs_region_valid(sb, sb->inode_table_start, sb->inode_table_blocks))
        return PROSENJITFS_ECORRUPT;
    uint64_t inode_end;
    if (pfs_region_end(sb->inode_table_start, sb->inode_table_blocks, &inode_end) != PROSENJITFS_OK ||
        sb->data_start >= sb->total_blocks || sb->data_start != inode_end + 1u)
        return PROSENJITFS_ECORRUPT;
    if ((uint64_t)sb->inode_table_blocks * sb->block_size <
        (uint64_t)sb->inode_count * PROSENJITFS_INODE_SIZE)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static void pfs_make_uuid(uint8_t uuid[16], uint64_t blocks,
                          uint32_t block_size)
{
    uint64_t a = PFS_UUID_SEED ^ blocks ^ ((uint64_t)block_size << 32);
    uint64_t b = 0x9E3779B97F4A7C15ULL ^ (blocks << 7) ^ block_size;
    for (uint32_t i = 0; i < 16; ++i) {
        a ^= a << 13; a ^= a >> 7; a ^= a << 17;
        b ^= b << 17; b ^= b >> 9; b ^= b << 8;
        uuid[i] = (uint8_t)(a ^ b);
    }
    uuid[6] = (uint8_t)((uuid[6] & 0x0Fu) | 0x40u);
    uuid[8] = (uint8_t)((uuid[8] & 0x3Fu) | 0x80u);
}

static void pfs_mark_bit(uint8_t *bitmap, uint64_t bit)
{
    bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

static int pfs_bitmap_region_build(uint8_t *bitmap, uint32_t bytes,
                                   uint64_t region_start, uint32_t region_blocks,
                                   uint32_t bitmap_index, uint32_t block_size)
{
    uint64_t end;
    uint64_t bits_per_block = (uint64_t)block_size * 8u;
    uint64_t bitmap_base = (uint64_t)bitmap_index * bits_per_block;
    if (!bitmap || pfs_region_end(region_start, region_blocks, &end) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    for (uint64_t b = region_start; b < end; ++b) {
        if (b < bitmap_base || b >= bitmap_base + bits_per_block)
            continue;
        uint64_t local = b - bitmap_base;
        if ((local >> 3) >= bytes)
            return PROSENJITFS_EOVERFLOW;
        pfs_mark_bit(bitmap, local);
    }
    return PROSENJITFS_OK;
}

int prosenjitfs_probe(struct pros_block_device *device)
{
    uint8_t *block;
    struct pros_prosenjitfs_superblock sb;
    if (!device || device->sector_size < 512u ||
        device->sector_size > 4096u ||
        (device->sector_size & (device->sector_size - 1u)) != 0u ||
        device->sector_count < PFS_MIN_BLOCKS)
        return PROSENJITFS_EINVAL;
    block = (uint8_t *)kmalloc(device->sector_size);
    if (!block)
        return PROSENJITFS_EIO;
    if (pfs_read_block(device, 0u, block) != PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_EIO;
    }
    if (pfs_superblock_valid(block, device->sector_size, &sb) == PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_OK;
    }
    if (pfs_read_block(device, 1u, block) != PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_EIO;
    }
    if (pfs_superblock_valid(block, device->sector_size, &sb) == PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_OK;
    }
    kfree(block);
    return PROSENJITFS_ENOTSUP;
}

int prosenjitfs_format(struct pros_block_device *device)
{
    uint32_t block_size;
    uint64_t total;
    uint32_t journal_blocks;
    uint32_t bitmap_blocks;
    uint32_t inode_bitmap_blocks;
    uint32_t inode_count;
    uint32_t inode_table_blocks;
    uint64_t cursor;
    uint8_t *block;
    struct pros_prosenjitfs_superblock sb;
    struct pros_prosenjitfs_inode root;
    struct pros_prosenjitfs_dir_index dir_index;

    if (!device || !device->write)
        return PROSENJITFS_EINVAL;
    block_size = device->sector_size;
    total = device->sector_count;
    if (block_size < 512u || block_size > 4096u ||
        (block_size & (block_size - 1u)) != 0u ||
        total < PFS_MIN_BLOCKS)
        return PROSENJITFS_EINVAL;

    journal_blocks = (uint32_t)(total / 16u);
    if (journal_blocks < PFS_MIN_JOURNAL) journal_blocks = PFS_MIN_JOURNAL;
    if (journal_blocks > PFS_MAX_JOURNAL) journal_blocks = PFS_MAX_JOURNAL;

    bitmap_blocks = (uint32_t)((total + ((uint64_t)block_size * 8u) - 1u) /
                               ((uint64_t)block_size * 8u));
    inode_count = (uint32_t)(total / 32u);
    if (inode_count < PFS_MIN_INODES) inode_count = PFS_MIN_INODES;
    if (inode_count > PFS_MAX_INODES) inode_count = PFS_MAX_INODES;
    inode_bitmap_blocks =
        (uint32_t)(((uint64_t)inode_count + block_size * 8u - 1u) /
                   (block_size * 8u));
    inode_table_blocks =
        (uint32_t)(((uint64_t)inode_count * PROSENJITFS_INODE_SIZE +
                    block_size - 1u) / block_size);

    cursor = 2u;
    if (cursor + journal_blocks + bitmap_blocks + inode_bitmap_blocks +
        inode_table_blocks + 1u >= total)
        return PROSENJITFS_ENOSPC;
    pfs_zero(&sb, sizeof(sb));
    sb.journal_start = cursor; sb.journal_blocks = journal_blocks; cursor += journal_blocks;
    sb.block_bitmap_start = cursor; sb.block_bitmap_blocks = bitmap_blocks; cursor += bitmap_blocks;
    sb.inode_bitmap_start = cursor; sb.inode_bitmap_blocks = inode_bitmap_blocks; cursor += inode_bitmap_blocks;
    sb.inode_table_start = cursor; sb.inode_table_blocks = inode_table_blocks; cursor += inode_table_blocks;
    sb.data_start = cursor + 1u;
    pfs_copy(sb.magic, PROSENJITFS_MAGIC, PROSENJITFS_MAGIC_LEN);
    sb.version = PROSENJITFS_VERSION;
    sb.header_size = sizeof(sb);
    sb.features = PROSENJITFS_FEATURE_CRC32C |
                  PROSENJITFS_FEATURE_JOURNAL |
                  PROSENJITFS_FEATURE_EXTENTS |
                  PROSENJITFS_FEATURE_UTF8;
    pfs_make_uuid(sb.uuid, total, block_size);
    sb.block_size = block_size;
    sb.total_blocks = total;
    sb.inode_size = PROSENJITFS_INODE_SIZE;
    sb.root_inode = 0u;
    sb.inode_count = inode_count;
    sb.max_name = PROSENJITFS_MAX_NAME;
    sb.transaction_sequence = 1u;

    block = (uint8_t *)kmalloc(block_size);
    if (!block)
        return PROSENJITFS_EIO;

    pfs_zero(block, block_size);
    pfs_copy(block, &sb, sizeof(sb));
    sb.checksum = pfs_crc_with_zero32(block, sb.header_size,
                                      (uint32_t)offsetof(
                                          struct pros_prosenjitfs_superblock,
                                          checksum));
    pfs_copy(block, &sb, sizeof(sb));

    if (pfs_write_block(device, 0u, block) != PROSENJITFS_OK ||
        pfs_write_block(device, 1u, block) != PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_EIO;
    }

    pfs_zero(block, block_size);
    for (uint32_t i = 0; i < journal_blocks; ++i)
        if (pfs_write_block(device, sb.journal_start + i, block) != PROSENJITFS_OK) {
            kfree(block);
            return PROSENJITFS_EIO;
        }

    for (uint32_t i = 0; i < bitmap_blocks; ++i) {
        pfs_zero(block, block_size);
        if (pfs_bitmap_region_build(block, block_size,
                                    0u, (uint32_t)sb.data_start, i, block_size) != PROSENJITFS_OK) {
            kfree(block);
            return PROSENJITFS_EOVERFLOW;
        }
        if (pfs_write_block(device, sb.block_bitmap_start + i, block) != PROSENJITFS_OK) {
            kfree(block);
            return PROSENJITFS_EIO;
        }
    }

    pfs_zero(block, block_size);
    pfs_mark_bit(block, 0u);
    if (pfs_write_block(device, sb.inode_bitmap_start, block) != PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_EIO;
    }
    for (uint32_t i = 1u; i < inode_bitmap_blocks; ++i) {
        pfs_zero(block, block_size);
        if (pfs_write_block(device, sb.inode_bitmap_start + i, block) != PROSENJITFS_OK) {
            kfree(block);
            return PROSENJITFS_EIO;
        }
    }

    pfs_zero(&root, sizeof(root));
    root.type = PROSENJITFS_INODE_DIR;
    root.mode = 0755u;
    root.uid = 0u;
    root.gid = 0u;
    root.links = 1u;
    root.generation = 1u;
    root.parent_inode = 0u;
    root.dir_index = sb.data_start;
    root.ctime = 0u; root.mtime = 0u; root.atime = 0u;

    pfs_zero(block, block_size);
    pfs_copy(block, &root, sizeof(root));
    root.checksum = pfs_crc_with_zero32(block, sizeof(root),
                                        (uint32_t)offsetof(
                                            struct pros_prosenjitfs_inode,
                                            checksum));
    pfs_copy(block, &root, sizeof(root));
    if (pfs_write_block(device, sb.inode_table_start, block) != PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_EIO;
    }

    pfs_zero(&dir_index, sizeof(dir_index));
    dir_index.magic = PROSENJITFS_DIR_MAGIC;
    dir_index.version = PROSENJITFS_VERSION;
    dir_index.bucket_count = PROSENJITFS_DIR_BUCKETS;
    dir_index.inode = 0u;
    dir_index.generation = 1u;
    pfs_zero(block, block_size);
    pfs_copy(block, &dir_index, sizeof(dir_index));
    dir_index.checksum = pfs_crc_with_zero32(block, sizeof(dir_index),
                                             (uint32_t)offsetof(
                                                 struct pros_prosenjitfs_dir_index,
                                                 checksum));
    pfs_copy(block, &dir_index, sizeof(dir_index));
    if (pfs_write_block(device, root.dir_index, block) != PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_EIO;
    }

    if (pfs_flush(device) != PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_EIO;
    }
    kfree(block);
    return PROSENJITFS_OK;
}

struct pfs_vnode_data {
    struct pros_prosenjitfs *fs;
    uint64_t inode_number;
};

static int pfs_vnode_lookup(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result);
static int pfs_vnode_create(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result);
static int pfs_vnode_mkdir(struct pros_vfs_vnode *parent, const char *name,
                           struct pros_vfs_vnode **result);
static int pfs_vnode_truncate(struct pros_vfs_vnode *vnode, uint64_t size);
static int pfs_make_vnode(struct pros_prosenjitfs *fs,
                          uint64_t inode_number,
                          struct pros_vfs_vnode *parent,
                          struct pros_vfs_vnode **result);
static int pfs_read_dir_entry(const struct pros_prosenjitfs *fs,
                              uint64_t entry_lba,
                              struct pros_prosenjitfs_dir_entry *entry);
static int pfs_dir_entry_by_ordinal(const struct pros_prosenjitfs *fs,
                                    const struct pros_prosenjitfs_inode *dir_inode,
                                    uint64_t ordinal,
                                    struct pros_prosenjitfs_dir_entry *entry);
static int pfs_find_dir_entry_location(const struct pros_prosenjitfs *fs,
                                       const struct pros_prosenjitfs_inode *dir_inode,
                                       const char *name,
                                       struct pros_prosenjitfs_dir_index *index,
                                       struct pros_prosenjitfs_dir_entry *found,
                                       uint64_t *entry_lba,
                                       uint64_t *previous_lba);
static int pfs_vnode_unlink(struct pros_vfs_vnode *parent, const char *name);
static int pfs_vnode_rename(struct pros_vfs_vnode *old_parent,
                            const char *old_name,
                            struct pros_vfs_vnode *new_parent,
                            const char *new_name);
static uint32_t pfs_name_hash(const uint8_t *name, uint32_t length);
static int pfs_utf8_name_valid(const char *name, uint32_t *length_out);
static int pfs_read_dir_index(const struct pros_prosenjitfs *fs,
                              const struct pros_prosenjitfs_inode *inode,
                              struct pros_prosenjitfs_dir_index *index);
static int pfs_find_dir_entry(const struct pros_prosenjitfs *fs,
                              const struct pros_prosenjitfs_inode *dir_inode,
                              const char *name,
                              struct pros_prosenjitfs_dir_entry *found);
static int pfs_bitmap_bit(const uint8_t *bitmap, uint64_t bit);
static int pfs_allocator_stage(struct pros_prosenjitfs *fs,
                               uint64_t first, uint32_t count,
                               uint8_t allocate);
static int pfs_inode_bitmap_stage(struct pros_prosenjitfs *fs,
                                  uint64_t inode_number, uint8_t allocate);
static int pfs_tx_reset(struct pros_prosenjitfs *fs);
static int pfs_tx_add(struct pros_prosenjitfs *fs, uint64_t target,
                      const void *image);
static int pfs_tx_commit(struct pros_prosenjitfs *fs);
static int pfs_vnode_readdir(struct pros_vfs_vnode *directory, uint64_t *cookie,
                             struct pros_vfs_dirent *entry);
static void pfs_vnode_release(struct pros_vfs_vnode *vnode);

static const struct pros_vfs_vnode_ops pfs_vnode_ops = {
    .lookup = pfs_vnode_lookup,
    .readdir = pfs_vnode_readdir,
    .create = pfs_vnode_create,
    .mkdir = pfs_vnode_mkdir,
    .truncate = pfs_vnode_truncate,
    .unlink = pfs_vnode_unlink,
    .rename = pfs_vnode_rename,
    .release = pfs_vnode_release
};

static int pfs_read_extent(struct pros_prosenjitfs *fs, uint64_t lba,
                               struct pros_prosenjitfs_extent *extent)
{
    uint8_t block[4096];
    if (!fs || !extent || fs->sb.block_size > sizeof(block) ||
        lba < fs->sb.data_start || lba >= fs->sb.total_blocks)
        return PROSENJITFS_ECORRUPT;
    if (pfs_read_block(fs->mount.device, lba, block) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_copy(extent, block, sizeof(*extent));
    if (extent->magic != PROSENJITFS_EXTENT_MAGIC ||
        extent->block_count == 0u ||
        extent->logical_block > UINT64_MAX - extent->block_count ||
        extent->physical_block < fs->sb.data_start ||
        extent->physical_block >= fs->sb.total_blocks ||
        extent->block_count > fs->sb.total_blocks - extent->physical_block ||
        (extent->next != 0u &&
         (extent->next < fs->sb.data_start || extent->next >= fs->sb.total_blocks)) ||
        pfs_crc_with_zero32(block, sizeof(*extent),
                           (uint32_t)offsetof(
                               struct pros_prosenjitfs_extent, checksum)) !=
        extent->checksum)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static int pfs_find_extent(struct pros_prosenjitfs *fs,
                           const struct pros_prosenjitfs_inode *inode,
                           uint64_t logical_block,
                           struct pros_prosenjitfs_extent *found,
                           uint64_t *extent_lba)
{
    uint64_t lba;
    uint64_t guard = 0u;
    struct pros_prosenjitfs_extent extent;
    if (!fs || !inode || !found || !extent_lba)
        return PROSENJITFS_EINVAL;
    lba = inode->extent_head;
    while (lba != 0u && guard++ < fs->sb.total_blocks) {
        int rc = pfs_read_extent(fs, lba, &extent);
        if (rc != PROSENJITFS_OK)
            return rc;
        if (logical_block >= extent.logical_block &&
            logical_block - extent.logical_block < extent.block_count) {
            *found = extent;
            *extent_lba = lba;
            return PROSENJITFS_OK;
        }
        if (logical_block < extent.logical_block)
            return PROSENJITFS_ENOENT;
        lba = extent.next;
    }
    return lba == 0u ? PROSENJITFS_ENOENT : PROSENJITFS_ECORRUPT;
}

static int pfs_find_free_run(const struct pros_prosenjitfs *fs,
                             uint64_t count, uint64_t *start)
{
    uint64_t run = 0u;
    uint64_t candidate = 0u;
    uint64_t bits_per_block;
    uint8_t bitmap[4096];

    if (!fs || !start || count == 0u || fs->sb.block_size > sizeof(bitmap))
        return PROSENJITFS_EINVAL;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    for (uint64_t b = fs->sb.data_start; b < fs->sb.total_blocks; ++b) {
        uint64_t bi = b / bits_per_block;
        if (bi >= fs->sb.block_bitmap_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_block(fs->mount.device,
                           fs->sb.block_bitmap_start + bi, bitmap) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        if (!pfs_bitmap_bit(bitmap, b % bits_per_block)) {
            if (run == 0u)
                candidate = b;
            ++run;
            if (run >= count) {
                *start = candidate;
                return PROSENJITFS_OK;
            }
        } else {
            run = 0u;
        }
    }
    return PROSENJITFS_ENOSPC;
}

static int pfs_file_read(struct pros_vfs_file *file, uint32_t offset,
                         uint32_t length, void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint8_t block[4096];
    uint8_t *out = (uint8_t *)buffer;
    uint32_t done = 0u;
    uint32_t block_size;

    if (!file || !buffer || !file->vnode || !file->vnode->private_data)
        return PROS_VFS_EINVAL;
    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data->fs || data->fs->sb.block_size > sizeof(block))
        return PROS_VFS_EIO;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &inode) !=
        PROSENJITFS_OK || inode.type != PROSENJITFS_INODE_FILE)
        return PROS_VFS_EIO;
    if ((uint64_t)offset > inode.size)
        return PROS_VFS_EOVERFLOW;
    if ((uint64_t)length > inode.size - offset)
        length = (uint32_t)(inode.size - offset);
    block_size = data->fs->sb.block_size;

    while (done < length) {
        uint64_t pos = (uint64_t)offset + done;
        uint64_t logical = pos / block_size;
        uint32_t within = (uint32_t)(pos % block_size);
        uint32_t take = block_size - within;
        struct pros_prosenjitfs_extent extent;
        uint64_t extent_lba;
        uint64_t physical;

        if (take > length - done)
            take = length - done;
        if (pfs_find_extent(data->fs, &inode, logical, &extent, &extent_lba) !=
            PROSENJITFS_OK)
            return PROS_VFS_EIO;
        physical = extent.physical_block + (logical - extent.logical_block);
        if (pfs_read_block(data->fs->mount.device, physical, block) !=
            PROSENJITFS_OK)
            return PROS_VFS_EIO;
        (void)extent_lba;
        pfs_copy(out + done, block + within, take);
        done += take;
    }
    return (int)done;
}

static int pfs_file_write(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, const void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t block_size;
    uint32_t done = 0u;

    if (!file || !buffer || !file->vnode || !file->vnode->private_data)
        return PROS_VFS_EINVAL;
    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data->fs || data->fs->read_only || data->fs->sb.block_size > 4096u)
        return PROS_VFS_EPERM;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &inode) !=
        PROSENJITFS_OK || inode.type != PROSENJITFS_INODE_FILE)
        return PROS_VFS_EIO;
    if ((uint64_t)offset > inode.size ||
        (uint64_t)length > UINT32_MAX - offset)
        return PROS_VFS_EOVERFLOW;
    block_size = data->fs->sb.block_size;

    while (done < length) {
        uint64_t pos = (uint64_t)offset + done;
        uint64_t old_size = inode.size;
        uint64_t logical = pos / block_size;
        uint32_t within = (uint32_t)(pos % block_size);
        uint32_t take = block_size - within;
        uint64_t required_end;
        struct pros_prosenjitfs_extent extent;
        uint64_t extent_lba;
        uint8_t block[4096];

        if (take > length - done)
            take = length - done;
        required_end = pos + take;

        if (logical * block_size < old_size &&
            pfs_find_extent(data->fs, &inode, logical, &extent, &extent_lba) ==
            PROSENJITFS_OK) {
            uint64_t physical = extent.physical_block +
                                (logical - extent.logical_block);
            if (pfs_read_block(data->fs->mount.device, physical, block) !=
                PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            pfs_copy(block + within, src + done, take);
            if (pfs_write_block(data->fs->mount.device, physical, block) !=
                PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            if (required_end > inode.size) {
                /*
                 * The extent already exists, so only EOF metadata changes.
                 * Flush file data before publishing the new size; otherwise
                 * a crash could expose a committed EOF before its data.
                 */
                if (pfs_flush(data->fs->mount.device) != PROSENJITFS_OK)
                    return done ? (int)done : PROS_VFS_EIO;
                inode.size = required_end;
                if (prosenjitfs_write_inode(data->fs, data->inode_number,
                                            &inode) != PROSENJITFS_OK)
                    return done ? (int)done : PROS_VFS_EIO;
            }
            done += take;
            continue;
        }

        /*
         * V1 grows files in contiguous extent chunks. The extent metadata
         * block is allocated immediately before its data run. Keeping the
         * allocation, extent publication and inode size update in one
         * journal transaction makes a crash leave either the old EOF or the
         * complete new extent visible.
         */
        {
            uint64_t need_blocks = (required_end + block_size - 1u) /
                                   block_size - inode.size / block_size;
            uint64_t chunk = need_blocks > 16u ? 16u : need_blocks;
            uint64_t run_start;
            uint64_t extent_block;
            uint64_t data_start;
            uint64_t logical_start = inode.size / block_size;
            uint8_t *extent_image;
            uint8_t *inode_image;
            uint8_t *old_extent_image = 0;
            uint64_t old_extent_lba = 0u;
            struct pros_prosenjitfs_extent new_extent;
            uint64_t byte_offset;
            uint64_t inode_lba;
            uint32_t inode_offset;
            int rc;

            if (chunk == 0u)
                return done ? (int)done : PROS_VFS_EIO;
            if (pfs_find_free_run(data->fs, chunk + 1u, &run_start) !=
                PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EOVERFLOW;
            extent_block = run_start;
            data_start = run_start + 1u;
            extent_image = (uint8_t *)kmalloc(block_size);
            inode_image = (uint8_t *)kmalloc(block_size);
            if (!extent_image || !inode_image) {
                if (extent_image) kfree(extent_image);
                if (inode_image) kfree(inode_image);
                return done ? (int)done : PROS_VFS_EIO;
            }

            pfs_zero(extent_image, block_size);
            pfs_zero(&new_extent, sizeof(new_extent));
            new_extent.magic = PROSENJITFS_EXTENT_MAGIC;
            new_extent.logical_block = logical_start;
            new_extent.physical_block = data_start;
            new_extent.block_count = chunk;
            new_extent.next = 0u;
            pfs_copy(extent_image, &new_extent, sizeof(new_extent));
            pfs_put32(extent_image +
                      offsetof(struct pros_prosenjitfs_extent, checksum),
                      pfs_crc_with_zero32(
                          extent_image, sizeof(new_extent),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_extent, checksum)));

            if (inode.extent_head != 0u) {
                uint64_t cursor = inode.extent_head;
                struct pros_prosenjitfs_extent tail;
                uint64_t tail_lba = 0u;
                uint64_t guard = 0u;
                while (cursor != 0u && guard++ < data->fs->sb.total_blocks) {
                    rc = pfs_read_extent(data->fs, cursor, &tail);
                    if (rc != PROSENJITFS_OK) break;
                    tail_lba = cursor;
                    cursor = tail.next;
                }
                if (rc != PROSENJITFS_OK || cursor != 0u) {
                    kfree(extent_image); kfree(inode_image);
                    return done ? (int)done : PROS_VFS_EIO;
                }
                old_extent_lba = tail_lba;
                old_extent_image = (uint8_t *)kmalloc(block_size);
                if (!old_extent_image ||
                    pfs_read_block(data->fs->mount.device, old_extent_lba,
                                   old_extent_image) != PROSENJITFS_OK) {
                    if (old_extent_image) kfree(old_extent_image);
                    kfree(extent_image); kfree(inode_image);
                    return done ? (int)done : PROS_VFS_EIO;
                }
                pfs_copy(&tail.next, &extent_block, sizeof(extent_block));
                pfs_copy(old_extent_image, &tail, sizeof(tail));
                pfs_put32(old_extent_image +
                          offsetof(struct pros_prosenjitfs_extent, checksum),
                          pfs_crc_with_zero32(
                              old_extent_image, sizeof(tail),
                              (uint32_t)offsetof(
                                  struct pros_prosenjitfs_extent, checksum)));
            } else {
                inode.extent_head = extent_block;
            }

            {
                uint8_t *dbuf = (uint8_t *)kmalloc(block_size);
                if (!dbuf) {
                    if (old_extent_image) kfree(old_extent_image);
                    kfree(extent_image); kfree(inode_image);
                    return done ? (int)done : PROS_VFS_EIO;
                }
                for (uint64_t bi = 0u; bi < chunk; ++bi) {
                    pfs_zero(dbuf, block_size);
                    uint64_t write_pos = ((uint64_t)logical_start + bi) *
                                         block_size;
                    uint32_t copy_len = 0u;
                    if (write_pos < required_end && write_pos + block_size > pos) {
                        uint64_t begin = write_pos > pos ? write_pos : pos;
                        uint64_t end = write_pos + block_size < required_end ?
                                       write_pos + block_size : required_end;
                        copy_len = (uint32_t)(end - begin);
                        pfs_copy(dbuf + (begin - write_pos),
                                 src + done + (begin - pos), copy_len);
                    }
                    if (pfs_write_block(data->fs->mount.device,
                                        data_start + bi, dbuf) !=
                        PROSENJITFS_OK) {
                        kfree(dbuf);
                        if (old_extent_image) kfree(old_extent_image);
                        kfree(extent_image); kfree(inode_image);
                        return done ? (int)done : PROS_VFS_EIO;
                    }
                }
                kfree(dbuf);
            }
            if (block_device_flush(data->fs->mount.device) != PROS_BLOCK_OK) {
                if (old_extent_image) kfree(old_extent_image);
                kfree(extent_image); kfree(inode_image);
                return done ? (int)done : PROS_VFS_EIO;
            }

            inode.size = required_end;
            inode.allocated_bytes += chunk * block_size;
            byte_offset = data->inode_number * PROSENJITFS_INODE_SIZE;
            inode_lba = data->fs->sb.inode_table_start +
                        byte_offset / block_size;
            inode_offset = (uint32_t)(byte_offset % block_size);
            if (pfs_read_block(data->fs->mount.device, inode_lba,
                               inode_image) != PROSENJITFS_OK) {
                if (old_extent_image) kfree(old_extent_image);
                kfree(extent_image); kfree(inode_image);
                return done ? (int)done : PROS_VFS_EIO;
            }
            pfs_copy(inode_image + inode_offset, &inode,
                     sizeof(inode));
            pfs_put32(inode_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(
                          inode_image + inode_offset, sizeof(inode),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_inode, checksum)));

            rc = pfs_tx_reset(data->fs);
            if (rc == PROSENJITFS_OK)
                rc = pfs_allocator_stage(data->fs, extent_block,
                                         chunk + 1u, 1u);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_add(data->fs, extent_block, extent_image);
            if (rc == PROSENJITFS_OK && old_extent_image)
                rc = pfs_tx_add(data->fs, old_extent_lba, old_extent_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_add(data->fs, inode_lba, inode_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_commit(data->fs);

            if (old_extent_image) kfree(old_extent_image);
            kfree(extent_image);
            kfree(inode_image);
            if (rc != PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            done += take;
        }
    }

    file->vnode->size = inode.size;
    return (int)done;
}

static int pfs_find_dir_entry_location(
    const struct pros_prosenjitfs *fs,
    const struct pros_prosenjitfs_inode *dir_inode,
    const char *name,
    struct pros_prosenjitfs_dir_index *index,
    struct pros_prosenjitfs_dir_entry *found,
    uint64_t *entry_lba,
    uint64_t *previous_lba)
{
    uint32_t name_length;
    uint32_t hash;
    uint64_t current;
    uint64_t previous = 0u;
    uint64_t steps = 0u;

    if (!fs || !dir_inode || !name || !index || !found ||
        !entry_lba || !previous_lba)
        return PROSENJITFS_EINVAL;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    if (pfs_read_dir_index(fs, dir_inode, index) != PROSENJITFS_OK)
        return PROSENJITFS_ECORRUPT;
    hash = pfs_name_hash((const uint8_t *)name, name_length);
    current = index->heads[hash % PROSENJITFS_DIR_BUCKETS];

    while (current != 0u) {
        struct pros_prosenjitfs_dir_entry entry;
        uint32_t equal = 1u;
        if (++steps > fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_dir_entry(fs, current, &entry) != PROSENJITFS_OK)
            return PROSENJITFS_ECORRUPT;
        if (entry.name_hash == hash && entry.name_length == name_length) {
            for (uint32_t i = 0u; i < name_length; ++i)
                if (entry.name[i] != (uint8_t)name[i]) {
                    equal = 0u;
                    break;
                }
            if (equal) {
                pfs_copy(found, &entry, sizeof(*found));
                *entry_lba = current;
                *previous_lba = previous;
                return PROSENJITFS_OK;
            }
        }
        previous = current;
        current = entry.next;
    }
    return PROSENJITFS_ENOENT;
}

static int pfs_directory_would_cycle(const struct pros_prosenjitfs *fs,
                                     uint64_t child_inode,
                                     uint64_t new_parent_inode)
{
    uint64_t current = new_parent_inode;
    uint64_t guard = 0u;

    if (!fs || child_inode >= fs->sb.inode_count ||
        new_parent_inode >= fs->sb.inode_count)
        return PROSENJITFS_EINVAL;
    while (guard++ < fs->sb.inode_count) {
        struct pros_prosenjitfs_inode inode;
        if (current == child_inode)
            return PROSENJITFS_EEXIST;
        if (current == fs->sb.root_inode)
            return PROSENJITFS_OK;
        if (prosenjitfs_read_inode(fs, current, &inode) != PROSENJITFS_OK ||
            inode.type != PROSENJITFS_INODE_DIR ||
            inode.parent_inode >= fs->sb.inode_count ||
            inode.parent_inode == current)
            return PROSENJITFS_ECORRUPT;
        current = inode.parent_inode;
    }
    return PROSENJITFS_ECORRUPT;
}

static int pfs_vnode_unlink(struct pros_vfs_vnode *parent, const char *name)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode parent_inode;
    struct pros_prosenjitfs_inode child_inode;
    struct pros_prosenjitfs_dir_index index;
    struct pros_prosenjitfs_dir_entry entry;
    uint64_t entry_lba;
    uint64_t previous_lba;
    uint8_t *index_image = 0;
    uint8_t *previous_image = 0;
    uint8_t *inode_image = 0;
    uint64_t inode_byte_offset;
    uint64_t inode_lba;
    uint32_t inode_offset;
    int rc;
    uint8_t child_was_dir = 0u;
    uint64_t child_dir_index = 0u;

    if (!parent || !name || !parent->private_data ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    data = (struct pfs_vnode_data *)parent->private_data;
    if (!data->fs || data->fs->read_only)
        return PROS_VFS_EPERM;
    if (prosenjitfs_read_inode(data->fs, data->inode_number,
                               &parent_inode) != PROSENJITFS_OK ||
        parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_EIO;
    rc = pfs_find_dir_entry_location(data->fs, &parent_inode, name, &index,
                                     &entry, &entry_lba, &previous_lba);
    if (rc == PROSENJITFS_ENOENT)
        return PROS_VFS_ENOENT;
    if (rc != PROSENJITFS_OK)
        return PROS_VFS_EIO;
    if (entry.inode == data->inode_number)
        return PROS_VFS_EPERM;
    if (prosenjitfs_read_inode(data->fs, entry.inode, &child_inode) !=
            PROSENJITFS_OK ||
        child_inode.type == PROSENJITFS_INODE_FREE)
        return PROS_VFS_EIO;

    if (child_inode.type == PROSENJITFS_INODE_DIR) {
        int empty_rc;
        child_was_dir = 1u;
        child_dir_index = child_inode.dir_index;
        {
            struct pros_prosenjitfs_dir_entry child_entry;
            empty_rc = pfs_dir_entry_by_ordinal(data->fs, &child_inode, 0u,
                                                &child_entry);
        }
        if (empty_rc == PROSENJITFS_OK)
            return PROS_VFS_ENOTEMPTY;
        if (empty_rc != PROSENJITFS_ENOENT)
            return PROS_VFS_EIO;
    } else if (child_inode.type == PROSENJITFS_INODE_FILE) {
        if (child_inode.size != 0u || child_inode.extent_head != 0u) {
            struct pros_vfs_vnode *child = 0;
            if (pfs_make_vnode(data->fs, entry.inode, parent, &child) !=
                    PROS_VFS_OK)
                return PROS_VFS_EIO;
            rc = pfs_vnode_truncate(child, 0u);
            vfs_vnode_put(child);
            if (rc != PROS_VFS_OK)
                return rc;
            if (prosenjitfs_read_inode(data->fs, entry.inode,
                                        &child_inode) != PROSENJITFS_OK)
                return PROS_VFS_EIO;
        }
    } else {
        return PROS_VFS_ENOSYS;
    }

    index_image = (uint8_t *)kmalloc(data->fs->sb.block_size);
    inode_image = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!index_image || !inode_image) {
        if (index_image) kfree(index_image);
        if (inode_image) kfree(inode_image);
        return PROS_VFS_EIO;
    }
    if (pfs_read_block(data->fs->mount.device, parent_inode.dir_index,
                       index_image) != PROSENJITFS_OK) {
        kfree(index_image); kfree(inode_image);
        return PROS_VFS_EIO;
    }

    if (previous_lba == 0u) {
        struct pros_prosenjitfs_dir_index *idx =
            (struct pros_prosenjitfs_dir_index *)index_image;
        idx->heads[entry.name_hash % PROSENJITFS_DIR_BUCKETS] = entry.next;
        idx->checksum = 0u;
        idx->checksum = pfs_crc_with_zero32(
            index_image, sizeof(*idx),
            (uint32_t)offsetof(struct pros_prosenjitfs_dir_index, checksum));
    } else {
        previous_image = (uint8_t *)kmalloc(data->fs->sb.block_size);
        if (!previous_image ||
            pfs_read_block(data->fs->mount.device, previous_lba,
                           previous_image) != PROSENJITFS_OK) {
            if (previous_image) kfree(previous_image);
            kfree(index_image); kfree(inode_image);
            return PROS_VFS_EIO;
        }
        {
            struct pros_prosenjitfs_dir_entry *prev =
                (struct pros_prosenjitfs_dir_entry *)previous_image;
            prev->next = entry.next;
            prev->checksum = 0u;
            prev->checksum = pfs_crc_with_zero32(
                previous_image, sizeof(*prev),
                (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry,
                                   checksum));
        }
    }

    pfs_zero(&child_inode, sizeof(child_inode));
    inode_byte_offset = entry.inode * PROSENJITFS_INODE_SIZE;
    inode_lba = data->fs->sb.inode_table_start +
                inode_byte_offset / data->fs->sb.block_size;
    inode_offset = (uint32_t)(inode_byte_offset % data->fs->sb.block_size);
    if (pfs_read_block(data->fs->mount.device, inode_lba, inode_image) !=
        PROSENJITFS_OK) {
        if (previous_image) kfree(previous_image);
        kfree(index_image); kfree(inode_image);
        return PROS_VFS_EIO;
    }
    pfs_copy(inode_image + inode_offset, &child_inode, sizeof(child_inode));
    pfs_put32(inode_image + inode_offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(
                  inode_image + inode_offset, sizeof(child_inode),
                  (uint32_t)offsetof(struct pros_prosenjitfs_inode,
                                     checksum)));

    rc = pfs_tx_reset(data->fs);
    if (rc == PROSENJITFS_OK)
        rc = pfs_allocator_stage(data->fs, entry_lba, 1u, 0u);
    if (rc == PROSENJITFS_OK && child_was_dir)
        rc = pfs_allocator_stage(data->fs, child_dir_index, 1u, 0u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_inode_bitmap_stage(data->fs, entry.inode, 0u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, parent_inode.dir_index, index_image);
    if (rc == PROSENJITFS_OK && previous_image)
        rc = pfs_tx_add(data->fs, previous_lba, previous_image);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, inode_lba, inode_image);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_commit(data->fs);

    if (previous_image) kfree(previous_image);
    kfree(index_image);
    kfree(inode_image);
    return rc == PROSENJITFS_OK ? PROS_VFS_OK : PROS_VFS_EIO;
}

static int pfs_vnode_rename(struct pros_vfs_vnode *old_parent,
                            const char *old_name,
                            struct pros_vfs_vnode *new_parent,
                            const char *new_name)
{
    struct pfs_vnode_data *old_data;
    struct pfs_vnode_data *new_data;
    struct pros_prosenjitfs_inode old_dir;
    struct pros_prosenjitfs_inode new_dir;
    struct pros_prosenjitfs_inode child;
    struct pros_prosenjitfs_dir_index old_index;
    struct pros_prosenjitfs_dir_index new_index;
    struct pros_prosenjitfs_dir_entry source;
    struct pros_prosenjitfs_dir_entry target;
    uint64_t source_lba, source_prev;
    uint64_t target_lba, target_prev;
    uint32_t old_hash, new_hash, old_bucket, new_bucket, old_len, new_len;
    uint8_t *source_image = 0;
    uint8_t *old_index_image = 0;
    uint8_t *new_index_image = 0;
    uint8_t *prev_image = 0;
    uint8_t *inode_image = 0;
    uint64_t inode_lba = 0u;
    uint32_t inode_offset = 0u;
    int rc;

    if (!old_parent || !new_parent || !old_name || !new_name ||
        !old_parent->private_data || !new_parent->private_data ||
        old_parent->type != PROS_VFS_NODE_DIRECTORY ||
        new_parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    old_data = (struct pfs_vnode_data *)old_parent->private_data;
    new_data = (struct pfs_vnode_data *)new_parent->private_data;
    if (!old_data->fs || old_data->fs != new_data->fs ||
        old_data->fs->read_only)
        return PROS_VFS_EPERM;
    if (pfs_utf8_name_valid(old_name, &old_len) != PROSENJITFS_OK ||
        pfs_utf8_name_valid(new_name, &new_len) != PROSENJITFS_OK)
        return PROS_VFS_EINVAL;
    old_hash = pfs_name_hash((const uint8_t *)old_name, old_len);
    new_hash = pfs_name_hash((const uint8_t *)new_name, new_len);
    old_bucket = old_hash % PROSENJITFS_DIR_BUCKETS;
    new_bucket = new_hash % PROSENJITFS_DIR_BUCKETS;
    if (prosenjitfs_read_inode(old_data->fs, old_data->inode_number,
                               &old_dir) != PROSENJITFS_OK ||
        old_dir.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_EIO;
    rc = pfs_find_dir_entry_location(old_data->fs, &old_dir, old_name,
                                     &old_index, &source, &source_lba,
                                     &source_prev);
    if (rc != PROSENJITFS_OK)
        return rc == PROSENJITFS_ENOENT ? PROS_VFS_ENOENT : PROS_VFS_EIO;
    if (prosenjitfs_read_inode(old_data->fs, new_data->inode_number,
                               &new_dir) != PROSENJITFS_OK ||
        new_dir.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_EIO;
    rc = pfs_find_dir_entry_location(old_data->fs, &new_dir, new_name,
                                     &new_index, &target, &target_lba,
                                     &target_prev);
    if (rc == PROSENJITFS_OK) {
        if (target.inode == source.inode &&
            old_data->inode_number == new_data->inode_number &&
            old_len == new_len && old_hash == new_hash)
            return PROS_VFS_OK;
        return PROS_VFS_EEXIST;
    }
    if (rc != PROSENJITFS_ENOENT)
        return PROS_VFS_EIO;
    (void)target_lba;
    (void)target_prev;

    if (source.inode >= old_data->fs->sb.inode_count)
        return PROS_VFS_EIO;
    if (prosenjitfs_read_inode(old_data->fs, source.inode, &child) !=
            PROSENJITFS_OK ||
        (child.type != PROSENJITFS_INODE_FILE &&
         child.type != PROSENJITFS_INODE_DIR))
        return PROS_VFS_EIO;
    if (child.type == PROSENJITFS_INODE_DIR &&
        old_data->inode_number != new_data->inode_number) {
        rc = pfs_directory_would_cycle(old_data->fs, source.inode,
                                       new_data->inode_number);
        if (rc != PROSENJITFS_OK)
            return rc == PROSENJITFS_EEXIST ? PROS_VFS_ELOOP :
                                              PROS_VFS_EIO;
    }

    source_image = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    old_index_image = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    new_index_image = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    inode_image = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    if (!source_image || !old_index_image || !new_index_image || !inode_image) {
        if (source_image) kfree(source_image);
        if (old_index_image) kfree(old_index_image);
        if (new_index_image) kfree(new_index_image);
        if (inode_image) kfree(inode_image);
        return PROS_VFS_EIO;
    }
    if (pfs_read_block(old_data->fs->mount.device, source_lba, source_image) !=
            PROSENJITFS_OK ||
        pfs_read_block(old_data->fs->mount.device, old_dir.dir_index,
                       old_index_image) != PROSENJITFS_OK ||
        pfs_read_block(old_data->fs->mount.device, new_dir.dir_index,
                       new_index_image) != PROSENJITFS_OK) {
        kfree(source_image); kfree(old_index_image);
        kfree(new_index_image); kfree(inode_image);
        return PROS_VFS_EIO;
    }

    {
        struct pros_prosenjitfs_dir_entry *src =
            (struct pros_prosenjitfs_dir_entry *)source_image;
        struct pros_prosenjitfs_dir_index *oi =
            (struct pros_prosenjitfs_dir_index *)old_index_image;
        struct pros_prosenjitfs_dir_index *ni =
            (struct pros_prosenjitfs_dir_index *)new_index_image;
        src->name_hash = new_hash;
        src->name_length = (uint16_t)new_len;
        pfs_zero(src->name, sizeof(src->name));
        pfs_copy(src->name, new_name, new_len);

        if (old_data->inode_number == new_data->inode_number &&
            old_bucket != new_bucket) {
            if (source_prev == 0u)
                oi->heads[old_bucket] = source.next;
            else {
                prev_image = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
                if (!prev_image ||
                    pfs_read_block(old_data->fs->mount.device, source_prev,
                                   prev_image) != PROSENJITFS_OK) {
                    if (prev_image) kfree(prev_image);
                    kfree(source_image); kfree(old_index_image);
                    kfree(new_index_image); kfree(inode_image);
                    return PROS_VFS_EIO;
                }
                {
                    struct pros_prosenjitfs_dir_entry *prev =
                        (struct pros_prosenjitfs_dir_entry *)prev_image;
                    prev->next = source.next;
                    prev->checksum = 0u;
                    prev->checksum = pfs_crc_with_zero32(
                        prev_image, sizeof(*prev),
                        (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry,
                                           checksum));
                }
            }
            pfs_copy(ni, oi, sizeof(*ni));
            src->next = ni->heads[new_bucket];
            ni->heads[new_bucket] = source_lba;
        } else if (old_data->inode_number != new_data->inode_number) {
            if (source_prev == 0u)
                oi->heads[old_bucket] = source.next;
            else {
                prev_image = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
                if (!prev_image ||
                    pfs_read_block(old_data->fs->mount.device, source_prev,
                                   prev_image) != PROSENJITFS_OK) {
                    if (prev_image) kfree(prev_image);
                    kfree(source_image); kfree(old_index_image);
                    kfree(new_index_image); kfree(inode_image);
                    return PROS_VFS_EIO;
                }
                {
                    struct pros_prosenjitfs_dir_entry *prev =
                        (struct pros_prosenjitfs_dir_entry *)prev_image;
                    prev->next = source.next;
                    prev->checksum = 0u;
                    prev->checksum = pfs_crc_with_zero32(
                        prev_image, sizeof(*prev),
                        (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry,
                                           checksum));
                }
            }
            src->next = ni->heads[new_bucket];
            ni->heads[new_bucket] = source_lba;
            if (child.type == PROSENJITFS_INODE_DIR) {
                child.parent_inode = new_data->inode_number;
                inode_lba = old_data->fs->sb.inode_table_start +
                            (source.inode * PROSENJITFS_INODE_SIZE) /
                            old_data->fs->sb.block_size;
                inode_offset = (uint32_t)((source.inode *
                                           PROSENJITFS_INODE_SIZE) %
                                          old_data->fs->sb.block_size);
                if (pfs_read_block(old_data->fs->mount.device, inode_lba,
                                   inode_image) != PROSENJITFS_OK) {
                    if (prev_image) kfree(prev_image);
                    kfree(source_image); kfree(old_index_image);
                    kfree(new_index_image); kfree(inode_image);
                    return PROS_VFS_EIO;
                }
                pfs_copy(inode_image + inode_offset, &child,
                         sizeof(child));
                pfs_put32(inode_image + inode_offset +
                          offsetof(struct pros_prosenjitfs_inode, checksum),
                          pfs_crc_with_zero32(
                              inode_image + inode_offset, sizeof(child),
                              (uint32_t)offsetof(
                                  struct pros_prosenjitfs_inode, checksum)));
            }
        }

        oi->checksum = 0u;
        oi->checksum = pfs_crc_with_zero32(
            old_index_image, sizeof(*oi),
            (uint32_t)offsetof(struct pros_prosenjitfs_dir_index, checksum));
        if (old_data->inode_number == new_data->inode_number) {
            ni->checksum = 0u;
            ni->checksum = pfs_crc_with_zero32(
                new_index_image, sizeof(*ni),
                (uint32_t)offsetof(struct pros_prosenjitfs_dir_index,
                                   checksum));
        } else {
            ni->checksum = 0u;
            ni->checksum = pfs_crc_with_zero32(
                new_index_image, sizeof(*ni),
                (uint32_t)offsetof(struct pros_prosenjitfs_dir_index,
                                   checksum));
        }
        src->checksum = 0u;
        src->checksum = pfs_crc_with_zero32(
            source_image, sizeof(*src),
            (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
    }

    rc = pfs_tx_reset(old_data->fs);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(old_data->fs, old_dir.dir_index, old_index_image);
    if (rc == PROSENJITFS_OK && old_data->inode_number != new_data->inode_number)
        rc = pfs_tx_add(old_data->fs, new_dir.dir_index, new_index_image);
    if (rc == PROSENJITFS_OK && prev_image)
        rc = pfs_tx_add(old_data->fs, source_prev, prev_image);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(old_data->fs, source_lba, source_image);
    if (rc == PROSENJITFS_OK && inode_lba != 0u)
        rc = pfs_tx_add(old_data->fs, inode_lba, inode_image);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_commit(old_data->fs);

    if (prev_image) kfree(prev_image);
    kfree(source_image); kfree(old_index_image);
    kfree(new_index_image); kfree(inode_image);
    return rc == PROSENJITFS_OK ? PROS_VFS_OK : PROS_VFS_EIO;
}

static int pfs_vnode_truncate(struct pros_vfs_vnode *vnode, uint64_t new_size)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint32_t block_size;

    if (!vnode || !vnode->private_data ||
        vnode->type != PROS_VFS_NODE_FILE)
        return PROS_VFS_EINVAL;
    data = (struct pfs_vnode_data *)vnode->private_data;
    if (!data->fs || data->fs->read_only ||
        data->fs->sb.block_size > 4096u)
        return PROS_VFS_EPERM;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &inode) !=
            PROSENJITFS_OK ||
        inode.type != PROSENJITFS_INODE_FILE)
        return PROS_VFS_EIO;
    if (new_size == inode.size)
        return PROS_VFS_OK;

    /*
     * VFS file offsets are currently 32-bit, so keep the truncate growth
     * path within the same representable range as pfs_file_write().
     */
    if (new_size > UINT32_MAX)
        return PROS_VFS_EOVERFLOW;

    if (new_size > inode.size) {
        struct pros_vfs_file file;
        uint8_t zeroes[4096];
        uint64_t offset = inode.size;

        pfs_zero(&file, sizeof(file));
        pfs_zero(zeroes, sizeof(zeroes));
        file.mount = vnode->mount;
        file.vnode = vnode;
        file.flags = PROS_VFS_O_WRITE;
        file.open = 1u;

        while (offset < new_size) {
            uint32_t chunk = (uint32_t)(new_size - offset);
            uint32_t written;
            if (chunk > sizeof(zeroes))
                chunk = sizeof(zeroes);
            written = (uint32_t)pfs_file_write(&file, (uint32_t)offset,
                                               chunk, zeroes);
            if (written != chunk)
                return PROS_VFS_EIO;
            offset += written;
        }
        vnode->size = new_size;
        return PROS_VFS_OK;
    }

    /*
     * Shrinking is performed from the tail. Each transaction first removes
     * the tail from the live extent chain (or shortens the final extent) and
     * updates EOF, while releasing the no-longer-needed blocks in the same
     * transaction. If a crash interrupts a multi-extent shrink, the file can
     * temporarily retain unreachable tail space, but its live chain never
     * points at freed blocks.
     */
    block_size = data->fs->sb.block_size;
    for (;;) {
        uint64_t required_blocks =
            (new_size + block_size - 1u) / block_size;
        uint64_t cursor = inode.extent_head;
        uint64_t previous_lba = 0u;
        uint64_t last_lba = 0u;
        struct pros_prosenjitfs_extent previous;
        struct pros_prosenjitfs_extent last;
        uint64_t guard = 0u;
        int rc;

        if (cursor == 0u) {
            inode.size = new_size;
            inode.allocated_bytes = 0u;
            if (prosenjitfs_write_inode(data->fs, data->inode_number,
                                        &inode) != PROSENJITFS_OK)
                return PROS_VFS_EIO;
            vnode->size = new_size;
            return PROS_VFS_OK;
        }

        while (cursor != 0u && guard++ < data->fs->sb.total_blocks) {
            struct pros_prosenjitfs_extent current;
            rc = pfs_read_extent(data->fs, cursor, &current);
            if (rc != PROSENJITFS_OK)
                return PROS_VFS_EIO;
            previous_lba = last_lba;
            if (last_lba != 0u)
                pfs_copy(&previous, &last, sizeof(previous));
            last_lba = cursor;
            pfs_copy(&last, &current, sizeof(last));
            cursor = current.next;
        }
        if (cursor != 0u || last_lba == 0u)
            return PROS_VFS_EIO;

        if (last.logical_block >= required_blocks) {
            uint8_t *last_image;
            uint8_t *previous_image = 0;
            uint8_t *inode_image;
            uint64_t inode_byte_offset;
            uint64_t inode_lba;
            uint32_t inode_offset;
            uint64_t released = last.block_count;
            uint64_t old_allocated = inode.allocated_bytes;

            if (released > old_allocated / block_size)
                return PROS_VFS_EIO;
            last_image = (uint8_t *)kmalloc(block_size);
            inode_image = (uint8_t *)kmalloc(block_size);
            if (!last_image || !inode_image) {
                if (last_image) kfree(last_image);
                if (inode_image) kfree(inode_image);
                return PROS_VFS_EIO;
            }
            if (previous_lba != 0u) {
                previous_image = (uint8_t *)kmalloc(block_size);
                if (!previous_image) {
                    kfree(last_image); kfree(inode_image);
                    return PROS_VFS_EIO;
                }
            }
            if (pfs_read_block(data->fs->mount.device, last_lba, last_image) !=
                    PROSENJITFS_OK ||
                (previous_image &&
                 pfs_read_block(data->fs->mount.device, previous_lba,
                                previous_image) != PROSENJITFS_OK)) {
                if (previous_image) kfree(previous_image);
                kfree(last_image); kfree(inode_image);
                return PROS_VFS_EIO;
            }

            if (previous_image) {
                struct pros_prosenjitfs_extent *prev =
                    (struct pros_prosenjitfs_extent *)previous_image;
                prev->next = 0u;
                prev->checksum = 0u;
                prev->checksum = pfs_crc_with_zero32(
                    previous_image, sizeof(*prev),
                    (uint32_t)offsetof(struct pros_prosenjitfs_extent,
                                       checksum));
            } else {
                inode.extent_head = 0u;
            }

            inode.size = new_size;
            inode.allocated_bytes = old_allocated -
                                    released * block_size;
            inode_byte_offset = data->inode_number * PROSENJITFS_INODE_SIZE;
            inode_lba = data->fs->sb.inode_table_start +
                        inode_byte_offset / block_size;
            inode_offset = (uint32_t)(inode_byte_offset % block_size);
            if (pfs_read_block(data->fs->mount.device, inode_lba,
                               inode_image) != PROSENJITFS_OK) {
                if (previous_image) kfree(previous_image);
                kfree(last_image); kfree(inode_image);
                return PROS_VFS_EIO;
            }
            pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
            pfs_put32(inode_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(
                          inode_image + inode_offset, sizeof(inode),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_inode, checksum)));

            rc = pfs_tx_reset(data->fs);
            if (rc == PROSENJITFS_OK)
                rc = pfs_allocator_stage(data->fs, last.physical_block,
                                         (uint32_t)last.block_count, 0u);
            if (rc == PROSENJITFS_OK)
                rc = pfs_allocator_stage(data->fs, last_lba, 1u, 0u);
            if (rc == PROSENJITFS_OK && previous_image)
                rc = pfs_tx_add(data->fs, previous_lba, previous_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_add(data->fs, inode_lba, inode_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_commit(data->fs);

            if (previous_image) kfree(previous_image);
            kfree(last_image);
            kfree(inode_image);
            if (rc != PROSENJITFS_OK)
                return PROS_VFS_EIO;
            continue;
        }

        if (last.logical_block + last.block_count > required_blocks) {
            uint64_t keep = required_blocks - last.logical_block;
            uint64_t released = last.block_count - keep;
            uint8_t *extent_image = (uint8_t *)kmalloc(block_size);
            uint8_t *inode_image = (uint8_t *)kmalloc(block_size);
            uint64_t inode_byte_offset;
            uint64_t inode_lba;
            uint32_t inode_offset;

            if (keep == 0u || released == 0u ||
                released > inode.allocated_bytes / block_size ||
                !extent_image || !inode_image) {
                if (extent_image) kfree(extent_image);
                if (inode_image) kfree(inode_image);
                return released == 0u ? PROS_VFS_EIO : PROS_VFS_EIO;
            }
            if (pfs_read_block(data->fs->mount.device, last_lba,
                               extent_image) != PROSENJITFS_OK ||
                pfs_read_block(data->fs->mount.device,
                               data->fs->sb.inode_table_start +
                               (data->inode_number * PROSENJITFS_INODE_SIZE) /
                               block_size,
                               inode_image) != PROSENJITFS_OK) {
                kfree(extent_image); kfree(inode_image);
                return PROS_VFS_EIO;
            }
            {
                struct pros_prosenjitfs_extent *extent =
                    (struct pros_prosenjitfs_extent *)extent_image;
                extent->block_count = keep;
                extent->checksum = 0u;
                extent->checksum = pfs_crc_with_zero32(
                    extent_image, sizeof(*extent),
                    (uint32_t)offsetof(struct pros_prosenjitfs_extent,
                                       checksum));
            }
            inode.size = new_size;
            inode.allocated_bytes -= released * block_size;
            inode_byte_offset = data->inode_number * PROSENJITFS_INODE_SIZE;
            inode_lba = data->fs->sb.inode_table_start +
                        inode_byte_offset / block_size;
            inode_offset = (uint32_t)(inode_byte_offset % block_size);
            pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
            pfs_put32(inode_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(
                          inode_image + inode_offset, sizeof(inode),
                          (uint32_t)offsetof(struct pros_prosenjitfs_inode,
                                             checksum)));

            rc = pfs_tx_reset(data->fs);
            if (rc == PROSENJITFS_OK)
                rc = pfs_allocator_stage(
                    data->fs,
                    last.physical_block + keep,
                    (uint32_t)released, 0u);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_add(data->fs, last_lba, extent_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_add(data->fs, inode_lba, inode_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_commit(data->fs);
            kfree(extent_image);
            kfree(inode_image);
            if (rc != PROSENJITFS_OK)
                return PROS_VFS_EIO;
            vnode->size = new_size;
            return PROS_VFS_OK;
        }

        /*
         * The tail extent is fully before the new EOF. Because new_size is
         * smaller than the old size, another extent should follow the kept
         * range; reaching here means the on-disk extent chain is inconsistent.
         */
        return PROS_VFS_EIO;
    }
}

static int pfs_file_close_stub(struct pros_vfs_file *file)
{
    (void)file;
    return PROS_VFS_OK;
}

static const struct pros_vfs_file_ops pfs_file_ops = {
    .read = pfs_file_read,
    .write = pfs_file_write,
    .close = pfs_file_close_stub
};

static int pfs_make_vnode(struct pros_prosenjitfs *fs, uint64_t inode_number,
                          struct pros_vfs_vnode *parent,
                          struct pros_vfs_vnode **result)
{
    struct pros_prosenjitfs_inode inode;
    struct pfs_vnode_data *data;
    enum pros_vfs_node_type type;

    if (!fs || !result || inode_number >= fs->sb.inode_count)
        return PROS_VFS_EINVAL;
    if (prosenjitfs_read_inode(fs, inode_number, &inode) != PROSENJITFS_OK)
        return PROS_VFS_ENOENT;
    if (inode.type == PROSENJITFS_INODE_DIR)
        type = PROS_VFS_NODE_DIRECTORY;
    else if (inode.type == PROSENJITFS_INODE_FILE)
        type = PROS_VFS_NODE_FILE;
    else
        return PROS_VFS_ENOSYS;

    data = (struct pfs_vnode_data *)kmalloc(sizeof(*data));
    if (!data)
        return PROS_VFS_EIO;
    data->fs = fs;
    data->inode_number = inode_number;

    /*
     * The child vnode owns one reference to its parent so ".." remains
     * valid after the caller releases its parent reference.
     */
    if (parent && vfs_vnode_get(parent) != PROS_VFS_OK) {
        kfree(data);
        return PROS_VFS_EOVERFLOW;
    }

    *result = (struct pros_vfs_vnode *)kmalloc(sizeof(**result));
    if (!*result) {
        if (parent)
            vfs_vnode_put(parent);
        kfree(data);
        return PROS_VFS_EIO;
    }
    if (vfs_vnode_init(*result, &fs->mount, parent, type,
                       &pfs_vnode_ops,
                       type == PROS_VFS_NODE_FILE ? &pfs_file_ops : 0,
                       data, inode.size) != PROS_VFS_OK) {
        if (parent)
            vfs_vnode_put(parent);
        kfree(data);
        kfree(*result);
        *result = 0;
        return PROS_VFS_EINVAL;
    }
    return PROS_VFS_OK;
}


static int pfs_find_free_inode(const struct pros_prosenjitfs *fs,
                               uint64_t *inode_number)
{
    uint64_t bits_per_block;
    uint8_t block[4096];

    if (!fs || !inode_number || fs->sb.block_size > sizeof(block))
        return PROSENJITFS_EINVAL;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    for (uint64_t inode = 1u; inode < fs->sb.inode_count; ++inode) {
        uint64_t bi = inode / bits_per_block;
        uint64_t within = inode % bits_per_block;
        if (bi >= fs->sb.inode_bitmap_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_block(fs->mount.device,
                           fs->sb.inode_bitmap_start + bi, block) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        if (!pfs_bitmap_bit(block, within)) {
            *inode_number = inode;
            return PROSENJITFS_OK;
        }
    }
    return PROSENJITFS_ENOSPC;
}

static int pfs_find_free_data_block(const struct pros_prosenjitfs *fs,
                                    uint64_t *block_number)
{
    uint64_t bits_per_block;
    uint8_t block[4096];

    if (!fs || !block_number || fs->sb.block_size > sizeof(block))
        return PROSENJITFS_EINVAL;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    for (uint64_t b = fs->sb.data_start; b < fs->sb.total_blocks; ++b) {
        uint64_t bitmap_lba = fs->sb.block_bitmap_start + b / bits_per_block;
        if (pfs_read_block(fs->mount.device, bitmap_lba, block) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        if (!pfs_bitmap_bit(block, b % bits_per_block)) {
            *block_number = b;
            return PROSENJITFS_OK;
        }
    }
    return PROSENJITFS_ENOSPC;
}

static int pfs_vnode_create(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode parent_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_inode child_inode;
    struct pros_prosenjitfs_dir_entry dir_entry;
    uint8_t *dir_block;
    uint8_t *entry_block;
    uint8_t *inode_block;
    uint64_t inode_number;
    uint64_t entry_lba;
    uint64_t byte_offset;
    uint64_t inode_lba;
    uint32_t inode_offset;
    uint32_t name_length;
    uint32_t dir_bucket;
    int rc;

    if (!parent || !name || !result || !parent->private_data ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    *result = 0;
    data = (struct pfs_vnode_data *)parent->private_data;
    if (!data->fs || data->fs->read_only)
        return PROS_VFS_EPERM;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK)
        return PROS_VFS_EINVAL;

    if (prosenjitfs_read_inode(data->fs, data->inode_number, &parent_inode) !=
        PROSENJITFS_OK || parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_EIO;
    rc = pfs_find_dir_entry(data->fs, &parent_inode, name, 0);
    if (rc == PROSENJITFS_OK)
        return PROS_VFS_EEXIST;
    if (rc != PROSENJITFS_ENOENT)
        return PROS_VFS_EIO;
    if (pfs_read_dir_index(data->fs, &parent_inode, &dir_index) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;
    rc = pfs_find_free_inode(data->fs, &inode_number);
    if (rc != PROSENJITFS_OK)
        return rc == PROSENJITFS_ENOSPC ? PROS_VFS_EOVERFLOW : PROS_VFS_EIO;
    rc = pfs_find_free_data_block(data->fs, &entry_lba);
    if (rc != PROSENJITFS_OK)
        return rc == PROSENJITFS_ENOSPC ? PROS_VFS_EOVERFLOW : PROS_VFS_EIO;

    dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    entry_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    inode_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!dir_block || !entry_block || !inode_block) {
        if (dir_block) kfree(dir_block);
        if (entry_block) kfree(entry_block);
        if (inode_block) kfree(inode_block);
        return PROS_VFS_EIO;
    }

    if (pfs_read_block(data->fs->mount.device, parent_inode.dir_index, dir_block) !=
        PROSENJITFS_OK ||
        pfs_read_block(data->fs->mount.device, entry_lba, entry_block) !=
        PROSENJITFS_OK) {
        kfree(dir_block); kfree(entry_block); kfree(inode_block);
        return PROS_VFS_EIO;
    }
    pfs_zero(entry_block, data->fs->sb.block_size);
    pfs_zero(&dir_entry, sizeof(dir_entry));
    dir_entry.magic = PROSENJITFS_ENTRY_MAGIC;
    dir_entry.next = dir_index.heads[pfs_name_hash(
        (const uint8_t *)name, name_length) % PROSENJITFS_DIR_BUCKETS];
    dir_entry.inode = inode_number;
    dir_entry.name_hash = pfs_name_hash((const uint8_t *)name, name_length);
    dir_entry.type = PROSENJITFS_INODE_FILE;
    dir_entry.name_length = (uint16_t)name_length;
    for (uint32_t i = 0u; i < name_length; ++i)
        dir_entry.name[i] = (uint8_t)name[i];
    dir_entry.checksum = pfs_crc_with_zero32(
        (const uint8_t *)&dir_entry, sizeof(dir_entry),
        (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
    pfs_copy(entry_block, &dir_entry, sizeof(dir_entry));

    dir_bucket = dir_entry.name_hash % PROSENJITFS_DIR_BUCKETS;
    dir_index.heads[dir_bucket] = entry_lba;
    pfs_zero(dir_block, data->fs->sb.block_size);
    pfs_copy(dir_block, &dir_index, sizeof(dir_index));
    pfs_put32(dir_block + offsetof(struct pros_prosenjitfs_dir_index, checksum),
              pfs_crc_with_zero32(dir_block, sizeof(dir_index),
                                  (uint32_t)offsetof(
                                      struct pros_prosenjitfs_dir_index, checksum)));

    pfs_zero(&child_inode, sizeof(child_inode));
    child_inode.type = PROSENJITFS_INODE_FILE;
    child_inode.mode = 0644u;
    child_inode.uid = 0u;
    child_inode.gid = 0u;
    child_inode.links = 1u;
    child_inode.generation = 1u;
    child_inode.parent_inode = data->inode_number;
    child_inode.size = 0u;
    child_inode.allocated_bytes = 0u;

    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    inode_lba = data->fs->sb.inode_table_start +
                byte_offset / data->fs->sb.block_size;
    inode_offset = (uint32_t)(byte_offset % data->fs->sb.block_size);
    if (pfs_read_block(data->fs->mount.device, inode_lba, inode_block) !=
        PROSENJITFS_OK) {
        kfree(dir_block); kfree(entry_block); kfree(inode_block);
        return PROS_VFS_EIO;
    }
    pfs_copy(inode_block + inode_offset, &child_inode, sizeof(child_inode));
    pfs_put32(inode_block + inode_offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(inode_block + inode_offset,
                                  sizeof(child_inode),
                                  (uint32_t)offsetof(
                                      struct pros_prosenjitfs_inode, checksum)));

    rc = pfs_tx_reset(data->fs);
    if (rc == PROSENJITFS_OK)
        rc = pfs_inode_bitmap_stage(data->fs, inode_number, 1u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_allocator_stage(data->fs, entry_lba, 1u, 1u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, inode_lba, inode_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, parent_inode.dir_index, dir_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, entry_lba, entry_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_commit(data->fs);

    kfree(dir_block);
    kfree(entry_block);
    kfree(inode_block);
    if (rc != PROSENJITFS_OK)
        return PROS_VFS_EIO;

    return pfs_make_vnode(data->fs, inode_number, parent, result);
}

static int pfs_vnode_mkdir(struct pros_vfs_vnode *parent, const char *name,
                           struct pros_vfs_vnode **result)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode parent_inode;
    struct pros_prosenjitfs_dir_index parent_index;
    struct pros_prosenjitfs_inode child_inode;
    struct pros_prosenjitfs_dir_index child_index;
    struct pros_prosenjitfs_dir_entry dir_entry;
    uint8_t *parent_block = 0;
    uint8_t *child_index_block = 0;
    uint8_t *entry_block = 0;
    uint8_t *inode_block = 0;
    uint64_t inode_number;
    uint64_t entry_lba;
    uint64_t child_index_lba;
    uint64_t byte_offset;
    uint64_t inode_lba;
    uint32_t inode_offset;
    uint32_t name_length;
    uint32_t bucket;
    int rc;

    if (!parent || !name || !result || !parent->private_data ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    *result = 0;
    data = (struct pfs_vnode_data *)parent->private_data;
    if (!data->fs || data->fs->read_only)
        return PROS_VFS_EPERM;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK)
        return PROS_VFS_EINVAL;

    if (prosenjitfs_read_inode(data->fs, data->inode_number, &parent_inode) !=
        PROSENJITFS_OK || parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_EIO;
    rc = pfs_find_dir_entry(data->fs, &parent_inode, name, 0);
    if (rc == PROSENJITFS_OK)
        return PROS_VFS_EEXIST;
    if (rc != PROSENJITFS_ENOENT)
        return PROS_VFS_EIO;
    if (pfs_read_dir_index(data->fs, &parent_inode, &parent_index) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;

    rc = pfs_find_free_inode(data->fs, &inode_number);
    if (rc != PROSENJITFS_OK)
        return rc == PROSENJITFS_ENOSPC ? PROS_VFS_EOVERFLOW : PROS_VFS_EIO;
    rc = pfs_find_free_data_block(data->fs, &child_index_lba);
    if (rc != PROSENJITFS_OK)
        return rc == PROSENJITFS_ENOSPC ? PROS_VFS_EOVERFLOW : PROS_VFS_EIO;

    /*
     * Do not assume the next free block is still free: find a second block
     * without mutating disk, and reject the pathological one-block case.
     */
    entry_lba = child_index_lba + 1u;
    while (entry_lba < data->fs->sb.total_blocks &&
           entry_lba == child_index_lba)
        ++entry_lba;
    if (entry_lba >= data->fs->sb.total_blocks)
        return PROS_VFS_EOVERFLOW;
    {
        uint8_t bitmap[4096];
        uint64_t bits_per_block = (uint64_t)data->fs->sb.block_size * 8u;
        uint64_t bi = entry_lba / bits_per_block;
        if (data->fs->sb.block_size > sizeof(bitmap) ||
            bi >= data->fs->sb.block_bitmap_blocks ||
            pfs_read_block(data->fs->mount.device,
                           data->fs->sb.block_bitmap_start + bi, bitmap) !=
                           PROSENJITFS_OK ||
            pfs_bitmap_bit(bitmap, entry_lba % bits_per_block))
            return PROS_VFS_EOVERFLOW;
    }

    parent_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    child_index_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    entry_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    inode_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!parent_block || !child_index_block || !entry_block || !inode_block) {
        if (parent_block) kfree(parent_block);
        if (child_index_block) kfree(child_index_block);
        if (entry_block) kfree(entry_block);
        if (inode_block) kfree(inode_block);
        return PROS_VFS_EIO;
    }

    if (pfs_read_block(data->fs->mount.device, parent_inode.dir_index,
                       parent_block) != PROSENJITFS_OK ||
        pfs_read_block(data->fs->mount.device, child_index_lba,
                       child_index_block) != PROSENJITFS_OK ||
        pfs_read_block(data->fs->mount.device, entry_lba,
                       entry_block) != PROSENJITFS_OK) {
        kfree(parent_block); kfree(child_index_block);
        kfree(entry_block); kfree(inode_block);
        return PROS_VFS_EIO;
    }

    pfs_zero(child_index_block, data->fs->sb.block_size);
    pfs_zero(&child_index, sizeof(child_index));
    child_index.magic = PROSENJITFS_DIR_MAGIC;
    child_index.version = PROSENJITFS_VERSION;
    child_index.bucket_count = PROSENJITFS_DIR_BUCKETS;
    child_index.inode = inode_number;
    child_index.generation = 1u;
    pfs_copy(child_index_block, &child_index, sizeof(child_index));
    pfs_put32(child_index_block +
              offsetof(struct pros_prosenjitfs_dir_index, checksum),
              pfs_crc_with_zero32(
                  child_index_block, sizeof(child_index),
                  (uint32_t)offsetof(
                      struct pros_prosenjitfs_dir_index, checksum)));

    pfs_zero(entry_block, data->fs->sb.block_size);
    pfs_zero(&dir_entry, sizeof(dir_entry));
    dir_entry.magic = PROSENJITFS_ENTRY_MAGIC;
    dir_entry.name_hash = pfs_name_hash((const uint8_t *)name, name_length);
    bucket = dir_entry.name_hash % PROSENJITFS_DIR_BUCKETS;
    dir_entry.next = parent_index.heads[bucket];
    dir_entry.inode = inode_number;
    dir_entry.type = PROSENJITFS_INODE_DIR;
    dir_entry.name_length = (uint16_t)name_length;
    for (uint32_t i = 0u; i < name_length; ++i)
        dir_entry.name[i] = (uint8_t)name[i];
    dir_entry.checksum = pfs_crc_with_zero32(
        (const uint8_t *)&dir_entry, sizeof(dir_entry),
        (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
    pfs_copy(entry_block, &dir_entry, sizeof(dir_entry));
    parent_index.heads[bucket] = entry_lba;
    pfs_zero(parent_block, data->fs->sb.block_size);
    pfs_copy(parent_block, &parent_index, sizeof(parent_index));
    pfs_put32(parent_block + offsetof(struct pros_prosenjitfs_dir_index, checksum),
              pfs_crc_with_zero32(
                  parent_block, sizeof(parent_index),
                  (uint32_t)offsetof(
                      struct pros_prosenjitfs_dir_index, checksum)));

    pfs_zero(&child_inode, sizeof(child_inode));
    child_inode.type = PROSENJITFS_INODE_DIR;
    child_inode.mode = 0755u;
    child_inode.uid = 0u;
    child_inode.gid = 0u;
    child_inode.links = 1u;
    child_inode.generation = 1u;
    child_inode.parent_inode = data->inode_number;
    child_inode.dir_index = child_index_lba;

    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    inode_lba = data->fs->sb.inode_table_start +
                byte_offset / data->fs->sb.block_size;
    inode_offset = (uint32_t)(byte_offset % data->fs->sb.block_size);
    if (pfs_read_block(data->fs->mount.device, inode_lba, inode_block) !=
        PROSENJITFS_OK) {
        kfree(parent_block); kfree(child_index_block);
        kfree(entry_block); kfree(inode_block);
        return PROS_VFS_EIO;
    }
    pfs_copy(inode_block + inode_offset, &child_inode, sizeof(child_inode));
    pfs_put32(inode_block + inode_offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(
                  inode_block + inode_offset, sizeof(child_inode),
                  (uint32_t)offsetof(
                      struct pros_prosenjitfs_inode, checksum)));

    rc = pfs_tx_reset(data->fs);
    if (rc == PROSENJITFS_OK)
        rc = pfs_inode_bitmap_stage(data->fs, inode_number, 1u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_allocator_stage(data->fs, child_index_lba, 1u, 1u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_allocator_stage(data->fs, entry_lba, 1u, 1u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, inode_lba, inode_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, child_index_lba, child_index_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, parent_inode.dir_index, parent_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, entry_lba, entry_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_commit(data->fs);

    kfree(parent_block); kfree(child_index_block);
    kfree(entry_block); kfree(inode_block);
    if (rc != PROSENJITFS_OK)
        return rc == PROSENJITFS_ENOSPC ? PROS_VFS_EOVERFLOW :
               rc == PROSENJITFS_EOVERFLOW ? PROS_VFS_EOVERFLOW :
               PROS_VFS_EIO;

    return pfs_make_vnode(data->fs, inode_number, parent, result);
}



static void pfs_vnode_release(struct pros_vfs_vnode *vnode)
{
    struct pros_vfs_vnode *parent;
    if (!vnode)
        return;
    parent = vnode->parent;
    vnode->parent = 0;
    if (vnode->private_data)
        kfree(vnode->private_data);
    vnode->private_data = 0;
    kfree(vnode);
    if (parent)
        vfs_vnode_put(parent);
}


static uint32_t pfs_name_hash(const uint8_t *name, uint32_t length)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < length; ++i) {
        hash ^= name[i];
        hash *= 16777619u;
    }
    return hash;
}

static int pfs_utf8_name_valid(const char *name, uint32_t *length_out)
{
    const uint8_t *s = (const uint8_t *)name;
    uint32_t length = 0u;
    if (!name || !length_out)
        return PROSENJITFS_EINVAL;
    while (s[length] != 0u) {
        if (s[length] == '/' || length >= PROSENJITFS_MAX_NAME)
            return PROSENJITFS_EINVAL;
        ++length;
    }
    if (length == 0u || (length == 1u && s[0] == '.') ||
        (length == 2u && s[0] == '.' && s[1] == '.'))
        return PROSENJITFS_EINVAL;
    for (uint32_t i = 0u; i < length;) {
        uint8_t c = s[i];
        uint32_t need, cp;
        if (c < 0x80u) {
            ++i;
            continue;
        } else if (c >= 0xC2u && c <= 0xDFu) {
            need = 1u; cp = c & 0x1Fu;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            need = 2u; cp = c & 0x0Fu;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            need = 3u; cp = c & 0x07u;
        } else {
            return PROSENJITFS_EINVAL;
        }
        if (i + need >= length)
            return PROSENJITFS_EINVAL;
        for (uint32_t j = 1u; j <= need; ++j) {
            if ((s[i + j] & 0xC0u) != 0x80u)
                return PROSENJITFS_EINVAL;
            cp = (cp << 6) | (s[i + j] & 0x3Fu);
        }
        if ((need == 2u && cp < 0x800u) ||
            (need == 3u && cp < 0x10000u) ||
            (cp >= 0xD800u && cp <= 0xDFFFu) ||
            cp > 0x10FFFFu)
            return PROSENJITFS_EINVAL;
        i += need + 1u;
    }
    *length_out = length;
    return PROSENJITFS_OK;
}

static int pfs_read_dir_index(const struct pros_prosenjitfs *fs,
                              const struct pros_prosenjitfs_inode *inode,
                              struct pros_prosenjitfs_dir_index *index)
{
    uint8_t block[4096];
    uint32_t checksum;
    if (!fs || !inode || !index || inode->type != PROSENJITFS_INODE_DIR ||
        inode->dir_index < 2u || inode->dir_index >= fs->sb.total_blocks ||
        fs->sb.block_size > sizeof(block))
        return PROSENJITFS_ECORRUPT;
    if (pfs_read_block(fs->mount.device, inode->dir_index, block) !=
        PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_copy(index, block, sizeof(*index));
    if (index->magic != PROSENJITFS_DIR_MAGIC ||
        index->version != PROSENJITFS_VERSION ||
        index->bucket_count != PROSENJITFS_DIR_BUCKETS ||
        index->inode >= fs->sb.inode_count)
        return PROSENJITFS_ECORRUPT;
    checksum = pfs_crc_with_zero32(block, sizeof(*index),
                                   (uint32_t)offsetof(
                                       struct pros_prosenjitfs_dir_index,
                                       checksum));
    if (checksum != index->checksum)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static int pfs_read_dir_entry(const struct pros_prosenjitfs *fs,
                              uint64_t block_lba,
                              struct pros_prosenjitfs_dir_entry *entry)
{
    uint8_t block[4096];
    uint32_t name_length;
    uint32_t checksum;
    if (!fs || !entry || fs->sb.block_size > sizeof(block) ||
        block_lba < fs->sb.data_start || block_lba >= fs->sb.total_blocks)
        return PROSENJITFS_ECORRUPT;
    if (pfs_read_block(fs->mount.device, block_lba, block) !=
        PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_copy(entry, block, sizeof(*entry));
    if (entry->magic != PROSENJITFS_ENTRY_MAGIC ||
        entry->name_length == 0u ||
        entry->name_length > PROSENJITFS_MAX_NAME ||
        entry->name[entry->name_length] != 0u ||
        entry->inode >= fs->sb.inode_count ||
        (entry->type != PROSENJITFS_INODE_FILE &&
         entry->type != PROSENJITFS_INODE_DIR) ||
        (entry->next != 0u &&
         (entry->next < fs->sb.data_start ||
          entry->next >= fs->sb.total_blocks)))
        return PROSENJITFS_ECORRUPT;
    checksum = pfs_crc_with_zero32(block, sizeof(*entry),
                                   (uint32_t)offsetof(
                                       struct pros_prosenjitfs_dir_entry,
                                       checksum));
    if (checksum != entry->checksum)
        return PROSENJITFS_ECORRUPT;
    if (pfs_utf8_name_valid((const char *)entry->name, &name_length) !=
        PROSENJITFS_OK || name_length != entry->name_length)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static int pfs_find_dir_entry(const struct pros_prosenjitfs *fs,
                              const struct pros_prosenjitfs_inode *dir_inode,
                              const char *name,
                              struct pros_prosenjitfs_dir_entry *found)
{
    struct pros_prosenjitfs_dir_index index;
    struct pros_prosenjitfs_dir_entry entry;
    uint32_t name_length;
    uint32_t hash;
    uint64_t current;
    uint64_t steps = 0u;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    if (pfs_read_dir_index(fs, dir_inode, &index) != PROSENJITFS_OK)
        return PROSENJITFS_ECORRUPT;
    hash = pfs_name_hash((const uint8_t *)name, name_length);
    current = index.heads[hash % PROSENJITFS_DIR_BUCKETS];
    while (current != 0u) {
        if (++steps > fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_dir_entry(fs, current, &entry) != PROSENJITFS_OK)
            return PROSENJITFS_ECORRUPT;
        if (entry.name_hash == hash && entry.name_length == name_length) {
            uint32_t equal = 1u;
            for (uint32_t i = 0u; i < name_length; ++i)
                if (entry.name[i] != (uint8_t)name[i]) {
                    equal = 0u;
                    break;
                }
            if (equal) {
                if (found)
                    pfs_copy(found, &entry, sizeof(*found));
                return PROSENJITFS_OK;
            }
        }
        current = entry.next;
    }
    return PROSENJITFS_ENOENT;
}

static int pfs_dir_entry_by_ordinal(const struct pros_prosenjitfs *fs,
                                    const struct pros_prosenjitfs_inode *dir_inode,
                                    uint64_t ordinal,
                                    struct pros_prosenjitfs_dir_entry *found)
{
    struct pros_prosenjitfs_dir_index index;
    struct pros_prosenjitfs_dir_entry entry;
    uint64_t seen = 0u;
    if (pfs_read_dir_index(fs, dir_inode, &index) != PROSENJITFS_OK)
        return PROSENJITFS_ECORRUPT;
    for (uint32_t bucket = 0u; bucket < PROSENJITFS_DIR_BUCKETS; ++bucket) {
        uint64_t current = index.heads[bucket];
        uint64_t steps = 0u;
        while (current != 0u) {
            if (++steps > fs->sb.total_blocks)
                return PROSENJITFS_ECORRUPT;
            if (pfs_read_dir_entry(fs, current, &entry) != PROSENJITFS_OK)
                return PROSENJITFS_ECORRUPT;
            if (seen == ordinal) {
                if (found)
                    pfs_copy(found, &entry, sizeof(*found));
                return PROSENJITFS_OK;
            }
            ++seen;
            current = entry.next;
        }
    }
    return PROSENJITFS_ENOENT;
}

static int pfs_vnode_lookup(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result)
{
    struct pfs_vnode_data *data;
    if (!parent || !name || !result || !parent->private_data ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    *result = 0;
    data = (struct pfs_vnode_data *)parent->private_data;

    if (name[0] == '.' && name[1] == '\0') {
        if (vfs_vnode_get(parent) != PROS_VFS_OK)
            return PROS_VFS_EINVAL;
        *result = parent;
        return PROS_VFS_OK;
    }

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        struct pros_vfs_vnode *p = parent->parent ? parent->parent : parent;
        if (vfs_vnode_get(p) != PROS_VFS_OK)
            return PROS_VFS_EINVAL;
        *result = p;
        return PROS_VFS_OK;
    }

    {
        struct pros_prosenjitfs_inode dir_inode;
        struct pros_prosenjitfs_dir_entry disk_entry;
        int rc;
        if (prosenjitfs_read_inode(data->fs, data->inode_number, &dir_inode) !=
            PROSENJITFS_OK || dir_inode.type != PROSENJITFS_INODE_DIR)
            return PROS_VFS_EIO;
        rc = pfs_find_dir_entry(data->fs, &dir_inode, name, &disk_entry);
        if (rc == PROSENJITFS_ENOENT)
            return PROS_VFS_ENOENT;
        if (rc != PROSENJITFS_OK)
            return PROS_VFS_EIO;
        return pfs_make_vnode(data->fs, disk_entry.inode, parent, result);
    }
}

static int pfs_vnode_readdir(struct pros_vfs_vnode *directory, uint64_t *cookie,
                             struct pros_vfs_dirent *entry)
{
    if (!directory || !cookie || !entry ||
        directory->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    pfs_zero(entry, sizeof(*entry));
    if (*cookie == 0u) {
        pfs_copy(entry->name, ".", 2u);
        entry->type = PROS_VFS_NODE_DIRECTORY;
        ++*cookie;
        return PROS_VFS_OK;
    }
    if (*cookie == 1u) {
        pfs_copy(entry->name, "..", 3u);
        entry->type = PROS_VFS_NODE_DIRECTORY;
        ++*cookie;
        return PROS_VFS_OK;
    }
    {
        struct pfs_vnode_data *data;
        struct pros_prosenjitfs_inode dir_inode;
        struct pros_prosenjitfs_inode child_inode;
        struct pros_prosenjitfs_dir_entry disk_entry;
        uint64_t ordinal = *cookie - 2u;
        if (!directory->private_data)
            return PROS_VFS_EINVAL;
        data = (struct pfs_vnode_data *)directory->private_data;
        if (prosenjitfs_read_inode(data->fs, data->inode_number, &dir_inode) !=
            PROSENJITFS_OK || dir_inode.type != PROSENJITFS_INODE_DIR)
            return PROS_VFS_EIO;
        {
            int dir_rc = pfs_dir_entry_by_ordinal(data->fs, &dir_inode,
                                                  ordinal, &disk_entry);
            if (dir_rc == PROSENJITFS_ENOENT)
                return PROS_VFS_ENOENT;
            if (dir_rc != PROSENJITFS_OK)
                return PROS_VFS_EIO;
        }
        if (prosenjitfs_read_inode(data->fs, disk_entry.inode, &child_inode) !=
            PROSENJITFS_OK)
            return PROS_VFS_EIO;
        for (uint32_t i = 0u; i < disk_entry.name_length; ++i)
            entry->name[i] = (char)disk_entry.name[i];
        entry->name[disk_entry.name_length] = '\0';
        entry->type = child_inode.type == PROSENJITFS_INODE_DIR
                    ? PROS_VFS_NODE_DIRECTORY : PROS_VFS_NODE_FILE;
        entry->size = child_inode.size;
        ++*cookie;
        return PROS_VFS_OK;
    }
}

static int pfs_tx_reset(struct pros_prosenjitfs *fs)
{
    if (!fs || !fs->scratch || !fs->tx.images)
        return PROSENJITFS_EINVAL;
    fs->tx.count = 0u;
    for (uint32_t i = 0; i < PROSENJITFS_MAX_TX_BLOCKS; ++i)
        fs->tx.targets[i] = 0u;
    return PROSENJITFS_OK;
}

static int pfs_tx_add(struct pros_prosenjitfs *fs, uint64_t target,
                      const void *image)
{
    if (!fs || !image || target >= fs->sb.total_blocks)
        return PROSENJITFS_EINVAL;
    for (uint32_t i = 0; i < fs->tx.count; ++i)
        if (fs->tx.targets[i] == target) {
            pfs_copy(fs->tx.images + (uint64_t)i * fs->sb.block_size,
                     image, fs->sb.block_size);
            return PROSENJITFS_OK;
        }
    if (fs->tx.count >= PROSENJITFS_MAX_TX_BLOCKS)
        return PROSENJITFS_EOVERFLOW;
    fs->tx.targets[fs->tx.count] = target;
    pfs_copy(fs->tx.images + (uint64_t)fs->tx.count * fs->sb.block_size,
             image, fs->sb.block_size);
    ++fs->tx.count;
    return PROSENJITFS_OK;
}

static uint32_t pfs_journal_checksum(const uint8_t *header,
                                     const uint8_t *images,
                                     uint32_t block_size,
                                     uint32_t count,
                                     const uint8_t *commit)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < block_size; ++i) {
        uint8_t v = (i >= offsetof(struct pros_prosenjitfs_journal_header, checksum) &&
                     i < offsetof(struct pros_prosenjitfs_journal_header, checksum) + 4u)
                    ? 0u : header[i];
        crc ^= v;
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    for (uint32_t n = 0; n < count * block_size; ++n) {
        crc ^= images[n];
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    for (uint32_t i = 0; i < block_size; ++i) {
        uint8_t v = (i >= offsetof(struct pros_prosenjitfs_journal_commit, checksum) &&
                     i < offsetof(struct pros_prosenjitfs_journal_commit, checksum) + 4u)
                    ? 0u : commit[i];
        crc ^= v;
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    return ~crc;
}

static int pfs_journal_clear(struct pros_prosenjitfs *fs)
{
    pfs_zero(fs->scratch, fs->sb.block_size);
    for (uint32_t i = 0; i < fs->sb.journal_blocks; ++i)
        if (pfs_write_block(fs->mount.device, fs->sb.journal_start + i,
                            fs->scratch) != PROSENJITFS_OK)
            return PROSENJITFS_EIO;
    return pfs_flush(fs->mount.device);
}

static int pfs_journal_recover(struct pros_prosenjitfs *fs)
{
    pfs_journal_debug_stage = 0;
    uint8_t *header = fs->scratch;
    uint8_t *images = fs->tx.images;
    uint8_t *commit = images + (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                                  fs->sb.block_size;
    struct pros_prosenjitfs_journal_header h;
    struct pros_prosenjitfs_journal_commit c;
    uint32_t expected;

    if (pfs_read_block(fs->mount.device, fs->sb.journal_start, header) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_journal_debug_stage = 1;
    pfs_copy(&h, header, sizeof(h));
    if (h.magic == 0u)
        return PROSENJITFS_OK;
    pfs_journal_debug_stage = 2;
    if (h.magic != PROSENJITFS_JOURNAL_MAGIC ||
        h.record_count == 0u || h.record_count > PROSENJITFS_MAX_TX_BLOCKS ||
        h.record_count + 2u > fs->sb.journal_blocks ||
        pfs_crc_with_zero32(header, fs->sb.block_size,
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_journal_header,
                                checksum)) != h.checksum)
        return PROSENJITFS_ECORRUPT;

    pfs_journal_debug_stage = 3;
    for (uint32_t i = 0; i < h.record_count; ++i) {
        if (h.targets[i] >= fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_block(fs->mount.device,
                           fs->sb.journal_start + 1u + i,
                           images + (uint64_t)i * fs->sb.block_size) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
    }
    pfs_journal_debug_stage = 4;
    if (pfs_read_block(fs->mount.device,
                       fs->sb.journal_start + 1u + h.record_count,
                       commit) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_copy(&c, commit, sizeof(c));
    if (c.magic == 0u)
        return pfs_journal_clear(fs);
    pfs_journal_debug_stage = 5;
    if (c.magic != PROSENJITFS_JOURNAL_COMMIT ||
        c.transaction_id != h.transaction_id ||
        c.record_count != h.record_count)
        return PROSENJITFS_ECORRUPT;
    expected = pfs_journal_checksum(header, images, fs->sb.block_size,
                                    h.record_count, commit);
    if (expected != c.checksum)
        return PROSENJITFS_ECORRUPT;
    pfs_journal_debug_stage = 6;

    for (uint32_t i = 0; i < h.record_count; ++i)
        if (pfs_write_block(fs->mount.device, h.targets[i],
                            images + (uint64_t)i * fs->sb.block_size) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
    if (pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    return pfs_journal_clear(fs);
}

static int pfs_tx_commit(struct pros_prosenjitfs *fs)
{
    uint8_t *header = fs->scratch;
    uint8_t *commit = fs->tx.images + (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                                      fs->sb.block_size;
    struct pros_prosenjitfs_journal_header h;
    struct pros_prosenjitfs_journal_commit c;
    uint32_t checksum;

    if (!fs || fs->read_only || fs->tx.count == 0u ||
        fs->tx.count + 2u > fs->sb.journal_blocks)
        return PROSENJITFS_EINVAL;

    pfs_zero(header, fs->sb.block_size);
    pfs_zero(commit, fs->sb.block_size);
    pfs_zero(&h, sizeof(h));
    h.magic = PROSENJITFS_JOURNAL_MAGIC;
    h.record_count = fs->tx.count;
    h.transaction_id = fs->sb.transaction_sequence++;
    for (uint32_t i = 0; i < fs->tx.count; ++i)
        h.targets[i] = fs->tx.targets[i];
    pfs_copy(header, &h, sizeof(h));
    h.checksum = pfs_crc_with_zero32(header, fs->sb.block_size,
                                     (uint32_t)offsetof(
                                         struct pros_prosenjitfs_journal_header,
                                         checksum));
    pfs_copy(header, &h, sizeof(h));

    if (pfs_write_block(fs->mount.device, fs->sb.journal_start,
                        header) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    for (uint32_t i = 0; i < fs->tx.count; ++i)
        if (pfs_write_block(fs->mount.device,
                            fs->sb.journal_start + 1u + i,
                            fs->tx.images + (uint64_t)i * fs->sb.block_size) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
    if (pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;

    pfs_zero(&c, sizeof(c));
    c.magic = PROSENJITFS_JOURNAL_COMMIT;
    c.record_count = fs->tx.count;
    c.transaction_id = h.transaction_id;
    pfs_copy(commit, &c, sizeof(c));
    checksum = pfs_journal_checksum(header, fs->tx.images,
                                    fs->sb.block_size, fs->tx.count, commit);
    c.checksum = checksum;
    pfs_copy(commit, &c, sizeof(c));
    if (pfs_write_block(fs->mount.device,
                        fs->sb.journal_start + 1u + fs->tx.count,
                        commit) != PROSENJITFS_OK ||
        pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;

    for (uint32_t i = 0; i < fs->tx.count; ++i)
        if (pfs_write_block(fs->mount.device, fs->tx.targets[i],
                            fs->tx.images + (uint64_t)i * fs->sb.block_size) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
    if (pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;

    return pfs_journal_clear(fs);
}

static int pfs_bitmap_bit(const uint8_t *bitmap, uint64_t bit)
{
    return (bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0;
}

static int pfs_allocator_stage(struct pros_prosenjitfs *fs,
                              uint64_t first, uint32_t count,
                              uint8_t allocate)
{
    uint64_t bits_per_block;
    uint64_t last;
    uint64_t first_bitmap;
    uint64_t last_bitmap;

    if (!fs || count == 0u || first < fs->sb.data_start ||
        first >= fs->sb.total_blocks ||
        (uint64_t)count > fs->sb.total_blocks - first)
        return PROSENJITFS_EINVAL;

    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    last = first + (uint64_t)count - 1u;
    first_bitmap = first / bits_per_block;
    last_bitmap = last / bits_per_block;

    if (last_bitmap < first_bitmap ||
        last_bitmap - first_bitmap + 1u > PROSENJITFS_MAX_TX_BLOCKS ||
        fs->sb.block_size > 4096u)
        return PROSENJITFS_EOVERFLOW;

    for (uint64_t bi = first_bitmap; bi <= last_bitmap; ++bi) {
        uint8_t image[4096];
        uint64_t lba = fs->sb.block_bitmap_start + bi;
        uint64_t bitmap_base = bi * bits_per_block;
        uint64_t range_start = first > bitmap_base ? first - bitmap_base : 0u;
        uint64_t range_end = last < bitmap_base + bits_per_block - 1u
                           ? last - bitmap_base : bits_per_block - 1u;
        uint32_t slot;

        /*
         * Reuse an already-staged bitmap image when multiple allocations in
         * the same bitmap block belong to one transaction. Reading the home
         * block again here would discard earlier staged bit changes.
         */
        slot = 0u;
        while (slot < fs->tx.count && fs->tx.targets[slot] != lba)
            ++slot;
        if (slot == fs->tx.count) {
            if (pfs_read_block(fs->mount.device, lba, image) != PROSENJITFS_OK)
                return PROSENJITFS_EIO;
            if (pfs_tx_add(fs, lba, image) != PROSENJITFS_OK)
                return PROSENJITFS_EOVERFLOW;
            slot = 0u;
            while (slot < fs->tx.count && fs->tx.targets[slot] != lba)
                ++slot;
            if (slot == fs->tx.count)
                return PROSENJITFS_EINVAL;
        }

        for (uint64_t bit = range_start; bit <= range_end; ++bit) {
            uint8_t *tx_image =
                fs->tx.images + (uint64_t)slot * fs->sb.block_size;
            uint8_t mask = (uint8_t)(1u << (bit & 7u));
            uint8_t old = tx_image[bit >> 3];

            if (allocate) {
                if (old & mask)
                    return PROSENJITFS_EEXIST;
                tx_image[bit >> 3] = (uint8_t)(old | mask);
            } else {
                if ((old & mask) == 0u)
                    return PROSENJITFS_EINVAL;
                tx_image[bit >> 3] = (uint8_t)(old & (uint8_t)~mask);
            }

            if (bit == range_end)
                break;
        }
    }
    return PROSENJITFS_OK;
}

static int pfs_block_allocated(const struct pros_prosenjitfs *fs,
                                  uint64_t block)
{
    uint64_t bits_per_block;
    uint64_t bitmap_index;
    uint8_t bitmap[4096];

    if (!fs || block >= fs->sb.total_blocks ||
        fs->sb.block_size > sizeof(bitmap))
        return PROSENJITFS_ECORRUPT;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    bitmap_index = block / bits_per_block;
    if (bitmap_index >= fs->sb.block_bitmap_blocks ||
        pfs_read_block(fs->mount.device,
                       fs->sb.block_bitmap_start + bitmap_index,
                       bitmap) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    return pfs_bitmap_bit(bitmap, block % bits_per_block) ?
           1 : 0;
}

static int pfs_validate_extent_chain(const struct pros_prosenjitfs *fs,
                                     const struct pros_prosenjitfs_inode *inode)
{
    uint64_t current;
    uint64_t logical_end = 0u;
    uint64_t block_sum = 0u;
    uint64_t guard = 0u;
    uint64_t required_blocks;

    if (!fs || !inode || inode->type != PROSENJITFS_INODE_FILE)
        return PROSENJITFS_ECORRUPT;
    current = inode->extent_head;
    required_blocks = (inode->size + fs->sb.block_size - 1u) /
                      fs->sb.block_size;

    while (current != 0u) {
        struct pros_prosenjitfs_extent extent;
        uint64_t previous = inode->extent_head;
        uint64_t scan_guard = 0u;
        int allocated;

        if (++guard > fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        allocated = pfs_block_allocated(fs, current);
        if (allocated != 1)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_extent((struct pros_prosenjitfs *)fs, current,
                            &extent) != PROSENJITFS_OK)
            return PROSENJITFS_ECORRUPT;
        if (extent.logical_block < logical_end)
            return PROSENJITFS_ECORRUPT;
        if ((uint64_t)extent.block_count >
            fs->sb.total_blocks - extent.physical_block)
            return PROSENJITFS_ECORRUPT;
        if (extent.physical_block <= current &&
            current - extent.physical_block < extent.block_count)
            return PROSENJITFS_ECORRUPT;
        if (pfs_block_allocated(fs, extent.physical_block) != 1 ||
            pfs_block_allocated(fs, extent.physical_block +
                                extent.block_count - 1u) != 1)
            return PROSENJITFS_ECORRUPT;
        if (block_sum > UINT64_MAX - extent.block_count)
            return PROSENJITFS_EOVERFLOW;
        block_sum += extent.block_count;

        /*
         * Extent physical ranges must not overlap. The format is linked,
         * so a bounded rescan gives deterministic corruption detection
         * without allocating a bitmap proportional to the whole volume.
         */
        while (previous != current) {
            struct pros_prosenjitfs_extent prior;
            if (++scan_guard > fs->sb.total_blocks)
                return PROSENJITFS_ECORRUPT;
            if (pfs_read_extent((struct pros_prosenjitfs *)fs, previous,
                                &prior) != PROSENJITFS_OK)
                return PROSENJITFS_ECORRUPT;
            if (prior.physical_block <= current &&
                current - prior.physical_block < prior.block_count)
                return PROSENJITFS_ECORRUPT;
            if (extent.physical_block <= previous &&
                previous - extent.physical_block < extent.block_count)
                return PROSENJITFS_ECORRUPT;
            previous = prior.next;
        }

        if (extent.logical_block >
            UINT64_MAX - extent.block_count)
            return PROSENJITFS_ECORRUPT;
        logical_end = extent.logical_block + extent.block_count;
        current = extent.next;
    }

    if (block_sum > UINT64_MAX / fs->sb.block_size ||
        inode->allocated_bytes != block_sum * fs->sb.block_size)
        return PROSENJITFS_ECORRUPT;
    if (inode->size > inode->allocated_bytes)
        return PROSENJITFS_ECORRUPT;
    if ((inode->flags & PROSENJITFS_INODE_FLAG_SPARSE) == 0u &&
        logical_end != required_blocks)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static int pfs_validate_directory(const struct pros_prosenjitfs *fs,
                                  uint64_t inode_number,
                                  const struct pros_prosenjitfs_inode *inode)
{
    struct pros_prosenjitfs_dir_index index;

    if (!fs || !inode || inode->type != PROSENJITFS_INODE_DIR ||
        inode->dir_index < fs->sb.data_start ||
        inode->dir_index >= fs->sb.total_blocks ||
        pfs_block_allocated(fs, inode->dir_index) != 1 ||
        pfs_read_dir_index(fs, inode, &index) != PROSENJITFS_OK ||
        index.inode != inode_number)
        return PROSENJITFS_ECORRUPT;

    for (uint32_t bucket = 0u; bucket < PROSENJITFS_DIR_BUCKETS; ++bucket) {
        uint64_t current = index.heads[bucket];
        uint64_t guard = 0u;
        while (current != 0u) {
            struct pros_prosenjitfs_dir_entry entry;
            struct pros_prosenjitfs_inode child;
            uint32_t expected_hash;

            if (++guard > fs->sb.total_blocks ||
                pfs_block_allocated(fs, current) != 1 ||
                pfs_read_dir_entry(fs, current, &entry) != PROSENJITFS_OK)
                return PROSENJITFS_ECORRUPT;
            expected_hash = pfs_name_hash(entry.name, entry.name_length);
            if (entry.name_hash != expected_hash ||
                prosenjitfs_read_inode(fs, entry.inode, &child) !=
                    PROSENJITFS_OK ||
                child.links == 0u ||
                child.parent_inode != inode_number ||
                ((entry.type == PROSENJITFS_INODE_FILE) &&
                 child.type != PROSENJITFS_INODE_FILE) ||
                ((entry.type == PROSENJITFS_INODE_DIR) &&
                 child.type != PROSENJITFS_INODE_DIR))
                return PROSENJITFS_ECORRUPT;
            current = entry.next;
        }
    }
    return PROSENJITFS_OK;
}

static int pfs_validate_metadata(struct pros_prosenjitfs *fs)
{
    uint64_t bits_per_block;
    uint64_t root_bitmap_index;
    uint8_t inode_bitmap[4096];

    if (!fs || fs->sb.block_size > sizeof(inode_bitmap))
        return PROSENJITFS_ECORRUPT;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    root_bitmap_index = fs->sb.root_inode / bits_per_block;
    if (root_bitmap_index >= fs->sb.inode_bitmap_blocks ||
        pfs_read_block(fs->mount.device,
                       fs->sb.inode_bitmap_start + root_bitmap_index,
                       inode_bitmap) != PROSENJITFS_OK ||
        !pfs_bitmap_bit(inode_bitmap, fs->sb.root_inode % bits_per_block))
        return PROSENJITFS_ECORRUPT;

    for (uint64_t inode_number = 0u;
         inode_number < fs->sb.inode_count; ++inode_number) {
        uint64_t bitmap_index = inode_number / bits_per_block;
        int allocated;
        struct pros_prosenjitfs_inode inode;
        int rc;

        if (bitmap_index >= fs->sb.inode_bitmap_blocks ||
            pfs_read_block(fs->mount.device,
                           fs->sb.inode_bitmap_start + bitmap_index,
                           inode_bitmap) != PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        allocated = pfs_bitmap_bit(inode_bitmap,
                                    inode_number % bits_per_block);
        rc = prosenjitfs_read_inode(fs, inode_number, &inode);
        if (allocated) {
            if (rc != PROSENJITFS_OK)
                return PROSENJITFS_ECORRUPT;
            if (inode_number == fs->sb.root_inode &&
                inode.type != PROSENJITFS_INODE_DIR)
                return PROSENJITFS_ECORRUPT;
            if (inode.type == PROSENJITFS_INODE_FILE) {
                if (pfs_validate_extent_chain(fs, &inode) !=
                    PROSENJITFS_OK)
                    return PROSENJITFS_ECORRUPT;
            } else if (inode.type == PROSENJITFS_INODE_DIR) {
                if (pfs_validate_directory(fs, inode_number, &inode) !=
                    PROSENJITFS_OK)
                    return PROSENJITFS_ECORRUPT;
            }
        } else if (rc != PROSENJITFS_ENOENT) {
            return PROSENJITFS_ECORRUPT;
        }
    }
    return PROSENJITFS_OK;
}

static int pfs_allocator_update(struct pros_prosenjitfs *fs,
                                uint64_t first, uint32_t count,
                                uint8_t allocate)
{
    if (!fs || fs->read_only || pfs_tx_reset(fs) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    if (pfs_allocator_stage(fs, first, count, allocate) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    return pfs_tx_commit(fs);
}

int prosenjitfs_alloc_blocks(struct pros_prosenjitfs *fs,
                             uint32_t count, uint64_t *first_block)
{
    uint64_t data_start;
    uint64_t run = 0u;

    if (!fs || !first_block || count == 0u || fs->read_only)
        return PROSENJITFS_EINVAL;

    data_start = pfs_data_start(&fs->sb);
    if (fs->sb.block_size > 4096u)
        return PROSENJITFS_EOVERFLOW;

    for (uint64_t b = data_start; b < fs->sb.total_blocks; ++b) {
        uint64_t bits_per_block = (uint64_t)fs->sb.block_size * 8u;
        uint64_t bitmap_lba = fs->sb.block_bitmap_start + b / bits_per_block;
        uint8_t bitbuf[4096];

        if (pfs_read_block(fs->mount.device, bitmap_lba, bitbuf) != PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        if (!pfs_bitmap_bit(bitbuf, b % bits_per_block)) {
            ++run;
            if (run == count) {
                *first_block = b + 1u - count;
                if (*first_block < data_start ||
                    count > fs->sb.total_blocks - *first_block)
                    return PROSENJITFS_EOVERFLOW;
                return pfs_allocator_update(fs, *first_block, count, 1u);
            }
        } else {
            run = 0u;
        }
    }
    return PROSENJITFS_ENOSPC;
}

int prosenjitfs_free_blocks(struct pros_prosenjitfs *fs,
                            uint64_t first_block, uint32_t count)
{
    uint64_t data_start;

    if (!fs || count == 0u || fs->read_only)
        return PROSENJITFS_EINVAL;
    data_start = pfs_data_start(&fs->sb);
    if (first_block < data_start ||
        (uint64_t)count > fs->sb.total_blocks - first_block)
        return PROSENJITFS_EINVAL;
    return pfs_allocator_update(fs, first_block, count, 0u);
}

static int pfs_inode_bitmap_stage(struct pros_prosenjitfs *fs,
                                  uint64_t inode_number, uint8_t allocate)
{
    uint64_t bits_per_block;
    uint64_t bi;
    uint64_t within;
    uint64_t lba;
    uint8_t block[4096];
    uint32_t slot;
    uint8_t mask;

    if (!fs || fs->read_only || inode_number >= fs->sb.inode_count ||
        fs->sb.block_size > sizeof(block))
        return PROSENJITFS_EINVAL;

    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    bi = inode_number / bits_per_block;
    within = inode_number % bits_per_block;
    if (bi >= fs->sb.inode_bitmap_blocks)
        return PROSENJITFS_EOVERFLOW;

    lba = fs->sb.inode_bitmap_start + bi;
    if (pfs_read_block(fs->mount.device, lba, block) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    if (pfs_tx_add(fs, lba, block) != PROSENJITFS_OK)
        return PROSENJITFS_EOVERFLOW;

    slot = 0u;
    while (slot < fs->tx.count && fs->tx.targets[slot] != lba)
        ++slot;
    if (slot == fs->tx.count)
        return PROSENJITFS_EINVAL;

    mask = (uint8_t)(1u << (within & 7u));
    {
        uint8_t *image = fs->tx.images + (uint64_t)slot * fs->sb.block_size;
        uint8_t old = image[within >> 3];
        if (allocate) {
            if (old & mask)
                return PROSENJITFS_EEXIST;
            image[within >> 3] = (uint8_t)(old | mask);
        } else {
            if ((old & mask) == 0u)
                return PROSENJITFS_EINVAL;
            image[within >> 3] = (uint8_t)(old & (uint8_t)~mask);
        }
    }
    return PROSENJITFS_OK;
}

static int pfs_inode_bitmap_update(struct pros_prosenjitfs *fs,
                                   uint64_t inode_number, uint8_t allocate)
{
    if (!fs || fs->read_only || pfs_tx_reset(fs) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    if (pfs_inode_bitmap_stage(fs, inode_number, allocate) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    return pfs_tx_commit(fs);
}

int prosenjitfs_alloc_inode(struct pros_prosenjitfs *fs,
                            uint64_t *inode_number)
{
    uint64_t bits_per_block;
    uint8_t block[4096];

    if (!fs || !inode_number || fs->read_only ||
        fs->sb.block_size > sizeof(block))
        return PROSENJITFS_EINVAL;

    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    for (uint64_t inode = 1u; inode < fs->sb.inode_count; ++inode) {
        uint64_t bi = inode / bits_per_block;
        uint64_t within = inode % bits_per_block;

        if (bi >= fs->sb.inode_bitmap_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_block(fs->mount.device,
                           fs->sb.inode_bitmap_start + bi, block) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        if (!pfs_bitmap_bit(block, within)) {
            *inode_number = inode;
            return pfs_inode_bitmap_update(fs, inode, 1u);
        }
    }
    return PROSENJITFS_ENOSPC;
}

int prosenjitfs_read_inode(const struct pros_prosenjitfs *fs,
                           uint64_t inode_number,
                           struct pros_prosenjitfs_inode *inode)
{
    uint64_t byte_offset;
    uint64_t block_lba;
    uint32_t offset;
    uint8_t block[4096];
    if (!fs || !inode || inode_number >= fs->sb.inode_count ||
        fs->sb.block_size > sizeof(block))
        return PROSENJITFS_EINVAL;
    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    block_lba = fs->sb.inode_table_start + byte_offset / fs->sb.block_size;
    offset = (uint32_t)(byte_offset % fs->sb.block_size);
    if (pfs_read_block(fs->mount.device, block_lba, block) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_copy(inode, block + offset, sizeof(*inode));
    if (inode->type == PROSENJITFS_INODE_FREE)
        return PROSENJITFS_ENOENT;
    if (inode->type != PROSENJITFS_INODE_FILE &&
        inode->type != PROSENJITFS_INODE_DIR &&
        inode->type != PROSENJITFS_INODE_SYMLINK)
        return PROSENJITFS_ECORRUPT;
    if (pfs_crc_with_zero32(block + offset, sizeof(*inode),
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_inode, checksum)) !=
        inode->checksum)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

int prosenjitfs_write_inode(struct pros_prosenjitfs *fs,
                            uint64_t inode_number,
                            const struct pros_prosenjitfs_inode *inode)
{
    uint64_t byte_offset;
    uint64_t block_lba;
    uint32_t offset;
    uint8_t block[4096];
    if (!fs || !inode || fs->read_only || inode_number >= fs->sb.inode_count ||
        fs->sb.block_size > sizeof(block))
        return PROSENJITFS_EINVAL;
    if (inode->type != PROSENJITFS_INODE_FILE &&
        inode->type != PROSENJITFS_INODE_DIR &&
        inode->type != PROSENJITFS_INODE_SYMLINK)
        return PROSENJITFS_EINVAL;
    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    block_lba = fs->sb.inode_table_start + byte_offset / fs->sb.block_size;
    offset = (uint32_t)(byte_offset % fs->sb.block_size);
    if (pfs_read_block(fs->mount.device, block_lba, block) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_copy(block + offset, inode, sizeof(*inode));
    pfs_put32(block + offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(block + offset, sizeof(*inode),
                                 (uint32_t)offsetof(
                                     struct pros_prosenjitfs_inode, checksum)));
    if (pfs_tx_reset(fs) != PROSENJITFS_OK ||
        pfs_tx_add(fs, block_lba, block) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    return pfs_tx_commit(fs);
}

int prosenjitfs_mount(struct pros_prosenjitfs *fs,
                      struct pros_block_device *device,
                      uint8_t read_only)
{
    uint8_t *block;
    struct pros_prosenjitfs_superblock primary;
    struct pros_prosenjitfs_superblock backup;
    int primary_ok, backup_ok;
    if (!fs || !device || device->sector_size < 512u ||
        device->sector_size > 4096u ||
        (device->sector_size & (device->sector_size - 1u)) != 0u)
        return PROSENJITFS_EINVAL;
    pfs_mount_debug_stage = 0;
    pfs_zero(fs, sizeof(*fs));
    block = (uint8_t *)kmalloc(device->sector_size);
    if (!block)
        return PROSENJITFS_EIO;
    fs->scratch = block;
    fs->tx.images = (uint8_t *)kmalloc((uint64_t)(PROSENJITFS_MAX_TX_BLOCKS + 1u) *
                                       device->sector_size);
    if (!fs->tx.images) {
        kfree(block);
        fs->scratch = 0;
        return PROSENJITFS_EIO;
    }
    if (pfs_read_block(device, 0u, block) != PROSENJITFS_OK) {
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_EIO;
    }
    primary_ok = pfs_superblock_valid(block, device->sector_size, &primary) == PROSENJITFS_OK;
    if (pfs_read_block(device, 1u, block) != PROSENJITFS_OK) {
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_EIO;
    }
    backup_ok = pfs_superblock_valid(block, device->sector_size, &backup) == PROSENJITFS_OK;
    if (!primary_ok && !backup_ok) {
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_ECORRUPT;
    }
    fs->sb = primary_ok ? primary : backup;
    fs->degraded_superblock = primary_ok ? 0u : 1u;
    if (fs->sb.total_blocks > device->sector_count) {
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_ECORRUPT;
    }
    fs->read_only = read_only || fs->degraded_superblock;
    pfs_mount_debug_stage = 1;
    if (vfs_mount_init(&fs->mount, PROS_FS_PROSENJIT, device, fs,
                       fs->read_only) != PROS_VFS_OK) {
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_EINVAL;
    }
    if (pfs_journal_recover(fs) != PROSENJITFS_OK) {
        vfs_mount_deactivate(&fs->mount);
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_ECORRUPT;
    }
    pfs_mount_debug_stage = 2;
    if (pfs_validate_metadata(fs) != PROSENJITFS_OK) {
        vfs_mount_deactivate(&fs->mount);
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_ECORRUPT;
    }
    pfs_mount_debug_stage = 3;
    struct pros_vfs_vnode *root_vnode = 0;
    if (pfs_make_vnode(fs, fs->sb.root_inode, 0, &root_vnode) != PROS_VFS_OK ||
        vfs_mount_set_root(&fs->mount, root_vnode) != PROS_VFS_OK) {
        if (root_vnode) {
            vfs_vnode_put(root_vnode);
            vfs_vnode_put(root_vnode);
        }
        vfs_mount_deactivate(&fs->mount);
        kfree(fs->tx.images); kfree(fs->scratch);
        fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_EIO;
    }
    /* vfs_mount_set_root() acquired the mount's ownership reference. */
    vfs_vnode_put(root_vnode);

    pfs_mount_debug_stage = 4;
    if (vfs_mount_activate(&fs->mount) != PROS_VFS_OK) {
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_EINVAL;
    }
    return PROSENJITFS_OK;
}

int prosenjitfs_unmount(struct pros_prosenjitfs *fs)
{
    int rc;
    if (!fs || !fs->mount.active)
        return PROSENJITFS_EINVAL;
    rc = pfs_flush(fs->mount.device);
    vfs_mount_deactivate(&fs->mount);
    if (fs->tx.images) kfree(fs->tx.images);
    if (fs->scratch) kfree(fs->scratch);
    fs->tx.images = 0;
    fs->scratch = 0;
    return rc;
}

struct pfs_test_disk {
    uint8_t data[512u * 512u];
};

static int pfs_test_read(struct pros_block_device *device, uint64_t lba,
                         uint32_t count, void *buffer)
{
    struct pfs_test_disk *disk = (struct pfs_test_disk *)device->private_data;
    if (!disk || !buffer || lba >= 512u || count > 512u - lba)
        return PROS_BLOCK_EINVAL;
    pfs_copy(buffer, disk->data + lba * 512u, count * 512u);
    return PROS_BLOCK_OK;
}

static int pfs_test_write(struct pros_block_device *device, uint64_t lba,
                          uint32_t count, const void *buffer)
{
    struct pfs_test_disk *disk = (struct pfs_test_disk *)device->private_data;
    if (!disk || !buffer || lba >= 512u || count > 512u - lba)
        return PROS_BLOCK_EINVAL;
    pfs_copy(disk->data + lba * 512u, buffer, count * 512u);
    return PROS_BLOCK_OK;
}

static int pfs_test_flush(struct pros_block_device *device)
{
    (void)device;
    return PROS_BLOCK_OK;
}

int prosenjitfs_selftest(void)
{
    static struct pfs_test_disk disk;
    struct pros_block_device device;
    struct pros_prosenjitfs fs;
    struct pros_prosenjitfs_inode inode;
    uint64_t first_block;
    uint64_t inode_number;
    uint8_t saved;
    struct pros_vfs_vnode *parent = 0;
    if (block_device_init(&device, 512u, 512u, pfs_test_read,
                          pfs_test_write, &disk) != PROS_BLOCK_OK)
        return -1;
    if (block_device_set_flush(&device, pfs_test_flush) != PROS_BLOCK_OK)
        return -2;
    pfs_zero(&disk, sizeof(disk));
    if (prosenjitfs_format(&device) != PROSENJITFS_OK)
        return -3;
    {
        int mount_rc = prosenjitfs_mount(&fs, &device, 0u);
        if (mount_rc != PROSENJITFS_OK)
            if (pfs_mount_debug_stage == 2)
                return -300 - pfs_journal_debug_stage;
            return -200 - pfs_mount_debug_stage;
    }
    if (fs.sb.root_inode != 0u ||
        prosenjitfs_read_inode(&fs, 0u, &inode) != PROSENJITFS_OK ||
        inode.type != PROSENJITFS_INODE_DIR ||
        inode.dir_index == 0u ||
        !fs.mount.root)
        return -5;
    {
        struct pros_vfs_vnode *root = 0;
        struct pros_vfs_dir dir;
        struct pros_vfs_dirent entry;
        struct pros_vfs_vnode *created = 0;
        struct pros_vfs_vnode *looked_up = 0;
        struct pros_vfs_vnode *subdir = 0;
        struct pros_vfs_vnode *nested = 0;
        if (vfs_path_lookup(&fs.mount, "/", &root) != PROS_VFS_OK ||
            root != fs.mount.root ||
            root->ops->create(root, "hello", &created) != PROS_VFS_OK ||
            !created ||
            vfs_path_lookup(&fs.mount, "/hello", &looked_up) != PROS_VFS_OK ||
            !looked_up ||
            looked_up->type != PROS_VFS_NODE_FILE ||
            vfs_path_mkdir(&fs.mount, "/sub", &subdir) != PROS_VFS_OK ||
            !subdir ||
            subdir->type != PROS_VFS_NODE_DIRECTORY ||
            subdir->ops->create(subdir, "nested", &nested) != PROS_VFS_OK ||
            !nested ||
            vfs_vnode_put(looked_up) != PROS_VFS_OK ||
            vfs_vnode_put(subdir) != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/sub/nested", &looked_up) != PROS_VFS_OK ||
            looked_up->type != PROS_VFS_NODE_FILE ||
            looked_up->ops->lookup(looked_up, "..", &parent) != PROS_VFS_OK ||
            !parent ||
            parent->type != PROS_VFS_NODE_DIRECTORY ||
            vfs_vnode_put(parent) != PROS_VFS_OK ||
            vfs_vnode_put(looked_up) != PROS_VFS_OK ||
            vfs_vnode_put(nested) != PROS_VFS_OK ||
            vfs_vnode_put(created) != PROS_VFS_OK ||
            vfs_dir_open(&fs.mount, "/", &dir) != PROS_VFS_OK ||
            vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
            entry.name[0] != '.' ||
            vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
            entry.name[0] != '.' ||
            vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
            entry.name[0] != 'h' ||
            vfs_dir_close(&dir) != PROS_VFS_OK)
            return -5;

        {
            static const char unicode_name[] = "বাংলা";
            static const char invalid_utf8[] = "\xC0\xAF";
            struct pros_vfs_vnode *unicode = 0;
            if (root->ops->create(root, unicode_name, &unicode) !=
                    PROS_VFS_OK ||
                !unicode ||
                vfs_vnode_put(unicode) != PROS_VFS_OK ||
                root->ops->create(root, invalid_utf8, &unicode) !=
                    PROS_VFS_EINVAL ||
                unicode != 0)
                return -5;
            if (vfs_path_lookup(&fs.mount, "/বাংলা", &unicode) !=
                    PROS_VFS_OK ||
                !unicode ||
                vfs_vnode_put(unicode) != PROS_VFS_OK)
                return -5;
        }
        if (vfs_vnode_put(root) != PROS_VFS_OK)
            return -5;
    }
    {
        static const char message[] = "ProsenjitFS persistent file I/O";
        static const char suffix[] = "!";
        char readback[sizeof(message) + sizeof(suffix)];
        struct pros_vfs_file file;
        uint32_t count = 0u;

        pfs_zero(&file, sizeof(file));
        if (vfs_file_open(&fs.mount, "/hello",
                          PROS_VFS_O_READ | PROS_VFS_O_WRITE,
                          &file) != PROS_VFS_OK ||
            vfs_file_write(&file, message, (uint32_t)sizeof(message),
                           &count) != PROS_VFS_OK ||
            count != sizeof(message) ||
            vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
            vfs_file_read(&file, readback, (uint32_t)sizeof(readback),
                          &count) != PROS_VFS_OK ||
            count != sizeof(message) ||
            pfs_mem_equal(readback, message, sizeof(message)) != 0 ||
            vfs_file_seek(&file, sizeof(message)) != PROS_VFS_OK ||
            vfs_file_write(&file, suffix, (uint32_t)(sizeof(suffix) - 1u),
                           &count) != PROS_VFS_OK ||
            count != sizeof(suffix) - 1u ||
            vfs_file_truncate(&file, 5u) != PROS_VFS_OK ||
            vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
            vfs_file_read(&file, readback, 5u, &count) != PROS_VFS_OK ||
            count != 5u ||
            pfs_mem_equal(readback, "Prose", 5u) != 0 ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_file_open(&fs.mount, "/hello", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK ||
            vfs_file_read(&file, readback, 5u, &count) != PROS_VFS_OK ||
            count != 5u ||
            pfs_mem_equal(readback, "Prose", 5u) != 0 ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_path_rename(&fs.mount, "/sub/nested", "/sub/renamed") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/sub/nested", &parent) != PROS_VFS_ENOENT ||
            vfs_path_lookup(&fs.mount, "/sub/renamed", &parent) != PROS_VFS_OK ||
            vfs_vnode_put(parent) != PROS_VFS_OK ||
            vfs_path_unlink(&fs.mount, "/hello") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/hello", &parent) != PROS_VFS_ENOENT)
            return -6;
    }
    {
        struct pros_vfs_file file;
        struct pros_vfs_vnode *moved = 0;

        pfs_zero(&file, sizeof(file));
        if (vfs_file_open(&fs.mount, "/old", PROS_VFS_O_WRITE |
                          PROS_VFS_O_CREATE, &file) != PROS_VFS_OK ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_path_rename(&fs.mount, "/old", "/new") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/old", &moved) != PROS_VFS_ENOENT ||
            vfs_path_lookup(&fs.mount, "/new", &moved) != PROS_VFS_OK ||
            moved->type != PROS_VFS_NODE_FILE ||
            vfs_vnode_put(moved) != PROS_VFS_OK)
            return -7;

        if (vfs_path_mkdir(&fs.mount, "/dst", &moved) != PROS_VFS_OK ||
            !moved ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_rename(&fs.mount, "/sub", "/dst/sub") !=
                PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/dst/sub/renamed", &moved) !=
                PROS_VFS_OK ||
            moved->type != PROS_VFS_NODE_FILE ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_rename(&fs.mount, "/dst/sub", "/dst/sub/moved") !=
                PROS_VFS_ELOOP ||
            vfs_path_unlink(&fs.mount, "/dst/sub") != PROS_VFS_ENOTEMPTY ||
            vfs_path_unlink(&fs.mount, "/dst/sub/renamed") != PROS_VFS_OK ||
            vfs_path_unlink(&fs.mount, "/dst/sub") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/dst/sub", &moved) != PROS_VFS_ENOENT ||
            vfs_path_lookup(&fs.mount, "/dst", &moved) != PROS_VFS_OK ||
            moved->type != PROS_VFS_NODE_DIRECTORY ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_mkdir(&fs.mount, "/empty", &moved) != PROS_VFS_OK ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_unlink(&fs.mount, "/empty") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/empty", &moved) != PROS_VFS_ENOENT ||
            vfs_path_mkdir(&fs.mount, "/empty", &moved) != PROS_VFS_OK ||
            !moved ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_unlink(&fs.mount, "/empty") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/empty", &moved) != PROS_VFS_ENOENT ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_path_lookup(&fs.mount, "/empty", &moved) != PROS_VFS_ENOENT ||
            vfs_path_mkdir(&fs.mount, "/empty", &moved) != PROS_VFS_OK ||
            !moved ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_unlink(&fs.mount, "/empty") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/empty", &moved) != PROS_VFS_ENOENT ||
            vfs_file_open(&fs.mount, "/collision", PROS_VFS_O_WRITE |
                          PROS_VFS_O_CREATE, (struct pros_vfs_file *)&file) !=
                PROS_VFS_OK ||
            vfs_file_close((struct pros_vfs_file *)&file) != PROS_VFS_OK ||
            vfs_path_rename(&fs.mount, "/new", "/collision") != PROS_VFS_EEXIST ||
            vfs_path_lookup(&fs.mount, "/new", &moved) != PROS_VFS_OK ||
            moved->type != PROS_VFS_NODE_FILE ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/collision", &moved) != PROS_VFS_OK ||
            moved->type != PROS_VFS_NODE_FILE ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_rename(&fs.mount, "/new", "/dst/new") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/new", &moved) != PROS_VFS_ENOENT ||
            vfs_path_lookup(&fs.mount, "/dst/new", &moved) != PROS_VFS_OK ||
            moved->type != PROS_VFS_NODE_FILE ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_path_lookup(&fs.mount, "/dst/new", &moved) != PROS_VFS_OK ||
            moved->type != PROS_VFS_NODE_FILE ||
            vfs_vnode_put(moved) != PROS_VFS_OK ||
            vfs_path_rename(&fs.mount, "/dst/new", "/new") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/new", &moved) != PROS_VFS_OK ||
            moved->type != PROS_VFS_NODE_FILE ||
            vfs_vnode_put(moved) != PROS_VFS_OK)
            return -8;
    }
    {
        static uint8_t payload[8704];
        static uint8_t verify[8704];
        struct pros_vfs_file file;
        struct pros_vfs_vnode *removed = 0;
        uint32_t count = 0u;
        for (uint32_t i = 0u; i < sizeof(payload); ++i)
            payload[i] = (uint8_t)((i * 37u) ^ (i >> 3));
        pfs_zero(&file, sizeof(file));
        if (vfs_file_open(&fs.mount, "/new", PROS_VFS_O_READ |
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
            vfs_file_write(&file, payload, (uint32_t)sizeof(payload),
                           &count) != PROS_VFS_OK ||
            count != sizeof(payload) ||
            vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
            vfs_file_read(&file, verify, (uint32_t)sizeof(verify),
                          &count) != PROS_VFS_OK ||
            count != sizeof(verify) ||
            pfs_mem_equal(verify, payload, sizeof(payload)) != 0 ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return -16;

        pfs_zero(&file, sizeof(file));
        if (vfs_file_open(&fs.mount, "/new", PROS_VFS_O_READ |
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 8192u) != PROS_VFS_OK ||
            vfs_file_read(&file, verify, 512u, &count) != PROS_VFS_OK ||
            count != 512u ||
            pfs_mem_equal(verify, payload + 8192u, 512u) != 0 ||
            vfs_file_truncate(&file, 700u) != PROS_VFS_OK ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return -17;

        pfs_zero(&file, sizeof(file));
        if (vfs_file_open(&fs.mount, "/new", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK ||
            vfs_file_read(&file, verify, 8704u, &count) != PROS_VFS_OK ||
            count != 700u ||
            pfs_mem_equal(verify, payload, 700u) != 0 ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_path_unlink(&fs.mount, "/new") != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/new", &removed) != PROS_VFS_ENOENT)
            return -18;
    }
    {
        uint8_t *header = fs.scratch;
        uint8_t *image = fs.tx.images;
        uint8_t *commit = fs.tx.images +
                          (uint64_t)PROSENJITFS_MAX_TX_BLOCKS * fs.sb.block_size;
        struct pros_prosenjitfs_journal_header h;
        struct pros_prosenjitfs_journal_commit j;
        uint64_t replay_lba = fs.sb.data_start + 20u;
        uint8_t original[4096];
        uint32_t checksum;

        if (replay_lba >= fs.sb.total_blocks || fs.sb.block_size > sizeof(original) ||
            pfs_read_block(fs.mount.device, replay_lba, original) != PROSENJITFS_OK)
            return -12;
        pfs_zero(header, fs.sb.block_size);
        pfs_zero(image, fs.sb.block_size);
        pfs_zero(commit, fs.sb.block_size);
        pfs_copy(image, original, fs.sb.block_size);
        image[0] = 0xA5u;
        pfs_zero(&h, sizeof(h));
        h.magic = PROSENJITFS_JOURNAL_MAGIC;
        h.record_count = 1u;
        h.transaction_id = fs.sb.transaction_sequence++;
        h.targets[0] = replay_lba;
        pfs_copy(header, &h, sizeof(h));
        h.checksum = pfs_crc_with_zero32(
            header, fs.sb.block_size,
            (uint32_t)offsetof(struct pros_prosenjitfs_journal_header, checksum));
        pfs_copy(header, &h, sizeof(h));
        pfs_zero(&j, sizeof(j));
        j.magic = PROSENJITFS_JOURNAL_COMMIT;
        j.record_count = 1u;
        j.transaction_id = h.transaction_id;
        pfs_copy(commit, &j, sizeof(j));
        checksum = pfs_journal_checksum(header, image, fs.sb.block_size, 1u, commit);
        j.checksum = checksum;
        pfs_copy(commit, &j, sizeof(j));
        if (pfs_write_block(fs.mount.device, fs.sb.journal_start, header) != PROSENJITFS_OK ||
            pfs_write_block(fs.mount.device, fs.sb.journal_start + 1u, image) != PROSENJITFS_OK ||
            pfs_write_block(fs.mount.device, fs.sb.journal_start + 2u, commit) != PROSENJITFS_OK ||
            pfs_flush(fs.mount.device) != PROSENJITFS_OK)
            return -13;
        pfs_zero(image, fs.sb.block_size);
        if (pfs_write_block(fs.mount.device, replay_lba, image) != PROSENJITFS_OK ||
            pfs_flush(fs.mount.device) != PROSENJITFS_OK)
            return -14;
        vfs_mount_deactivate(&fs.mount);
        if (fs.tx.images) kfree(fs.tx.images);
        if (fs.scratch) kfree(fs.scratch);
        fs.tx.images = 0;
        fs.scratch = 0;
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            pfs_read_block(fs.mount.device, replay_lba, original) != PROSENJITFS_OK ||
            original[0] != 0xA5u)
            return -15;
    }
    {
        uint64_t journal_target = fs.sb.data_start;
        uint8_t *header = fs.scratch;
        uint8_t *image = fs.tx.images;
        uint8_t *commit = fs.tx.images +
                          (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                          fs.sb.block_size;
        struct pros_prosenjitfs_journal_header h;
        struct pros_prosenjitfs_journal_commit c;

        pfs_zero(header, fs.sb.block_size);
        pfs_zero(image, fs.sb.block_size);
        pfs_zero(commit, fs.sb.block_size);
        image[0] = 0xA5u;
        image[1] = 0x5Au;
        pfs_zero(&h, sizeof(h));
        h.magic = PROSENJITFS_JOURNAL_MAGIC;
        h.record_count = 1u;
        h.transaction_id = fs.sb.transaction_sequence + 1u;
        h.targets[0] = journal_target;
        pfs_copy(header, &h, sizeof(h));
        h.checksum = pfs_crc_with_zero32(
            header, fs.sb.block_size,
            (uint32_t)offsetof(struct pros_prosenjitfs_journal_header,
                               checksum));
        pfs_copy(header, &h, sizeof(h));

        pfs_zero(&c, sizeof(c));
        c.magic = PROSENJITFS_JOURNAL_COMMIT;
        c.record_count = 1u;
        c.transaction_id = h.transaction_id;
        pfs_copy(commit, &c, sizeof(c));
        c.checksum = pfs_journal_checksum(header, image,
                                          fs.sb.block_size, 1u, commit);
        pfs_copy(commit, &c, sizeof(c));

        if (pfs_write_block(fs.mount.device, fs.sb.journal_start,
                            header) != PROSENJITFS_OK ||
            pfs_write_block(fs.mount.device, fs.sb.journal_start + 1u,
                            image) != PROSENJITFS_OK ||
            pfs_write_block(fs.mount.device, fs.sb.journal_start + 2u,
                            commit) != PROSENJITFS_OK ||
            pfs_flush(fs.mount.device) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            disk.data[journal_target * 512u] != 0xA5u ||
            disk.data[journal_target * 512u + 1u] != 0x5Au)
            return -9;
    }
    {
        uint64_t journal_target = fs.sb.data_start + 1u;
        uint8_t *header = fs.scratch;
        uint8_t *image = fs.tx.images;
        uint8_t *commit = fs.tx.images +
                          (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                          fs.sb.block_size;
        struct pros_prosenjitfs_journal_header h;
        struct pros_prosenjitfs_journal_commit c;

        pfs_zero(header, fs.sb.block_size);
        pfs_zero(image, fs.sb.block_size);
        pfs_zero(commit, fs.sb.block_size);
        image[0] = 0xC3u;
        pfs_zero(&h, sizeof(h));
        h.magic = PROSENJITFS_JOURNAL_MAGIC;
        h.record_count = 1u;
        h.transaction_id = fs.sb.transaction_sequence + 2u;
        h.targets[0] = journal_target;
        pfs_copy(header, &h, sizeof(h));
        h.checksum = pfs_crc_with_zero32(
            header, fs.sb.block_size,
            (uint32_t)offsetof(struct pros_prosenjitfs_journal_header,
                               checksum));
        pfs_copy(header, &h, sizeof(h));
        pfs_zero(&c, sizeof(c));
        c.magic = PROSENJITFS_JOURNAL_COMMIT;
        c.record_count = 1u;
        c.transaction_id = h.transaction_id;
        pfs_copy(commit, &c, sizeof(c));
        c.checksum = pfs_journal_checksum(header, image,
                                          fs.sb.block_size, 1u, commit);
        c.checksum ^= 1u;
        pfs_copy(commit, &c, sizeof(c));

        if (pfs_write_block(fs.mount.device, fs.sb.journal_start,
                            header) != PROSENJITFS_OK ||
            pfs_write_block(fs.mount.device, fs.sb.journal_start + 1u,
                            image) != PROSENJITFS_OK ||
            pfs_write_block(fs.mount.device, fs.sb.journal_start + 2u,
                            commit) != PROSENJITFS_OK ||
            pfs_flush(fs.mount.device) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_ECORRUPT)
            return -10;
        disk.data[fs.sb.journal_start * 512u] = 0u;
        if (pfs_test_flush(&device) != PROS_BLOCK_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return -10;
    }
    if (prosenjitfs_alloc_blocks(&fs, 4u, &first_block) != PROSENJITFS_OK)
        return -6;
    if (first_block < fs.sb.data_start ||
        first_block + 4u > fs.sb.total_blocks)
        return -6;
    if (prosenjitfs_alloc_inode(&fs, &inode_number) != PROSENJITFS_OK ||
        inode_number == fs.sb.root_inode ||
        inode_number >= fs.sb.inode_count)
        return -7;
    if (prosenjitfs_read_inode(&fs, inode_number, &inode) != PROSENJITFS_ENOENT)
        return -8;
    if (prosenjitfs_free_blocks(&fs, fs.sb.total_blocks, 1u) != PROSENJITFS_EINVAL)
        return -9;
    if (prosenjitfs_free_blocks(&fs, first_block, 4u) != PROSENJITFS_OK)
        return -10;
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -11;

    saved = disk.data[0];
    disk.data[0] ^= 0x01u;
    disk.data[512u] ^= 0x01u;
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_ECORRUPT)
        return -9;
    disk.data[0] = saved;
    disk.data[512u] ^= 0x01u;

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return -10;
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -11;
    return PROSENJITFS_OK;
}