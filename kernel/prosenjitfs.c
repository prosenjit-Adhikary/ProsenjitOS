#include "prosenjitfs.h"
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

static int pfs_allocator_update(struct pros_prosenjitfs *fs,
                                uint64_t first, uint32_t count,
                                uint8_t allocate)
{
    uint64_t bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    uint64_t last = first + count - 1u;
    uint64_t first_bitmap = first / bits_per_block;
    uint64_t last_bitmap = last / bits_per_block;
    if (last < first || last_bitmap - first_bitmap + 1u > PROSENJITFS_MAX_TX_BLOCKS)
        return PROSENJITFS_EOVERFLOW;

    if (pfs_tx_reset(fs) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;

    for (uint64_t bi = first_bitmap; bi <= last_bitmap; ++bi) {
        uint8_t *image = fs->tx.images +
                         (uint64_t)fs->tx.count * fs->sb.block_size;
        uint64_t lba = fs->sb.block_bitmap_start + bi;
        if (pfs_read_block(fs->mount.device, lba, image) != PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        if (pfs_tx_add(fs, lba, image) != PROSENJITFS_OK)
            return PROSENJITFS_EOVERFLOW;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t bit = first + i;
        uint64_t bi = bit / bits_per_block;
        uint64_t within = bit % bits_per_block;
        uint32_t slot = 0u;
        while (slot < fs->tx.count &&
               fs->tx.targets[slot] != fs->sb.block_bitmap_start + bi)
            ++slot;
        if (slot == fs->tx.count)
            return PROSENJITFS_EINVAL;
        uint8_t *image = fs->tx.images +
                         (uint64_t)slot * fs->sb.block_size;
        uint8_t mask = (uint8_t)(1u << (within & 7u));
        uint64_t byte = within >> 3;
        if (allocate) image[byte] |= mask;
        else image[byte] &= (uint8_t)~mask;
    }
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
    for (uint64_t b = data_start; b < fs->sb.total_blocks; ++b) {
        uint64_t bits_per_block = (uint64_t)fs->sb.block_size * 8u;
        uint64_t bitmap_lba = fs->sb.block_bitmap_start + b / bits_per_block;
        uint64_t byte = (b % bits_per_block) >> 3;
        uint8_t bitbuf[4096];
        if (pfs_read_block(fs->mount.device, bitmap_lba, bitbuf) != PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        if (!pfs_bitmap_bit(bitbuf, (b % bits_per_block))) {
            ++run;
            if (run == count) {
                *first_block = b + 1u - count;
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
    if (!fs || count == 0u || fs->read_only ||
        first_block > UINT64_MAX - count)
        return PROSENJITFS_EINVAL;
    data_start = pfs_data_start(&fs->sb);
    if (first_block < data_start ||
        (uint64_t)count > fs->sb.total_blocks - first_block)
        return PROSENJITFS_EINVAL;
    return pfs_allocator_update(fs, first_block, count, 0u);
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
    uint64_t first;
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
        inode.dir_index == 0u)
        return -5;
    if (prosenjitfs_alloc_blocks(&fs, 4u, &first) != PROSENJITFS_OK)
        return -6;
    if (first < pfs_data_start(&fs.sb) ||
        prosenjitfs_free_blocks(&fs, first, 4u) != PROSENJITFS_OK)
        return -7;
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -8;

    saved = disk.data[0];
    disk.data[0] ^= 0x01u;
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_ECORRUPT)
        return -9;
    disk.data[0] = saved;

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return -10;
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return -11;
    return PROSENJITFS_OK;
}
