#include "prosenjitfs.h"
#include "heap.h"
#include <stddef.h>

#define PFS_MIN_BLOCKS 256u
#define PFS_MIN_JOURNAL 16u
#define PFS_MAX_JOURNAL 64u
#define PFS_MAX_INODES 1024u
#define PFS_MIN_INODES 64u
#define PFS_UUID_SEED 0x50524F53454E4A49ULL

/* Forward declarations for VFS callbacks and helpers used before their
 * definitions. Keep the provider in one translation unit without relying on
 * legacy implicit function declarations. */
static int pfs_vnode_create(struct pros_vfs_vnode *, const char *,
                            struct pros_vfs_vnode **);
static int pfs_vnode_mkdir(struct pros_vfs_vnode *, const char *,
                           struct pros_vfs_vnode **);
static int pfs_vnode_rmdir(struct pros_vfs_vnode *, const char *);
static uint32_t pfs_name_hash(const uint8_t *, uint32_t);
static int pfs_utf8_name_valid(const char *, uint32_t *);
static int pfs_read_dir_index(const struct pros_prosenjitfs *,
                              const struct pros_prosenjitfs_inode *,
                              struct pros_prosenjitfs_dir_index *);
static int pfs_find_dir_entry(const struct pros_prosenjitfs *,
                              const struct pros_prosenjitfs_inode *,
                              const char *,
                              struct pros_prosenjitfs_dir_entry *);
static int pfs_tx_reset(struct pros_prosenjitfs *);
static int pfs_tx_add(struct pros_prosenjitfs *, uint64_t, const void *);
static int pfs_tx_commit(struct pros_prosenjitfs *);
static int pfs_bitmap_bit(const uint8_t *, uint64_t);
static int pfs_allocator_stage(struct pros_prosenjitfs *, uint64_t, uint32_t,
                               uint8_t);
static int pfs_inode_bitmap_stage(struct pros_prosenjitfs *, uint64_t, uint8_t);

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


static void pfs_copy(void *dst, const void *src, uint32_t size)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < size; ++i)
        d[i] = s[i];
}

/*
 * Isolated ProsenjitFS write-path diagnostic.
 * The self-test enables this only for the first 512-byte file-data block.
 * It records the transaction image byte, the immediate post-commit physical
 * read, the extent-mapped physical LBA, and the later VFS read byte.
 */
struct pfs_write_diag {
    uint8_t enabled;
    uint8_t found;
    uint8_t read_enabled;
    uint8_t read_found;
    uint8_t tx_byte;
    uint8_t post_commit_byte;
    uint8_t direct_byte;
    uint8_t vfs_byte;
    uint64_t physical_lba;
    uint64_t read_physical_lba;
};

static struct pfs_write_diag pfs_diag;

void prosenjitfs_write_diag_reset(void)
{
    pfs_zero(&pfs_diag, sizeof(pfs_diag));
    pfs_diag.enabled = 1u;
}

void prosenjitfs_write_diag_begin_read(void)
{
    pfs_diag.read_enabled = 1u;
    pfs_diag.read_found = 0u;
    pfs_diag.read_physical_lba = 0u;
}

int prosenjitfs_write_diag_get(uint64_t *physical_lba, uint8_t *tx_byte,
                               uint8_t *post_commit_byte,
                               uint8_t *direct_byte, uint8_t *vfs_byte)
{
    if (!physical_lba || !tx_byte || !post_commit_byte ||
        !direct_byte || !vfs_byte)
        return PROSENJITFS_EINVAL;
    if (!pfs_diag.found)
        return PROSENJITFS_ENOENT;
    *physical_lba = pfs_diag.physical_lba;
    *tx_byte = pfs_diag.tx_byte;
    *post_commit_byte = pfs_diag.post_commit_byte;
    *direct_byte = pfs_diag.direct_byte;
    *vfs_byte = pfs_diag.vfs_byte;
    return PROSENJITFS_OK;
}

int prosenjitfs_write_diag_get_read_lba(uint64_t *physical_lba)
{
    if (!physical_lba)
        return PROSENJITFS_EINVAL;
    if (!pfs_diag.read_found)
        return PROSENJITFS_ENOENT;
    *physical_lba = pfs_diag.read_physical_lba;
    return PROSENJITFS_OK;
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
    root.dir_index = cursor;
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
static int pfs_vnode_unlink(struct pros_vfs_vnode *, const char *);
static int pfs_vnode_rename(struct pros_vfs_vnode *, const char *,
                                struct pros_vfs_vnode *, const char *);
static int pfs_vnode_readdir(struct pros_vfs_vnode *directory, uint64_t *cookie,
                             struct pros_vfs_dirent *entry);
static void pfs_vnode_release(struct pros_vfs_vnode *vnode);

static const struct pros_vfs_vnode_ops pfs_vnode_ops = {
    .lookup = pfs_vnode_lookup,
    .readdir = pfs_vnode_readdir,
    .create = pfs_vnode_create,
    .mkdir = pfs_vnode_mkdir,
    .rmdir = pfs_vnode_rmdir,
    .unlink = pfs_vnode_unlink,
    .rename = pfs_vnode_rename,
    .release = pfs_vnode_release
};

static int pfs_file_read(struct pros_vfs_file *, uint32_t, uint32_t, void *);
static int pfs_file_write(struct pros_vfs_file *, uint32_t, uint32_t, const void *);
static int pfs_file_truncate(struct pros_vfs_file *, uint64_t);
static int pfs_file_close(struct pros_vfs_file *);

static int pfs_read_extent(const struct pros_prosenjitfs *fs, uint64_t lba,
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
        extent->physical_block > fs->sb.total_blocks ||
        extent->block_count > fs->sb.total_blocks - extent->physical_block ||
        (extent->next != 0u &&
         (extent->next < fs->sb.data_start ||
          extent->next >= fs->sb.total_blocks)))
        return PROSENJITFS_ECORRUPT;
    if (pfs_crc_with_zero32(block, sizeof(*extent),
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_extent, checksum)) !=
        extent->checksum)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static int pfs_find_extent(const struct pros_prosenjitfs *fs,
                           const struct pros_prosenjitfs_inode *inode,
                           uint64_t logical_block,
                           struct pros_prosenjitfs_extent *found,
                           uint64_t *found_lba,
                           uint64_t *tail_lba)
{
    uint64_t current;
    uint64_t steps = 0u;
    struct pros_prosenjitfs_extent extent;

    if (!fs || !inode || !found_lba || !tail_lba)
        return PROSENJITFS_EINVAL;
    current = inode->extent_head;
    *found_lba = 0u;
    *tail_lba = 0u;
    while (current != 0u) {
        if (++steps > fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_extent(fs, current, &extent) != PROSENJITFS_OK)
            return PROSENJITFS_ECORRUPT;
        *tail_lba = current;
        if (logical_block >= extent.logical_block &&
            logical_block - extent.logical_block < extent.block_count) {
            if (found)
                pfs_copy(found, &extent, sizeof(*found));
            *found_lba = current;
            return PROSENJITFS_OK;
        }
        current = extent.next;
    }
    return PROSENJITFS_ENOENT;
}

static int pfs_find_free_data_block_except(const struct pros_prosenjitfs *fs,
                                           uint64_t avoid,
                                           uint64_t *block_number)
{
    uint64_t b;
    if (!fs || !block_number || fs->sb.block_size > 4096u)
        return PROSENJITFS_EINVAL;
    for (b = fs->sb.data_start; b < fs->sb.total_blocks; ++b) {
        if (b == avoid)
            continue;
        {
            uint64_t bits_per_block = (uint64_t)fs->sb.block_size * 8u;
            uint64_t bitmap_lba = fs->sb.block_bitmap_start +
                                  b / bits_per_block;
            uint8_t bitmap[4096];
            if (pfs_read_block(fs->mount.device, bitmap_lba, bitmap) !=
                PROSENJITFS_OK)
                return PROSENJITFS_EIO;
            if (!pfs_bitmap_bit(bitmap, b % bits_per_block)) {
                *block_number = b;
                return PROSENJITFS_OK;
            }
        }
    }
    return PROSENJITFS_ENOSPC;
}

static int pfs_inode_block_location(const struct pros_prosenjitfs *fs,
                                    uint64_t inode_number,
                                    uint64_t *block_lba,
                                    uint32_t *offset)
{
    uint64_t byte_offset;
    if (!fs || !block_lba || !offset ||
        inode_number >= fs->sb.inode_count ||
        fs->sb.block_size > 4096u)
        return PROSENJITFS_EINVAL;
    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    *block_lba = fs->sb.inode_table_start +
                 byte_offset / fs->sb.block_size;
    *offset = (uint32_t)(byte_offset % fs->sb.block_size);
    return PROSENJITFS_OK;
}

static int pfs_file_read(struct pros_vfs_file *file, uint32_t offset,
                         uint32_t length, void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint32_t done = 0u;

    if (!file || !file->vnode || !buffer)
        return PROS_VFS_EINVAL;
    if (offset > file->vnode->size)
        return PROS_VFS_EOVERFLOW;
    if (length == 0u || offset == file->vnode->size)
        return 0;
    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs)
        return PROS_VFS_EIO;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &inode) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;

    while (done < length && offset + done < inode.size) {
        uint64_t position = (uint64_t)offset + done;
        uint64_t logical_block = position / data->fs->sb.block_size;
        uint32_t in_block = (uint32_t)(position % data->fs->sb.block_size);
        uint32_t want = length - done;
        uint64_t available = inode.size - position;
        uint8_t block[4096];
        struct pros_prosenjitfs_extent extent;
        uint64_t extent_lba = 0u, tail_lba = 0u;
        int rc;

        if (want > data->fs->sb.block_size - in_block)
            want = data->fs->sb.block_size - in_block;
        if ((uint64_t)want > available)
            want = (uint32_t)available;

        rc = pfs_find_extent(data->fs, &inode, logical_block, &extent,
                             &extent_lba, &tail_lba);
        if (rc == PROSENJITFS_ENOENT) {
            for (uint32_t i = 0u; i < want; ++i)
                ((uint8_t *)buffer)[done + i] = 0u;
            done += want;
            continue;
        }
        if (rc != PROSENJITFS_OK || extent_lba == 0u)
            return done ? (int)done : PROS_VFS_EIO;

        {
            uint64_t delta = logical_block - extent.logical_block;
            uint64_t physical = extent.physical_block + delta;
            if (physical >= data->fs->sb.total_blocks ||
                pfs_read_block(data->fs->mount.device, physical, block) !=
                    PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            if (pfs_diag.read_enabled && !pfs_diag.read_found) {
                pfs_diag.read_physical_lba = physical;
                pfs_diag.read_found = 1u;
                pfs_diag.read_enabled = 0u;
            }
        }
        for (uint32_t i = 0u; i < want; ++i)
            ((uint8_t *)buffer)[done + i] = block[in_block + i];
        done += want;
    }
    return (int)done;
}

static int pfs_file_write(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, const void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;

    if (!file || !file->vnode || !buffer)
        return PROS_VFS_EINVAL;
    if (file->mount->read_only || !file->mount->active)
        return PROS_VFS_EPERM;
    if (length == 0u)
        return 0;
    if ((uint64_t)offset + length > UINT64_MAX)
        return PROS_VFS_EOVERFLOW;

    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs)
        return PROS_VFS_EIO;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &inode) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;

    uint32_t done = 0u;
    while (done < length) {
        uint64_t position = (uint64_t)offset + done;
        uint64_t logical_block = position / data->fs->sb.block_size;
        uint32_t in_block = (uint32_t)(position % data->fs->sb.block_size);
        uint32_t want = length - done;
        uint8_t data_image[4096];
        uint8_t extent_image[4096];
        uint8_t tail_image[4096];
        uint8_t inode_image[4096];
        struct pros_prosenjitfs_extent extent;
        uint64_t extent_lba = 0u, tail_lba = 0u;
        uint64_t physical = 0u, new_extent_lba = 0u;
        uint64_t inode_lba;
        uint32_t inode_offset;
        int rc;

        if (want > data->fs->sb.block_size - in_block)
            want = data->fs->sb.block_size - in_block;

        rc = pfs_find_extent(data->fs, &inode, logical_block, &extent,
                             &extent_lba, &tail_lba);
        if (rc == PROSENJITFS_ENOENT) {
            rc = pfs_find_free_data_block_except(data->fs, 0u, &physical);
            if (rc != PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            rc = pfs_find_free_data_block_except(data->fs, physical,
                                                 &new_extent_lba);
            if (rc != PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            pfs_zero(data_image, data->fs->sb.block_size);
            pfs_zero(extent_image, data->fs->sb.block_size);
            pfs_zero(tail_image, data->fs->sb.block_size);
            pfs_zero(inode_image, data->fs->sb.block_size);

            if (in_block != 0u) {
                /*
                 * A new block is initially a hole. Preserve the zero-filled
                 * prefix and only replace the bytes being written.
                 */
            }
            for (uint32_t i = 0u; i < want; ++i)
                data_image[in_block + i] =
                    ((const uint8_t *)buffer)[done + i];

            struct pros_prosenjitfs_extent new_extent;
            pfs_zero(&new_extent, sizeof(new_extent));
            new_extent.magic = PROSENJITFS_EXTENT_MAGIC;
            new_extent.logical_block = logical_block;
            new_extent.physical_block = physical;
            new_extent.block_count = 1u;
            new_extent.next = 0u;
            pfs_copy(extent_image, &new_extent, sizeof(new_extent));
            pfs_put32(extent_image +
                      offsetof(struct pros_prosenjitfs_extent, checksum),
                      pfs_crc_with_zero32(extent_image, sizeof(new_extent),
                                          (uint32_t)offsetof(
                                              struct pros_prosenjitfs_extent,
                                              checksum)));

            if (pfs_inode_block_location(data->fs, data->inode_number,
                                         &inode_lba, &inode_offset) !=
                PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            if (pfs_read_block(data->fs->mount.device, inode_lba, inode_image) !=
                PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;

            if (tail_lba != 0u) {
                if (pfs_read_block(data->fs->mount.device, tail_lba, tail_image) !=
                    PROSENJITFS_OK)
                    return done ? (int)done : PROS_VFS_EIO;
                struct pros_prosenjitfs_extent tail;
                pfs_copy(&tail, tail_image, sizeof(tail));
                tail.next = new_extent_lba;
                pfs_put32(tail_image +
                          offsetof(struct pros_prosenjitfs_extent, checksum),
                          pfs_crc_with_zero32(tail_image, sizeof(tail),
                                              (uint32_t)offsetof(
                                                  struct pros_prosenjitfs_extent,
                                                  checksum)));
            }

            pfs_copy(&inode, inode_image + inode_offset, sizeof(inode));
            if (inode.extent_head == 0u)
                inode.extent_head = new_extent_lba;
            inode.size = ((uint64_t)offset + done + want > inode.size)
                       ? (uint64_t)offset + done + want : inode.size;
            inode.allocated_bytes += data->fs->sb.block_size;
            pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
            pfs_put32(inode_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(inode_image + inode_offset,
                                          sizeof(inode),
                                          (uint32_t)offsetof(
                                              struct pros_prosenjitfs_inode,
                                              checksum)));

            if (pfs_tx_reset(data->fs) != PROSENJITFS_OK ||
                pfs_allocator_stage(data->fs, physical, 1u, 1u) !=
                    PROSENJITFS_OK ||
                pfs_allocator_stage(data->fs, new_extent_lba, 1u, 1u) !=
                    PROSENJITFS_OK ||
                pfs_tx_add(data->fs, physical, data_image) != PROSENJITFS_OK ||
                pfs_tx_add(data->fs, new_extent_lba, extent_image) !=
                    PROSENJITFS_OK ||
                (tail_lba != 0u &&
                 pfs_tx_add(data->fs, tail_lba, tail_image) != PROSENJITFS_OK) ||
                pfs_tx_add(data->fs, inode_lba, inode_image) != PROSENJITFS_OK ||
                pfs_tx_commit(data->fs) != PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
        } else if (rc == PROSENJITFS_OK) {
            uint64_t delta = logical_block - extent.logical_block;
            uint8_t inode_changed = 0u;
            physical = extent.physical_block + delta;
            if (physical >= data->fs->sb.total_blocks)
                return done ? (int)done : PROS_VFS_EIO;
            if (pfs_read_block(data->fs->mount.device, physical, data_image) !=
                PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            for (uint32_t i = 0u; i < want; ++i)
                data_image[in_block + i] =
                    ((const uint8_t *)buffer)[done + i];

            if ((uint64_t)offset + done + want > inode.size) {
                inode.size = (uint64_t)offset + done + want;
                inode_changed = 1u;
                if (pfs_inode_block_location(data->fs, data->inode_number,
                                             &inode_lba, &inode_offset) !=
                    PROSENJITFS_OK ||
                    pfs_read_block(data->fs->mount.device, inode_lba,
                                   inode_image) != PROSENJITFS_OK)
                    return done ? (int)done : PROS_VFS_EIO;
                pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
                pfs_put32(inode_image + inode_offset +
                          offsetof(struct pros_prosenjitfs_inode, checksum),
                          pfs_crc_with_zero32(inode_image + inode_offset,
                                              sizeof(inode),
                                              (uint32_t)offsetof(
                                                  struct pros_prosenjitfs_inode,
                                                  checksum)));
            }

            if (pfs_tx_reset(data->fs) != PROSENJITFS_OK ||
                pfs_tx_add(data->fs, physical, data_image) != PROSENJITFS_OK ||
                (inode_changed &&
                 pfs_tx_add(data->fs, inode_lba, inode_image) != PROSENJITFS_OK) ||
                pfs_tx_commit(data->fs) != PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
        } else {
            return done ? (int)done : PROS_VFS_EIO;
        }

        done += want;
        file->vnode->size = inode.size;
    }
    return (int)done;
}

static int pfs_file_truncate(struct pros_vfs_file *file, uint64_t size)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint64_t old_size, keep_blocks;
    uint64_t current, previous = 0u;
    uint64_t steps = 0u;

    if (!file || !file->vnode || !file->mount ||
        size > UINT32_MAX)
        return PROS_VFS_EINVAL;
    if (file->mount->read_only || !file->mount->active)
        return PROS_VFS_EPERM;
    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs)
        return PROS_VFS_EIO;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &inode) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;

    old_size = inode.size;
    if (size == old_size)
        return PROS_VFS_OK;

    /*
     * Growing a file is deliberately implemented as zero-filled writes.
     * This preserves the existing extent allocator and guarantees that
     * bytes in the newly visible range read back as zero after remount.
     */
    if (size > old_size) {
        static const uint8_t zeros[512] = { 0 };
        uint64_t left = size - old_size;
        uint64_t position = old_size;
        while (left != 0u) {
            uint32_t chunk = left > sizeof(zeros) ? sizeof(zeros) : (uint32_t)left;
            int rc = pfs_file_write(file, (uint32_t)position, chunk, zeros);
            if (rc != (int)chunk)
                return rc < 0 ? rc : PROS_VFS_EIO;
            position += chunk;
            left -= chunk;
        }
        return PROS_VFS_OK;
    }

    keep_blocks = (size + data->fs->sb.block_size - 1u) /
                  data->fs->sb.block_size;

    current = inode.extent_head;
    while (current != 0u) {
        struct pros_prosenjitfs_extent extent;
        uint64_t next;
        if (++steps > data->fs->sb.total_blocks)
            return PROS_VFS_EIO;
        if (pfs_read_extent(data->fs, current, &extent) != PROSENJITFS_OK)
            return PROS_VFS_EIO;
        next = extent.next;

        if (extent.logical_block >= keep_blocks) {
            uint8_t inode_image[4096];
            uint8_t prev_image[4096];
            uint64_t inode_lba = 0u, prev_lba = 0u;
            uint32_t inode_offset = 0u;
            struct pros_prosenjitfs_inode new_inode = inode;
            int rc;

            if (pfs_inode_block_location(data->fs, data->inode_number,
                                         &inode_lba, &inode_offset) !=
                PROSENJITFS_OK)
                return PROS_VFS_EIO;

            if (pfs_tx_reset(data->fs) != PROSENJITFS_OK)
                return PROS_VFS_EIO;

            if (previous != 0u) {
                struct pros_prosenjitfs_extent prev_extent;
                if (pfs_read_block(data->fs->mount.device, previous,
                                   prev_image) != PROSENJITFS_OK)
                    return PROS_VFS_EIO;
                pfs_copy(&prev_extent, prev_image, sizeof(prev_extent));
                if (prev_extent.magic != PROSENJITFS_EXTENT_MAGIC)
                    return PROS_VFS_EIO;
                prev_extent.next = next;
                pfs_copy(prev_image, &prev_extent, sizeof(prev_extent));
                pfs_put32(prev_image +
                          offsetof(struct pros_prosenjitfs_extent, checksum),
                          pfs_crc_with_zero32(prev_image, sizeof(prev_extent),
                                              (uint32_t)offsetof(
                                                  struct pros_prosenjitfs_extent,
                                                  checksum)));
                prev_lba = previous;
            } else {
                new_inode.extent_head = next;
            }

            if (extent.block_count > 0u) {
                rc = pfs_allocator_stage(data->fs, extent.physical_block,
                                         extent.block_count, 0u);
                if (rc != PROSENJITFS_OK)
                    return PROS_VFS_EIO;
            }
            rc = pfs_allocator_stage(data->fs, current, 1u, 0u);
            if (rc != PROSENJITFS_OK)
                return PROS_VFS_EIO;

            if (previous != 0u) {
                if (pfs_tx_add(data->fs, prev_lba, prev_image) != PROSENJITFS_OK)
                    return PROS_VFS_EIO;
            } else {
                if (pfs_read_block(data->fs->mount.device, inode_lba,
                                   inode_image) != PROSENJITFS_OK)
                    return PROS_VFS_EIO;
                pfs_copy(inode_image + inode_offset, &new_inode, sizeof(new_inode));
                pfs_put32(inode_image + inode_offset +
                          offsetof(struct pros_prosenjitfs_inode, checksum),
                          pfs_crc_with_zero32(inode_image + inode_offset,
                                              sizeof(new_inode),
                                              (uint32_t)offsetof(
                                                  struct pros_prosenjitfs_inode,
                                                  checksum)));
                if (pfs_tx_add(data->fs, inode_lba, inode_image) != PROSENJITFS_OK)
                    return PROS_VFS_EIO;
            }

            if (pfs_tx_commit(data->fs) != PROSENJITFS_OK)
                return PROS_VFS_EIO;

            inode = new_inode;
            current = next;
            continue;
        }

        if (extent.logical_block < keep_blocks &&
            extent.logical_block + extent.block_count > keep_blocks) {
            /*
             * Current writers create one-block extents. Keep this branch
             * defensive for future multi-block extents by shrinking the
             * tail extent and releasing its unused physical suffix.
             */
            uint64_t keep_in_extent = keep_blocks - extent.logical_block;
            uint64_t free_start = extent.physical_block + keep_in_extent;
            uint32_t free_count = extent.block_count - (uint32_t)keep_in_extent;
            uint8_t extent_image[4096];
            struct pros_prosenjitfs_extent shortened = extent;
            if (free_count == 0u)
                return PROS_VFS_EIO;
            shortened.block_count = (uint32_t)keep_in_extent;
            if (pfs_read_block(data->fs->mount.device, current,
                               extent_image) != PROSENJITFS_OK)
                return PROS_VFS_EIO;
            pfs_copy(extent_image, &shortened, sizeof(shortened));
            pfs_put32(extent_image +
                      offsetof(struct pros_prosenjitfs_extent, checksum),
                      pfs_crc_with_zero32(extent_image, sizeof(shortened),
                                          (uint32_t)offsetof(
                                              struct pros_prosenjitfs_extent,
                                              checksum)));
            if (pfs_tx_reset(data->fs) != PROSENJITFS_OK ||
                pfs_allocator_stage(data->fs, free_start, free_count, 0u) !=
                    PROSENJITFS_OK ||
                pfs_tx_add(data->fs, current, extent_image) != PROSENJITFS_OK)
                return PROS_VFS_EIO;
            if (pfs_tx_commit(data->fs) != PROSENJITFS_OK)
                return PROS_VFS_EIO;
            break;
        }

        previous = current;
        current = next;
    }

    {
        uint8_t inode_image[4096];
        uint64_t inode_lba;
        uint32_t inode_offset;
        if (pfs_inode_block_location(data->fs, data->inode_number,
                                     &inode_lba, &inode_offset) != PROSENJITFS_OK ||
            pfs_read_block(data->fs->mount.device, inode_lba, inode_image) !=
                PROSENJITFS_OK)
            return PROS_VFS_EIO;
        inode.size = size;
        inode.allocated_bytes = keep_blocks * data->fs->sb.block_size;
        pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
        pfs_put32(inode_image + inode_offset +
                  offsetof(struct pros_prosenjitfs_inode, checksum),
                  pfs_crc_with_zero32(inode_image + inode_offset,
                                      sizeof(inode),
                                      (uint32_t)offsetof(
                                          struct pros_prosenjitfs_inode,
                                          checksum)));
        if (pfs_tx_reset(data->fs) != PROSENJITFS_OK ||
            pfs_tx_add(data->fs, inode_lba, inode_image) != PROSENJITFS_OK ||
            pfs_tx_commit(data->fs) != PROSENJITFS_OK)
            return PROS_VFS_EIO;
    }
    file->vnode->size = size;
    return PROS_VFS_OK;
}

static int pfs_file_close(struct pros_vfs_file *file)
{
    return file && file->vnode ? PROS_VFS_OK : PROS_VFS_EINVAL;
}

static const struct pros_vfs_file_ops pfs_file_ops = {
    .read = pfs_file_read,
    .write = pfs_file_write,
    .truncate = pfs_file_truncate,
    .close = pfs_file_close
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

    *result = (struct pros_vfs_vnode *)kmalloc(sizeof(**result));
    if (!*result) {
        kfree(data);
        return PROS_VFS_EIO;
    }
    if (vfs_vnode_init(*result, &fs->mount, parent, type,
                       &pfs_vnode_ops,
                       type == PROS_VFS_NODE_FILE ? &pfs_file_ops : 0,
                       data, inode.size) != PROS_VFS_OK) {
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



static int pfs_vnode_mkdir(struct pros_vfs_vnode *parent, const char *name,
                           struct pros_vfs_vnode **result)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode parent_inode, child_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_dir_entry entry;
    uint8_t *dir_block = 0, *entry_block = 0, *inode_block = 0, *child_dir_block = 0;
    uint64_t inode_number, entry_lba, child_dir_lba, inode_lba;
    uint32_t inode_offset, name_length, bucket;
    int rc;

    if (!parent || !name || !result || !parent->private_data ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_ENOTDIR;
    *result = 0;
    data = (struct pfs_vnode_data *)parent->private_data;
    if (!data->fs || parent->mount->read_only || !parent->mount->active)
        return PROS_VFS_EPERM;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK ||
        name_length == 0u || name_length > PROSENJITFS_MAX_NAME ||
        (name_length == 1u && name[0] == '.') ||
        (name_length == 2u && name[0] == '.' && name[1] == '.'))
        return PROS_VFS_EINVAL;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &parent_inode) !=
            PROSENJITFS_OK ||
        parent_inode.type != PROSENJITFS_INODE_DIR ||
        pfs_read_dir_index(data->fs, &parent_inode, &dir_index) !=
            PROSENJITFS_OK)
        return PROS_VFS_EIO;
    rc = pfs_find_dir_entry(data->fs, &parent_inode, name, 0);
    if (rc == PROSENJITFS_OK)
        return PROS_VFS_EEXIST;
    if (rc != PROSENJITFS_ENOENT)
        return PROS_VFS_EIO;
    if (pfs_find_free_inode(data->fs, &inode_number) != PROSENJITFS_OK ||
        pfs_find_free_data_block(data->fs, &entry_lba) != PROSENJITFS_OK)
        return PROS_VFS_EOVERFLOW;
    if (pfs_find_free_data_block(data->fs, &child_dir_lba) != PROSENJITFS_OK)
        return PROS_VFS_EOVERFLOW;

    dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    entry_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    inode_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    child_dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!dir_block || !entry_block || !inode_block || !child_dir_block) {
        if (dir_block) kfree(dir_block);
        if (entry_block) kfree(entry_block);
        if (inode_block) kfree(inode_block);
        if (child_dir_block) kfree(child_dir_block);
        return PROS_VFS_EIO;
    }

    pfs_zero(&child_inode, sizeof(child_inode));
    child_inode.type = PROSENJITFS_INODE_DIR;
    child_inode.mode = 0755u;
    child_inode.uid = 0u;
    child_inode.gid = 0u;
    child_inode.links = 1u;
    child_inode.generation = 1u;
    child_inode.parent_inode = data->inode_number;
    child_inode.dir_index = child_dir_lba;

    pfs_zero(child_dir_block, data->fs->sb.block_size);
    {
        struct pros_prosenjitfs_dir_index child_index;
        pfs_zero(&child_index, sizeof(child_index));
        child_index.magic = PROSENJITFS_DIR_MAGIC;
        child_index.version = PROSENJITFS_VERSION;
        child_index.bucket_count = PROSENJITFS_DIR_BUCKETS;
        child_index.inode = inode_number;
        child_index.generation = 1u;
        pfs_copy(child_dir_block, &child_index, sizeof(child_index));
        pfs_put32(child_dir_block + offsetof(struct pros_prosenjitfs_dir_index,
                                             checksum),
                  pfs_crc_with_zero32(child_dir_block, sizeof(child_index),
                                      (uint32_t)offsetof(
                                          struct pros_prosenjitfs_dir_index,
                                          checksum)));
    }

    bucket = pfs_name_hash((const uint8_t *)name, name_length) %
             PROSENJITFS_DIR_BUCKETS;
    pfs_zero(&entry, sizeof(entry));
    entry.magic = PROSENJITFS_ENTRY_MAGIC;
    entry.next = dir_index.heads[bucket];
    entry.inode = inode_number;
    entry.name_hash = pfs_name_hash((const uint8_t *)name, name_length);
    entry.type = PROSENJITFS_INODE_DIR;
    entry.name_length = (uint16_t)name_length;
    pfs_copy(entry.name, name, name_length);
    entry.checksum = prosenjitfs_crc32c(&entry, sizeof(entry));
    /* Recompute using the on-disk checksum convention. */
    entry.checksum = pfs_crc_with_zero32((const uint8_t *)&entry, sizeof(entry),
        (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
    pfs_zero(entry_block, data->fs->sb.block_size);
    pfs_copy(entry_block, &entry, sizeof(entry));
    dir_index.heads[bucket] = entry_lba;
    pfs_zero(dir_block, data->fs->sb.block_size);
    pfs_copy(dir_block, &dir_index, sizeof(dir_index));
    pfs_put32(dir_block + offsetof(struct pros_prosenjitfs_dir_index, checksum),
              pfs_crc_with_zero32(dir_block, sizeof(dir_index),
                                  (uint32_t)offsetof(
                                      struct pros_prosenjitfs_dir_index,
                                      checksum)));

    if (pfs_inode_block_location(data->fs, inode_number, &inode_lba,
                                 &inode_offset) != PROSENJITFS_OK ||
        pfs_read_block(data->fs->mount.device, inode_lba, inode_block) !=
            PROSENJITFS_OK)
        { rc = PROS_VFS_EIO; goto mkdir_out; }
    pfs_copy(inode_block + inode_offset, &child_inode, sizeof(child_inode));
    pfs_put32(inode_block + inode_offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(inode_block + inode_offset,
                                  sizeof(child_inode),
                                  (uint32_t)offsetof(
                                      struct pros_prosenjitfs_inode, checksum)));

    rc = pfs_tx_reset(data->fs);
    if (rc == PROSENJITFS_OK) rc = pfs_inode_bitmap_stage(data->fs, inode_number, 1u);
    if (rc == PROSENJITFS_OK) rc = pfs_allocator_stage(data->fs, entry_lba, 1u, 1u);
    if (rc == PROSENJITFS_OK) rc = pfs_allocator_stage(data->fs, child_dir_lba, 1u, 1u);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(data->fs, inode_lba, inode_block);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(data->fs, parent_inode.dir_index, dir_block);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(data->fs, entry_lba, entry_block);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(data->fs, child_dir_lba, child_dir_block);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_commit(data->fs);
    if (rc == PROSENJITFS_OK)
        rc = pfs_make_vnode(data->fs, inode_number, parent, result);
    if (rc != PROSENJITFS_OK && data->fs->tx.count != 0u)
        pfs_tx_reset(data->fs);

mkdir_out:
    if (dir_block) kfree(dir_block);
    if (entry_block) kfree(entry_block);
    if (inode_block) kfree(inode_block);
    if (child_dir_block) kfree(child_dir_block);
    return rc == PROSENJITFS_OK ? PROS_VFS_OK : PROS_VFS_EIO;
}

static int pfs_vnode_rmdir(struct pros_vfs_vnode *parent, const char *name)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode parent_inode, child_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_dir_entry entry;
    uint8_t *dir_block = 0, *prev_block = 0, *inode_block = 0, *entry_block = 0;
    uint64_t current, previous = 0u, target_lba = 0u, inode_lba, steps = 0u;
    uint64_t inode_number;
    uint32_t inode_offset, name_length, bucket;
    int rc = PROSENJITFS_EIO;

    if (!parent || !name || !parent->private_data ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_ENOTDIR;
    data = (struct pfs_vnode_data *)parent->private_data;
    if (!data->fs || parent->mount->read_only || !parent->mount->active)
        return PROS_VFS_EPERM;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK ||
        name_length == 0u || name_length > PROSENJITFS_MAX_NAME ||
        (name_length == 1u && name[0] == '.') ||
        (name_length == 2u && name[0] == '.' && name[1] == '.'))
        return PROS_VFS_EINVAL;
    if (prosenjitfs_read_inode(data->fs, data->inode_number, &parent_inode) !=
            PROSENJITFS_OK ||
        parent_inode.type != PROSENJITFS_INODE_DIR ||
        pfs_read_dir_index(data->fs, &parent_inode, &dir_index) !=
            PROSENJITFS_OK)
        return PROS_VFS_EIO;

    entry_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!entry_block)
        { rc = PROS_VFS_EIO; goto rmdir_out; }

    bucket = pfs_name_hash((const uint8_t *)name, name_length) %
             PROSENJITFS_DIR_BUCKETS;
    current = dir_index.heads[bucket];
    while (current != 0u) {
        if (++steps > data->fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        if (current < data->fs->sb.data_start ||
            current >= data->fs->sb.total_blocks ||
            pfs_read_block(data->fs->mount.device, current, entry_block) !=
                PROSENJITFS_OK) {
            /* entry_block is allocated below; defer to the normal error path. */
            break;
        }
        pfs_copy(&entry, entry_block, sizeof(entry));
        if (entry.magic != PROSENJITFS_ENTRY_MAGIC ||
            entry.name_length == 0u || entry.name_length > PROSENJITFS_MAX_NAME ||
            pfs_crc_with_zero32(entry_block, sizeof(entry),
                (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum)) !=
                entry.checksum ||
            entry.next >= data->fs->sb.total_blocks ||
            (entry.next != 0u && entry.next < data->fs->sb.data_start))
            break;
        if (entry.name_length == name_length &&
            pfs_name_hash(entry.name, entry.name_length) ==
                pfs_name_hash((const uint8_t *)name, name_length)) {
            uint32_t i = 0u;
            while (i < name_length && entry.name[i] == (uint8_t)name[i]) ++i;
            if (i == name_length) {
                target_lba = current;
                inode_number = entry.inode;
                break;
            }
        }
        previous = current;
        current = entry.next;
    }
    if (target_lba == 0u) { rc = PROSENJITFS_ENOENT; goto rmdir_out; }
    if (inode_number == data->inode_number) { rc = PROS_VFS_EPERM; goto rmdir_out; }
    if (prosenjitfs_read_inode(data->fs, inode_number, &child_inode) !=
            PROSENJITFS_OK)
        goto rmdir_out;
    if (child_inode.type != PROSENJITFS_INODE_DIR) {
        rc = PROS_VFS_ENOTDIR; goto rmdir_out;
    }

    /* A directory is removable only when every bucket is empty. */
    {
        uint8_t *scan = (uint8_t *)kmalloc(data->fs->sb.block_size);
        if (!scan) { rc = PROS_VFS_EIO; goto rmdir_out; }
        for (uint32_t b = 0u; b < PROSENJITFS_DIR_BUCKETS; ++b) {
            current = 0u; steps = 0u;
            if (pfs_read_dir_index(data->fs, &child_inode, &dir_index) !=
                    PROSENJITFS_OK) { kfree(scan); rc = PROS_VFS_EIO; goto rmdir_out; }
            current = dir_index.heads[b];
            while (current != 0u) {
                if (++steps > data->fs->sb.total_blocks) { kfree(scan); rc = PROSENJITFS_ECORRUPT; goto rmdir_out; }
                if (pfs_read_block(data->fs->mount.device, current, scan) != PROSENJITFS_OK) { kfree(scan); rc = PROSENJITFS_ECORRUPT; goto rmdir_out; }
                {
                    struct pros_prosenjitfs_dir_entry child_entry;
                    pfs_copy(&child_entry, scan, sizeof(child_entry));
                    if (child_entry.magic != PROSENJITFS_ENTRY_MAGIC ||
                        child_entry.name_length == 0u ||
                        child_entry.name_length > PROSENJITFS_MAX_NAME ||
                        child_entry.next >= data->fs->sb.total_blocks ||
                        (child_entry.next != 0u && child_entry.next < data->fs->sb.data_start) ||
                        pfs_crc_with_zero32(scan, sizeof(child_entry),
                            (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum)) !=
                            child_entry.checksum) {
                        kfree(scan); rc = PROSENJITFS_ECORRUPT; goto rmdir_out;
                    }
                    return (kfree(scan), (int)PROS_VFS_ENOTEMPTY);
                }
            }
        }
        kfree(scan);
    }

    if (pfs_inode_block_location(data->fs, inode_number, &inode_lba,
                                 &inode_offset) != PROSENJITFS_OK)
        goto rmdir_out;
    inode_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    prev_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!inode_block || !dir_block || !prev_block)
        goto rmdir_out;
    if (pfs_read_block(data->fs->mount.device, inode_lba, inode_block) != PROSENJITFS_OK ||
        pfs_read_block(data->fs->mount.device, parent_inode.dir_index, dir_block) != PROSENJITFS_OK)
        goto rmdir_out;

    if (previous == 0u) dir_index.heads[bucket] = entry.next;
    else {
        if (pfs_read_block(data->fs->mount.device, previous, prev_block) != PROSENJITFS_OK)
            goto rmdir_out;
        {
            struct pros_prosenjitfs_dir_entry prev;
            pfs_copy(&prev, prev_block, sizeof(prev));
            if (prev.magic != PROSENJITFS_ENTRY_MAGIC || prev.next != target_lba)
                { rc = PROSENJITFS_ECORRUPT; goto rmdir_out; }
            prev.next = entry.next;
            pfs_copy(prev_block, &prev, sizeof(prev));
            pfs_put32(prev_block + offsetof(struct pros_prosenjitfs_dir_entry, checksum),
                pfs_crc_with_zero32(prev_block, sizeof(prev),
                    (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum)));
        }
    }
    pfs_zero(dir_block, data->fs->sb.block_size);
    pfs_copy(dir_block, &dir_index, sizeof(dir_index));
    pfs_put32(dir_block + offsetof(struct pros_prosenjitfs_dir_index, checksum),
        pfs_crc_with_zero32(dir_block, sizeof(dir_index),
            (uint32_t)offsetof(struct pros_prosenjitfs_dir_index, checksum)));
    pfs_zero(inode_block + inode_offset, sizeof(child_inode));
    pfs_put32(inode_block + inode_offset + offsetof(struct pros_prosenjitfs_inode, checksum),
        pfs_crc_with_zero32(inode_block + inode_offset, sizeof(child_inode),
            (uint32_t)offsetof(struct pros_prosenjitfs_inode, checksum)));

    if (pfs_tx_reset(data->fs) != PROSENJITFS_OK ||
        pfs_tx_add(data->fs, parent_inode.dir_index, dir_block) != PROSENJITFS_OK ||
        (previous != 0u && pfs_tx_add(data->fs, previous, prev_block) != PROSENJITFS_OK) ||
        pfs_tx_add(data->fs, inode_lba, inode_block) != PROSENJITFS_OK ||
        pfs_inode_bitmap_stage(data->fs, inode_number, 0u) != PROSENJITFS_OK ||
        pfs_allocator_stage(data->fs, target_lba, 1u, 0u) != PROSENJITFS_OK ||
        pfs_allocator_stage(data->fs, child_inode.dir_index, 1u, 0u) != PROSENJITFS_OK ||
        pfs_tx_commit(data->fs) != PROSENJITFS_OK) {
        pfs_tx_reset(data->fs);
        goto rmdir_out;
    }
    rc = PROS_VFS_OK;
rmdir_out:
    if (dir_block) kfree(dir_block);
    if (prev_block) kfree(prev_block);
    if (inode_block) kfree(inode_block);
    if (entry_block) kfree(entry_block);
    return rc;
}

static int pfs_vnode_unlink(struct pros_vfs_vnode *parent, const char *name)
{
    struct pfs_vnode_data *parent_data;
    struct pros_prosenjitfs_inode parent_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_dir_entry entry;
    struct pros_prosenjitfs_inode child_inode;
    uint8_t *dir_block = 0;
    uint8_t *prev_block = 0;
    uint8_t *inode_block = 0;
    uint8_t *entry_block = 0;
    uint64_t current, previous = 0u, target_lba = 0u;
    uint64_t inode_number;
    uint64_t inode_byte_offset, inode_lba;
    uint32_t inode_offset;
    uint32_t name_length = 0u;
    uint32_t bucket;
    uint64_t steps = 0u;
    int rc = PROSENJITFS_EIO;

    if (!parent || !name || !parent->private_data ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_ENOTDIR;
    parent_data = (struct pfs_vnode_data *)parent->private_data;
    if (!parent_data->fs || parent->mount->read_only || !parent->mount->active)
        return PROS_VFS_EPERM;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK ||
        name_length == 0u || name_length > PROSENJITFS_MAX_NAME ||
        (name_length == 1u && name[0] == '.') ||
        (name_length == 2u && name[0] == '.' && name[1] == '.'))
        return PROS_VFS_EINVAL;
    if (prosenjitfs_read_inode(parent_data->fs, parent_data->inode_number,
                               &parent_inode) != PROSENJITFS_OK ||
        parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_ENOTDIR;
    if (pfs_read_dir_index(parent_data->fs, &parent_inode, &dir_index) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;

    dir_block = (uint8_t *)kmalloc(parent_data->fs->sb.block_size);
    prev_block = (uint8_t *)kmalloc(parent_data->fs->sb.block_size);
    inode_block = (uint8_t *)kmalloc(parent_data->fs->sb.block_size);
    entry_block = (uint8_t *)kmalloc(parent_data->fs->sb.block_size);
    if (!dir_block || !prev_block || !inode_block || !entry_block) {
        rc = PROSENJITFS_EIO;
        goto out;
    }

    bucket = pfs_name_hash((const uint8_t *)name, name_length) %
             PROSENJITFS_DIR_BUCKETS;
    current = dir_index.heads[bucket];
    while (current != 0u) {
        if (++steps > parent_data->fs->sb.total_blocks) {
            rc = PROSENJITFS_ECORRUPT;
            goto out;
        }
        if (current < parent_data->fs->sb.data_start ||
            current >= parent_data->fs->sb.total_blocks ||
            pfs_read_block(parent_data->fs->mount.device, current,
                           entry_block) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            goto out;
        }
        pfs_copy(&entry, entry_block, sizeof(entry));
        if (entry.magic != PROSENJITFS_ENTRY_MAGIC ||
            entry.name_length == 0u ||
            entry.name_length > PROSENJITFS_MAX_NAME ||
            pfs_crc_with_zero32(entry_block, sizeof(entry),
                                (uint32_t)offsetof(
                                    struct pros_prosenjitfs_dir_entry,
                                    checksum)) != entry.checksum ||
            entry.next >= parent_data->fs->sb.total_blocks ||
            (entry.next != 0u && entry.next < parent_data->fs->sb.data_start)) {
            rc = PROSENJITFS_ECORRUPT;
            goto out;
        }
        if (entry.name_length == name_length &&
            pfs_name_hash(entry.name, entry.name_length) ==
                entry.name_hash) {
            uint32_t i = 0u;
            while (i < name_length && entry.name[i] == (uint8_t)name[i])
                ++i;
            if (i == name_length) {
                target_lba = current;
                inode_number = entry.inode;
                break;
            }
        }
        previous = current;
        current = entry.next;
    }

    if (target_lba == 0u)
        { rc = PROSENJITFS_ENOENT; goto out; }
    if (inode_number == parent_data->inode_number)
        { rc = PROSENJITFS_EPERM; goto out; }
    if (prosenjitfs_read_inode(parent_data->fs, inode_number, &child_inode) !=
        PROSENJITFS_OK)
        { rc = PROSENJITFS_EIO; goto out; }
    if (child_inode.type == PROSENJITFS_INODE_DIR)
        { rc = PROS_VFS_EISDIR; goto out; }

    inode_byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    inode_lba = parent_data->fs->sb.inode_table_start +
                inode_byte_offset / parent_data->fs->sb.block_size;
    inode_offset = (uint32_t)(inode_byte_offset %
                              parent_data->fs->sb.block_size);
    if (pfs_read_block(parent_data->fs->mount.device, inode_lba,
                       inode_block) != PROSENJITFS_OK)
        { rc = PROSENJITFS_EIO; goto out; }

    if (pfs_tx_reset(parent_data->fs) != PROSENJITFS_OK)
        { rc = PROSENJITFS_EIO; goto out; }

    /*
     * Remove the namespace reference and reclaim every owned block in one
     * journal transaction. If the transaction cannot represent the complete
     * operation (for example, an unusually large file spanning many bitmap
     * blocks), nothing is committed and the caller can retry safely.
     */
    if (previous == 0u) {
        dir_index.heads[bucket] = entry.next;
        pfs_zero(dir_block, parent_data->fs->sb.block_size);
        pfs_copy(dir_block, &dir_index, sizeof(dir_index));
        pfs_put32(dir_block + offsetof(struct pros_prosenjitfs_dir_index,
                                       checksum),
                  pfs_crc_with_zero32(dir_block, sizeof(dir_index),
                                      (uint32_t)offsetof(
                                          struct pros_prosenjitfs_dir_index,
                                          checksum)));
        rc = pfs_tx_add(parent_data->fs, parent_inode.dir_index, dir_block);
        if (rc != PROSENJITFS_OK) goto out_reset;
    } else {
        if (pfs_read_block(parent_data->fs->mount.device, previous,
                           prev_block) != PROSENJITFS_OK)
            { rc = PROSENJITFS_EIO; goto out_reset; }
        {
            struct pros_prosenjitfs_dir_entry prev;
            pfs_copy(&prev, prev_block, sizeof(prev));
            if (prev.magic != PROSENJITFS_ENTRY_MAGIC ||
                prev.next != target_lba)
                { rc = PROSENJITFS_ECORRUPT; goto out_reset; }
            prev.next = entry.next;
            pfs_copy(prev_block, &prev, sizeof(prev));
            pfs_put32(prev_block + offsetof(
                          struct pros_prosenjitfs_dir_entry, checksum),
                      pfs_crc_with_zero32(
                          prev_block, sizeof(prev),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_dir_entry, checksum)));
        }
        rc = pfs_tx_add(parent_data->fs, previous, prev_block);
        if (rc != PROSENJITFS_OK) goto out_reset;
    }

    pfs_zero(inode_block + inode_offset, sizeof(child_inode));
    pfs_put32(inode_block + inode_offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(inode_block + inode_offset,
                                  sizeof(child_inode),
                                  (uint32_t)offsetof(
                                      struct pros_prosenjitfs_inode,
                                      checksum)));
    rc = pfs_tx_add(parent_data->fs, inode_lba, inode_block);
    if (rc != PROSENJITFS_OK) goto out_reset;

    rc = pfs_inode_bitmap_stage(parent_data->fs, inode_number, 0u);
    if (rc != PROSENJITFS_OK) goto out_reset;

    rc = pfs_allocator_stage(parent_data->fs, target_lba, 1u, 0u);
    if (rc != PROSENJITFS_OK) goto out_reset;

    current = child_inode.extent_head;
    steps = 0u;
    while (current != 0u) {
        struct pros_prosenjitfs_extent extent;
        uint64_t next;
        if (++steps > parent_data->fs->sb.total_blocks) {
            rc = PROSENJITFS_ECORRUPT;
            goto out_reset;
        }
        if (pfs_read_extent(parent_data->fs, current, &extent) !=
            PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            goto out_reset;
        }
        next = extent.next;
        if (extent.block_count > UINT32_MAX) {
            rc = PROSENJITFS_EOVERFLOW;
            goto out_reset;
        }
        rc = pfs_allocator_stage(parent_data->fs, extent.physical_block,
                                 (uint32_t)extent.block_count, 0u);
        if (rc != PROSENJITFS_OK) goto out_reset;
        rc = pfs_allocator_stage(parent_data->fs, current, 1u, 0u);
        if (rc != PROSENJITFS_OK) goto out_reset;
        current = next;
    }

    rc = pfs_tx_commit(parent_data->fs);
    if (rc == PROSENJITFS_OK)
        rc = PROS_VFS_OK;
    goto out;

out_reset:
    pfs_tx_reset(parent_data->fs);
out:
    if (dir_block) kfree(dir_block);
    if (prev_block) kfree(prev_block);
    if (inode_block) kfree(inode_block);
    if (entry_block) kfree(entry_block);
    return rc;
}


static int pfs_vnode_rename(struct pros_vfs_vnode *old_parent,
                            const char *old_name,
                            struct pros_vfs_vnode *new_parent,
                            const char *new_name)
{
    struct pfs_vnode_data *data;
    struct pfs_vnode_data *new_data;
    struct pros_prosenjitfs_inode parent_inode;
    struct pros_prosenjitfs_inode new_parent_inode;
    struct pros_prosenjitfs_inode child_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_dir_index new_dir_index;
    struct pros_prosenjitfs_dir_entry entry;
    uint8_t *dir_block = 0;
    uint8_t *new_dir_block = 0;
    uint8_t *prev_block = 0;
    uint8_t *entry_block = 0;
    uint8_t *inode_block = 0;
    uint64_t current, previous = 0u, target_lba = 0u;
    uint32_t old_length = 0u, new_length = 0u;
    uint32_t old_bucket, new_bucket;
    uint64_t steps = 0u;
    int rc = PROS_VFS_EIO;

    if (!old_parent || !new_parent || !old_name || !new_name ||
        !old_parent->private_data || !new_parent->private_data ||
        old_parent->type != PROS_VFS_NODE_DIRECTORY ||
        new_parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_ENOTDIR;

    data = (struct pfs_vnode_data *)old_parent->private_data;
    new_data = (struct pfs_vnode_data *)new_parent->private_data;
    if (!data->fs || data->fs != new_data->fs)
        return PROS_VFS_ENOSYS;
    if (old_parent->mount->read_only || !old_parent->mount->active)
        return PROS_VFS_EPERM;

    if (pfs_utf8_name_valid(old_name, &old_length) != PROSENJITFS_OK ||
        pfs_utf8_name_valid(new_name, &new_length) != PROSENJITFS_OK ||
        old_length == 0u || new_length == 0u ||
        old_length > PROSENJITFS_MAX_NAME ||
        new_length > PROSENJITFS_MAX_NAME ||
        (old_length == 1u && old_name[0] == '.') ||
        (old_length == 2u && old_name[0] == '.' && old_name[1] == '.') ||
        (new_length == 1u && new_name[0] == '.') ||
        (new_length == 2u && new_name[0] == '.' && new_name[1] == '.'))
        return PROS_VFS_EINVAL;

    if (old_parent == new_parent ||
        data->inode_number == new_data->inode_number) {
        if (old_length == new_length) {
            uint32_t i = 0u;
            while (i < old_length && old_name[i] == new_name[i])
                ++i;
            if (i == old_length)
                return PROS_VFS_OK;
        }
    }

    if (prosenjitfs_read_inode(data->fs, data->inode_number,
                               &parent_inode) != PROSENJITFS_OK ||
        parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_ENOTDIR;
    if (pfs_read_dir_index(data->fs, &parent_inode, &dir_index) !=
        PROSENJITFS_OK)
        return PROS_VFS_EIO;

    if (data->inode_number == new_data->inode_number) {
        new_parent_inode = parent_inode;
        new_dir_index = dir_index;
    } else {
        if (prosenjitfs_read_inode(data->fs, new_data->inode_number,
                                   &new_parent_inode) != PROSENJITFS_OK ||
            new_parent_inode.type != PROSENJITFS_INODE_DIR ||
            pfs_read_dir_index(data->fs, &new_parent_inode, &new_dir_index) !=
                PROSENJITFS_OK)
            return PROS_VFS_ENOTDIR;
    }

    /* Rename/move never overwrites an existing directory entry. */
    rc = pfs_find_dir_entry(data->fs, &new_parent_inode, new_name, 0);
    if (rc == PROSENJITFS_OK)
        return PROS_VFS_EEXIST;
    if (rc != PROSENJITFS_ENOENT)
        return PROS_VFS_EIO;

    old_bucket = pfs_name_hash((const uint8_t *)old_name, old_length) %
                 PROSENJITFS_DIR_BUCKETS;
    new_bucket = pfs_name_hash((const uint8_t *)new_name, new_length) %
                 PROSENJITFS_DIR_BUCKETS;

    entry_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!entry_block || !dir_block)
        goto out;

    current = dir_index.heads[old_bucket];
    while (current != 0u) {
        if (++steps > data->fs->sb.total_blocks) {
            rc = PROSENJITFS_ECORRUPT;
            goto out;
        }
        if (current < data->fs->sb.data_start ||
            current >= data->fs->sb.total_blocks) {
            rc = PROSENJITFS_ECORRUPT;
            goto out;
        }
        if (pfs_read_block(data->fs->mount.device, current, entry_block) !=
            PROSENJITFS_OK) {
            rc = PROSENJITFS_EIO;
            goto out;
        }
        pfs_copy(&entry, entry_block, sizeof(entry));
        if (entry.magic != PROSENJITFS_ENTRY_MAGIC ||
            entry.name_length == 0u ||
            entry.name_length > PROSENJITFS_MAX_NAME ||
            pfs_crc_with_zero32(entry_block, sizeof(entry),
                                (uint32_t)offsetof(
                                    struct pros_prosenjitfs_dir_entry,
                                    checksum)) != entry.checksum ||
            entry.next >= data->fs->sb.total_blocks ||
            (entry.next != 0u && entry.next < data->fs->sb.data_start)) {
            rc = PROSENJITFS_ECORRUPT;
            goto out;
        }
        if (entry.name_length == old_length &&
            pfs_name_hash(entry.name, entry.name_length) ==
                pfs_name_hash((const uint8_t *)old_name, old_length)) {
            uint32_t i = 0u;
            while (i < old_length && entry.name[i] == (uint8_t)old_name[i])
                ++i;
            if (i == old_length) {
                target_lba = current;
                break;
            }
        }
        previous = current;
        current = entry.next;
    }

    if (target_lba == 0u) {
        rc = PROSENJITFS_ENOENT;
        goto out;
    }

    /*
     * Read the child inode before staging either parent. For directories,
     * moving across parents changes only parent_inode; regular files keep
     * their inode metadata unchanged.
     */
    if (prosenjitfs_read_inode(data->fs, entry.inode, &child_inode) !=
        PROSENJITFS_OK) {
        rc = PROS_VFS_EIO;
        goto out;
    }
    if (child_inode.type == PROSENJITFS_INODE_DIR &&
        data->inode_number != new_data->inode_number) {
        uint64_t ancestor = new_data->inode_number;
        uint64_t depth = 0u;
        while (ancestor != data->inode_number &&
               ancestor != data->fs->sb.root_inode) {
            struct pros_prosenjitfs_inode ancestor_inode;
            if (++depth > data->fs->sb.inode_count ||
                prosenjitfs_read_inode(data->fs, ancestor,
                                        &ancestor_inode) != PROSENJITFS_OK ||
                ancestor_inode.type != PROSENJITFS_INODE_DIR) {
                rc = PROSENJITFS_ECORRUPT;
                goto out;
            }
            if (ancestor_inode.parent_inode == ancestor) {
                rc = PROSENJITFS_ECORRUPT;
                goto out;
            }
            ancestor = ancestor_inode.parent_inode;
        }
        if (ancestor == data->inode_number) {
            rc = PROS_VFS_EINVAL;
            goto out;
        }
    }

    prev_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    new_dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!prev_block || !new_dir_block)
        goto out;

    if (pfs_tx_reset(data->fs) != PROSENJITFS_OK)
        goto out;

    if (data->inode_number == new_data->inode_number) {
        /*
         * Same-directory rename retains the existing directory entry block.
         * The inode and all file attributes remain untouched.
         */
        if (old_bucket == new_bucket) {
            entry.next = entry.next;
        } else {
            uint64_t destination_head = dir_index.heads[new_bucket];
            if (previous == 0u)
                dir_index.heads[old_bucket] = entry.next;
            else {
                if (pfs_read_block(data->fs->mount.device, previous,
                                   prev_block) != PROSENJITFS_OK)
                    goto out_reset;
                {
                    struct pros_prosenjitfs_dir_entry prev;
                    pfs_copy(&prev, prev_block, sizeof(prev));
                    if (prev.magic != PROSENJITFS_ENTRY_MAGIC ||
                        prev.next != target_lba) {
                        rc = PROSENJITFS_ECORRUPT;
                        goto out_reset;
                    }
                    prev.next = entry.next;
                    pfs_copy(prev_block, &prev, sizeof(prev));
                    pfs_put32(prev_block + offsetof(
                                  struct pros_prosenjitfs_dir_entry, checksum),
                              pfs_crc_with_zero32(
                                  prev_block, sizeof(prev),
                                  (uint32_t)offsetof(
                                      struct pros_prosenjitfs_dir_entry,
                                      checksum)));
                }
                if (pfs_tx_add(data->fs, previous, prev_block) !=
                    PROSENJITFS_OK)
                    goto out_reset;
            }
            dir_index.heads[new_bucket] = target_lba;
            entry.next = destination_head;
            pfs_zero(dir_block, data->fs->sb.block_size);
            pfs_copy(dir_block, &dir_index, sizeof(dir_index));
            pfs_put32(dir_block + offsetof(
                          struct pros_prosenjitfs_dir_index, checksum),
                      pfs_crc_with_zero32(
                          dir_block, sizeof(dir_index),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_dir_index, checksum)));
            if (pfs_tx_add(data->fs, parent_inode.dir_index, dir_block) !=
                PROSENJITFS_OK)
                goto out_reset;
        }
    } else {
        /*
         * Cross-directory move: atomically remove the entry from the source
         * bucket and insert the same entry block at the destination bucket.
         */
        if (previous == 0u)
            dir_index.heads[old_bucket] = entry.next;
        else {
            if (pfs_read_block(data->fs->mount.device, previous,
                               prev_block) != PROSENJITFS_OK)
                goto out_reset;
            {
                struct pros_prosenjitfs_dir_entry prev;
                pfs_copy(&prev, prev_block, sizeof(prev));
                if (prev.magic != PROSENJITFS_ENTRY_MAGIC ||
                    prev.next != target_lba) {
                    rc = PROSENJITFS_ECORRUPT;
                    goto out_reset;
                }
                prev.next = entry.next;
                pfs_copy(prev_block, &prev, sizeof(prev));
                pfs_put32(prev_block + offsetof(
                              struct pros_prosenjitfs_dir_entry, checksum),
                          pfs_crc_with_zero32(
                              prev_block, sizeof(prev),
                              (uint32_t)offsetof(
                                  struct pros_prosenjitfs_dir_entry, checksum)));
            }
            if (pfs_tx_add(data->fs, previous, prev_block) != PROSENJITFS_OK)
                goto out_reset;
        }

        pfs_zero(dir_block, data->fs->sb.block_size);
        pfs_copy(dir_block, &dir_index, sizeof(dir_index));
        pfs_put32(dir_block + offsetof(struct pros_prosenjitfs_dir_index,
                                       checksum),
                  pfs_crc_with_zero32(
                      dir_block, sizeof(dir_index),
                      (uint32_t)offsetof(
                          struct pros_prosenjitfs_dir_index, checksum)));
        if (pfs_tx_add(data->fs, parent_inode.dir_index, dir_block) !=
            PROSENJITFS_OK)
            goto out_reset;

        entry.next = new_dir_index.heads[new_bucket];
        entry.name_hash = pfs_name_hash((const uint8_t *)new_name,
                                        new_length);
        entry.name_length = (uint16_t)new_length;
        pfs_zero(entry.name, sizeof(entry.name));
        pfs_copy(entry.name, new_name, new_length);
        pfs_zero(entry_block, data->fs->sb.block_size);
        pfs_copy(entry_block, &entry, sizeof(entry));
        pfs_put32(entry_block + offsetof(struct pros_prosenjitfs_dir_entry,
                                         checksum),
                  pfs_crc_with_zero32(
                      entry_block, sizeof(entry),
                      (uint32_t)offsetof(
                          struct pros_prosenjitfs_dir_entry, checksum)));
        if (pfs_tx_add(data->fs, target_lba, entry_block) !=
            PROSENJITFS_OK)
            goto out_reset;

        new_dir_index.heads[new_bucket] = target_lba;
        pfs_zero(new_dir_block, data->fs->sb.block_size);
        pfs_copy(new_dir_block, &new_dir_index, sizeof(new_dir_index));
        pfs_put32(new_dir_block + offsetof(
                      struct pros_prosenjitfs_dir_index, checksum),
                  pfs_crc_with_zero32(
                      new_dir_block, sizeof(new_dir_index),
                      (uint32_t)offsetof(
                          struct pros_prosenjitfs_dir_index, checksum)));
        if (pfs_tx_add(data->fs, new_parent_inode.dir_index, new_dir_block) !=
            PROSENJITFS_OK)
            goto out_reset;

        if (child_inode.type == PROSENJITFS_INODE_DIR) {
            uint64_t inode_lba;
            uint32_t inode_offset;
            if (pfs_inode_block_location(data->fs, entry.inode,
                                         &inode_lba, &inode_offset) !=
                PROSENJITFS_OK ||
                pfs_read_block(data->fs->mount.device, inode_lba, inode_block) !=
                PROSENJITFS_OK)
                goto out_reset;
            child_inode.parent_inode = new_data->inode_number;
            pfs_copy(inode_block + inode_offset, &child_inode,
                     sizeof(child_inode));
            pfs_put32(inode_block + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(
                          inode_block + inode_offset,
                          sizeof(child_inode),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_inode, checksum)));
            if (pfs_tx_add(data->fs, inode_lba, inode_block) !=
                PROSENJITFS_OK)
                goto out_reset;
        }
    }

    if (data->inode_number == new_data->inode_number &&
        old_bucket == new_bucket) {
        entry.name_hash = pfs_name_hash((const uint8_t *)new_name, new_length);
        entry.name_length = (uint16_t)new_length;
        pfs_zero(entry.name, sizeof(entry.name));
        pfs_copy(entry.name, new_name, new_length);
        pfs_zero(entry_block, data->fs->sb.block_size);
        pfs_copy(entry_block, &entry, sizeof(entry));
        pfs_put32(entry_block + offsetof(struct pros_prosenjitfs_dir_entry,
                                         checksum),
                  pfs_crc_with_zero32(
                      entry_block, sizeof(entry),
                      (uint32_t)offsetof(
                          struct pros_prosenjitfs_dir_entry, checksum)));
        if (pfs_tx_add(data->fs, target_lba, entry_block) != PROSENJITFS_OK)
            goto out_reset;
    }

    rc = pfs_tx_commit(data->fs);
    if (rc == PROSENJITFS_OK)
        rc = PROS_VFS_OK;
    goto out;

out_reset:
    pfs_tx_reset(data->fs);
out:
    if (dir_block) kfree(dir_block);
    if (new_dir_block) kfree(new_dir_block);
    if (prev_block) kfree(prev_block);
    if (entry_block) kfree(entry_block);
    if (inode_block) kfree(inode_block);
    return rc;
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

static void pfs_vnode_release(struct pros_vfs_vnode *vnode)
{
    if (!vnode)
        return;
    if (vnode->private_data)
        kfree(vnode->private_data);
    vnode->private_data = 0;
    kfree(vnode);
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
        if (pfs_dir_entry_by_ordinal(data->fs, &dir_inode, ordinal, &disk_entry) !=
            PROSENJITFS_OK)
            return PROS_VFS_ENOENT;
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
    if (!fs || !image || target >= fs->sb.total_blocks ||
        fs->tx.count >= PROSENJITFS_MAX_TX_BLOCKS)
        return PROSENJITFS_EINVAL;
    for (uint32_t i = 0; i < fs->tx.count; ++i)
        if (fs->tx.targets[i] == target) {
            pfs_copy(fs->tx.images + (uint64_t)i * fs->sb.block_size,
                     image, fs->sb.block_size);
            return PROSENJITFS_OK;
        }
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
    uint8_t *header = fs->scratch;
    uint8_t *images = fs->tx.images;
    uint8_t *commit = images + (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                                  fs->sb.block_size;
    struct pros_prosenjitfs_journal_header h;
    struct pros_prosenjitfs_journal_commit c;
    uint32_t expected;

    if (pfs_read_block(fs->mount.device, fs->sb.journal_start, header) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_copy(&h, header, sizeof(h));
    if (h.magic == 0u)
        return PROSENJITFS_OK;
    if (h.magic != PROSENJITFS_JOURNAL_MAGIC ||
        h.record_count == 0u || h.record_count > PROSENJITFS_MAX_TX_BLOCKS ||
        h.record_count + 2u > fs->sb.journal_blocks ||
        pfs_crc_with_zero32(header, fs->sb.block_size,
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_journal_header,
                                checksum)) != h.checksum)
        return PROSENJITFS_ECORRUPT;

    for (uint32_t i = 0; i < h.record_count; ++i) {
        if (h.targets[i] >= fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_read_block(fs->mount.device,
                           fs->sb.journal_start + 1u + i,
                           images + (uint64_t)i * fs->sb.block_size) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
    }
    if (pfs_read_block(fs->mount.device,
                       fs->sb.journal_start + 1u + h.record_count,
                       commit) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    pfs_copy(&c, commit, sizeof(c));
    if (c.magic != PROSENJITFS_JOURNAL_COMMIT ||
        c.transaction_id != h.transaction_id ||
        c.record_count != h.record_count)
        return pfs_journal_clear(fs);
    expected = pfs_journal_checksum(header, images, fs->sb.block_size,
                                    h.record_count, commit);
    if (expected != c.checksum)
        return PROSENJITFS_ECORRUPT;

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

    for (uint32_t i = 0; i < fs->tx.count; ++i) {
        uint8_t *tx_image =
            fs->tx.images + (uint64_t)i * fs->sb.block_size;
        if (pfs_diag.enabled && !pfs_diag.found &&
            fs->sb.block_size > 188u &&
            fs->tx.targets[i] >= fs->sb.data_start &&
            tx_image[188u] == 0xD6u) {
            pfs_diag.found = 1u;
            pfs_diag.tx_byte = tx_image[188u];
            pfs_diag.physical_lba = fs->tx.targets[i];
            pfs_diag.enabled = 0u;
        }
        if (pfs_write_block(fs->mount.device, fs->tx.targets[i], tx_image) !=
            PROSENJITFS_OK)
            return PROSENJITFS_EIO;
    }
    if (pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    if (pfs_diag.found) {
        uint8_t verify[4096];
        if (pfs_read_block(fs->mount.device, pfs_diag.physical_lba,
                           verify) != PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        pfs_diag.post_commit_byte = verify[188u];
        if (pfs_diag.post_commit_byte != 0xD6u)
            return PROSENJITFS_EIO;
    }

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

        if (pfs_read_block(fs->mount.device, lba, image) != PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        if (pfs_tx_add(fs, lba, image) != PROSENJITFS_OK)
            return PROSENJITFS_EOVERFLOW;

        slot = 0u;
        while (slot < fs->tx.count && fs->tx.targets[slot] != lba)
            ++slot;
        if (slot == fs->tx.count)
            return PROSENJITFS_EINVAL;

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

static int pfs_allocator_update(struct pros_prosenjitfs *fs,
                                uint64_t first, uint32_t count,
                                uint8_t allocate)
{
    if (!fs || fs->read_only || pfs_tx_reset(fs) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    /* Staging must complete before commit; callers never receive a partially
     * applied allocator transaction when the journal capacity is exceeded. */
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
    if (block_device_init(&device, 512u, 512u, pfs_test_read,
                          pfs_test_write, &disk) != PROS_BLOCK_OK)
        return -1;
    if (block_device_set_flush(&device, pfs_test_flush) != PROS_BLOCK_OK)
        return -2;
    pfs_zero(&disk, sizeof(disk));
    if (prosenjitfs_format(&device) != PROSENJITFS_OK)
        return -3;
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return -4;
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
        if (vfs_path_lookup(&fs.mount, "/", &root) != PROS_VFS_OK ||
            root != fs.mount.root ||
            vfs_vnode_put(root) != PROS_VFS_OK ||
            vfs_dir_open(&fs.mount, "/", &dir) != PROS_VFS_OK ||
            vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
            entry.name[0] != '.' ||
            vfs_dir_close(&dir) != PROS_VFS_OK)
            return -5;
    }
    {
        struct pros_vfs_file file;
        uint8_t write_data[700];
        uint8_t read_data[700];
        uint32_t done = 0u;
        for (uint32_t i = 0u; i < sizeof(write_data); ++i)
            write_data[i] = (uint8_t)((i * 13u) ^ 0x5Au);

        prosenjitfs_write_diag_reset();
        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, write_data, sizeof(write_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(write_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return -6;

        if (!pfs_diag.found)
            return -0x20000;
        if (pfs_diag.tx_byte != 0xD6u)
            return -0x21000 - (int)pfs_diag.tx_byte;
        if (pfs_diag.post_commit_byte != 0xD6u)
            return -0x22000 - (int)pfs_diag.post_commit_byte;

        if (prosenjitfs_read_inode(&fs, 1u, &inode) != PROSENJITFS_OK)
            return -0x21000;
        {
            struct pros_prosenjitfs_extent diag_extent;
            uint64_t diag_extent_lba = 0u, diag_tail_lba = 0u;
            uint8_t direct_block[4096];
            int diag_rc = pfs_find_extent(&fs, &inode, 0u, &diag_extent,
                                          &diag_extent_lba, &diag_tail_lba);
            if (diag_rc != PROSENJITFS_OK ||
                diag_extent_lba == 0u ||
                diag_extent.physical_block != pfs_diag.physical_lba)
                return -0x21000;
            if (pfs_read_block(&device, pfs_diag.physical_lba,
                               direct_block) != PROSENJITFS_OK)
                return -0x22000;
            pfs_diag.direct_byte = direct_block[188u];
            if (pfs_diag.direct_byte != 0xD6u)
                return -0x22000 - (int)pfs_diag.direct_byte;
        }

        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data))
            return -6;
        pfs_diag.vfs_byte = read_data[188u];
        if (pfs_diag.vfs_byte != 0xD6u)
            return -0x23000 - (int)pfs_diag.vfs_byte;
        for (uint32_t i = 0u; i < sizeof(read_data); ++i)
            if (read_data[i] != write_data[i])
                return -0x24000 - (int)i;
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return -6;
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

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return -12;
    {
        struct pros_vfs_file file;
        uint8_t read_data[700];
        uint32_t done = 0u;
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK) {
            prosenjitfs_unmount(&fs);
            return -12;
        }
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE, &file) !=
                PROS_VFS_OK)
            return -12;
        {
            uint8_t patch[37];
            uint32_t wrote = 0u;
            for (uint32_t i = 0u; i < sizeof(patch); ++i)
                patch[i] = (uint8_t)(0xA0u + i);
            if (vfs_file_write(&file, patch, sizeof(patch), &wrote) !=
                    PROS_VFS_OK ||
                wrote != sizeof(patch) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return -12;
        }


        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return -12;

        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            uint8_t expected = i < 37u
                ? (uint8_t)(0xA0u + i)
                : (uint8_t)((i * 13u) ^ 0x5Au);
            if (read_data[i] != expected) {
                prosenjitfs_unmount(&fs);
                return -12;
            }
        }

        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
            vfs_file_truncate(&file, 100u) != PROS_VFS_OK ||
            file.vnode->size != 100u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return -12;

        if (prosenjitfs_read_inode(&fs, 1u, &inode) != PROSENJITFS_OK ||
            inode.size != 100u ||
            inode.allocated_bytes != fs.sb.block_size ||
            inode.extent_head == 0u)
            return -12;
    }
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -12;

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return -12;
    {
        struct pros_vfs_file file;
        uint8_t read_data[100];
        uint32_t done = 0u;
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return -12;
        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            uint8_t expected = i < 37u
                ? (uint8_t)(0xA0u + i)
                : (uint8_t)((i * 13u) ^ 0x5Au);
            if (read_data[i] != expected) {
                prosenjitfs_unmount(&fs);
                return -12;
            }
        }
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE, &file) !=
                PROS_VFS_OK ||
            vfs_file_truncate(&file, 0u) != PROS_VFS_OK ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return -12;
    }
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -12;
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return -12;
    {
        struct pros_vfs_file file;
        uint8_t read_data[1];
        uint32_t done = 99u;
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != 0u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return -12;
    }
    {
        struct pros_prosenjitfs_inode before;
        struct pros_prosenjitfs_inode after;
        struct pros_vfs_vnode *hidden = 0;

        if (prosenjitfs_read_inode(&fs, 1u, &before) != PROSENJITFS_OK ||
            vfs_rename(&fs.mount, "/hello.bin", "/.hello.bin") !=
                PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/hello.bin", &hidden) !=
                PROS_VFS_ENOENT ||
            hidden != 0)
            return -12;
        if (vfs_path_lookup(&fs.mount, "/.hello.bin", &hidden) !=
                PROS_VFS_OK ||
            !hidden ||
            vfs_vnode_put(hidden) != PROS_VFS_OK ||
            prosenjitfs_read_inode(&fs, 1u, &after) != PROSENJITFS_OK ||
            after.type != before.type ||
            after.mode != before.mode ||
            after.uid != before.uid ||
            after.gid != before.gid ||
            after.links != before.links ||
            after.generation != before.generation ||
            after.size != before.size ||
            after.allocated_bytes != before.allocated_bytes ||
            after.parent_inode != before.parent_inode ||
            after.extent_head != before.extent_head ||
            after.dir_index != before.dir_index ||
            after.atime != before.atime ||
            after.mtime != before.mtime ||
            after.ctime != before.ctime)
            return -12;

        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_path_lookup(&fs.mount, "/hello.bin", &hidden) !=
                PROS_VFS_ENOENT ||
            hidden != 0 ||
            vfs_path_lookup(&fs.mount, "/.hello.bin", &hidden) !=
                PROS_VFS_OK ||
            !hidden ||
            vfs_vnode_put(hidden) != PROS_VFS_OK ||
            vfs_rename(&fs.mount, "/.hello.bin", "/hello.bin") !=
                PROS_VFS_OK)
            return -12;
    }
    /*
     * Directory mutation regression:
     *  - mkdir allocates both inode and directory-index storage.
     *  - file creation works below the new directory.
     *  - cross-directory rename preserves the inode/data.
     *  - directory moves update parent_inode and reject cycles.
     *  - rmdir rejects non-empty directories and reclaims empty directories.
     */
    if (vfs_mkdir(&fs.mount, "/dir1") != PROS_VFS_OK ||
        vfs_mkdir(&fs.mount, "/dir2") != PROS_VFS_OK ||
        vfs_mkdir(&fs.mount, "/dir1/subdir") != PROS_VFS_OK)
        return -12;
    {
        struct pros_vfs_file moved_file;
        struct pros_vfs_vnode *hidden = 0;
        uint8_t marker = 0x5Au, verify = 0u;
        uint32_t done = 0u;
        if (vfs_file_open(&fs.mount, "/dir1/moved.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &moved_file) != PROS_VFS_OK ||
            vfs_file_write(&moved_file, &marker, 1u, &done) != PROS_VFS_OK ||
            done != 1u ||
            vfs_file_close(&moved_file) != PROS_VFS_OK)
            return -12;
        if (vfs_rename(&fs.mount, "/dir1/moved.bin", "/dir2/moved.bin") !=
                PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/dir1/moved.bin", &hidden) !=
                PROS_VFS_ENOENT)
            return -12;
        if (hidden)
            vfs_vnode_put(hidden);
        if (vfs_file_open(&fs.mount, "/dir2/moved.bin",
                          PROS_VFS_O_READ, &moved_file) != PROS_VFS_OK ||
            vfs_file_read(&moved_file, &verify, 1u, &done) != PROS_VFS_OK ||
            done != 1u || verify != marker ||
            vfs_file_close(&moved_file) != PROS_VFS_OK)
            return -12;
    }
    if (vfs_rename(&fs.mount, "/dir1/subdir", "/dir2/subdir") !=
            PROS_VFS_OK)
        return -12;
    {
        struct pros_vfs_vnode *moved_dir = 0;
        struct pros_vfs_vnode *parent_dir = 0;
        struct pfs_vnode_data *moved_data;
        struct pfs_vnode_data *parent_data;
        struct pros_prosenjitfs_inode moved_inode;
        if (vfs_path_lookup(&fs.mount, "/dir2/subdir", &moved_dir) !=
                PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/dir2", &parent_dir) !=
                PROS_VFS_OK || !moved_dir || !parent_dir)
            return -12;
        moved_data = (struct pfs_vnode_data *)moved_dir->private_data;
        parent_data = (struct pfs_vnode_data *)parent_dir->private_data;
        if (!moved_data || !parent_data ||
            prosenjitfs_read_inode(&fs, moved_data->inode_number, &moved_inode) !=
                PROSENJITFS_OK ||
            moved_inode.parent_inode != parent_data->inode_number) {
            vfs_vnode_put(moved_dir);
            vfs_vnode_put(parent_dir);
            return -12;
        }
        vfs_vnode_put(moved_dir);
        vfs_vnode_put(parent_dir);
    }
    if (vfs_rename(&fs.mount, "/dir2", "/dir2/subdir/loop") !=
            PROS_VFS_EINVAL ||
        vfs_rmdir(&fs.mount, "/dir2") != PROS_VFS_ENOTEMPTY ||
        vfs_unlink(&fs.mount, "/dir2/moved.bin") != PROS_VFS_OK ||
        vfs_rmdir(&fs.mount, "/dir2/subdir") != PROS_VFS_OK ||
        vfs_rmdir(&fs.mount, "/dir1") != PROS_VFS_OK ||
        vfs_rmdir(&fs.mount, "/dir2") != PROS_VFS_OK)
        return -12;

    if (vfs_unlink(&fs.mount, "/hello.bin") != PROS_VFS_OK)
        return -12;
    {
        struct pros_vfs_vnode *deleted = 0;
        if (vfs_path_lookup(&fs.mount, "/hello.bin", &deleted) !=
                PROS_VFS_ENOENT)
            return -12;
        if (deleted)
            vfs_vnode_put(deleted);
    }
    if (prosenjitfs_read_inode(&fs, 1u, &inode) != PROSENJITFS_ENOENT)
        return -12;
    if (prosenjitfs_alloc_inode(&fs, &inode_number) != PROSENJITFS_OK ||
        inode_number != 1u)
        return -12;
    if (pfs_inode_bitmap_update(&fs, inode_number, 0u) != PROSENJITFS_OK)
        return -12;
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -12;
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return -12;
    if (prosenjitfs_read_inode(&fs, 1u, &inode) != PROSENJITFS_ENOENT)
        return -12;
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -12;

    /*
     * Journal recovery regression:
     *  1. A fully committed redo record must replay on the next mount.
     *  2. A header/images without a commit record must be discarded.
     *  3. A committed record with a bad checksum must fail closed.
     */
    {
        uint64_t recovery_block;
        uint64_t journal_start = fs.sb.journal_start;
        uint32_t journal_blocks = fs.sb.journal_blocks;
        uint8_t header[512], image[512], commit[512], verify[512];
        struct pros_prosenjitfs_journal_header h;
        struct pros_prosenjitfs_journal_commit jc;
        uint32_t expected;

        if (fs.sb.block_size != 512u ||
            prosenjitfs_alloc_blocks(&fs, 1u, &recovery_block) != PROSENJITFS_OK)
            return -12;

        pfs_zero(header, sizeof(header));
        pfs_zero(image, sizeof(image));
        pfs_zero(commit, sizeof(commit));
        for (uint32_t i = 0u; i < sizeof(image); ++i)
            image[i] = (uint8_t)(0xC3u ^ (uint8_t)i);

        pfs_zero(&h, sizeof(h));
        h.magic = PROSENJITFS_JOURNAL_MAGIC;
        h.record_count = 1u;
        h.transaction_id = 0x1122334455667788ULL;
        h.targets[0] = recovery_block;
        pfs_copy(header, &h, sizeof(h));
        h.checksum = pfs_crc_with_zero32(header, sizeof(header),
            (uint32_t)offsetof(struct pros_prosenjitfs_journal_header, checksum));
        pfs_copy(header, &h, sizeof(h));

        pfs_zero(&jc, sizeof(jc));
        jc.magic = PROSENJITFS_JOURNAL_COMMIT;
        jc.record_count = 1u;
        jc.transaction_id = h.transaction_id;
        pfs_copy(commit, &jc, sizeof(jc));
        expected = pfs_journal_checksum(header, image, 512u, 1u, commit);
        jc.checksum = expected;
        pfs_copy(commit, &jc, sizeof(jc));

        if (pfs_write_block(&device, journal_start, header) != PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 1u, image) != PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 2u, commit) != PROSENJITFS_OK ||
            pfs_flush(&device) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            pfs_read_block(&device, recovery_block, verify) != PROSENJITFS_OK)
            return -12;
        for (uint32_t i = 0u; i < sizeof(verify); ++i)
            if (verify[i] != image[i])
                return -12;

        /* Torn transaction: header/images without a commit must not apply. */
        pfs_zero(header, sizeof(header));
        pfs_zero(&h, sizeof(h));
        h.magic = PROSENJITFS_JOURNAL_MAGIC;
        h.record_count = 1u;
        h.transaction_id = 0x8877665544332211ULL;
        h.targets[0] = recovery_block;
        pfs_copy(header, &h, sizeof(h));
        h.checksum = pfs_crc_with_zero32(header, sizeof(header),
            (uint32_t)offsetof(struct pros_prosenjitfs_journal_header, checksum));
        pfs_copy(header, &h, sizeof(h));
        for (uint32_t i = 0u; i < sizeof(image); ++i)
            image[i] = (uint8_t)(0x5Au ^ (uint8_t)i);
        if (pfs_write_block(&device, journal_start, header) != PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 1u, image) != PROSENJITFS_OK ||
            pfs_flush(&device) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            pfs_read_block(&device, recovery_block, verify) != PROSENJITFS_OK)
            return -12;
        for (uint32_t i = 0u; i < sizeof(verify); ++i)
            if (verify[i] != (uint8_t)(0xC3u ^ (uint8_t)i))
                return -12;

        /* Corrupt commit checksum must fail closed. */
        pfs_zero(header, sizeof(header));
        pfs_zero(commit, sizeof(commit));
        pfs_zero(&h, sizeof(h));
        h.magic = PROSENJITFS_JOURNAL_MAGIC;
        h.record_count = 1u;
        h.transaction_id = 0xCAFEBABE12345678ULL;
        h.targets[0] = recovery_block;
        pfs_copy(header, &h, sizeof(h));
        h.checksum = pfs_crc_with_zero32(header, sizeof(header),
            (uint32_t)offsetof(struct pros_prosenjitfs_journal_header, checksum));
        pfs_copy(header, &h, sizeof(h));
        for (uint32_t i = 0u; i < sizeof(image); ++i)
            image[i] = (uint8_t)(0xE1u ^ (uint8_t)i);
        pfs_zero(&jc, sizeof(jc));
        jc.magic = PROSENJITFS_JOURNAL_COMMIT;
        jc.record_count = 1u;
        jc.transaction_id = h.transaction_id;
        pfs_copy(commit, &jc, sizeof(jc));
        jc.checksum = pfs_journal_checksum(header, image, 512u, 1u, commit) ^ 1u;
        pfs_copy(commit, &jc, sizeof(jc));
        if (pfs_write_block(&device, journal_start, header) != PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 1u, image) != PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 2u, commit) != PROSENJITFS_OK ||
            pfs_flush(&device) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_ECORRUPT)
            return -12;

        pfs_zero(header, sizeof(header));
        for (uint32_t i = 0u; i < journal_blocks; ++i)
            if (pfs_write_block(&device, journal_start + i, header) != PROSENJITFS_OK)
                return -12;
        if (pfs_flush(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            pfs_read_block(&device, recovery_block, verify) != PROSENJITFS_OK)
            return -12;
        for (uint32_t i = 0u; i < sizeof(verify); ++i)
            if (verify[i] != (uint8_t)(0xC3u ^ (uint8_t)i))
                return -12;
        if (prosenjitfs_free_blocks(&fs, recovery_block, 1u) != PROSENJITFS_OK)
            return -12;
    }
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -12;

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