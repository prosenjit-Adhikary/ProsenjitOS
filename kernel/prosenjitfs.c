/* Native ProsenjitFS: all on-disk address arithmetic must remain checked. */
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
static int pfs_inode_block_location(const struct pros_prosenjitfs *, uint64_t,
                                      uint64_t *, uint32_t *);

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
 * read, the extent-mapped physical LBA, and the later VFS read byte. The read
 * phase also records the byte observed immediately after the physical block
 * read, so a mismatch can be localized below or above the VFS buffer.
 */
struct pfs_write_diag {
    uint8_t enabled;
    uint8_t found;
    uint8_t read_enabled;
    uint8_t read_found;
    uint8_t verify_pending;
    uint8_t tx_byte;
    uint8_t post_commit_byte;
    uint8_t direct_byte;
    uint8_t vfs_byte;
    uint8_t read_block_byte;
    uint8_t read_block_found;
    uint64_t physical_lba;
    uint64_t read_physical_lba;
};

static struct pfs_write_diag pfs_diag;

static int pfs_vfs_error(int rc)
{
    switch (rc) {
    case PROSENJITFS_OK:
        return PROS_VFS_OK;
    case PROSENJITFS_EINVAL:
        return PROS_VFS_EINVAL;
    case PROSENJITFS_EIO:
        return PROS_VFS_EIO;
    case PROSENJITFS_ENOTSUP:
        return PROS_VFS_ENOTSUP;
    case PROSENJITFS_ECORRUPT:
        return PROS_VFS_ECORRUPT;
    case PROSENJITFS_ENOSPC:
        return PROS_VFS_ENOSPC;
    case PROSENJITFS_ENOENT:
        return PROS_VFS_ENOENT;
    case PROSENJITFS_EEXIST:
        return PROS_VFS_EEXIST;
    case PROSENJITFS_EOVERFLOW:
        return PROS_VFS_EOVERFLOW;
    case PROSENJITFS_ENOTDIR:
        return PROS_VFS_ENOTDIR;
    case PROSENJITFS_ENOTEMPTY:
        return PROS_VFS_ENOTEMPTY;
    case PROSENJITFS_EPERM:
        return PROS_VFS_EPERM;
    case PROSENJITFS_EBUSY:
        return PROS_VFS_EBUSY;
    case PROSENJITFS_ESTALE:
        return PROS_VFS_ESTALE;
    default:
        return PROS_VFS_EIO;
    }
}


/* Temporary mkdir regression trace: stage identifies the first failing operation. */
static uint32_t pfs_mkdir_diag_stage;
static int pfs_mkdir_diag_rc;
static uint32_t pfs_tx_diag_stage;
static uint32_t pfs_tx_diag_count;
static uint32_t pfs_tx_diag_count_before_clear;
static uint32_t pfs_tx_diag_count_after_clear;
static int pfs_tx_diag_clear_rc;

/*
 * Deterministic crash injection used only by the native ProsenjitFS
 * self-test.  The injection happens after the named transaction stage has
 * reached the real block-device path; the self-test then discards volatile
 * device writes and reconstructs the filesystem object, modelling loss of
 * process/CPU state plus device-cache contents without calling unmount().
 */
static uint32_t pfs_crash_inject_stage;
static uint32_t pfs_crash_injected;

static int pfs_tx_crash_inject(uint32_t stage)
{
    if (pfs_crash_inject_stage != stage)
        return PROSENJITFS_OK;
    pfs_crash_injected = 1u;
    return PROSENJITFS_EIO;
}


void prosenjitfs_write_diag_reset(void)
{
    pfs_zero(&pfs_diag, sizeof(pfs_diag));
    pfs_diag.enabled = 1u;
    pfs_mkdir_diag_stage = 0u;
    pfs_mkdir_diag_rc = PROSENJITFS_OK;
    pfs_tx_diag_stage = 0u;
    pfs_tx_diag_count = 0u;
    pfs_tx_diag_count_before_clear = 0u;
    pfs_tx_diag_count_after_clear = 0u;
    pfs_tx_diag_clear_rc = PROSENJITFS_OK;
}

int prosenjitfs_mkdir_diag_get(uint32_t *mkdir_stage, int *mkdir_rc,
                               uint32_t *tx_stage, uint32_t *tx_count)
{
    if (!mkdir_stage || !mkdir_rc || !tx_stage || !tx_count)
        return PROSENJITFS_EINVAL;
    *mkdir_stage = pfs_mkdir_diag_stage;
    *mkdir_rc = pfs_mkdir_diag_rc;
    *tx_stage = pfs_tx_diag_stage;
    *tx_count = pfs_tx_diag_count;
    return PROSENJITFS_OK;
}

int prosenjitfs_tx_diag_get(uint32_t *stage, uint32_t *count,
                            uint32_t *count_before_clear,
                            uint32_t *count_after_clear, int *clear_rc)
{
    if (!stage || !count || !count_before_clear ||
        !count_after_clear || !clear_rc)
        return PROSENJITFS_EINVAL;
    *stage = pfs_tx_diag_stage;
    *count = pfs_tx_diag_count;
    *count_before_clear = pfs_tx_diag_count_before_clear;
    *count_after_clear = pfs_tx_diag_count_after_clear;
    *clear_rc = pfs_tx_diag_clear_rc;
    return PROSENJITFS_OK;
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

int prosenjitfs_write_diag_get_read_byte(uint8_t *read_block_byte)
{
    if (!read_block_byte)
        return PROSENJITFS_EINVAL;
    if (!pfs_diag.read_block_found)
        return PROSENJITFS_ENOENT;
    *read_block_byte = pfs_diag.read_block_byte;
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

static uint32_t pfs_region_end(uint64_t start, uint64_t blocks,
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

static int pfs_lba_add(struct pros_block_device *device,
                         uint64_t base, uint64_t offset, uint64_t *result)
{
    if (!device || !result || base >= device->sector_count)
        return PROSENJITFS_EINVAL;
    if (offset > UINT64_MAX - base)
        return PROSENJITFS_EOVERFLOW;
    if (base + offset >= device->sector_count)
        return PROSENJITFS_EINVAL;
    *result = base + offset;
    return PROSENJITFS_OK;
}
static __attribute__((unused)) int pfs_write_at_offset(struct pros_block_device *device,
                                uint64_t base, uint64_t offset,
                                const void *buffer)
{
    uint64_t lba;
    int rc = pfs_lba_add(device, base, offset, &lba);
    if (rc != PROSENJITFS_OK)
        return rc;
    return pfs_write_block(device, lba, buffer);
}
static int pfs_read_at_offset(struct pros_block_device *device,
                               uint64_t base, uint64_t offset,
                               void *buffer)
{
    uint64_t lba;
    int rc = pfs_lba_add(device, base, offset, &lba);
    if (rc != PROSENJITFS_OK)
        return rc;
    return pfs_read_block(device, lba, buffer);
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
    {
        uint64_t journal_end, bitmap_end, inode_bitmap_end, inode_end;
        uint64_t bits_per_block = (uint64_t)sb->block_size * 8u;
        uint64_t required_block_bitmap_blocks;
        uint64_t required_inode_bitmap_blocks;

        if (pfs_region_end(sb->journal_start, sb->journal_blocks, &journal_end) != PROSENJITFS_OK ||
            pfs_region_end(sb->block_bitmap_start, sb->block_bitmap_blocks, &bitmap_end) != PROSENJITFS_OK ||
            pfs_region_end(sb->inode_bitmap_start, sb->inode_bitmap_blocks, &inode_bitmap_end) != PROSENJITFS_OK ||
            pfs_region_end(sb->inode_table_start, sb->inode_table_blocks, &inode_end) != PROSENJITFS_OK ||
            journal_end > sb->block_bitmap_start ||
            bitmap_end > sb->inode_bitmap_start ||
            inode_bitmap_end > sb->inode_table_start ||
            inode_end == UINT64_MAX ||
            sb->data_start != inode_end + 1u ||
            sb->data_start >= sb->total_blocks)
            return PROSENJITFS_ECORRUPT;

        /*
         * A valid bitmap must be large enough to represent every addressable
         * block/inode. Without these checks a corrupted, but checksummed,
         * superblock could make allocation index into another metadata region.
         */
        required_block_bitmap_blocks =
            sb->total_blocks / bits_per_block +
            ((sb->total_blocks % bits_per_block) != 0u ? 1u : 0u);
        required_inode_bitmap_blocks =
            (uint64_t)sb->inode_count / bits_per_block +
            (((uint64_t)sb->inode_count % bits_per_block) != 0u ? 1u : 0u);
        if ((uint64_t)sb->block_bitmap_blocks < required_block_bitmap_blocks ||
            (uint64_t)sb->inode_bitmap_blocks < required_inode_bitmap_blocks)
            return PROSENJITFS_ECORRUPT;
    }
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
                                   uint64_t region_start, uint64_t region_blocks,
                                   uint32_t bitmap_index, uint32_t block_size)
{
    uint64_t end;
    uint64_t bits_per_block = (uint64_t)block_size * 8u;
    uint64_t bitmap_base;
    if (!bitmap || block_size == 0u || block_size > 4096u ||
        bits_per_block == 0u || bitmap_index > UINT64_MAX / bits_per_block ||
        pfs_region_end(region_start, region_blocks, &end) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    bitmap_base = (uint64_t)bitmap_index * bits_per_block;
    for (uint64_t b = region_start; b < end; ++b) {
        if (bitmap_base > UINT64_MAX - bits_per_block)
            return PROSENJITFS_EOVERFLOW;
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

    {
        uint64_t journal_count = total / 16u;
        uint64_t bits_per_block = (uint64_t)block_size * 8u;
        uint64_t bitmap_count = total / bits_per_block;
        uint64_t inode_count64 = total / 32u;

        if (total % 16u != 0u)
            ++journal_count;
        if (total % bits_per_block != 0u)
            ++bitmap_count;
        if (journal_count > UINT32_MAX || bitmap_count > UINT32_MAX)
            return PROSENJITFS_EOVERFLOW;

        journal_blocks = (uint32_t)journal_count;
        if (journal_blocks < PFS_MIN_JOURNAL) journal_blocks = PFS_MIN_JOURNAL;
        if (journal_blocks > PFS_MAX_JOURNAL) journal_blocks = PFS_MAX_JOURNAL;

        bitmap_blocks = (uint32_t)bitmap_count;
        if (inode_count64 < PFS_MIN_INODES)
            inode_count = PFS_MIN_INODES;
        else if (inode_count64 > PFS_MAX_INODES)
            inode_count = PFS_MAX_INODES;
        else
            inode_count = (uint32_t)inode_count64;
    }
    inode_bitmap_blocks =
        (uint32_t)(((uint64_t)inode_count + block_size * 8u - 1u) /
                   (block_size * 8u));
    inode_table_blocks =
        (uint32_t)(((uint64_t)inode_count * PROSENJITFS_INODE_SIZE +
                    block_size - 1u) / block_size);

    cursor = 2u;
    pfs_zero(&sb, sizeof(sb));
    sb.journal_start = cursor;
    sb.journal_blocks = journal_blocks;
    if ((uint64_t)journal_blocks > total - cursor)
        return PROSENJITFS_ENOSPC;
    cursor += journal_blocks;

    sb.block_bitmap_start = cursor;
    sb.block_bitmap_blocks = bitmap_blocks;
    if ((uint64_t)bitmap_blocks > total - cursor)
        return PROSENJITFS_ENOSPC;
    cursor += bitmap_blocks;

    sb.inode_bitmap_start = cursor;
    sb.inode_bitmap_blocks = inode_bitmap_blocks;
    if ((uint64_t)inode_bitmap_blocks > total - cursor)
        return PROSENJITFS_ENOSPC;
    cursor += inode_bitmap_blocks;

    sb.inode_table_start = cursor;
    sb.inode_table_blocks = inode_table_blocks;
    if ((uint64_t)inode_table_blocks > total - cursor)
        return PROSENJITFS_ENOSPC;
    cursor += inode_table_blocks;

    if (cursor >= total - 1u)
        return PROSENJITFS_ENOSPC;
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

    {
        int rc = pfs_write_block(device, 0u, block);
        if (rc == PROSENJITFS_OK)
            rc = pfs_write_block(device, 1u, block);
        if (rc != PROSENJITFS_OK) {
            kfree(block);
            return rc;
        }
    }

    pfs_zero(block, block_size);
    for (uint32_t i = 0; i < journal_blocks; ++i) {
        uint64_t lba;
        {
            int rc = pfs_lba_add(device, sb.journal_start, i, &lba);
            if (rc == PROSENJITFS_OK)
                rc = pfs_write_block(device, lba, block);
            if (rc != PROSENJITFS_OK) {
                kfree(block);
                return rc;
            }
        }
    }

    for (uint32_t i = 0; i < bitmap_blocks; ++i) {
        pfs_zero(block, block_size);
        if (pfs_bitmap_region_build(block, block_size,
                                    0u, sb.data_start + 1u, i, block_size) != PROSENJITFS_OK) {
            kfree(block);
            return PROSENJITFS_EOVERFLOW;
        }
        {
            uint64_t lba;
            {
                int rc = pfs_lba_add(device, sb.block_bitmap_start, i, &lba);
                if (rc == PROSENJITFS_OK)
                    rc = pfs_write_block(device, lba, block);
                if (rc != PROSENJITFS_OK) {
                    kfree(block);
                    return rc;
                }
            }
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
        {
            uint64_t lba;
            {
                int rc = pfs_lba_add(device, sb.inode_bitmap_start, i, &lba);
                if (rc == PROSENJITFS_OK)
                    rc = pfs_write_block(device, lba, block);
                if (rc != PROSENJITFS_OK) {
                    kfree(block);
                    return rc;
                }
            }
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
    uint64_t generation;
};

static void pfs_io_lock(struct pros_prosenjitfs *fs)
{
    if (!fs)
        return;
    while (__sync_lock_test_and_set(&fs->io_lock, 1u) != 0u) {
    }
    __sync_synchronize();
}

static void pfs_io_unlock(struct pros_prosenjitfs *fs)
{
    if (!fs)
        return;
    __sync_synchronize();
    __sync_lock_release(&fs->io_lock);
}

static int pfs_vnode_validate_inode(struct pfs_vnode_data *data,
                                    struct pros_prosenjitfs_inode *inode)
{
    int rc;
    if (!data || !data->fs || !inode || !data->fs->mount.active)
        return PROSENJITFS_EPERM;
    rc = prosenjitfs_read_inode(data->fs, data->inode_number, inode);
    if (rc != PROSENJITFS_OK)
        return rc;
    if (inode->generation != data->generation ||
        (inode->type != PROSENJITFS_INODE_FILE &&
         inode->type != PROSENJITFS_INODE_DIR))
        return PROSENJITFS_ESTALE;
    return PROSENJITFS_OK;
}

static int pfs_next_generation(uint64_t current, uint64_t *next)
{
    if (!next || current == UINT64_MAX)
        return PROSENJITFS_EOVERFLOW;
    *next = current + 1u;
    return *next == 0u ? PROSENJITFS_EOVERFLOW : PROSENJITFS_OK;
}

static int pfs_allocate_generation(struct pros_prosenjitfs *fs,
                                   uint64_t inode_number,
                                   uint64_t *generation)
{
    uint8_t block[4096];
    uint64_t lba;
    uint32_t offset;
    struct pros_prosenjitfs_inode old_inode;
    int rc;

    if (!fs || !generation || fs->sb.block_size > sizeof(block))
        return PROSENJITFS_EINVAL;
    rc = pfs_inode_block_location(fs, inode_number, &lba, &offset);
    if (rc != PROSENJITFS_OK)
        return rc;
    rc = pfs_read_block(fs->mount.device, lba, block);
    if (rc != PROSENJITFS_OK)
        return rc;
    pfs_copy(&old_inode, block + offset, sizeof(old_inode));
    if (old_inode.type != PROSENJITFS_INODE_FREE &&
        old_inode.type != PROSENJITFS_INODE_FILE &&
        old_inode.type != PROSENJITFS_INODE_DIR)
        return PROSENJITFS_ECORRUPT;
    if (old_inode.type == PROSENJITFS_INODE_FREE &&
        old_inode.checksum != pfs_crc_with_zero32(
            block + offset, sizeof(old_inode),
            (uint32_t)offsetof(struct pros_prosenjitfs_inode, checksum)))
        return PROSENJITFS_ECORRUPT;
    return pfs_next_generation(old_inode.generation, generation);
}

static int pfs_vnode_lookup(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result);
static int pfs_vnode_unlink(struct pros_vfs_vnode *, const char *);
static int pfs_vnode_rename(struct pros_vfs_vnode *, const char *,
                            struct pros_vfs_vnode *, const char *);
static int __attribute__((unused)) pfs_find_dir_entry_lba(const struct pros_prosenjitfs *,
                                   const struct pros_prosenjitfs_inode *,
                                   const char *,
                                   struct pros_prosenjitfs_dir_entry *,
                                   uint64_t *,
                                   uint64_t *);
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
    {
        int rc = pfs_read_block(fs->mount.device, lba, block);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
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
    uint64_t previous_logical_end = 0u;
    struct pros_prosenjitfs_extent extent;

    if (!fs || !inode || !found_lba || !tail_lba)
        return PROSENJITFS_EINVAL;
    current = inode->extent_head;
    *found_lba = 0u;
    *tail_lba = 0u;
    while (current != 0u) {
        if (++steps > fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        {
            int rc = pfs_read_extent(fs, current, &extent);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
        *tail_lba = current;
        /* Extent metadata must not overlap its own data range. Physical
         * blocks may legitimately move backwards when freed blocks are reused;
         * only logical extent order is a chain invariant. */
        if (current >= extent.physical_block &&
            current - extent.physical_block < extent.block_count)
            return PROSENJITFS_ECORRUPT;
        if (extent.logical_block < previous_logical_end ||
            extent.logical_block > UINT64_MAX - extent.block_count ||
            extent.physical_block > UINT64_MAX - extent.block_count)
            return PROSENJITFS_ECORRUPT;
        previous_logical_end = extent.logical_block + extent.block_count;
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
            uint64_t bitmap_lba;
            uint8_t bitmap[4096];
            {
                int rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                                     b / bits_per_block, &bitmap_lba);
                if (rc != PROSENJITFS_OK)
                    return rc;
                rc = pfs_read_block(fs->mount.device, bitmap_lba, bitmap);
                if (rc != PROSENJITFS_OK)
                    return rc;
            }
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
    if (inode_number > UINT64_MAX / PROSENJITFS_INODE_SIZE)
        return PROSENJITFS_EOVERFLOW;
    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    {
        int rc = pfs_lba_add(fs->mount.device, fs->sb.inode_table_start,
                             byte_offset / fs->sb.block_size, block_lba);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    *offset = (uint32_t)(byte_offset % fs->sb.block_size);
    if ((uint64_t)*offset + sizeof(struct pros_prosenjitfs_inode) >
        fs->sb.block_size)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static int pfs_file_write_error(uint32_t done, int rc)
{
    if (done != 0u)
        return (int)done;
    switch (rc) {
    case PROSENJITFS_ENOSPC:
        return PROS_VFS_ENOSPC;
    case PROSENJITFS_EOVERFLOW:
        return PROS_VFS_EOVERFLOW;
    case PROSENJITFS_EINVAL:
        return PROS_VFS_EINVAL;
    case PROSENJITFS_EPERM:
        return PROS_VFS_EPERM;
    case PROSENJITFS_EBUSY:
        return PROS_VFS_EBUSY;
    case PROSENJITFS_ENOTSUP:
        return PROS_VFS_ENOTSUP;
    case PROSENJITFS_ENOENT:
        return PROS_VFS_ENOENT;
    case PROSENJITFS_EEXIST:
        return PROS_VFS_EEXIST;
    case PROSENJITFS_ECORRUPT:
        return PROS_VFS_ECORRUPT;
    case PROSENJITFS_ENOTDIR:
        return PROS_VFS_ENOTDIR;
    case PROSENJITFS_ENOTEMPTY:
        return PROS_VFS_ENOTEMPTY;
    default:
        return PROS_VFS_EIO;
    }
}

static int pfs_file_read(struct pros_vfs_file *file, uint32_t offset,
                         uint32_t length, void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint32_t done = 0u;

    if (!file || !file->vnode || !buffer)
        return PROS_VFS_EINVAL;
    if (length == 0u)
        return 0;
    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs || !data->fs->mount.active)
        return PROS_VFS_EPERM;
    {
        int rc = pfs_vnode_validate_inode(data, &inode);
        if (rc != PROSENJITFS_OK)
            return pfs_file_write_error(0u, rc);
    }
    file->vnode->size = inode.size;
    if ((uint64_t)offset > inode.size)
        return 0;
    if ((uint64_t)offset == inode.size)
        return 0;

    while (done < length && (uint64_t)offset + done < inode.size) {
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
            uint64_t physical;
            {
                int lba_rc = pfs_lba_add(data->fs->mount.device,
                                         extent.physical_block, delta,
                                         &physical);
                if (lba_rc != PROSENJITFS_OK)
                    return done ? (int)done : pfs_vfs_error(lba_rc);
            }
            if (physical < data->fs->sb.data_start ||
                physical >= data->fs->sb.total_blocks ||
                pfs_read_block(data->fs->mount.device, physical, block) !=
                    PROSENJITFS_OK)
                return done ? (int)done : PROS_VFS_EIO;
            if (pfs_diag.read_enabled && !pfs_diag.read_found) {
                pfs_diag.read_physical_lba = physical;
                pfs_diag.read_block_byte = block[188u];
                pfs_diag.read_block_found = 1u;
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

static int pfs_file_write_unserialized(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, const void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;

    uint64_t end_position;
    uint64_t max_file_size;

    if (!file || !file->vnode || !file->mount || !buffer)
        return PROS_VFS_EINVAL;
    if (file->mount->read_only || !file->mount->active)
        return PROS_VFS_EPERM;
    if (length == 0u)
        return 0;
    end_position = (uint64_t)offset + (uint64_t)length;
    if (end_position < (uint64_t)offset)
        return PROS_VFS_EOVERFLOW;

    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs)
        return PROS_VFS_EIO;
    if (!data->fs->mount.active)
        return PROS_VFS_EPERM;
    if (data->fs->sb.block_size == 0u ||
        data->fs->sb.block_size > 4096u ||
        data->fs->sb.total_blocks < data->fs->sb.data_start ||
        (data->fs->sb.total_blocks - data->fs->sb.data_start) >
            UINT64_MAX / data->fs->sb.block_size)
        return PROS_VFS_ECORRUPT;
    {
        int rc = pfs_vnode_validate_inode(data, &inode);
        if (rc != PROSENJITFS_OK)
            return pfs_file_write_error(0u, rc);
    }
    /*
     * APPEND is inode-EOF based, not cached-vnode based. This prevents two
     * independently opened handles from appending at a stale offset.
     */
    if ((file->flags & PROS_VFS_O_APPEND) != 0u) {
        if (inode.size > UINT32_MAX)
            return PROS_VFS_EOVERFLOW;
        offset = (uint32_t)inode.size;
    }
    end_position = (uint64_t)offset + (uint64_t)length;
    if (end_position < (uint64_t)offset)
        return PROS_VFS_EOVERFLOW;
    max_file_size = (data->fs->sb.total_blocks - data->fs->sb.data_start) *
                    data->fs->sb.block_size;
    if (end_position > max_file_size)
        return PROS_VFS_EOVERFLOW;

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
                return pfs_file_write_error(done, rc);
            rc = pfs_find_free_data_block_except(data->fs, physical,
                                                 &new_extent_lba);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
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

            /*
             * Keep the extent chain ordered by logical_block. A sparse write
             * can fill a hole between two existing extents, so blindly
             * appending at tail would make later lookups report corruption.
             */
            {
                uint64_t prev_lba = 0u;
                uint64_t current_lba = inode.extent_head;
                struct pros_prosenjitfs_extent current_extent;

                while (current_lba != 0u) {
                    rc = pfs_read_extent(data->fs, current_lba, &current_extent);
                    if (rc != PROSENJITFS_OK)
                        return pfs_file_write_error(done, rc);
                    if (logical_block < current_extent.logical_block)
                        break;
                    prev_lba = current_lba;
                    current_lba = current_extent.next;
                }
                new_extent.next = current_lba;
                if (prev_lba != 0u) {
                    rc = pfs_read_block(data->fs->mount.device, prev_lba,
                                        tail_image);
                    if (rc != PROSENJITFS_OK)
                        return pfs_file_write_error(done, rc);
                    {
                        struct pros_prosenjitfs_extent prev_extent;
                        pfs_copy(&prev_extent, tail_image, sizeof(prev_extent));
                        prev_extent.next = new_extent_lba;
                        pfs_copy(tail_image, &prev_extent, sizeof(prev_extent));
                        pfs_put32(
                            tail_image +
                            offsetof(struct pros_prosenjitfs_extent, checksum),
                            pfs_crc_with_zero32(
                                tail_image, sizeof(prev_extent),
                                (uint32_t)offsetof(
                                    struct pros_prosenjitfs_extent, checksum)));
                    }
                    tail_lba = prev_lba;
                } else {
                    inode.extent_head = new_extent_lba;
                    tail_lba = 0u;
                }
            }

            pfs_copy(extent_image, &new_extent, sizeof(new_extent));
            pfs_put32(extent_image +
                      offsetof(struct pros_prosenjitfs_extent, checksum),
                      pfs_crc_with_zero32(extent_image, sizeof(new_extent),
                                          (uint32_t)offsetof(
                                              struct pros_prosenjitfs_extent,
                                              checksum)));

            rc = pfs_inode_block_location(data->fs, data->inode_number,
                                          &inode_lba, &inode_offset);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_read_block(data->fs->mount.device, inode_lba, inode_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);

            {
                uint64_t staged_extent_head = inode.extent_head;
                pfs_copy(&inode, inode_image + inode_offset, sizeof(inode));
                /*
                 * Preserve the newly selected head when the new extent was
                 * inserted before the previous head. The disk inode read
                 * above contains the old head by definition.
                 */
                if (tail_lba == 0u && staged_extent_head != 0u &&
                    staged_extent_head != inode.extent_head)
                    inode.extent_head = staged_extent_head;
            }
            if ((uint64_t)offset > inode.size)
                inode.flags |= PROSENJITFS_INODE_FLAG_SPARSE;
            inode.size = ((uint64_t)offset + done + want > inode.size)
                       ? (uint64_t)offset + done + want : inode.size;
            if (inode.allocated_bytes >
                UINT64_MAX - data->fs->sb.block_size)
                return pfs_file_write_error(done, PROSENJITFS_EOVERFLOW);
            inode.allocated_bytes += data->fs->sb.block_size;
            pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
            pfs_put32(inode_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(inode_image + inode_offset,
                                          sizeof(inode),
                                          (uint32_t)offsetof(
                                              struct pros_prosenjitfs_inode,
                                              checksum)));

            rc = pfs_tx_reset(data->fs);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_allocator_stage(data->fs, physical, 1u, 1u);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_allocator_stage(data->fs, new_extent_lba, 1u, 1u);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_tx_add(data->fs, physical, data_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_tx_add(data->fs, new_extent_lba, extent_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            if (tail_lba != 0u) {
                rc = pfs_tx_add(data->fs, tail_lba, tail_image);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, rc);
            }
            rc = pfs_tx_add(data->fs, inode_lba, inode_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_tx_commit(data->fs);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
        } else if (rc == PROSENJITFS_OK) {
            uint64_t delta = logical_block - extent.logical_block;
            uint8_t inode_changed = 0u;
            {
                int lba_rc = pfs_lba_add(data->fs->mount.device,
                                         extent.physical_block, delta,
                                         &physical);
                if (lba_rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, lba_rc);
            }
            if (physical < data->fs->sb.data_start ||
                physical >= data->fs->sb.total_blocks)
                return pfs_file_write_error(done, PROSENJITFS_ECORRUPT);
            rc = pfs_read_block(data->fs->mount.device, physical, data_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            for (uint32_t i = 0u; i < want; ++i)
                data_image[in_block + i] =
                    ((const uint8_t *)buffer)[done + i];

            if ((uint64_t)offset > inode.size)
                inode.flags |= PROSENJITFS_INODE_FLAG_SPARSE;
            if ((uint64_t)offset + done + want > inode.size) {
                inode.size = (uint64_t)offset + done + want;
                inode_changed = 1u;
                rc = pfs_inode_block_location(data->fs, data->inode_number,
                                               &inode_lba, &inode_offset);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, rc);
                rc = pfs_read_block(data->fs->mount.device, inode_lba,
                                    inode_image);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, rc);
                pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
                pfs_put32(inode_image + inode_offset +
                          offsetof(struct pros_prosenjitfs_inode, checksum),
                          pfs_crc_with_zero32(inode_image + inode_offset,
                                              sizeof(inode),
                                              (uint32_t)offsetof(
                                                  struct pros_prosenjitfs_inode,
                                                  checksum)));
            }

            {
                int txrc = pfs_tx_reset(data->fs);
                if (txrc == PROSENJITFS_OK)
                    txrc = pfs_tx_add(data->fs, physical, data_image);
                if (txrc == PROSENJITFS_OK && inode_changed)
                    txrc = pfs_tx_add(data->fs, inode_lba, inode_image);
                if (txrc == PROSENJITFS_OK)
                    txrc = pfs_tx_commit(data->fs);
                if (txrc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, txrc);
            }
        } else {
            return pfs_file_write_error(done, rc);
        }

        done += want;
        file->vnode->size = inode.size;
    }
    return (int)done;
}

static int pfs_file_write(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, const void *buffer)
{
    int rc;
    int append = file && file->vnode &&
                 (file->flags & PROS_VFS_O_APPEND) != 0u;
    if (append)
        pfs_io_lock(((struct pfs_vnode_data *)file->vnode->private_data)->fs);
    rc = pfs_file_write_unserialized(file, offset, length, buffer);
    if (append)
        pfs_io_unlock(((struct pfs_vnode_data *)file->vnode->private_data)->fs);
    return rc;
}

static int pfs_file_truncate(struct pros_vfs_file *file, uint64_t size)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint64_t keep_blocks;
    uint64_t current;
    uint64_t steps = 0u;
    uint32_t affected = 0u;
    uint64_t retained_blocks = 0u;
    uint64_t inode_lba = 0u;
    uint32_t inode_offset = 0u;

    if (!file || !file->vnode || !file->mount || size > UINT32_MAX)
        return PROS_VFS_EINVAL;
    if (file->mount->read_only || !file->mount->active)
        return PROS_VFS_EPERM;

    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs)
        return PROS_VFS_EIO;

    {
        int rc = pfs_vnode_validate_inode(data, &inode);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
    }

    if (size == inode.size)
        return PROS_VFS_OK;

    if (size > inode.size) {
        static const uint8_t zeros[512] = { 0 };
        uint64_t left = size - inode.size;
        uint64_t position = inode.size;
        while (left != 0u) {
            uint32_t chunk = left > sizeof(zeros) ? sizeof(zeros) :
                             (uint32_t)left;
            int rc = pfs_file_write(file, (uint32_t)position, chunk, zeros);
            if (rc != (int)chunk)
                return rc < 0 ? rc : PROS_VFS_EIO;
            position += chunk;
            left -= chunk;
        }
        return PROS_VFS_OK;
    }

    if (data->fs->sb.block_size == 0u ||
        data->fs->sb.block_size > 4096u)
        return PROS_VFS_ECORRUPT;

    keep_blocks = size / data->fs->sb.block_size +
                  ((size % data->fs->sb.block_size) != 0u ? 1u : 0u);

    /*
     * Version-1 journal transactions are bounded by MAX_TX_BLOCKS. Shrink
     * is therefore preflighted and published atomically: if the complete
     * metadata update cannot fit, return before changing any home block.
     */
    current = inode.extent_head;
    while (current != 0u) {
        struct pros_prosenjitfs_extent extent;
        int rc;

        if (++steps > data->fs->sb.total_blocks)
            return PROS_VFS_ECORRUPT;
        rc = pfs_read_extent(data->fs, current, &extent);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);

        if (extent.logical_block > UINT64_MAX - extent.block_count)
            return PROS_VFS_ECORRUPT;

        if (extent.logical_block >= keep_blocks ||
            extent.logical_block + extent.block_count > keep_blocks)
            ++affected;

        current = extent.next;
    }

    /*
     * Conservative target bound: one inode block, one retained predecessor
     * extent block, and up to two bitmap blocks per affected extent (one for
     * data and one for extent metadata). This guarantees pfs_tx_add() cannot
     * discover transaction overflow after persistent changes begin.
     */
    if (affected > 7u)
        return PROS_VFS_EOVERFLOW;

    current = inode.extent_head;
    {
        uint64_t previous = 0u;
        uint64_t first_removed = 0u;
        uint64_t first_removed_next = 0u;

        while (current != 0u) {
            struct pros_prosenjitfs_extent extent;
            int rc = pfs_read_extent(data->fs, current, &extent);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);
            if (extent.logical_block >= keep_blocks) {
                first_removed = current;
                first_removed_next = extent.next;
                break;
            }
            if (extent.logical_block > UINT64_MAX - extent.block_count)
                return PROS_VFS_ECORRUPT;
            if (extent.logical_block + extent.block_count > keep_blocks) {
                first_removed = 0u;
                break;
            }
            if (retained_blocks > UINT64_MAX - extent.block_count)
                return PROS_VFS_EOVERFLOW;
            retained_blocks += extent.block_count;
            previous = current;
            current = extent.next;
        }

        if (first_removed != 0u) {
            uint64_t scan = first_removed;
            uint8_t inode_image[4096];
            uint64_t inode_lba;
            uint32_t inode_offset;
            int rc;

            rc = pfs_inode_block_location(data->fs, data->inode_number,
                                          &inode_lba, &inode_offset);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);

            rc = pfs_read_block(data->fs->mount.device, inode_lba,
                                inode_image);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);

            rc = pfs_tx_reset(data->fs);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);

            if (previous != 0u) {
                uint8_t prev_image[4096];
                struct pros_prosenjitfs_extent prev_extent;

                rc = pfs_read_block(data->fs->mount.device, previous,
                                    prev_image);
                if (rc != PROSENJITFS_OK)
                    goto truncate_abort;
                pfs_copy(&prev_extent, prev_image, sizeof(prev_extent));
                if (prev_extent.magic != PROSENJITFS_EXTENT_MAGIC ||
                    prev_extent.next != first_removed) {
                    rc = PROSENJITFS_ECORRUPT;
                    goto truncate_abort;
                }
                prev_extent.next = first_removed_next;
                pfs_copy(prev_image, &prev_extent, sizeof(prev_extent));
                pfs_put32(prev_image +
                          offsetof(struct pros_prosenjitfs_extent, checksum),
                          pfs_crc_with_zero32(
                              prev_image, sizeof(prev_extent),
                              (uint32_t)offsetof(
                                  struct pros_prosenjitfs_extent, checksum)));
                rc = pfs_tx_add(data->fs, previous, prev_image);
                if (rc != PROSENJITFS_OK)
                    goto truncate_abort;
            } else {
                inode.extent_head = first_removed_next;
            }

            scan = first_removed;
            while (scan != 0u) {
                struct pros_prosenjitfs_extent extent;
                uint64_t next;

                rc = pfs_read_extent(data->fs, scan, &extent);
                if (rc != PROSENJITFS_OK)
                    goto truncate_abort;
                next = extent.next;
                if (extent.block_count > UINT32_MAX ||
                    extent.block_count > UINT64_MAX /
                        data->fs->sb.block_size) {
                    rc = PROSENJITFS_EOVERFLOW;
                    goto truncate_abort;
                }
                if (inode.allocated_bytes <
                    extent.block_count * data->fs->sb.block_size) {
                    rc = PROSENJITFS_ECORRUPT;
                    goto truncate_abort;
                }
                inode.allocated_bytes -=
                    extent.block_count * data->fs->sb.block_size;
                rc = pfs_allocator_stage(data->fs, extent.physical_block,
                                         (uint32_t)extent.block_count, 0u);
                if (rc != PROSENJITFS_OK)
                    goto truncate_abort;
                rc = pfs_allocator_stage(data->fs, scan, 1u, 0u);
                if (rc != PROSENJITFS_OK)
                    goto truncate_abort;
                scan = next;
            }
        } else {
            current = inode.extent_head;
            while (current != 0u) {
                struct pros_prosenjitfs_extent extent;
                int rc = pfs_read_extent(data->fs, current, &extent);
                if (rc != PROSENJITFS_OK)
                    return pfs_vfs_error(rc);
                if (extent.logical_block > UINT64_MAX - extent.block_count)
                    return PROS_VFS_ECORRUPT;
                if (extent.logical_block < keep_blocks &&
                    extent.logical_block + extent.block_count > keep_blocks) {
                    uint64_t keep_in_extent =
                        keep_blocks - extent.logical_block;
                    uint64_t free_start;
                    uint32_t free_count;
                    uint8_t extent_image[4096];

                    if (keep_in_extent == 0u ||
                        keep_in_extent > extent.block_count ||
                        keep_in_extent > UINT32_MAX ||
                        extent.block_count - keep_in_extent > UINT32_MAX ||
                        extent.physical_block > UINT64_MAX - keep_in_extent)
                        return PROS_VFS_ECORRUPT;

                    free_start = extent.physical_block + keep_in_extent;
                    free_count = (uint32_t)(extent.block_count -
                                            keep_in_extent);
                    if (free_count != 0u) {
                        uint64_t released =
                            (uint64_t)free_count * data->fs->sb.block_size;
                        if (released / data->fs->sb.block_size != free_count ||
                            inode.allocated_bytes < released)
                            return PROS_VFS_ECORRUPT;
                        inode.allocated_bytes -= released;
                    }
                    if (keep_in_extent > UINT64_MAX - retained_blocks)
                        return PROS_VFS_EOVERFLOW;
                    retained_blocks += keep_in_extent;

                    rc = pfs_inode_block_location(data->fs,
                                                  data->inode_number,
                                                  &inode_lba,
                                                  &inode_offset);
                    if (rc != PROSENJITFS_OK)
                        return pfs_vfs_error(rc);
                    rc = pfs_read_block(data->fs->mount.device, current,
                                        extent_image);
                    if (rc != PROSENJITFS_OK)
                        return pfs_vfs_error(rc);

                    extent.block_count = keep_in_extent;
                    pfs_copy(extent_image, &extent, sizeof(extent));
                    pfs_put32(
                        extent_image +
                        offsetof(struct pros_prosenjitfs_extent, checksum),
                        pfs_crc_with_zero32(
                            extent_image, sizeof(extent),
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_extent, checksum)));

                    rc = pfs_tx_reset(data->fs);
                    if (rc == PROSENJITFS_OK && free_count != 0u)
                        rc = pfs_allocator_stage(data->fs, free_start,
                                                 free_count, 0u);
                    if (rc == PROSENJITFS_OK)
                        rc = pfs_tx_add(data->fs, current, extent_image);
                    if (rc != PROSENJITFS_OK) {
                        pfs_tx_reset(data->fs);
                        return pfs_vfs_error(rc);
                    }
                    break;
                }
                if (retained_blocks > UINT64_MAX - extent.block_count)
                    return PROS_VFS_EOVERFLOW;
                retained_blocks += extent.block_count;
                current = extent.next;
            }
        }

        if (inode.extent_head == 0u && retained_blocks != 0u)
            return PROS_VFS_ECORRUPT;

        inode.size = size;
        if (retained_blocks > UINT64_MAX / data->fs->sb.block_size)
            return PROS_VFS_EOVERFLOW;
        if (affected != 0u)
            inode.allocated_bytes =
                retained_blocks * data->fs->sb.block_size;

        {
            uint8_t inode_image[4096];
            int rc;
            if (affected == 0u) {
                rc = pfs_tx_reset(data->fs);
                if (rc != PROSENJITFS_OK)
                    return pfs_vfs_error(rc);
            }
            rc = pfs_inode_block_location(data->fs, data->inode_number,
                                              &inode_lba, &inode_offset);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);
            rc = pfs_read_block(data->fs->mount.device, inode_lba,
                                inode_image);
            if (rc != PROSENJITFS_OK) {
                pfs_tx_reset(data->fs);
                return pfs_vfs_error(rc);
            }
            pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
            pfs_put32(inode_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(
                          inode_image + inode_offset, sizeof(inode),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_inode, checksum)));
            int rc2 = pfs_tx_add(data->fs, inode_lba, inode_image);
            if (rc2 != PROSENJITFS_OK) {
                pfs_tx_reset(data->fs);
                return pfs_vfs_error(rc2);
            }
            rc2 = pfs_tx_commit(data->fs);
            if (rc2 != PROSENJITFS_OK)
                return pfs_vfs_error(rc2);
        }
    }

    file->vnode->size = size;
    return PROS_VFS_OK;

truncate_abort:
    pfs_tx_reset(data->fs);
    return pfs_vfs_error(rc);
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
    {
        int rc = prosenjitfs_read_inode(fs, inode_number, &inode);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
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
    data->generation = inode.generation;

    *result = (struct pros_vfs_vnode *)kmalloc(sizeof(**result));
    if (!*result) {
        kfree(data);
        return PROS_VFS_EIO;
    }
    /*
     * A vnode keeps a non-owning parent pointer by default, but path lookup
     * drops its current-vnode reference immediately after lookup() returns.
     * Nested vnodes therefore must explicitly retain their parent or the
     * parent becomes a dangling pointer as soon as lookup advances.
     */
    if (parent && vfs_vnode_get(parent) != PROS_VFS_OK) {
        kfree(data);
        kfree(*result);
        *result = 0;
        return PROS_VFS_EINVAL;
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
    uint8_t *block;

    if (!fs || !inode_number || fs->sb.block_size < 512u ||
        fs->sb.block_size > 4096u)
        return PROSENJITFS_EINVAL;
    block = (uint8_t *)kmalloc(fs->sb.block_size);
    if (!block)
        return PROSENJITFS_EIO;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    for (uint64_t inode = 1u; inode < fs->sb.inode_count; ++inode) {
        uint64_t bi = inode / bits_per_block;
        uint64_t within = inode % bits_per_block;
        if (bi >= fs->sb.inode_bitmap_blocks) {
            kfree(block);
            return PROSENJITFS_ECORRUPT;
        }
        {
            uint64_t bitmap_lba;
            int rc = pfs_lba_add(fs->mount.device, fs->sb.inode_bitmap_start,
                                 bi, &bitmap_lba);
            if (rc != PROSENJITFS_OK) {
                kfree(block);
                return rc;
            }
            rc = pfs_read_block(fs->mount.device, bitmap_lba, block);
            if (rc != PROSENJITFS_OK) {
                kfree(block);
                return rc;
            }
        }
        if (!pfs_bitmap_bit(block, within)) {
            *inode_number = inode;
            kfree(block);
            return PROSENJITFS_OK;
        }
    }
    kfree(block);
    return PROSENJITFS_ENOSPC;
}

static int pfs_find_free_data_block(const struct pros_prosenjitfs *fs,
                                    uint64_t *block_number)
{
    uint64_t bits_per_block;
    uint8_t *block;

    if (!fs || !block_number || fs->sb.block_size < 512u ||
        fs->sb.block_size > 4096u)
        return PROSENJITFS_EINVAL;
    block = (uint8_t *)kmalloc(fs->sb.block_size);
    if (!block)
        return PROSENJITFS_EIO;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    for (uint64_t b = fs->sb.data_start; b < fs->sb.total_blocks; ++b) {
        uint64_t bitmap_lba;
        int rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                             b / bits_per_block, &bitmap_lba);
        if (rc != PROSENJITFS_OK) {
            kfree(block);
            return rc;
        }
        rc = pfs_read_block(fs->mount.device, bitmap_lba, block);
        if (rc != PROSENJITFS_OK) {
            kfree(block);
            return rc;
        }
        if (!pfs_bitmap_bit(block, b % bits_per_block)) {
            *block_number = b;
            kfree(block);
            return PROSENJITFS_OK;
        }
    }
    kfree(block);
    return PROSENJITFS_ENOSPC;
}



static int pfs_find_free_data_block_excluding(const struct pros_prosenjitfs *fs,
                                              uint64_t excluded,
                                              uint64_t *block_number)
{
    uint64_t bits_per_block;
    uint8_t *block;

    if (!fs || !block_number || fs->sb.block_size < 512u ||
        fs->sb.block_size > 4096u)
        return PROSENJITFS_EINVAL;
    block = (uint8_t *)kmalloc(fs->sb.block_size);
    if (!block)
        return PROSENJITFS_EIO;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    for (uint64_t b = fs->sb.data_start; b < fs->sb.total_blocks; ++b) {
        if (b == excluded)
            continue;
        uint64_t bitmap_lba;
        {
            int rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                                 b / bits_per_block, &bitmap_lba);
            if (rc != PROSENJITFS_OK) {
                kfree(block);
                return rc;
            }
            rc = pfs_read_block(fs->mount.device, bitmap_lba, block);
            if (rc != PROSENJITFS_OK) {
                kfree(block);
                return rc;
            }
        }
        if (!pfs_bitmap_bit(block, b % bits_per_block)) {
            *block_number = b;
            kfree(block);
            return PROSENJITFS_OK;
        }
    }
    kfree(block);
    return PROSENJITFS_ENOSPC;
}

static int pfs_vnode_mkdir(struct pros_vfs_vnode *parent, const char *name,
                           struct pros_vfs_vnode **result)
{
    pfs_mkdir_diag_stage = 0u;
    pfs_mkdir_diag_rc = PROSENJITFS_OK;
    pfs_tx_diag_stage = 0u;
    pfs_tx_diag_count = 0u;
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode parent_inode, child_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_dir_entry entry;
    uint8_t *dir_block = 0, *entry_block = 0, *inode_block = 0, *child_dir_block = 0;
    uint64_t inode_number, entry_lba, child_dir_lba, inode_lba;
    uint64_t child_generation;
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
    rc = prosenjitfs_read_inode(data->fs, data->inode_number, &parent_inode);
    if (rc != PROSENJITFS_OK) {
        pfs_mkdir_diag_stage = 2u; pfs_mkdir_diag_rc = rc;
        return pfs_vfs_error(rc);
    }
    if (parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_ENOTDIR;
    rc = pfs_read_dir_index(data->fs, &parent_inode, &dir_index);
    if (rc != PROSENJITFS_OK) {
        pfs_mkdir_diag_stage = 2u; pfs_mkdir_diag_rc = rc;
        return pfs_vfs_error(rc);
    }
    rc = pfs_find_dir_entry(data->fs, &parent_inode, name, 0);
    if (rc == PROSENJITFS_OK)
        return PROS_VFS_EEXIST;
    if (rc != PROSENJITFS_ENOENT)
        return pfs_vfs_error(rc);
    rc = pfs_find_free_inode(data->fs, &inode_number);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 3u; pfs_mkdir_diag_rc = rc; goto mkdir_out; }
    rc = pfs_allocate_generation(data->fs, inode_number, &child_generation);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 3u; pfs_mkdir_diag_rc = rc; goto mkdir_out; }
    rc = pfs_find_free_data_block(data->fs, &entry_lba);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 4u; pfs_mkdir_diag_rc = rc; goto mkdir_out; }
    /*
     * entry_lba and child_dir_lba are committed together. Exclude the first
     * allocation so both logical objects never target the same physical block.
     */
    rc = pfs_find_free_data_block_excluding(data->fs, entry_lba,
                                           &child_dir_lba);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 5u; pfs_mkdir_diag_rc = rc; goto mkdir_out; }

    dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    entry_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    inode_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    child_dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!dir_block || !entry_block || !inode_block || !child_dir_block) {
        pfs_mkdir_diag_stage = 6u; pfs_mkdir_diag_rc = PROSENJITFS_EIO;
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
    child_inode.generation = child_generation;
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
    /* Compute using the on-disk checksum convention (checksum field zeroed). */
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

    rc = pfs_inode_block_location(data->fs, inode_number, &inode_lba,
                                   &inode_offset);
    if (rc != PROSENJITFS_OK) {
        pfs_mkdir_diag_stage = 7u; pfs_mkdir_diag_rc = rc; goto mkdir_out;
    }
    rc = pfs_read_block(data->fs->mount.device, inode_lba, inode_block);
    if (rc != PROSENJITFS_OK) {
        pfs_mkdir_diag_stage = 7u; pfs_mkdir_diag_rc = rc; goto mkdir_out;
    }
    pfs_copy(inode_block + inode_offset, &child_inode, sizeof(child_inode));
    pfs_put32(inode_block + inode_offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(inode_block + inode_offset,
                                  sizeof(child_inode),
                                  (uint32_t)offsetof(
                                      struct pros_prosenjitfs_inode, checksum)));

    rc = pfs_tx_reset(data->fs);
    if (rc == PROSENJITFS_OK) rc = pfs_inode_bitmap_stage(data->fs, inode_number, 1u);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 8u; pfs_mkdir_diag_rc = rc; }
    if (rc == PROSENJITFS_OK) rc = pfs_allocator_stage(data->fs, entry_lba, 1u, 1u);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 9u; pfs_mkdir_diag_rc = rc; }
    if (rc == PROSENJITFS_OK) rc = pfs_allocator_stage(data->fs, child_dir_lba, 1u, 1u);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 10u; pfs_mkdir_diag_rc = rc; }
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(data->fs, inode_lba, inode_block);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 11u; pfs_mkdir_diag_rc = rc; }
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(data->fs, parent_inode.dir_index, dir_block);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 12u; pfs_mkdir_diag_rc = rc; }
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(data->fs, entry_lba, entry_block);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 13u; pfs_mkdir_diag_rc = rc; }
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(data->fs, child_dir_lba, child_dir_block);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 14u; pfs_mkdir_diag_rc = rc; }
    if (rc == PROSENJITFS_OK) rc = pfs_tx_commit(data->fs);
    if (rc != PROSENJITFS_OK) { pfs_mkdir_diag_stage = 15u; pfs_mkdir_diag_rc = rc; }
    if (rc == PROSENJITFS_OK)
        rc = pfs_make_vnode(data->fs, inode_number, parent, result);
    if (rc != PROSENJITFS_OK && pfs_mkdir_diag_stage == 0u) { pfs_mkdir_diag_stage = 16u; pfs_mkdir_diag_rc = rc; }
    if (rc != PROSENJITFS_OK && data->fs->tx.count != 0u)
        pfs_tx_reset(data->fs);

mkdir_out:
    if (dir_block) kfree(dir_block);
    if (entry_block) kfree(entry_block);
    if (inode_block) kfree(inode_block);
    if (child_dir_block) kfree(child_dir_block);
    if (rc == PROSENJITFS_OK)
        return PROS_VFS_OK;

    /*
     * Preserve the VFS-visible error class instead of collapsing every
     * filesystem failure to EIO.  In particular, allocation/journal
     * validation failures must remain distinguishable from actual I/O
     * failures so callers and the mkdir regression can identify the
     * failing contract.
     */
    switch (rc) {
    case PROSENJITFS_EINVAL:
        return PROS_VFS_EINVAL;
    case PROSENJITFS_EIO:
        return PROS_VFS_EIO;
    case PROSENJITFS_ECORRUPT:
        return PROS_VFS_ECORRUPT;
    case PROSENJITFS_ENOTSUP:
        return PROS_VFS_ENOTSUP;
    case PROSENJITFS_ENOSPC:
        return PROS_VFS_ENOSPC;
    case PROSENJITFS_ENOENT:
        return PROS_VFS_ENOENT;
    case PROSENJITFS_EEXIST:
        return PROS_VFS_EEXIST;
    case PROSENJITFS_EOVERFLOW:
        return PROS_VFS_EOVERFLOW;
    case PROSENJITFS_ENOTDIR:
        return PROS_VFS_ENOTDIR;
    case PROSENJITFS_ENOTEMPTY:
        return PROS_VFS_ENOTEMPTY;
    case PROSENJITFS_EPERM:
        return PROS_VFS_EPERM;
    default:
        return PROS_VFS_EIO;
    }
}

static int pfs_vnode_rmdir(struct pros_vfs_vnode *parent, const char *name)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode parent_inode, child_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_dir_index child_dir_index;
    struct pros_prosenjitfs_dir_entry entry;
    uint8_t *dir_block = 0, *prev_block = 0, *inode_block = 0, *entry_block = 0;
    uint64_t current = 0u, previous = 0u, target_lba = 0u, inode_lba, steps = 0u;
    uint64_t inode_number;
    struct pros_prosenjitfs_dir_entry child_entry;
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
    rc = prosenjitfs_read_inode(data->fs, data->inode_number, &parent_inode);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);
    if (parent_inode.type != PROSENJITFS_INODE_DIR) {
        rc = PROS_VFS_ENOTDIR;
        goto rmdir_out;
    }
    rc = pfs_read_dir_index(data->fs, &parent_inode, &dir_index);
    if (rc != PROSENJITFS_OK)
        goto rmdir_out;

    entry_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!entry_block)
        { rc = PROS_VFS_EIO; goto rmdir_out; }

    bucket = pfs_name_hash((const uint8_t *)name, name_length) %
             PROSENJITFS_DIR_BUCKETS;
    current = dir_index.heads[bucket];
    while (current != 0u) {
        if (++steps > data->fs->sb.total_blocks) {
            rc = PROS_VFS_EIO;
            goto rmdir_out;
        }
        if (current < data->fs->sb.data_start ||
            current >= data->fs->sb.total_blocks) {
            rc = PROSENJITFS_ECORRUPT;
            goto rmdir_out;
        }
        rc = pfs_read_block(data->fs->mount.device, current, entry_block);
        if (rc != PROSENJITFS_OK)
            goto rmdir_out;
        pfs_copy(&entry, entry_block, sizeof(entry));
        if (entry.magic != PROSENJITFS_ENTRY_MAGIC ||
            entry.name_length == 0u || entry.name_length > PROSENJITFS_MAX_NAME ||
            pfs_crc_with_zero32(entry_block, sizeof(entry),
                (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum)) !=
                entry.checksum ||
            entry.next >= data->fs->sb.total_blocks ||
            (entry.next != 0u && entry.next < data->fs->sb.data_start)) {
            rc = PROSENJITFS_ECORRUPT;
            goto rmdir_out;
        }
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
    if (inode_number >= data->fs->sb.inode_count) {
        rc = PROSENJITFS_ECORRUPT;
        goto rmdir_out;
    }
    rc = prosenjitfs_read_inode(data->fs, inode_number, &child_inode);
    if (rc != PROSENJITFS_OK)
        goto rmdir_out;
    if (child_inode.type != PROSENJITFS_INODE_DIR) {
        rc = PROSENJITFS_ENOTDIR; goto rmdir_out;
    }
    if (child_inode.extent_head != 0u) {
        rc = PROSENJITFS_ECORRUPT;
        goto rmdir_out;
    }
    if (entry.type != PROSENJITFS_INODE_DIR ||
        child_inode.parent_inode != data->inode_number) {
        rc = PROSENJITFS_ECORRUPT;
        goto rmdir_out;
    }

    /* A directory is removable only when every bucket is empty. */
    {
        uint8_t *scan = (uint8_t *)kmalloc(data->fs->sb.block_size);
        if (!scan) { rc = PROS_VFS_EIO; goto rmdir_out; }
        rc = pfs_read_dir_index(data->fs, &child_inode, &child_dir_index);
        if (rc != PROSENJITFS_OK) { kfree(scan); goto rmdir_out; }
        for (uint32_t b = 0u; b < PROSENJITFS_DIR_BUCKETS; ++b) {
            current = child_dir_index.heads[b];
            steps = 0u;
            while (current != 0u) {
                if (++steps > data->fs->sb.total_blocks) {
                    kfree(scan);
                    rc = PROSENJITFS_ECORRUPT;
                    goto rmdir_out;
                }
                if (current < data->fs->sb.data_start ||
                    current >= data->fs->sb.total_blocks) {
                    kfree(scan);
                    rc = PROSENJITFS_ECORRUPT;
                    goto rmdir_out;
                }
                rc = pfs_read_block(data->fs->mount.device, current, scan);
                if (rc != PROSENJITFS_OK) { kfree(scan); goto rmdir_out; }
                {
                    pfs_copy(&child_entry, scan, sizeof(child_entry));
                    if (child_entry.magic != PROSENJITFS_ENTRY_MAGIC ||
                        child_entry.name_length == 0u ||
                        child_entry.name_length > PROSENJITFS_MAX_NAME ||
                        child_entry.next >= data->fs->sb.total_blocks ||
                        (child_entry.next != 0u &&
                         child_entry.next < data->fs->sb.data_start) ||
                        pfs_crc_with_zero32(scan, sizeof(child_entry),
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_dir_entry,
                                checksum)) != child_entry.checksum) {
                        kfree(scan);
                        rc = PROSENJITFS_ECORRUPT;
                        goto rmdir_out;
                    }
                    kfree(scan);
                    return PROS_VFS_ENOTEMPTY;
                }
                current = child_entry.next;
            }
        }
        kfree(scan);
    }

    rc = pfs_inode_block_location(data->fs, inode_number, &inode_lba,
                                   &inode_offset);
    if (rc != PROSENJITFS_OK)
        goto rmdir_out;
    inode_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    prev_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!inode_block || !dir_block || !prev_block)
        goto rmdir_out;
    rc = pfs_read_block(data->fs->mount.device, inode_lba, inode_block);
    if (rc != PROSENJITFS_OK)
        goto rmdir_out;
    rc = pfs_read_block(data->fs->mount.device, parent_inode.dir_index, dir_block);
    if (rc != PROSENJITFS_OK)
        goto rmdir_out;

    if (previous == 0u) dir_index.heads[bucket] = entry.next;
    else {
        rc = pfs_read_block(data->fs->mount.device, previous, prev_block);
        if (rc != PROSENJITFS_OK)
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
    pfs_zero(&freed_inode, sizeof(freed_inode));
    freed_inode.type = PROSENJITFS_INODE_FREE;
    rc = pfs_next_generation(child_inode.generation, &freed_inode.generation);
    if (rc != PROSENJITFS_OK)
        goto rmdir_out;
    pfs_zero(inode_block + inode_offset, sizeof(child_inode));
    pfs_copy(inode_block + inode_offset, &freed_inode, sizeof(freed_inode));
    pfs_put32(inode_block + inode_offset + offsetof(struct pros_prosenjitfs_inode, checksum),
        pfs_crc_with_zero32(inode_block + inode_offset, sizeof(freed_inode),
            (uint32_t)offsetof(struct pros_prosenjitfs_inode, checksum)));

    rc = pfs_tx_reset(data->fs);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, parent_inode.dir_index, dir_block);
    if (rc == PROSENJITFS_OK && previous != 0u)
        rc = pfs_tx_add(data->fs, previous, prev_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(data->fs, inode_lba, inode_block);
    if (rc == PROSENJITFS_OK)
        rc = pfs_inode_bitmap_stage(data->fs, inode_number, 0u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_allocator_stage(data->fs, target_lba, 1u, 0u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_allocator_stage(data->fs, child_inode.dir_index, 1u, 0u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_commit(data->fs);
    if (rc != PROSENJITFS_OK) {
        pfs_tx_reset(data->fs);
        goto rmdir_out;
    }
    rc = PROS_VFS_OK;
rmdir_out:
    if (dir_block) kfree(dir_block);
    if (prev_block) kfree(prev_block);
    if (inode_block) kfree(inode_block);
    if (entry_block) kfree(entry_block);
    if (rc == PROSENJITFS_OK) return PROS_VFS_OK;
    return (rc < 0 && rc <= -200000) ? rc : pfs_vfs_error(rc);
}

static int pfs_vnode_unlink(struct pros_vfs_vnode *parent, const char *name)
{
    struct pfs_vnode_data *parent_data;
    struct pros_prosenjitfs_inode parent_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_dir_entry entry;
    struct pros_prosenjitfs_inode child_inode;
    struct pros_prosenjitfs_inode freed_inode;
    struct pros_prosenjitfs_inode freed_inode;
    uint8_t *dir_block = 0;
    uint8_t *prev_block = 0;
    uint8_t *inode_block = 0;
    uint8_t *entry_block = 0;
    uint64_t previous = 0u, target_lba = 0u;
    uint64_t current = 0u;
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
    rc = prosenjitfs_read_inode(parent_data->fs, parent_data->inode_number,
                                 &parent_inode);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);
    if (parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_ENOTDIR;
    { int dir_read_rc = pfs_read_dir_index(parent_data->fs, &parent_inode, &dir_index);
        if (dir_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(dir_read_rc); }

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
    if (inode_number >= parent_data->fs->sb.inode_count) {
        rc = PROSENJITFS_ECORRUPT;
        goto out;
    }
    {
        int inode_rc = prosenjitfs_read_inode(parent_data->fs, inode_number,
                                              &child_inode);
        if (inode_rc != PROSENJITFS_OK)
            { rc = inode_rc; goto out; }
    }
    if (child_inode.type == PROSENJITFS_INODE_DIR)
        { rc = PROS_VFS_EISDIR; goto out; }
    if (entry.type != PROSENJITFS_INODE_FILE ||
        child_inode.parent_inode != parent_data->inode_number) {
        rc = PROSENJITFS_ECORRUPT;
        goto out;
    }

    if (inode_number > UINT64_MAX / PROSENJITFS_INODE_SIZE) {
        rc = PROSENJITFS_EOVERFLOW;
        goto out;
    }
    inode_byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    {
        int lba_rc = pfs_lba_add(parent_data->fs->mount.device,
                                 parent_data->fs->sb.inode_table_start,
                                 inode_byte_offset / parent_data->fs->sb.block_size,
                                 &inode_lba);
        if (lba_rc != PROSENJITFS_OK) {
            rc = lba_rc;
            goto out;
        }
    }
    inode_offset = (uint32_t)(inode_byte_offset %
                              parent_data->fs->sb.block_size);
    {
        int io_rc = pfs_read_block(parent_data->fs->mount.device, inode_lba,
                                   inode_block);
        if (io_rc != PROSENJITFS_OK)
            { rc = io_rc; goto out; }
    }

    rc = pfs_tx_reset(parent_data->fs);
    if (rc != PROSENJITFS_OK)
        goto out;

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
        {
            int io_rc = pfs_read_block(parent_data->fs->mount.device, previous,
                                       prev_block);
            if (io_rc != PROSENJITFS_OK)
                { rc = io_rc; goto out_reset; }
        }
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

    pfs_zero(&freed_inode, sizeof(freed_inode));
    freed_inode.type = PROSENJITFS_INODE_FREE;
    rc = pfs_next_generation(child_inode.generation, &freed_inode.generation);
    if (rc != PROSENJITFS_OK)
        goto out_reset;
    pfs_zero(inode_block + inode_offset, sizeof(child_inode));
    pfs_copy(inode_block + inode_offset, &freed_inode, sizeof(freed_inode));
    pfs_put32(inode_block + inode_offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(inode_block + inode_offset,
                                  sizeof(freed_inode),
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
    if (rc == PROSENJITFS_OK) {
        rc = PROS_VFS_OK;
        goto out;
    }

out_reset:
    if (rc != PROSENJITFS_OK) {
        pfs_tx_reset(parent_data->fs);
    }

out:
    if (dir_block) kfree(dir_block);
    if (prev_block) kfree(prev_block);
    if (inode_block) kfree(inode_block);
    if (entry_block) kfree(entry_block);
    if (rc == PROS_VFS_OK) return PROS_VFS_OK;
    return pfs_vfs_error(rc);
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
    uint64_t child_generation;
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
    if (!parent->mount || !parent->mount->active)
        return PROS_VFS_EPERM;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK)
        return PROS_VFS_EINVAL;

    { int inode_read_rc = prosenjitfs_read_inode(data->fs, data->inode_number, &parent_inode);
        if (inode_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(inode_read_rc);
        if (parent_inode.type != PROSENJITFS_INODE_DIR)
            return PROS_VFS_ENOTDIR; }
    rc = pfs_find_dir_entry(data->fs, &parent_inode, name, 0);
    if (rc == PROSENJITFS_OK)
        return PROS_VFS_EEXIST;
    if (rc != PROSENJITFS_ENOENT)
        return pfs_vfs_error(rc);
    { int dir_read_rc = pfs_read_dir_index(data->fs, &parent_inode, &dir_index);
        if (dir_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(dir_read_rc); }
    rc = pfs_find_free_inode(data->fs, &inode_number);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);
    rc = pfs_allocate_generation(data->fs, inode_number, &child_generation);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);
    rc = pfs_find_free_data_block(data->fs, &entry_lba);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);

    dir_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    entry_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    inode_block = (uint8_t *)kmalloc(data->fs->sb.block_size);
    if (!dir_block || !entry_block || !inode_block) {
        if (dir_block) kfree(dir_block);
        if (entry_block) kfree(entry_block);
        if (inode_block) kfree(inode_block);
        return PROS_VFS_EIO;
    }

    rc = pfs_read_block(data->fs->mount.device, parent_inode.dir_index, dir_block);
    if (rc != PROSENJITFS_OK) {
        kfree(dir_block); kfree(entry_block); kfree(inode_block);
        return pfs_vfs_error(rc);
    }
    rc = pfs_read_block(data->fs->mount.device, entry_lba, entry_block);
    if (rc != PROSENJITFS_OK) {
        kfree(dir_block); kfree(entry_block); kfree(inode_block);
        return pfs_vfs_error(rc);
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
    child_inode.generation = child_generation;
    child_inode.parent_inode = data->inode_number;
    child_inode.size = 0u;
    child_inode.allocated_bytes = 0u;

    if (inode_number > UINT64_MAX / PROSENJITFS_INODE_SIZE) {
        kfree(dir_block); kfree(entry_block); kfree(inode_block);
        return PROS_VFS_EOVERFLOW;
    }
    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    {
        int lba_rc = pfs_lba_add(data->fs->mount.device,
                                 data->fs->sb.inode_table_start,
                                 byte_offset / data->fs->sb.block_size,
                                 &inode_lba);
        if (lba_rc != PROSENJITFS_OK) {
            kfree(dir_block); kfree(entry_block); kfree(inode_block);
            return pfs_vfs_error(lba_rc);
        }
    }
    inode_offset = (uint32_t)(byte_offset % data->fs->sb.block_size);
    rc = pfs_read_block(data->fs->mount.device, inode_lba, inode_block);
    if (rc != PROSENJITFS_OK) {
        kfree(dir_block); kfree(entry_block); kfree(inode_block);
        return pfs_vfs_error(rc);
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
        return pfs_vfs_error(rc);

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
    if (parent)
        vfs_vnode_put(parent);
    vnode->initialized = 0u;
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
    while (1) {
        if (length > PROSENJITFS_MAX_NAME)
            return PROSENJITFS_EINVAL;
        if (s[length] == 0u)
            break;
        if (s[length] == '/')
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
        inode->dir_index < fs->sb.data_start ||
        inode->dir_index >= fs->sb.total_blocks ||
        fs->sb.block_size > sizeof(block))
        return PROSENJITFS_ECORRUPT;
    {
        int rc = pfs_read_block(fs->mount.device, inode->dir_index, block);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
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
    {
        int rc = pfs_read_block(fs->mount.device, block_lba, block);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
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
    {
        int rc = pfs_read_dir_index(fs, dir_inode, &index);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    hash = pfs_name_hash((const uint8_t *)name, name_length);
    current = index.heads[hash % PROSENJITFS_DIR_BUCKETS];
    while (current != 0u) {
        if (++steps > fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        {
            int rc = pfs_read_dir_entry(fs, current, &entry);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
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
    {
        int rc = pfs_read_dir_index(fs, dir_inode, &index);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    for (uint32_t bucket = 0u; bucket < PROSENJITFS_DIR_BUCKETS; ++bucket) {
        uint64_t current = index.heads[bucket];
        uint64_t steps = 0u;
        while (current != 0u) {
            if (++steps > fs->sb.total_blocks)
                return PROSENJITFS_ECORRUPT;
            { int entry_read_rc = pfs_read_dir_entry(fs, current, &entry);
            if (entry_read_rc != PROSENJITFS_OK)
                return entry_read_rc; }
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

static int __attribute__((unused)) pfs_find_dir_entry_lba(const struct pros_prosenjitfs *fs,
                                   const struct pros_prosenjitfs_inode *dir_inode,
                                   const char *name,
                                   struct pros_prosenjitfs_dir_entry *found,
                                   uint64_t *found_lba,
                                   uint64_t *previous_lba)
{
    struct pros_prosenjitfs_dir_index index;
    struct pros_prosenjitfs_dir_entry entry;
    uint32_t name_length, hash;
    uint64_t current, previous = 0u, steps = 0u;
    if (!fs || !dir_inode || !name || !found_lba || !previous_lba)
        return PROSENJITFS_EINVAL;
    *found_lba = 0u;
    *previous_lba = 0u;
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    {
        int rc = pfs_read_dir_index(fs, dir_inode, &index);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    hash = pfs_name_hash((const uint8_t *)name, name_length);
    current = index.heads[hash % PROSENJITFS_DIR_BUCKETS];
    while (current != 0u) {
        if (++steps > fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        {
            int rc = pfs_read_dir_entry(fs, current, &entry);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
        if (entry.name_hash == hash && entry.name_length == name_length) {
            uint32_t equal = 1u;
            for (uint32_t i = 0u; i < name_length; ++i) {
                if (entry.name[i] != (uint8_t)name[i]) { equal = 0u; break; }
            }
            if (equal) {
                if (found) pfs_copy(found, &entry, sizeof(*found));
                *found_lba = current;
                *previous_lba = previous;
                return PROSENJITFS_OK;
            }
        }
        previous = current;
        current = entry.next;
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
        struct pros_prosenjitfs_inode dir_inode;
        int inode_read_rc = prosenjitfs_read_inode(data->fs, data->inode_number, &dir_inode);
        if (inode_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(inode_read_rc);
        if (dir_inode.parent_inode == 0 || dir_inode.parent_inode == data->inode_number || data->inode_number == data->fs->sb.root_inode) {
            if (vfs_vnode_get(parent) != PROS_VFS_OK)
                return PROS_VFS_EINVAL;
            *result = parent;
            return PROS_VFS_OK;
        }
        return pfs_make_vnode(data->fs, dir_inode.parent_inode, parent ? parent->parent : NULL, result);
    }

    {
        struct pros_prosenjitfs_inode dir_inode;
        struct pros_prosenjitfs_dir_entry disk_entry;
        int rc;
        { int inode_read_rc = prosenjitfs_read_inode(data->fs, data->inode_number, &dir_inode);
        if (inode_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(inode_read_rc);
        if (dir_inode.type != PROSENJITFS_INODE_DIR)
            return PROS_VFS_ENOTDIR; }
        rc = pfs_find_dir_entry(data->fs, &dir_inode, name, &disk_entry);
        if (rc == PROSENJITFS_ENOENT)
            return PROS_VFS_ENOENT;
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
        return pfs_make_vnode(data->fs, disk_entry.inode, parent, result);
    }
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
    struct pros_prosenjitfs_dir_entry entry;
    struct pros_prosenjitfs_dir_index old_index;
    struct pros_prosenjitfs_dir_index new_index;
    uint64_t old_lba = 0u, old_prev = 0u;
    uint64_t inode_lba = 0u, new_lba = 0u;
    uint64_t original_next = 0u;
    uint32_t old_len = 0u, new_len = 0u;
    uint32_t old_hash, new_hash, old_bucket, new_bucket;
    uint8_t *entry_block = 0;
    uint8_t *prev_block = 0;
    uint8_t *old_dir_block = 0;
    uint8_t *new_dir_block = 0;
    uint8_t *inode_block = 0;
    int rc;

    if (!old_parent || !new_parent || !old_name || !new_name ||
        !old_parent->private_data || !new_parent->private_data ||
        old_parent->type != PROS_VFS_NODE_DIRECTORY ||
        new_parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;

    old_data = (struct pfs_vnode_data *)old_parent->private_data;
    new_data = (struct pfs_vnode_data *)new_parent->private_data;
    if (!old_data->fs || old_data->fs != new_data->fs ||
        !old_parent->mount || old_parent->mount != new_parent->mount ||
        !old_parent->mount->active || old_parent->mount->read_only)
        return PROS_VFS_EPERM;

    if (pfs_utf8_name_valid(old_name, &old_len) != PROSENJITFS_OK ||
        pfs_utf8_name_valid(new_name, &new_len) != PROSENJITFS_OK)
        return PROS_VFS_EINVAL;

    if (old_parent == new_parent && old_len == new_len) {
        uint32_t same = 1u;
        for (uint32_t i = 0u; i < old_len; ++i)
            if (old_name[i] != new_name[i]) { same = 0u; break; }
        if (same)
            return PROS_VFS_OK;
    }

    rc = prosenjitfs_read_inode(old_data->fs, old_data->inode_number, &old_dir);
    if (rc != PROSENJITFS_OK || old_dir.type != PROSENJITFS_INODE_DIR)
        return rc == PROSENJITFS_OK ? PROS_VFS_ENOTDIR : pfs_vfs_error(rc);
    rc = prosenjitfs_read_inode(new_data->fs, new_data->inode_number, &new_dir);
    if (rc != PROSENJITFS_OK || new_dir.type != PROSENJITFS_INODE_DIR)
        return rc == PROSENJITFS_OK ? PROS_VFS_ENOTDIR : pfs_vfs_error(rc);

    rc = pfs_find_dir_entry_lba(old_data->fs, &old_dir, old_name, &entry,
                                 &old_lba, &old_prev);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);

    {
        struct pros_prosenjitfs_dir_entry existing;
        rc = pfs_find_dir_entry(new_data->fs, &new_dir, new_name, &existing);
        if (rc == PROSENJITFS_OK)
            return PROS_VFS_EEXIST;
        if (rc != PROSENJITFS_ENOENT)
            return pfs_vfs_error(rc);
    }

    old_hash = entry.name_hash;
    new_hash = pfs_name_hash((const uint8_t *)new_name, new_len);
    old_bucket = old_hash % PROSENJITFS_DIR_BUCKETS;
    new_bucket = new_hash % PROSENJITFS_DIR_BUCKETS;

    entry_block = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    prev_block = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    old_dir_block = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    new_dir_block = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    inode_block = (uint8_t *)kmalloc(old_data->fs->sb.block_size);
    if (!entry_block || !prev_block || !old_dir_block || !new_dir_block ||
        !inode_block) {
        rc = PROS_VFS_EIO;
        goto rename_out;
    }

    rc = pfs_read_block(old_data->fs->mount.device, old_lba, entry_block);
    if (rc != PROSENJITFS_OK) goto rename_out_error;
    if (old_prev != 0u) {
        rc = pfs_read_block(old_data->fs->mount.device, old_prev, prev_block);
        if (rc != PROSENJITFS_OK) goto rename_out_error;
    }

    if (old_parent == new_parent) {
        rc = pfs_read_dir_index(old_data->fs, &old_dir, &old_index);
        if (rc != PROSENJITFS_OK) goto rename_out_error;

        if (old_bucket == new_bucket) {
            entry.name_hash = new_hash;
            entry.name_length = (uint16_t)new_len;
            pfs_zero(entry.name, sizeof(entry.name));
            for (uint32_t i = 0u; i < new_len; ++i)
                entry.name[i] = (uint8_t)new_name[i];
            pfs_copy(entry_block, &entry, sizeof(entry));
            pfs_put32(entry_block + offsetof(struct pros_prosenjitfs_dir_entry,
                                             checksum),
                      pfs_crc_with_zero32(entry_block, sizeof(entry),
                          (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry,
                                             checksum)));

            rc = pfs_tx_reset(old_data->fs);
            if (rc == PROSENJITFS_OK) rc = pfs_tx_add(old_data->fs, old_lba, entry_block);
            if (rc == PROSENJITFS_OK) rc = pfs_tx_commit(old_data->fs);
            goto rename_out;
        }

        {
            original_next = entry.next;
            if (old_prev == 0u)
                old_index.heads[old_bucket] = original_next;
            entry.next = old_index.heads[new_bucket];
        }
        entry.name_hash = new_hash;
        entry.name_length = (uint16_t)new_len;
        pfs_zero(entry.name, sizeof(entry.name));
        for (uint32_t i = 0u; i < new_len; ++i)
            entry.name[i] = (uint8_t)new_name[i];
        pfs_copy(entry_block, &entry, sizeof(entry));
        pfs_put32(entry_block + offsetof(struct pros_prosenjitfs_dir_entry, checksum),
                  pfs_crc_with_zero32(entry_block, sizeof(entry),
                      (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum)));
        if (old_prev != 0u) {
            struct pros_prosenjitfs_dir_entry prev;
            pfs_copy(&prev, prev_block, sizeof(prev));
            if (prev.magic != PROSENJITFS_ENTRY_MAGIC || prev.next != old_lba) {
                rc = PROSENJITFS_ECORRUPT; goto rename_out;
            }
        }
        /*
         * For a bucket move the predecessor must skip the original entry.
         * Preserve its original successor before entry.next was replaced.
         */
        {
            struct pros_prosenjitfs_dir_entry original;
            /* entry.next was changed above; original_next is the on-disk
             * successor that the predecessor must retain.  Do not reread the
             * block into a fixed 4096-byte buffer: block_size is configurable. */
            original = entry;
            original.next = original_next;
            if (old_prev != 0u) {
                struct pros_prosenjitfs_dir_entry prev;
                pfs_copy(&prev, prev_block, sizeof(prev));
                prev.next = original.next;
                pfs_copy(prev_block, &prev, sizeof(prev));
                pfs_put32(prev_block + offsetof(struct pros_prosenjitfs_dir_entry,
                                                 checksum),
                          pfs_crc_with_zero32(prev_block, sizeof(prev),
                              (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry,
                                                 checksum)));
            }
        }
        pfs_zero(old_dir_block, old_data->fs->sb.block_size);
        pfs_copy(old_dir_block, &old_index, sizeof(old_index));
        pfs_put32(old_dir_block + offsetof(struct pros_prosenjitfs_dir_index, checksum),
                  pfs_crc_with_zero32(old_dir_block, sizeof(old_index),
                      (uint32_t)offsetof(struct pros_prosenjitfs_dir_index, checksum)));

        old_index.heads[new_bucket] = old_lba;
        pfs_zero(old_dir_block, old_data->fs->sb.block_size);
        pfs_copy(old_dir_block, &old_index, sizeof(old_index));
        pfs_put32(old_dir_block + offsetof(struct pros_prosenjitfs_dir_index, checksum),
                  pfs_crc_with_zero32(old_dir_block, sizeof(old_index),
                      (uint32_t)offsetof(struct pros_prosenjitfs_dir_index, checksum)));

        rc = pfs_tx_reset(old_data->fs);
        if (rc == PROSENJITFS_OK && old_prev != 0u)
            rc = pfs_tx_add(old_data->fs, old_prev, prev_block);
        if (rc == PROSENJITFS_OK) rc = pfs_tx_add(old_data->fs, old_dir.dir_index, old_dir_block);
        if (rc == PROSENJITFS_OK) rc = pfs_tx_add(old_data->fs, old_lba, entry_block);
        if (rc == PROSENJITFS_OK) rc = pfs_tx_commit(old_data->fs);
        goto rename_out;
    }

    /*
     * Directory moves are valid, but a directory may never be moved into
     * itself or one of its descendants.  The on-disk representation stores
     * the parent inode in the child inode (there is no physical ".." entry),
     * so updating that parent is sufficient once the cycle check succeeds.
     * Bound the walk by inode_count so corrupted parent chains fail closed.
     */
    if (entry.type == PROSENJITFS_INODE_DIR) {
        uint64_t moving_inode = entry.inode;
        uint64_t cursor = new_data->inode_number;
        uint64_t steps = 0u;

        if (moving_inode == old_data->fs->sb.root_inode ||
            moving_inode == new_data->inode_number)
            { rc = PROS_VFS_EPERM; goto rename_out; }

        while (cursor != old_data->fs->sb.root_inode) {
            struct pros_prosenjitfs_inode ancestor;

            if (++steps > old_data->fs->sb.inode_count) {
                rc = PROSENJITFS_ECORRUPT;
                goto rename_out;
            }
            if (cursor >= old_data->fs->sb.inode_count) {
                rc = PROSENJITFS_ECORRUPT;
                goto rename_out;
            }
            if (cursor == moving_inode) {
                rc = PROS_VFS_EINVAL;
                goto rename_out;
            }
            rc = prosenjitfs_read_inode(old_data->fs, cursor, &ancestor);
            if (rc != PROSENJITFS_OK)
                goto rename_out_error;
            if (ancestor.type != PROSENJITFS_INODE_DIR) {
                rc = PROSENJITFS_ECORRUPT;
                goto rename_out;
            }
            if (ancestor.parent_inode == cursor) {
                rc = PROSENJITFS_ECORRUPT;
                goto rename_out;
            }
            cursor = ancestor.parent_inode;
            if (cursor >= old_data->fs->sb.inode_count) {
                rc = PROSENJITFS_ECORRUPT;
                goto rename_out;
            }
        }
    }

    rc = pfs_read_dir_index(old_data->fs, &old_dir, &old_index);
    if (rc != PROSENJITFS_OK) goto rename_out_error;
    rc = pfs_read_dir_index(new_data->fs, &new_dir, &new_index);
    if (rc != PROSENJITFS_OK) goto rename_out_error;
    rc = pfs_find_free_data_block(old_data->fs, &new_lba);
    if (rc != PROSENJITFS_OK) goto rename_out_error;
    if (new_lba == old_lba) { rc = PROSENJITFS_ECORRUPT; goto rename_out; }
    rc = pfs_read_block(old_data->fs->mount.device, old_dir.dir_index, old_dir_block);
    if (rc != PROSENJITFS_OK) goto rename_out_error;
    rc = pfs_read_block(old_data->fs->mount.device, new_dir.dir_index, new_dir_block);
    if (rc != PROSENJITFS_OK) goto rename_out_error;

    {
        struct pros_prosenjitfs_dir_entry original = entry;
        original.next = new_index.heads[new_bucket];
        original.name_hash = new_hash;
        original.name_length = (uint16_t)new_len;
        pfs_zero(original.name, sizeof(original.name));
        for (uint32_t i = 0u; i < new_len; ++i)
            original.name[i] = (uint8_t)new_name[i];
        original.checksum = pfs_crc_with_zero32((const uint8_t *)&original,
                                                 sizeof(original),
                                                 (uint32_t)offsetof(
                                                     struct pros_prosenjitfs_dir_entry,
                                                     checksum));
        pfs_zero(entry_block, old_data->fs->sb.block_size);
        pfs_copy(entry_block, &original, sizeof(original));
    }

    if (old_prev == 0u)
        old_index.heads[old_bucket] = entry.next;
    if (old_prev != 0u) {
        struct pros_prosenjitfs_dir_entry prev;
        pfs_copy(&prev, prev_block, sizeof(prev));
        if (prev.magic != PROSENJITFS_ENTRY_MAGIC || prev.next != old_lba) {
            rc = PROSENJITFS_ECORRUPT; goto rename_out;
        }
        prev.next = entry.next;
        pfs_copy(prev_block, &prev, sizeof(prev));
        pfs_put32(prev_block + offsetof(struct pros_prosenjitfs_dir_entry, checksum),
                  pfs_crc_with_zero32(prev_block, sizeof(prev),
                      (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum)));
    }
    new_index.heads[new_bucket] = new_lba;

    {
        uint64_t inode_number = entry.inode;
        uint64_t byte_offset;
        uint32_t inode_offset;
        struct pros_prosenjitfs_inode child;
        rc = prosenjitfs_read_inode(old_data->fs, inode_number, &child);
        if (rc != PROSENJITFS_OK) goto rename_out_error;
        child.parent_inode = new_data->inode_number;
        if (inode_number > UINT64_MAX / PROSENJITFS_INODE_SIZE) {
            rc = PROSENJITFS_EOVERFLOW; goto rename_out;
        }
        byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
        rc = pfs_lba_add(old_data->fs->mount.device, old_data->fs->sb.inode_table_start,
                         byte_offset / old_data->fs->sb.block_size, &inode_lba);
        if (rc != PROSENJITFS_OK) goto rename_out;
        inode_offset = (uint32_t)(byte_offset % old_data->fs->sb.block_size);
        rc = pfs_read_block(old_data->fs->mount.device, inode_lba, inode_block);
        if (rc != PROSENJITFS_OK) goto rename_out_error;
        pfs_copy(inode_block + inode_offset, &child, sizeof(child));
        pfs_put32(inode_block + inode_offset +
                  offsetof(struct pros_prosenjitfs_inode, checksum),
                  pfs_crc_with_zero32(inode_block + inode_offset,
                      sizeof(child),
                      (uint32_t)offsetof(struct pros_prosenjitfs_inode, checksum)));
    }

    pfs_zero(old_dir_block, old_data->fs->sb.block_size);
    pfs_copy(old_dir_block, &old_index, sizeof(old_index));
    pfs_put32(old_dir_block + offsetof(struct pros_prosenjitfs_dir_index, checksum),
              pfs_crc_with_zero32(old_dir_block, sizeof(old_index),
                  (uint32_t)offsetof(struct pros_prosenjitfs_dir_index, checksum)));
    pfs_zero(new_dir_block, new_data->fs->sb.block_size);
    pfs_copy(new_dir_block, &new_index, sizeof(new_index));
    pfs_put32(new_dir_block + offsetof(struct pros_prosenjitfs_dir_index, checksum),
              pfs_crc_with_zero32(new_dir_block, sizeof(new_index),
                  (uint32_t)offsetof(struct pros_prosenjitfs_dir_index, checksum)));

    rc = pfs_tx_reset(old_data->fs);
    if (rc == PROSENJITFS_OK && old_prev != 0u) rc = pfs_tx_add(old_data->fs, old_prev, prev_block);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(old_data->fs, old_dir.dir_index, old_dir_block);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(old_data->fs, new_dir.dir_index, new_dir_block);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(old_data->fs, new_lba, entry_block);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_add(old_data->fs, inode_lba, inode_block);
    if (rc == PROSENJITFS_OK) rc = pfs_allocator_stage(old_data->fs, new_lba, 1u, 1u);
    if (rc == PROSENJITFS_OK) rc = pfs_allocator_stage(old_data->fs, old_lba, 1u, 0u);
    if (rc == PROSENJITFS_OK) rc = pfs_tx_commit(old_data->fs);

    goto rename_out;

rename_out_error:
    rc = pfs_vfs_error(rc);
rename_out:
    if (entry_block) kfree(entry_block);
    if (prev_block) kfree(prev_block);
    if (old_dir_block) kfree(old_dir_block);
    if (new_dir_block) kfree(new_dir_block);
    if (inode_block) kfree(inode_block);
    return rc;
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
        int rc;
        uint64_t ordinal = *cookie - 2u;
        if (!directory->private_data)
            return PROS_VFS_EINVAL;
        data = (struct pfs_vnode_data *)directory->private_data;
        { int inode_read_rc = prosenjitfs_read_inode(data->fs, data->inode_number, &dir_inode);
        if (inode_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(inode_read_rc);
        if (dir_inode.type != PROSENJITFS_INODE_DIR)
            return PROS_VFS_ENOTDIR; }
        rc = pfs_dir_entry_by_ordinal(data->fs, &dir_inode, ordinal, &disk_entry);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
        rc = prosenjitfs_read_inode(data->fs, disk_entry.inode, &child_inode);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
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
    if (!fs || !image || target < 2u || target >= fs->sb.total_blocks)
        return PROSENJITFS_EINVAL;
    {
        uint64_t journal_end;
        if (pfs_region_end(fs->sb.journal_start, fs->sb.journal_blocks,
                           &journal_end) != PROSENJITFS_OK)
            return PROSENJITFS_ECORRUPT;
        if (target >= fs->sb.journal_start && target < journal_end)
            return PROSENJITFS_EINVAL;
    }
    for (uint32_t i = 0; i < fs->tx.count; ++i)
        if (fs->tx.targets[i] == target) {
            pfs_copy(fs->tx.images + (uint64_t)i * fs->sb.block_size,
                     image, fs->sb.block_size);
            return PROSENJITFS_OK;
        }
    if (fs->tx.count >= PROSENJITFS_MAX_TX_BLOCKS)
        return PROSENJITFS_EINVAL;
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
    for (uint64_t n = 0; n < (uint64_t)count * block_size; ++n) {
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
    uint64_t lba;
    int rc;

    if (!fs) return -101;
    if (!fs->scratch) return -102;
    if (!fs->mount.device) return -103;

    pfs_zero(fs->scratch, fs->sb.block_size);
    
    rc = pfs_lba_add(fs->mount.device, fs->sb.journal_start, 0u, &lba);
    if (rc != PROSENJITFS_OK) return -104;

    rc = pfs_write_block(fs->mount.device, lba, fs->scratch);
    if (rc != PROSENJITFS_OK) return -105;

    rc = pfs_flush(fs->mount.device);
    if (rc != PROSENJITFS_OK) return -106;

    fs->tx.count = 0u;
    return PROSENJITFS_OK;
}

static int pfs_journal_recover(struct pros_prosenjitfs *fs)
{
    uint8_t *header;
    uint8_t *images;
    uint8_t *commit;
    if (!fs || !fs->scratch || !fs->tx.images)
        return PROSENJITFS_EINVAL;
    header = fs->scratch;
    images = fs->tx.images;
    commit = images + (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                              fs->sb.block_size;
    struct pros_prosenjitfs_journal_header h;
    struct pros_prosenjitfs_journal_commit c;
    uint32_t expected;

    {
        int rc = pfs_read_block(fs->mount.device, fs->sb.journal_start, header);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    pfs_copy(&h, header, sizeof(h));
    if (h.magic == 0u)
        return PROSENJITFS_OK;
    if (fs->read_only)
        return PROSENJITFS_EPERM;
    if (h.magic != PROSENJITFS_JOURNAL_MAGIC ||
        sizeof(h) > fs->sb.block_size || sizeof(c) > fs->sb.block_size ||
        h.record_count == 0u || h.record_count > PROSENJITFS_MAX_TX_BLOCKS ||
        h.record_count + 2u > fs->sb.journal_blocks ||
        pfs_crc_with_zero32(header, fs->sb.block_size,
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_journal_header,
                                checksum)) != h.checksum)
        return PROSENJITFS_ECORRUPT;

    for (uint32_t i = 0; i < h.record_count; ++i) {
        uint64_t journal_end;
        if (h.targets[i] < 2u || h.targets[i] >= fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;
        if (pfs_region_end(fs->sb.journal_start, fs->sb.journal_blocks,
                           &journal_end) != PROSENJITFS_OK)
            return PROSENJITFS_ECORRUPT;
        /* Journal records are never valid transaction targets. Reject a
         * corrupted target before replay so recovery cannot overwrite the
         * journal itself and create an unrecoverable replay loop. */
        if (h.targets[i] >= fs->sb.journal_start &&
            h.targets[i] < journal_end)
            return PROSENJITFS_ECORRUPT;
        for (uint32_t j = 0u; j < i; ++j) {
            if (h.targets[j] == h.targets[i])
                return PROSENJITFS_ECORRUPT;
        }
        {
            int rc = pfs_read_at_offset(fs->mount.device, fs->sb.journal_start,
                                        1u + i,
                                        images + (uint64_t)i * fs->sb.block_size);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
    }
    {
        int rc = pfs_read_at_offset(fs->mount.device, fs->sb.journal_start,
                                    1u + h.record_count, commit);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    pfs_copy(&c, commit, sizeof(c));
    if (c.magic != PROSENJITFS_JOURNAL_COMMIT ||
        c.transaction_id != h.transaction_id ||
        c.record_count != h.record_count)
        return pfs_journal_clear(fs);
    expected = pfs_journal_checksum(header, images, fs->sb.block_size,
                                    h.record_count, commit);
    if (expected != c.checksum)
        return PROSENJITFS_ECORRUPT;

    for (uint32_t i = 0; i < h.record_count; ++i) {
        int rc = pfs_write_block(fs->mount.device, h.targets[i],
                                 images + (uint64_t)i * fs->sb.block_size);
        if (rc != PROSENJITFS_OK)
            return rc;
        {
            int crash_rc = pfs_tx_crash_inject(10u);
            if (crash_rc != PROSENJITFS_OK)
                return crash_rc;
        }
    }
    {
        int rc = pfs_flush(fs->mount.device);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    {
        int crash_rc = pfs_tx_crash_inject(11u);
        if (crash_rc != PROSENJITFS_OK)
            return crash_rc;
    }
    return pfs_journal_clear(fs);
}

static int pfs_tx_validate(const struct pros_prosenjitfs *fs)
{
    uint64_t journal_end;

    if (!fs || !fs->tx.images || fs->tx.count == 0u ||
        fs->tx.count > PROSENJITFS_MAX_TX_BLOCKS ||
        fs->sb.block_size < 512u || fs->sb.block_size > 4096u ||
        sizeof(struct pros_prosenjitfs_journal_header) > fs->sb.block_size ||
        sizeof(struct pros_prosenjitfs_journal_commit) > fs->sb.block_size ||
        (fs->sb.block_size & (fs->sb.block_size - 1u)) != 0u)
        return PROSENJITFS_EINVAL;
    if (pfs_region_end(fs->sb.journal_start, fs->sb.journal_blocks,
                       &journal_end) != PROSENJITFS_OK ||
        fs->sb.journal_blocks < 2u ||
        fs->tx.count > fs->sb.journal_blocks - 2u)
        return PROSENJITFS_ECORRUPT;

    /* Revalidate the complete transaction immediately before the first
     * journal write.  pfs_tx_add() already checks these invariants, but the
     * commit boundary is the final fail-closed point before persistent state
     * can change. */
    for (uint32_t i = 0u; i < fs->tx.count; ++i) {
        uint64_t target = fs->tx.targets[i];
        if (target < 2u || target >= fs->sb.total_blocks ||
            (target >= fs->sb.journal_start && target < journal_end))
            return PROSENJITFS_ECORRUPT;
        for (uint32_t j = 0u; j < i; ++j) {
            if (fs->tx.targets[j] == target)
                return PROSENJITFS_ECORRUPT;
        }
    }
    return PROSENJITFS_OK;
}

static int pfs_tx_commit(struct pros_prosenjitfs *fs)
{
    uint8_t *header;
    uint8_t *commit;
    struct pros_prosenjitfs_journal_header h;
    struct pros_prosenjitfs_journal_commit c;
    uint32_t checksum;

    pfs_tx_diag_stage = 1u;
    pfs_tx_diag_count = 0u;
    if (!fs || !fs->scratch || !fs->tx.images || fs->read_only)
        return PROSENJITFS_EINVAL;
    {
        int validate_rc = pfs_tx_validate(fs);
        if (validate_rc != PROSENJITFS_OK)
            return validate_rc;
    }
    pfs_tx_diag_count = fs->tx.count;
    {
        int crash_rc = pfs_tx_crash_inject(1u);
        if (crash_rc != PROSENJITFS_OK)
            return crash_rc;
    }

    header = fs->scratch;
    commit = fs->tx.images + (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                              fs->sb.block_size;
    pfs_zero(header, fs->sb.block_size);
    pfs_zero(commit, fs->sb.block_size);
    pfs_zero(&h, sizeof(h));
    if (fs->sb.transaction_sequence == UINT64_MAX)
        return PROSENJITFS_EOVERFLOW;
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

    pfs_tx_diag_stage = 2u;
    {
        int rc = pfs_write_block(fs->mount.device, fs->sb.journal_start,
                                 header);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    {
        int crash_rc = pfs_tx_crash_inject(2u);
        if (crash_rc != PROSENJITFS_OK)
            return crash_rc;
    }
    pfs_tx_diag_stage = 3u;
    for (uint32_t i = 0; i < fs->tx.count; ++i) {
        uint64_t journal_lba;
        {
            int rc = pfs_lba_add(fs->mount.device, fs->sb.journal_start,
                                 1u + i, &journal_lba);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
            {
            int rc = pfs_write_block(fs->mount.device, journal_lba,
                                     fs->tx.images + (uint64_t)i * fs->sb.block_size);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
        {
            int crash_rc = pfs_tx_crash_inject(3u);
            if (crash_rc != PROSENJITFS_OK)
                return crash_rc;
        }
    }
    pfs_tx_diag_stage = 4u;
    if (pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    {
        int crash_rc = pfs_tx_crash_inject(4u);
        if (crash_rc != PROSENJITFS_OK)
            return crash_rc;
    }

    pfs_zero(&c, sizeof(c));
    c.magic = PROSENJITFS_JOURNAL_COMMIT;
    c.record_count = fs->tx.count;
    c.transaction_id = h.transaction_id;
    pfs_copy(commit, &c, sizeof(c));
    checksum = pfs_journal_checksum(header, fs->tx.images,
                                    fs->sb.block_size, fs->tx.count, commit);
    c.checksum = checksum;
    pfs_copy(commit, &c, sizeof(c));
    pfs_tx_diag_stage = 5u;
    {
        uint64_t commit_lba;
        {
            int rc = pfs_lba_add(fs->mount.device, fs->sb.journal_start,
                                 1u + fs->tx.count, &commit_lba);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
            {
            int rc = pfs_write_block(fs->mount.device, commit_lba, commit);
            if (rc == PROSENJITFS_OK)
                rc = pfs_flush(fs->mount.device);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
    }
    {
        int crash_rc = pfs_tx_crash_inject(5u);
        if (crash_rc != PROSENJITFS_OK)
            return crash_rc;
    }

    pfs_tx_diag_stage = 6u;
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
            pfs_diag.verify_pending = 1u;
            pfs_diag.enabled = 0u;
        }
        {
            int rc = pfs_write_block(fs->mount.device, fs->tx.targets[i], tx_image);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
        {
            int crash_rc = pfs_tx_crash_inject(6u);
            if (crash_rc != PROSENJITFS_OK)
                return crash_rc;
        }
    }
    pfs_tx_diag_stage = 7u;
    if (pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    {
        int crash_rc = pfs_tx_crash_inject(7u);
        if (crash_rc != PROSENJITFS_OK)
            return crash_rc;
    }
    if (pfs_diag.verify_pending) {
        uint8_t verify[4096];
        pfs_tx_diag_stage = 8u;
        if (pfs_read_block(fs->mount.device, pfs_diag.physical_lba,
                           verify) != PROSENJITFS_OK)
            return PROSENJITFS_EIO;
        pfs_diag.post_commit_byte = verify[188u];
        if (pfs_diag.post_commit_byte != 0xD6u)
            return PROSENJITFS_EIO;
        /* Verify the diagnostic block only for the transaction that created
         * the diagnostic marker. Later transactions may legitimately free
         * and reuse that block (for example during truncate tests). */
        pfs_diag.verify_pending = 0u;
    }

    pfs_tx_diag_stage = 8u;
    {
        int crash_rc = pfs_tx_crash_inject(8u);
        if (crash_rc != PROSENJITFS_OK)
            return crash_rc;
    }
    pfs_tx_diag_stage = 9u;
    {
        int crash_rc = pfs_tx_crash_inject(9u);
        if (crash_rc != PROSENJITFS_OK)
            return crash_rc;
    }
    pfs_tx_diag_count_before_clear = fs->tx.count;
    pfs_tx_diag_clear_rc = pfs_journal_clear(fs);
    pfs_tx_diag_count_after_clear = fs->tx.count;
    return pfs_tx_diag_clear_rc;
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

    if (!fs || count == 0u || allocate > 1u || first < fs->sb.data_start ||
        first >= fs->sb.total_blocks ||
        (uint64_t)count > fs->sb.total_blocks - first ||
        fs->sb.block_size == 0u || fs->sb.block_size > 4096u)
        return PROSENJITFS_EINVAL;

    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    last = first + (uint64_t)count - 1u;
    first_bitmap = first / bits_per_block;
    last_bitmap = last / bits_per_block;

    if (last_bitmap < first_bitmap ||
        last_bitmap - first_bitmap + 1u > PROSENJITFS_MAX_TX_BLOCKS ||
        last_bitmap >= (uint64_t)fs->sb.block_bitmap_blocks)
        return PROSENJITFS_EOVERFLOW;

    for (uint64_t bi = first_bitmap; ; ++bi) {
        uint8_t image[4096];
        uint64_t lba;
        uint64_t bitmap_base;

        if (bi > UINT64_MAX / bits_per_block)
            return PROSENJITFS_EOVERFLOW;
        bitmap_base = bi * bits_per_block;
        {
            int rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                                 bi, &lba);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
        uint64_t range_start = first > bitmap_base ? first - bitmap_base : 0u;
        uint64_t bitmap_end =
            bits_per_block - 1u <= UINT64_MAX - bitmap_base
            ? bitmap_base + bits_per_block - 1u : UINT64_MAX;
        uint64_t range_end = last < bitmap_end
                           ? last - bitmap_base : bits_per_block - 1u;
        uint32_t slot = 0u;

        while (slot < fs->tx.count && fs->tx.targets[slot] != lba)
            ++slot;
        if (slot == fs->tx.count) {
            {
                int rc = pfs_read_block(fs->mount.device, lba, image);
                if (rc != PROSENJITFS_OK)
                    return rc;
                rc = pfs_tx_add(fs, lba, image);
                if (rc != PROSENJITFS_OK)
                    return rc;
        }
            slot = 0u;
            while (slot < fs->tx.count && fs->tx.targets[slot] != lba)
                ++slot;
        }
        if (slot == fs->tx.count)
            return PROSENJITFS_EINVAL;

        for (uint64_t bit = range_start; ; ++bit) {
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

        if (bi == last_bitmap)
            break;
    }
    return PROSENJITFS_OK;
}

static int pfs_allocator_update(struct pros_prosenjitfs *fs,
                                uint64_t first, uint32_t count,
                                uint8_t allocate)
{
    if (!fs || fs->read_only)
        return PROSENJITFS_EINVAL;
    {
        int rc = pfs_tx_reset(fs);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    /* Staging must complete before commit; callers never receive a partially
     * applied allocator transaction when the journal capacity is exceeded. */
    {
        int rc = pfs_allocator_stage(fs, first, count, allocate);
        if (rc != PROSENJITFS_OK)
            return rc;
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
    if (fs->sb.block_size > 4096u)
        return PROSENJITFS_EOVERFLOW;

    for (uint64_t b = data_start; b < fs->sb.total_blocks; ++b) {
        uint64_t bits_per_block = (uint64_t)fs->sb.block_size * 8u;
        uint64_t bitmap_lba;
        {
            int rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                                 b / bits_per_block, &bitmap_lba);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
        uint8_t bitbuf[4096];

        {
            int rc = pfs_read_block(fs->mount.device, bitmap_lba, bitbuf);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
        if (!pfs_bitmap_bit(bitbuf, b % bits_per_block)) {
            ++run;
            if (run == count) {
                *first_block = b + 1u - count;
                if (*first_block < data_start ||
                    count > fs->sb.total_blocks - *first_block)
                    return PROSENJITFS_EINVAL;
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

    {
        int rc = pfs_lba_add(fs->mount.device, fs->sb.inode_bitmap_start,
                             bi, &lba);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    slot = 0u;
    while (slot < fs->tx.count && fs->tx.targets[slot] != lba)
        ++slot;
    if (slot == fs->tx.count) {
        {
            int rc = pfs_read_block(fs->mount.device, lba, block);
            if (rc != PROSENJITFS_OK)
                return rc;
            rc = pfs_tx_add(fs, lba, block);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
        slot = 0u;
        while (slot < fs->tx.count && fs->tx.targets[slot] != lba)
            ++slot;
    }
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
    if (!fs || fs->read_only)
        return PROSENJITFS_EINVAL;
    {
        int rc = pfs_tx_reset(fs);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    {
        int rc = pfs_inode_bitmap_stage(fs, inode_number, allocate);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
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
        {
            uint64_t bitmap_lba;
            int rc = pfs_lba_add(fs->mount.device, fs->sb.inode_bitmap_start,
                                 bi, &bitmap_lba);
            if (rc != PROSENJITFS_OK)
                return rc;
            rc = pfs_read_block(fs->mount.device, bitmap_lba, block);
            if (rc != PROSENJITFS_OK)
                return rc;
        }
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
    if (inode_number > UINT64_MAX / PROSENJITFS_INODE_SIZE)
        return PROSENJITFS_EOVERFLOW;
    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    {
        int rc = pfs_lba_add(fs->mount.device, fs->sb.inode_table_start,
                             byte_offset / fs->sb.block_size, &block_lba);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    offset = (uint32_t)(byte_offset % fs->sb.block_size);
    if ((uint64_t)offset + sizeof(*inode) > fs->sb.block_size)
        return PROSENJITFS_ECORRUPT;
    {
        int rc = pfs_read_block(fs->mount.device, block_lba, block);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
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
    if (inode_number > UINT64_MAX / PROSENJITFS_INODE_SIZE)
        return PROSENJITFS_EOVERFLOW;
    byte_offset = inode_number * PROSENJITFS_INODE_SIZE;
    {
        int rc = pfs_lba_add(fs->mount.device, fs->sb.inode_table_start,
                             byte_offset / fs->sb.block_size, &block_lba);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    offset = (uint32_t)(byte_offset % fs->sb.block_size);
    if ((uint64_t)offset + sizeof(*inode) > fs->sb.block_size)
        return PROSENJITFS_ECORRUPT;
    {
        int rc = pfs_read_block(fs->mount.device, block_lba, block);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    pfs_copy(block + offset, inode, sizeof(*inode));
    pfs_put32(block + offset +
              offsetof(struct pros_prosenjitfs_inode, checksum),
              pfs_crc_with_zero32(block + offset, sizeof(*inode),
                                 (uint32_t)offsetof(
                                     struct pros_prosenjitfs_inode, checksum)));
    {
        int rc = pfs_tx_reset(fs);
        if (rc != PROSENJITFS_OK)
            return rc;
        rc = pfs_tx_add(fs, block_lba, block);
        if (rc != PROSENJITFS_OK)
            return rc;
        return pfs_tx_commit(fs);
    }
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
        (device->sector_size & (device->sector_size - 1u)) != 0u ||
        device->sector_count < 2u)
        return PROSENJITFS_EINVAL;
    if (fs->mount.active)
        return PROSENJITFS_EBUSY;
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
    {
        int rc = pfs_read_block(device, 0u, block);
        if (rc != PROSENJITFS_OK) {
            kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
            return rc;
        }
    }
    primary_ok = pfs_superblock_valid(block, device->sector_size, &primary) == PROSENJITFS_OK;
    {
        int rc = pfs_read_block(device, 1u, block);
        if (rc != PROSENJITFS_OK) {
            kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
            return rc;
        }
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
    {
        int rc = pfs_journal_recover(fs);
        if (rc != PROSENJITFS_OK) {
            vfs_mount_deactivate(&fs->mount);
            kfree(fs->tx.images); kfree(fs->scratch);
            fs->tx.images = 0;
            fs->scratch = 0;
            return rc;
        }
    }
    struct pros_vfs_vnode *root_vnode = 0;
    {
        int rc = pfs_make_vnode(fs, fs->sb.root_inode, 0, &root_vnode);
        if (rc != PROS_VFS_OK) {
            vfs_mount_deactivate(&fs->mount);
            kfree(fs->tx.images); kfree(fs->scratch);
            fs->tx.images = 0; fs->scratch = 0;
            return rc;
        }
        rc = vfs_mount_set_root(&fs->mount, root_vnode);
        if (rc != PROS_VFS_OK) {
            vfs_vnode_put(root_vnode);
            vfs_mount_deactivate(&fs->mount);
            kfree(fs->tx.images); kfree(fs->scratch);
            fs->tx.images = 0; fs->scratch = 0;
            return rc;
        }
    }
    /* vfs_mount_set_root() acquired the mount's ownership reference. */
    vfs_vnode_put(root_vnode);

    if (vfs_mount_activate(&fs->mount) != PROS_VFS_OK) {
        vfs_mount_deactivate(&fs->mount);
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

    /*
     * Never tear down transaction/scratch state while a file or directory
     * handle still owns a vnode. This also prevents a stale handle from
     * becoming valid again if the same fs object is immediately remounted.
     */
    if (fs->mount.root && fs->mount.root->refcount > 1u)
        return PROSENJITFS_EBUSY;

    rc = pfs_flush(fs->mount.device);
    if (rc != PROSENJITFS_OK)
        return rc;
    rc = vfs_mount_deactivate(&fs->mount);
    if (rc != PROS_VFS_OK)
        return pfs_vfs_error(rc);
    if (fs->tx.images) kfree(fs->tx.images);
    if (fs->scratch) kfree(fs->scratch);
    fs->tx.images = 0;
    fs->scratch = 0;
    return PROSENJITFS_OK;
}


struct pfs_test_disk {
    /* data[] is the durable medium; volatile_data[] models a device write
     * cache. A power-loss reset discards only dirty volatile blocks. */
    uint8_t data[512u * 512u];
    uint8_t volatile_data[512u * 512u];
    uint8_t dirty[512u / 8u];
};

static int pfs_test_dirty(const struct pfs_test_disk *disk, uint64_t lba)
{
    return disk && lba < 512u &&
           (disk->dirty[lba >> 3] & (uint8_t)(1u << (lba & 7u))) != 0u;
}

static void pfs_test_mark_dirty(struct pfs_test_disk *disk, uint64_t lba)
{
    disk->dirty[lba >> 3] |= (uint8_t)(1u << (lba & 7u));
}

static void pfs_test_clear_dirty(struct pfs_test_disk *disk, uint64_t lba)
{
    disk->dirty[lba >> 3] &= (uint8_t)~(1u << (lba & 7u));
}

static int pfs_test_read(struct pros_block_device *device, uint64_t lba,
                         uint32_t count, void *buffer)
{
    struct pfs_test_disk *disk = (struct pfs_test_disk *)device->private_data;
    if (!disk || !buffer || lba >= 512u || count > 512u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t i = 0u; i < count; ++i) {
        uint64_t current = lba + i;
        const uint8_t *source = pfs_test_dirty(disk, current)
                              ? disk->volatile_data + current * 512u
                              : disk->data + current * 512u;
        pfs_copy((uint8_t *)buffer + (uint64_t)i * 512u,
                 source, 512u);
    }
    return PROS_BLOCK_OK;
}

static int pfs_test_write(struct pros_block_device *device, uint64_t lba,
                          uint32_t count, const void *buffer)
{
    struct pfs_test_disk *disk = (struct pfs_test_disk *)device->private_data;
    if (!disk || !buffer || lba >= 512u || count > 512u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t i = 0u; i < count; ++i) {
        uint64_t current = lba + i;
        pfs_copy(disk->volatile_data + current * 512u,
                 (const uint8_t *)buffer + (uint64_t)i * 512u,
                 512u);
        pfs_test_mark_dirty(disk, current);
    }
    return PROS_BLOCK_OK;
}

static int pfs_test_flush(struct pros_block_device *device)
{
    struct pfs_test_disk *disk = (struct pfs_test_disk *)device->private_data;
    if (!disk)
        return PROS_BLOCK_EINVAL;
    for (uint64_t lba = 0u; lba < 512u; ++lba) {
        if (pfs_test_dirty(disk, lba)) {
            pfs_copy(disk->data + lba * 512u,
                     disk->volatile_data + lba * 512u, 512u);
            pfs_test_clear_dirty(disk, lba);
        }
    }
    return PROS_BLOCK_OK;
}

static void pfs_test_power_loss(struct pfs_test_disk *disk)
{
    if (!disk)
        return;
    pfs_zero(disk->dirty, sizeof(disk->dirty));
}

static void pfs_test_crash_reset(struct pros_prosenjitfs *fs,
                                 struct pfs_test_disk *disk)
{
    if (!fs || !disk)
        return;
    pfs_test_power_loss(disk);
    (void)vfs_mount_deactivate(&fs->mount);
    if (fs->tx.images)
        kfree(fs->tx.images);
    if (fs->scratch)
        kfree(fs->scratch);
    pfs_zero(fs, sizeof(*fs));
}

static uint32_t pfs_selftest_diag_stage;
static int pfs_selftest_diag_rc;

static int pfs_selftest_fail(uint32_t stage, int rc)
{
    pfs_selftest_diag_stage = stage;
    pfs_selftest_diag_rc = rc;
    return rc;
}

void prosenjitfs_selftest_diag_get(uint32_t *stage, int *rc)
{
    if (!stage || !rc)
        return;
    *stage = pfs_selftest_diag_stage;
    *rc = pfs_selftest_diag_rc;
}


static int pfs_consistency_mark(uint8_t *owner, uint64_t total_blocks,
                                   uint64_t block)
{
    if (!owner || block >= total_blocks)
        return PROSENJITFS_ECORRUPT;
    if ((owner[block >> 3] & (uint8_t)(1u << (block & 7u))) != 0u)
        return PROSENJITFS_ECORRUPT;
    owner[block >> 3] |= (uint8_t)(1u << (block & 7u));
    return PROSENJITFS_OK;
}

static int pfs_selftest_consistency(const struct pros_prosenjitfs *fs)
{
    uint64_t bits_per_block;
    uint64_t owner_bytes;
    uint8_t *owner = 0;
    uint8_t *dir_refs = 0;
    uint8_t bitmap[4096];
    struct pros_prosenjitfs_inode inode;
    int rc = PROSENJITFS_EIO;

    if (!fs || !fs->mount.device || !fs->mount.active ||
        fs->sb.block_size == 0u || fs->sb.block_size > sizeof(bitmap) ||
        fs->sb.total_blocks > fs->mount.device->sector_count ||
        fs->sb.data_start >= fs->sb.total_blocks ||
        fs->sb.inode_count == 0u)
        return PROSENJITFS_ECORRUPT;

    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    if (fs->sb.total_blocks > UINT64_MAX - 7u)
        return PROSENJITFS_EOVERFLOW;
    owner_bytes = (fs->sb.total_blocks + 7u) / 8u;
    if (owner_bytes == 0u || owner_bytes > UINT32_MAX)
        return PROSENJITFS_EOVERFLOW;

    if (fs->sb.journal_start < 2u ||
        pfs_region_valid(&fs->sb, fs->sb.journal_start,
                         fs->sb.journal_blocks) == 0 ||
        pfs_region_valid(&fs->sb, fs->sb.block_bitmap_start,
                         fs->sb.block_bitmap_blocks) == 0 ||
        pfs_region_valid(&fs->sb, fs->sb.inode_bitmap_start,
                         fs->sb.inode_bitmap_blocks) == 0 ||
        pfs_region_valid(&fs->sb, fs->sb.inode_table_start,
                         fs->sb.inode_table_blocks) == 0)
        return PROSENJITFS_ECORRUPT;

    {
        uint64_t journal_end, bitmap_end, inode_bitmap_end, inode_end;
        uint64_t required_block_bitmap_blocks;
        uint64_t required_inode_bitmap_blocks;

        if (pfs_region_end(fs->sb.journal_start, fs->sb.journal_blocks,
                            &journal_end) != PROSENJITFS_OK ||
            pfs_region_end(fs->sb.block_bitmap_start,
                           fs->sb.block_bitmap_blocks, &bitmap_end) !=
                PROSENJITFS_OK ||
            pfs_region_end(fs->sb.inode_bitmap_start,
                           fs->sb.inode_bitmap_blocks, &inode_bitmap_end) !=
                PROSENJITFS_OK ||
            pfs_region_end(fs->sb.inode_table_start,
                           fs->sb.inode_table_blocks, &inode_end) !=
                PROSENJITFS_OK ||
            journal_end > fs->sb.block_bitmap_start ||
            bitmap_end > fs->sb.inode_bitmap_start ||
            inode_bitmap_end > fs->sb.inode_table_start ||
            inode_end == UINT64_MAX ||
            fs->sb.data_start != inode_end + 1u ||
            fs->sb.data_start >= fs->sb.total_blocks)
            return PROSENJITFS_ECORRUPT;

        required_block_bitmap_blocks =
            fs->sb.total_blocks / bits_per_block +
            ((fs->sb.total_blocks % bits_per_block) != 0u ? 1u : 0u);
        required_inode_bitmap_blocks =
            (uint64_t)fs->sb.inode_count / bits_per_block +
            (((uint64_t)fs->sb.inode_count % bits_per_block) != 0u ? 1u : 0u);
        if ((uint64_t)fs->sb.block_bitmap_blocks <
                required_block_bitmap_blocks ||
            (uint64_t)fs->sb.inode_bitmap_blocks <
                required_inode_bitmap_blocks)
            return PROSENJITFS_ECORRUPT;
    }

    if ((uint64_t)fs->sb.inode_table_blocks * fs->sb.block_size <
        (uint64_t)fs->sb.inode_count * PROSENJITFS_INODE_SIZE)
        return PROSENJITFS_ECORRUPT;

    if (fs->sb.root_inode >= fs->sb.inode_count ||
        prosenjitfs_read_inode(fs, fs->sb.root_inode, &inode) !=
            PROSENJITFS_OK ||
        inode.type != PROSENJITFS_INODE_DIR ||
        inode.parent_inode != fs->sb.root_inode ||
        inode.dir_index < fs->sb.data_start ||
        inode.dir_index >= fs->sb.total_blocks)
        return PROSENJITFS_ECORRUPT;

    owner = (uint8_t *)kmalloc(owner_bytes);
    dir_refs = (uint8_t *)kmalloc(fs->sb.inode_count);
    if (!owner || !dir_refs) {
        if (owner) kfree(owner);
        if (dir_refs) kfree(dir_refs);
        return PROSENJITFS_EIO;
    }
    pfs_zero(owner, (uint32_t)owner_bytes);
    pfs_zero(dir_refs, fs->sb.inode_count);

    /*
     * Validate every inode bitmap entry, inode checksum/type/parent, every
     * directory index/entry chain, and every file extent chain. The owner
     * bitmap is intentionally built from metadata references and then
     * compared with the on-disk allocation bitmap below.
     */
    for (uint64_t inode_number = 0u;
         inode_number < fs->sb.inode_count; ++inode_number) {
        uint64_t bi = inode_number / bits_per_block;
        uint64_t within = inode_number % bits_per_block;
        uint64_t lba;
        int allocated;

        if (bi >= fs->sb.inode_bitmap_blocks ||
            pfs_lba_add(fs->mount.device, fs->sb.inode_bitmap_start,
                        bi, &lba) != PROSENJITFS_OK ||
            pfs_read_block(fs->mount.device, lba, bitmap) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            goto consistency_out;
        }

        allocated = pfs_bitmap_bit(bitmap, within);
        rc = prosenjitfs_read_inode(fs, inode_number, &inode);
        if (!allocated) {
            if (rc != PROSENJITFS_ENOENT) {
                rc = PROSENJITFS_ECORRUPT;
                goto consistency_out;
            }
            continue;
        }
        if (rc == PROSENJITFS_ENOENT) {
            /* After mount/recovery an allocated inode must have a valid
             * on-disk inode image. Reservation is an in-memory transaction
             * concern and must never survive as an apparently clean state. */
            rc = PROSENJITFS_ECORRUPT;
            goto consistency_out;
        }
        if (rc != PROSENJITFS_OK ||
            (inode.type != PROSENJITFS_INODE_FILE &&
             inode.type != PROSENJITFS_INODE_DIR &&
             inode.type != PROSENJITFS_INODE_SYMLINK) ||
            inode.parent_inode >= fs->sb.inode_count) {
            rc = PROSENJITFS_ECORRUPT;
            goto consistency_out;
        }

        if (inode_number != fs->sb.root_inode) {
            struct pros_prosenjitfs_inode parent_inode;
            rc = prosenjitfs_read_inode(fs, inode.parent_inode, &parent_inode);
            if (rc != PROSENJITFS_OK || parent_inode.type != PROSENJITFS_INODE_DIR) {
                rc = PROSENJITFS_ECORRUPT;
                goto consistency_out;
            }
        }

        if (inode.type == PROSENJITFS_INODE_DIR) {
            struct pros_prosenjitfs_dir_index index;
            if (inode.extent_head != 0u ||
                inode.dir_index < fs->sb.data_start ||
                inode.dir_index >= fs->sb.total_blocks ||
                pfs_consistency_mark(owner, fs->sb.total_blocks,
                                     inode.dir_index) != PROSENJITFS_OK ||
                pfs_read_dir_index(fs, &inode, &index) != PROSENJITFS_OK) {
                rc = PROSENJITFS_ECORRUPT;
                goto consistency_out;
            }

            for (uint32_t bucket = 0u;
                 bucket < PROSENJITFS_DIR_BUCKETS; ++bucket) {
                uint64_t current = index.heads[bucket];
                uint64_t steps = 0u;

                while (current != 0u) {
                    struct pros_prosenjitfs_dir_entry entry;
                    uint8_t entry_block[4096];

                    if (++steps > fs->sb.total_blocks ||
                        current < fs->sb.data_start ||
                        current >= fs->sb.total_blocks ||
                        pfs_consistency_mark(owner, fs->sb.total_blocks,
                                             current) != PROSENJITFS_OK ||
                        pfs_read_block(fs->mount.device, current,
                                       entry_block) != PROSENJITFS_OK) {
                        rc = PROSENJITFS_ECORRUPT;
                        goto consistency_out;
                    }
                    pfs_copy(&entry, entry_block, sizeof(entry));
                    if (entry.magic != PROSENJITFS_ENTRY_MAGIC ||
                        entry.name_length == 0u ||
                        entry.name_length > PROSENJITFS_MAX_NAME ||
                        entry.inode >= fs->sb.inode_count ||
                        entry.next >= fs->sb.total_blocks ||
                        (entry.next != 0u &&
                         entry.next < fs->sb.data_start) ||
                        pfs_crc_with_zero32(
                            entry_block, sizeof(entry),
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_dir_entry,
                                checksum)) != entry.checksum) {
                        rc = PROSENJITFS_ECORRUPT;
                        goto consistency_out;
                    }
                    {
                        struct pros_prosenjitfs_inode child;
                        int child_rc = prosenjitfs_read_inode(
                            fs, entry.inode, &child);
                        if (child_rc != PROSENJITFS_OK ||
                            child.parent_inode != inode_number ||
                            ((entry.type == PROSENJITFS_INODE_DIR) !=
                             (child.type == PROSENJITFS_INODE_DIR)) ||
                            (entry.type != PROSENJITFS_INODE_FILE &&
                             entry.type != PROSENJITFS_INODE_DIR)) {
                            rc = PROSENJITFS_ECORRUPT;
                            goto consistency_out;
                        }
                        if (dir_refs[entry.inode] == 0xFFu) {
                            rc = PROSENJITFS_EOVERFLOW;
                            goto consistency_out;
                        }
                        ++dir_refs[entry.inode];
                    }
                    current = entry.next;
                }
            }
        } else if (inode.type == PROSENJITFS_INODE_FILE) {
            uint64_t current = inode.extent_head;
            uint64_t steps = 0u;
            uint64_t allocated_bytes = 0u;
            uint64_t logical_end = 0u;
            uint64_t max_logical_blocks =
                inode.size / fs->sb.block_size +
                ((inode.size % fs->sb.block_size) != 0u ? 1u : 0u);

            while (current != 0u) {
                struct pros_prosenjitfs_extent extent;
                uint64_t end_logical;
                uint64_t physical;

                if (++steps > fs->sb.total_blocks ||
                    pfs_consistency_mark(owner, fs->sb.total_blocks,
                                         current) != PROSENJITFS_OK ||
                    pfs_read_extent(fs, current, &extent) != PROSENJITFS_OK) {
                    rc = PROSENJITFS_ECORRUPT;
                    goto consistency_out;
                }
                if (extent.logical_block < logical_end ||
                    extent.logical_block >
                        UINT64_MAX - extent.block_count ||
                    extent.logical_block + extent.block_count >
                        max_logical_blocks ||
                    extent.block_count > UINT64_MAX / fs->sb.block_size ||
                    allocated_bytes >
                        UINT64_MAX -
                        extent.block_count * fs->sb.block_size) {
                    rc = PROSENJITFS_ECORRUPT;
                    goto consistency_out;
                }
                for (uint64_t n = 0u; n < extent.block_count; ++n) {
                    physical = extent.physical_block + n;
                    if (physical < fs->sb.data_start ||
                        physical >= fs->sb.total_blocks ||
                        pfs_consistency_mark(owner, fs->sb.total_blocks,
                                             physical) != PROSENJITFS_OK) {
                        rc = PROSENJITFS_ECORRUPT;
                        goto consistency_out;
                    }
                }
                allocated_bytes +=
                    extent.block_count * fs->sb.block_size;
                end_logical = extent.logical_block + extent.block_count;
                logical_end = end_logical;
                current = extent.next;
            }
            if (allocated_bytes != inode.allocated_bytes) {
                rc = PROSENJITFS_ECORRUPT;
                goto consistency_out;
            }
        }
    }

    /* Bits beyond inode_count are permanently invalid and must never
     * survive as allocated state. This catches bitmap tail corruption that
     * cannot be observed by inode lookup alone. */
    for (uint64_t bi = 0u; bi < fs->sb.inode_bitmap_blocks; ++bi) {
        uint64_t lba;
        if (pfs_lba_add(fs->mount.device, fs->sb.inode_bitmap_start,
                        bi, &lba) != PROSENJITFS_OK ||
            pfs_read_block(fs->mount.device, lba, bitmap) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            goto consistency_out;
        }
        for (uint64_t bit = 0u; bit < bits_per_block; ++bit) {
            uint64_t inode_number = bi * bits_per_block + bit;
            if (inode_number >= fs->sb.inode_count &&
                pfs_bitmap_bit(bitmap, bit)) {
                rc = PROSENJITFS_ECORRUPT;
                goto consistency_out;
            }
        }
    }

    if (dir_refs[fs->sb.root_inode] != 0u) {
        rc = PROSENJITFS_ECORRUPT;
        goto consistency_out;
    }
    for (uint64_t inode_number = 1u;
         inode_number < fs->sb.inode_count; ++inode_number) {
        uint64_t bi = inode_number / bits_per_block;
        uint64_t within = inode_number % bits_per_block;
        uint64_t lba;
        int allocated;

        if (bi >= fs->sb.inode_bitmap_blocks ||
            pfs_lba_add(fs->mount.device, fs->sb.inode_bitmap_start,
                        bi, &lba) != PROSENJITFS_OK ||
            pfs_read_block(fs->mount.device, lba, bitmap) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            goto consistency_out;
        }
        allocated = pfs_bitmap_bit(bitmap, within);
        if (allocated && prosenjitfs_read_inode(fs, inode_number, &inode) ==
                            PROSENJITFS_OK &&
            dir_refs[inode_number] != 1u) {
            rc = PROSENJITFS_ECORRUPT;
            goto consistency_out;
        }
    }

    /*
     * Metadata blocks are never allocatable. Every block below data_start
     * must be marked used; every referenced data/extent/directory-entry block
     * must be marked used; and every allocated data bit must have an owner.
     */
    for (uint64_t bi = 0u; bi < fs->sb.block_bitmap_blocks; ++bi) {
        uint64_t lba;
        uint64_t first_bit = bi * bits_per_block;
        if (pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                        bi, &lba) != PROSENJITFS_OK ||
            pfs_read_block(fs->mount.device, lba, bitmap) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            goto consistency_out;
        }
        for (uint64_t bit = 0u; bit < bits_per_block; ++bit) {
            uint64_t block = first_bit + bit;
            int set = pfs_bitmap_bit(bitmap, bit);
            int owned = block < fs->sb.total_blocks
                      ? (owner[block >> 3] &
                         (uint8_t)(1u << (block & 7u))) != 0
                      : 0;
            if ((block < fs->sb.data_start && !set) ||
                (block >= fs->sb.total_blocks && set) ||
                (block >= fs->sb.data_start && block < fs->sb.total_blocks &&
                 set != owned)) {
                rc = PROSENJITFS_ECORRUPT;
                goto consistency_out;
            }
        }
    }

    rc = PROSENJITFS_OK;

consistency_out:
    kfree(owner);
    kfree(dir_refs);
    return rc;
}

int prosenjitfs_selftest(void)
{
    pfs_selftest_diag_stage = 0u;
    pfs_selftest_diag_rc = PROSENJITFS_OK;
    pfs_crash_inject_stage = 0u;
    pfs_crash_injected = 0u;
    static struct pfs_test_disk disk;
    struct pros_block_device device;
    struct pros_prosenjitfs fs = { 0 };
    struct pros_prosenjitfs_inode inode;
    uint64_t first_block;
    uint64_t inode_number;
    uint8_t saved;
    if (block_device_init(&device, 512u, 512u, pfs_test_read,
                          pfs_test_write, &disk) != PROS_BLOCK_OK)
        return pfs_selftest_fail(__LINE__, -1);
    if (block_device_set_flush(&device, pfs_test_flush) != PROS_BLOCK_OK)
        return pfs_selftest_fail(__LINE__, -2);
    /*
     * LBA contract regression: offsets are device-relative sector offsets,
     * and the checked addition must reject both arithmetic overflow and the
     * first sector outside the device.
     */
    {
        uint64_t lba = 0u;
        if (pfs_lba_add(&device, 0u, 0u, &lba) != PROSENJITFS_OK ||
            lba != 0u ||
            pfs_lba_add(&device, device.sector_count - 1u, 0u, &lba) !=
                PROSENJITFS_OK ||
            lba != device.sector_count - 1u ||
            pfs_lba_add(&device, device.sector_count - 1u, 1u, &lba) !=
                PROSENJITFS_EINVAL ||
            pfs_lba_add(&device, 1u, UINT64_MAX, &lba) !=
                PROSENJITFS_EOVERFLOW ||
            pfs_lba_add(&device, UINT64_MAX, 0u, &lba) !=
                PROSENJITFS_EINVAL)
            return pfs_selftest_fail(__LINE__, -2);
    }
    pfs_zero(&disk, sizeof(disk));
    if (prosenjitfs_format(&device) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -3);
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -4);
    if (fs.sb.root_inode != 0u ||
        prosenjitfs_read_inode(&fs, 0u, &inode) != PROSENJITFS_OK ||
        inode.type != PROSENJITFS_INODE_DIR ||
        inode.dir_index == 0u ||
        !fs.mount.root)
        return pfs_selftest_fail(__LINE__, -5);
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_EBUSY)
        return pfs_selftest_fail(__LINE__, -5);
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
            return pfs_selftest_fail(__LINE__, -5);
    }
    {
        struct pros_vfs_file file;
        uint8_t write_data[700];
        uint8_t read_data[700];
        uint32_t done = 0u;
        for (uint32_t i = 0u; i < sizeof(write_data); ++i)
            write_data[i] = (uint8_t)((i * 13u) ^ 0x5Au);

        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -6);
        if (vfs_file_write(&file, write_data, sizeof(write_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(write_data))
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -6);
            }
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -6);

        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -6);
        if (vfs_file_read(&file, 0, 0u, &done) != PROS_VFS_OK ||
            done != 0u)
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -6);
            }
        if (vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data))
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -6);
            }
        for (uint32_t i = 0u; i < sizeof(read_data); ++i)
            if (read_data[i] != write_data[i])
                return pfs_selftest_fail(__LINE__, -6);
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -6);
    }

    /*
     * Mount lifecycle regression: an open handle must pin the vnode tree and
     * make unmount fail with EBUSY. After the handle closes, the same mount
     * object must unmount cleanly; this prevents stale-handle resurrection
     * after an immediate remount.
     */
    {
        struct pros_vfs_file file;
        uint8_t value = 0xA7u;
        uint8_t read_value = 0u;
        uint32_t done = 0u;

        if (vfs_file_open(&fs.mount, "/lifecycle.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, &value, 1u, &done) != PROS_VFS_OK ||
            done != 1u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -17);

        if (vfs_file_open(&fs.mount, "/lifecycle.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -17);

        if (prosenjitfs_unmount(&fs) != PROSENJITFS_EBUSY)
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -17);
            }

        if (vfs_file_read(&file, &read_value, 1u, &done) != PROS_VFS_OK ||
            done != 1u || read_value != value ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -17);
    }

    /* Append semantics must use the current EOF even after an explicit seek. */
    {
        struct pros_vfs_file file;
        static const uint8_t first[] = { 'a', 'b', 'c' };
        static const uint8_t second[] = { 'D', 'E', 'F' };
        uint8_t combined[6];
        uint32_t done = 0u;
        if (vfs_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) != PROS_VFS_OK ||
            vfs_file_write(&file, first, sizeof(first), &done) != PROS_VFS_OK ||
            done != sizeof(first) || vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        if (vfs_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_APPEND, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
            vfs_file_write(&file, second, sizeof(second), &done) != PROS_VFS_OK ||
            done != sizeof(second) || file.position != 6u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        if (vfs_file_open(&fs.mount, "/append.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, combined, sizeof(combined), &done) != PROS_VFS_OK ||
            done != sizeof(combined) || vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        for (uint32_t i = 0u; i < 6u; ++i)
            if (combined[i] != (i < 3u ? first[i] : second[i - 3u]))
                return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * VFS negative/error-contract matrix: invalid flags, missing paths,
     * directory-as-file, overflow seek/truncate, and read-only writes must
     * remain distinguishable instead of collapsing into generic I/O errors.
     */
    {
        struct pros_vfs_file file;
        uint8_t byte = 0x5Au;
        uint32_t done = 0u;

        if (vfs_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_APPEND, &file) != PROS_VFS_EINVAL ||
            vfs_file_open(&fs.mount, "/does-not-exist", PROS_VFS_O_READ,
                          &file) != PROS_VFS_ENOENT ||
            vfs_file_open(&fs.mount, "/", PROS_VFS_O_READ, &file) !=
                PROS_VFS_EISDIR)
            return pfs_selftest_fail(__LINE__, -18);

        if (vfs_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_write(&file, &byte, 1u, &done) != PROS_VFS_EPERM ||
            vfs_file_seek(&file, UINT64_MAX) != PROS_VFS_EOVERFLOW ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -18);

        if (vfs_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
            vfs_file_truncate(&file, UINT64_MAX) != PROS_VFS_EOVERFLOW ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -18);
    }

    /* Sparse logical-offset regression: seek beyond EOF, write one byte, and
     * verify the unwritten interval reads as zeros. */
    {
        struct pros_vfs_file file;
        uint8_t marker = 0xD6u;
        uint8_t hole[512];
        uint32_t done = 0u;
        if (vfs_file_open(&fs.mount, "/sparse.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 4096u) != PROS_VFS_OK ||
            vfs_file_write(&file, &marker, 1u, &done) != PROS_VFS_OK ||
            done != 1u || file.position != 4097u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);
        if (vfs_file_open(&fs.mount, "/sparse.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 3584u) != PROS_VFS_OK ||
            vfs_file_read(&file, hole, sizeof(hole), &done) != PROS_VFS_OK ||
            done != sizeof(hole)) {
            vfs_file_close(&file);
            return pfs_selftest_fail(__LINE__, -14);
        }
        for (uint32_t i = 0u; i < sizeof(hole); ++i)
            if (hole[i] != 0u) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -14);
            }
        if (vfs_file_seek(&file, 4096u) != PROS_VFS_OK ||
            vfs_file_read(&file, &marker, 1u, &done) != PROS_VFS_OK ||
            done != 1u || marker != 0xD6u || vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);
    }

    /* Sparse extent ordering regression: populate two separated logical
     * blocks, then fill the hole. The new extent must be inserted between
     * them rather than appended after the higher logical extent. */
    {
        struct pros_vfs_file file;
        uint8_t marker_a = 0xA1u;
        uint8_t marker_b = 0xB2u;
        uint32_t done = 0u;
        uint32_t bs = fs.sb.block_size;
        if (bs == 0u || bs > 4096u ||
            vfs_file_open(&fs.mount, "/sparse-order.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_seek(&file, (uint64_t)bs * 4u) != PROS_VFS_OK ||
            vfs_file_write(&file, &marker_b, 1u, &done) != PROS_VFS_OK ||
            done != 1u ||
            vfs_file_seek(&file, (uint64_t)bs * 2u) != PROS_VFS_OK ||
            vfs_file_write(&file, &marker_a, 1u, &done) != PROS_VFS_OK ||
            done != 1u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);

        if (vfs_file_open(&fs.mount, "/sparse-order.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, (uint64_t)bs * 2u) != PROS_VFS_OK ||
            vfs_file_read(&file, &marker_a, 1u, &done) != PROS_VFS_OK ||
            done != 1u || marker_a != 0xA1u ||
            vfs_file_seek(&file, (uint64_t)bs * 4u) != PROS_VFS_OK ||
            vfs_file_read(&file, &marker_b, 1u, &done) != PROS_VFS_OK ||
            done != 1u || marker_b != 0xB2u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);
    }

    if (prosenjitfs_alloc_blocks(&fs, 4u, &first_block) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -6);
    if (first_block < fs.sb.data_start ||
        first_block + 4u > fs.sb.total_blocks)
        return pfs_selftest_fail(__LINE__, -6);
    if (prosenjitfs_alloc_inode(&fs, &inode_number) != PROSENJITFS_OK ||
        inode_number == fs.sb.root_inode ||
        inode_number >= fs.sb.inode_count)
        return pfs_selftest_fail(__LINE__, -7);
    if (prosenjitfs_read_inode(&fs, inode_number, &inode) != PROSENJITFS_ENOENT)
        return pfs_selftest_fail(__LINE__, -8);
    if (prosenjitfs_free_blocks(&fs, fs.sb.total_blocks, 1u) != PROSENJITFS_EINVAL)
        return pfs_selftest_fail(__LINE__, -9);
    if (prosenjitfs_free_blocks(&fs, first_block, 4u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -10);
    if (prosenjitfs_free_blocks(&fs, first_block, 1u) != PROSENJITFS_EINVAL)
        return pfs_selftest_fail(__LINE__, -10);
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -11);

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    /* Persistence checks for append and sparse files after remount. */
    {
        struct pros_vfs_file file;
        uint8_t append_data[6];
        uint8_t sparse_data[513];
        uint32_t done = 0u;
        if (vfs_file_open(&fs.mount, "/append.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, append_data, sizeof(append_data), &done) != PROS_VFS_OK ||
            done != sizeof(append_data) || vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);
        if (append_data[0] != 'a' || append_data[1] != 'b' || append_data[2] != 'c' ||
            append_data[3] != 'D' || append_data[4] != 'E' || append_data[5] != 'F')
            return pfs_selftest_fail(__LINE__, -15);
        if (vfs_file_open(&fs.mount, "/sparse.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 3584u) != PROS_VFS_OK ||
            vfs_file_read(&file, sparse_data, sizeof(sparse_data), &done) != PROS_VFS_OK ||
            done != sizeof(sparse_data)) {
            vfs_file_close(&file);
            return pfs_selftest_fail(__LINE__, -15);
        }
        for (uint32_t i = 0u; i < 512u; ++i)
            if (sparse_data[i] != 0u) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -15);
            }
        if (sparse_data[512] != 0xD6u || vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);
    }
    {
        struct pros_vfs_file file;
        uint8_t read_data[700];
        uint32_t done = 0u;
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK) {
            prosenjitfs_unmount(&fs);
            return pfs_selftest_fail(__LINE__, -12);
        }
        if (vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data)) {
            vfs_file_close(&file);
            prosenjitfs_unmount(&fs);
            return pfs_selftest_fail(__LINE__, -12);
        }
        if (vfs_file_close(&file) != PROS_VFS_OK) {
            prosenjitfs_unmount(&fs);
            return pfs_selftest_fail(__LINE__, -12);
        }
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE, &file) !=
                PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        /*
         * A write-only handle must reject read attempts at the VFS layer.
         */
        {
            uint8_t denied_read[1];
            uint32_t denied_done = 0u;

            if (vfs_file_read(&file, denied_read, sizeof(denied_read),
                              &denied_done) != PROS_VFS_EPERM ||
                denied_done != 0u) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }
        }

        {
            uint8_t patch[37];
            uint32_t wrote = 0u;
            for (uint32_t i = 0u; i < sizeof(patch); ++i)
                patch[i] = (uint8_t)(0xA0u + i);
            if (vfs_file_write(&file, patch, sizeof(patch), &wrote) !=
                    PROS_VFS_OK ||
                wrote != sizeof(patch))
                {
                    vfs_file_close(&file);
                    return pfs_selftest_fail(__LINE__, -12);
                }
            if (vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -12);
        }


        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        if (vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data)) {
            vfs_file_close(&file);
            return pfs_selftest_fail(__LINE__, -12);
        }
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            uint8_t expected = i < 37u
                ? (uint8_t)(0xA0u + i)
                : (uint8_t)((i * 13u) ^ 0x5Au);
            if (read_data[i] != expected) {
                prosenjitfs_unmount(&fs);
                return pfs_selftest_fail(__LINE__, -12);
            }
        }

        /*
         * Explicitly exercise seek/read/write across the 512-byte block
         * boundary. The file is 700 bytes, so offset 500 + 200 bytes
         * crosses from logical block 0 into logical block 1.
         */
        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        if (vfs_file_seek(&file, 500u) != PROS_VFS_OK)
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }

        {
            uint8_t partial[200];
            uint32_t partial_done = 0u;

            if (vfs_file_read(&file, partial, sizeof(partial),
                              &partial_done) != PROS_VFS_OK ||
                partial_done != sizeof(partial)) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }

            for (uint32_t i = 0u; i < sizeof(partial); ++i) {
                uint32_t absolute = 500u + i;
                uint8_t expected = (absolute < 37u)
                    ? (uint8_t)(0xA0u + absolute)
                    : (uint8_t)((absolute * 13u) ^ 0x5Au);

                if (partial[i] != expected) {
                    vfs_file_close(&file);
                    return pfs_selftest_fail(__LINE__, -12);
                }
            }
        }

        /*
         * Seek beyond EOF must fail without changing the current position.
         */
        if (file.position != 700u ||
            vfs_file_seek(&file, 701u) != PROS_VFS_EOVERFLOW ||
            file.position != 700u) {
            vfs_file_close(&file);
            return pfs_selftest_fail(__LINE__, -12);
        }

        /*
         * A read beginning exactly at EOF is a successful zero-byte read.
         */
        if (vfs_file_seek(&file, 700u) != PROS_VFS_OK)
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }

        {
            uint8_t eof_byte = 0xFFu;
            uint32_t eof_done = 99u;

            if (vfs_file_read(&file, &eof_byte, 1u, &eof_done) !=
                    PROS_VFS_OK ||
                eof_done != 0u) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }
        }

        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        /*
         * Partial write starting at offset 500 must cross the 512-byte
         * boundary while preserving the file size.
         */
        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        if (vfs_file_seek(&file, 500u) != PROS_VFS_OK)
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }

        {
            uint8_t boundary_patch[37];
            uint32_t boundary_done = 0u;

            for (uint32_t i = 0u; i < sizeof(boundary_patch); ++i)
                boundary_patch[i] = (uint8_t)(0xD0u + i);

            if (vfs_file_write(&file, boundary_patch,
                               sizeof(boundary_patch),
                               &boundary_done) != PROS_VFS_OK ||
                boundary_done != sizeof(boundary_patch) ||
                file.position != 537u ||
                file.vnode->size != 700u) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }
        }

        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        /*
         * Verify the cross-boundary write immediately through the VFS.
         */
        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        if (vfs_file_seek(&file, 500u) != PROS_VFS_OK)
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }

        {
            uint8_t boundary_read[37];
            uint32_t boundary_read_done = 0u;

            if (vfs_file_read(&file, boundary_read,
                              sizeof(boundary_read),
                              &boundary_read_done) != PROS_VFS_OK ||
                boundary_read_done != sizeof(boundary_read)) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }

            for (uint32_t i = 0u; i < sizeof(boundary_read); ++i)
                if (boundary_read[i] != (uint8_t)(0xD0u + i)) {
                    vfs_file_close(&file);
                    return pfs_selftest_fail(__LINE__, -12);
                }
        }

        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        if (vfs_file_open(&fs.mount, "/hello.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        if (vfs_file_truncate(&file, 100u) != PROS_VFS_OK ||
            file.vnode->size != 100u)
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        if (prosenjitfs_read_inode(&fs, 1u, &inode) != PROSENJITFS_OK ||
            inode.size != 100u ||
            inode.allocated_bytes != fs.sb.block_size ||
            inode.extent_head == 0u)
            return pfs_selftest_fail(__LINE__, -12);
    }
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    {
        struct pros_vfs_file file;
        uint8_t read_data[100];
        uint32_t done = 0u;
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        if (vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data)) {
            vfs_file_close(&file);
            return pfs_selftest_fail(__LINE__, -12);
        }
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            uint8_t expected = i < 37u
                ? (uint8_t)(0xA0u + i)
                : (uint8_t)((i * 13u) ^ 0x5Au);
            if (read_data[i] != expected) {
                prosenjitfs_unmount(&fs);
                return pfs_selftest_fail(__LINE__, -12);
            }
        }
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE, &file) !=
                PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        if (vfs_file_truncate(&file, 0u) != PROS_VFS_OK)
            {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        if (prosenjitfs_read_inode(&fs, 1u, &inode) != PROSENJITFS_OK ||
            inode.size != 0u || inode.extent_head != 0u ||
            inode.allocated_bytes != 0u)
            return pfs_selftest_fail(__LINE__, -12);
    }
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    {
        struct pros_vfs_file file;
        uint8_t read_data[1];
        uint32_t done = 99u;
        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        if (vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != 0u) {
            vfs_file_close(&file);
            return pfs_selftest_fail(__LINE__, -12);
        }
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
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
            return pfs_selftest_fail(__LINE__, -12);
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
            return pfs_selftest_fail(__LINE__, -12);
        {
            int rc;

            rc = prosenjitfs_unmount(&fs);
            if (rc != PROSENJITFS_OK)
                return pfs_selftest_fail(41731u, -12);

            rc = prosenjitfs_mount(&fs, &device, 0u);
            if (rc != PROSENJITFS_OK)
                return pfs_selftest_fail(41732u, -12);

            hidden = 0;
            rc = vfs_path_lookup(&fs.mount, "/hello.bin", &hidden);
            if (rc != PROS_VFS_ENOENT)
                return pfs_selftest_fail(41733u, -12);

            if (hidden != 0)
                return pfs_selftest_fail(41734u, -12);

            rc = vfs_path_lookup(&fs.mount, "/.hello.bin", &hidden);
            if (rc != PROS_VFS_OK)
                return pfs_selftest_fail(41735u, -12);

            if (!hidden)
                return pfs_selftest_fail(41736u, -12);

            rc = vfs_vnode_put(hidden);
            if (rc != PROS_VFS_OK)
                return pfs_selftest_fail(41737u, -12);

            rc = vfs_rename(&fs.mount, "/.hello.bin", "/hello.bin");
            if (rc != PROS_VFS_OK)
                return pfs_selftest_fail(41738u, -12);
        }
    }
    /*
     * Stale-handle regression: unlink immediately invalidates an inode
     * generation, but the open VFS object remains safe to close. Reuse of
     * the same inode number must never make the old handle target the new
     * file.
     */
    {
        struct pros_vfs_file stale;
        struct pros_vfs_file replacement;
        uint8_t value = 0x31u;
        uint8_t replacement_value = 0xA7u;
        uint8_t verify = 0u;
        uint32_t done = 0u;

        if (vfs_file_open(&fs.mount, "/stale.bin",
                          PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                          PROS_VFS_O_CREATE, &stale) != PROS_VFS_OK ||
            vfs_file_write(&stale, &value, 1u, &done) != PROS_VFS_OK ||
            done != 1u)
            return pfs_selftest_fail(__LINE__, -13);

        if (vfs_unlink(&fs.mount, "/stale.bin") != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_file_read(&stale, &verify, 1u, &done) != PROS_VFS_ESTALE ||
            vfs_file_write(&stale, &value, 1u, &done) != PROS_VFS_ESTALE ||
            vfs_file_truncate(&stale, 0u) != PROS_VFS_ESTALE)
            return pfs_selftest_fail(__LINE__, -13);

        if (vfs_file_open(&fs.mount, "/stale.bin",
                          PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                          PROS_VFS_O_CREATE, &replacement) != PROS_VFS_OK ||
            vfs_file_write(&replacement, &replacement_value, 1u, &done) !=
                PROS_VFS_OK ||
            done != 1u ||
            vfs_file_close(&replacement) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_file_read(&stale, &verify, 1u, &done) != PROS_VFS_ESTALE ||
            vfs_file_write(&stale, &value, 1u, &done) != PROS_VFS_ESTALE ||
            vfs_file_truncate(&stale, 0u) != PROS_VFS_ESTALE ||
            vfs_file_close(&stale) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }
    if (vfs_unlink(&fs.mount, "/hello.bin") != PROS_VFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    {
        struct pros_vfs_vnode *deleted = 0;
        if (vfs_path_lookup(&fs.mount, "/hello.bin", &deleted) !=
                PROS_VFS_ENOENT)
            return pfs_selftest_fail(__LINE__, -12);
        if (deleted)
            vfs_vnode_put(deleted);
    }
    if (prosenjitfs_read_inode(&fs, 1u, &inode) != PROSENJITFS_ENOENT)
        return pfs_selftest_fail(__LINE__, -12);
    if (prosenjitfs_alloc_inode(&fs, &inode_number) != PROSENJITFS_OK ||
        inode_number != 1u)
        return pfs_selftest_fail(__LINE__, -12);
    if (pfs_inode_bitmap_update(&fs, inode_number, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    if (prosenjitfs_read_inode(&fs, 1u, &inode) != PROSENJITFS_ENOENT)
        return pfs_selftest_fail(__LINE__, -12);
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);


    /*
     * Full persistence/namespace/allocation regression.
     *
     * This deliberately crosses clean unmount/remount boundaries so that
     * every assertion observes the durable image rather than live VFS state.
     */
    {
        struct pros_vfs_file file;
        uint8_t data[900];
        uint8_t small_data[32];
        uint32_t done = 0u;
        uint64_t allocated_block = 0u;
        uint64_t allocated_inode = 0u;
        uint64_t inode_before;
        uint64_t rename_inode_before;
        struct pros_vfs_vnode *node = 0;
        struct pfs_vnode_data *node_data;

        for (uint32_t i = 0u; i < sizeof(data); ++i)
            data[i] = (uint8_t)((i * 29u) ^ 0xD6u);
        for (uint32_t i = 0u; i < sizeof(small_data); ++i)
            small_data[i] = (uint8_t)(0xA0u + i);

        if (vfs_mkdir(&fs.mount, "/persist") != PROS_VFS_OK ||
            vfs_mkdir(&fs.mount, "/persist/config") != PROS_VFS_OK ||
            vfs_mkdir(&fs.mount, "/persist/config/sub") != PROS_VFS_OK ||
            vfs_file_open(&fs.mount, "/persist/config/sub/tree.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, data, sizeof(data), &done) != PROS_VFS_OK ||
            done != sizeof(data) ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_file_open(&fs.mount, "/persist/config/rename.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, small_data, sizeof(small_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(small_data) ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/persist/config/sub/tree.bin",
                            &node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        node_data = (struct pfs_vnode_data *)node->private_data;
        if (!node_data || prosenjitfs_read_inode(&fs, node_data->inode_number,
                                                 &inode) != PROSENJITFS_OK)
            {
                vfs_vnode_put(node);
                return pfs_selftest_fail(__LINE__, -15);
            }
        inode_before = node_data->inode_number;
        if (vfs_vnode_put(node) != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/persist/config/rename.bin",
                            &node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);
        node_data = (struct pfs_vnode_data *)node->private_data;
        if (!node_data) {
            vfs_vnode_put(node);
            return pfs_selftest_fail(__LINE__, -15);
        }
        rename_inode_before = node_data->inode_number;
        if (vfs_vnode_put(node) != PROS_VFS_OK ||
            vfs_rename(&fs.mount, "/persist/config/rename.bin",
                       "/persist/config/sub/renamed.bin") != PROS_VFS_OK ||
            vfs_file_open(&fs.mount, "/persist/config/sub/tree.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
            vfs_file_truncate(&file, 513u) != PROS_VFS_OK ||
            vfs_file_seek(&file, 513u) != PROS_VFS_OK ||
            vfs_file_write(&file, small_data, sizeof(small_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(small_data) ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        if (pfs_selftest_consistency(&fs) != PROSENJITFS_OK ||
            vfs_path_lookup(&fs.mount, "/persist/config/sub/tree.bin",
                            &node) != PROS_VFS_OK ||
            node->size != 513u + sizeof(small_data) ||
            vfs_vnode_put(node) != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/persist/config/sub/renamed.bin",
                            &node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);
        node_data = (struct pfs_vnode_data *)node->private_data;
        if (!node_data || node_data->inode_number != rename_inode_before ||
            node_data->inode_number == inode_before ||
            vfs_vnode_put(node) != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/persist/config",
                            &node) != PROS_VFS_OK ||
            vfs_vnode_put(node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        done = 0u;
        if (vfs_file_open(&fs.mount, "/persist/config/sub/tree.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, data, sizeof(data), &done) != PROS_VFS_OK ||
            done != 545u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        if (vfs_unlink(&fs.mount, "/persist/config/sub/renamed.bin") !=
                PROS_VFS_OK ||
            vfs_rmdir(&fs.mount, "/persist/config/sub") != PROS_VFS_ENOTEMPTY)
            return pfs_selftest_fail(__LINE__, -15);

        if (vfs_unlink(&fs.mount, "/persist/config/sub/tree.bin") !=
                PROS_VFS_OK ||
            vfs_rmdir(&fs.mount, "/persist/config/sub") != PROS_VFS_OK ||
            vfs_rmdir(&fs.mount, "/persist/config") != PROS_VFS_OK ||
            vfs_rmdir(&fs.mount, "/persist") != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);


        /*
         * Boundary-size persistence matrix: exercise empty, byte, sector and
         * block edges across independent unmount/remount cycles.
         */
        {
            static const uint32_t sizes[] = {
                0u, 1u, 511u, 512u, 513u, 1023u, 1024u, 1025u
            };
            uint8_t size_data[1025];
            uint8_t read_sizes[1025];
            for (uint32_t i = 0u; i < sizeof(size_data); ++i)
                size_data[i] = (uint8_t)((i * 31u) ^ 0x5Au);

            for (uint32_t si = 0u;
                 si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
                uint32_t size = sizes[si];
                uint32_t bytes = 0u;

                if (vfs_file_open(&fs.mount, "/sizes.bin",
                                  PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                                  &file) != PROS_VFS_OK ||
                    vfs_file_truncate(&file, size) != PROS_VFS_OK ||
                    vfs_file_seek(&file, 0u) != PROS_VFS_OK)
                    return pfs_selftest_fail(__LINE__, -15);
                if (size != 0u &&
                    (vfs_file_write(&file, size_data, size, &bytes) !=
                         PROS_VFS_OK ||
                     bytes != size)) {
                    vfs_file_close(&file);
                    return pfs_selftest_fail(__LINE__, -15);
                }
                if (vfs_file_close(&file) != PROS_VFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -15);

                bytes = 0u;
                if (vfs_file_open(&fs.mount, "/sizes.bin",
                                  PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                    vfs_file_read(&file, read_sizes, sizeof(read_sizes), &bytes) !=
                        PROS_VFS_OK ||
                    bytes != size) {
                    vfs_file_close(&file);
                    return pfs_selftest_fail(__LINE__, -15);
                }
                for (uint32_t i = 0u; i < size; ++i) {
                    if (read_sizes[i] != size_data[i]) {
                        vfs_file_close(&file);
                        return pfs_selftest_fail(__LINE__, -15);
                    }
                }
                if (vfs_file_close(&file) != PROS_VFS_OK ||
                    pfs_selftest_consistency(&fs) != PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -15);
            }
        }

        if (prosenjitfs_alloc_blocks(&fs, 2u, &allocated_block) !=
                PROSENJITFS_OK ||
            prosenjitfs_alloc_inode(&fs, &allocated_inode) !=
                PROSENJITFS_OK ||
            pfs_selftest_consistency(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        {
            uint64_t bits_per_block = (uint64_t)fs.sb.block_size * 8u;
            uint8_t bitmap[4096];
            uint64_t lba;
            if (allocated_block < fs.sb.data_start ||
                allocated_block + 2u > fs.sb.total_blocks ||
                pfs_lba_add(&device, fs.sb.block_bitmap_start,
                            allocated_block / bits_per_block, &lba) !=
                    PROSENJITFS_OK ||
                pfs_read_block(&device, lba, bitmap) != PROSENJITFS_OK ||
                !pfs_bitmap_bit(bitmap, allocated_block % bits_per_block))
                return pfs_selftest_fail(__LINE__, -15);
        }

        if (prosenjitfs_free_blocks(&fs, allocated_block, 2u) !=
                PROSENJITFS_OK ||
            pfs_selftest_consistency(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        {
            uint64_t bits_per_block = (uint64_t)fs.sb.block_size * 8u;
            uint8_t bitmap[4096];
            uint64_t lba;
            if (pfs_lba_add(&device, fs.sb.block_bitmap_start,
                            allocated_block / bits_per_block, &lba) !=
                    PROSENJITFS_OK ||
                pfs_read_block(&device, lba, bitmap) != PROSENJITFS_OK ||
                pfs_bitmap_bit(bitmap, allocated_block % bits_per_block))
                return pfs_selftest_fail(__LINE__, -15);
        }

        if (prosenjitfs_alloc_blocks(&fs, 2u, &first_block) !=
                PROSENJITFS_OK ||
            prosenjitfs_free_blocks(&fs, first_block, 2u) != PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, allocated_inode, &inode) !=
                PROSENJITFS_ENOENT)
            return pfs_selftest_fail(__LINE__, -15);

        /*
         * Negative/boundary arithmetic is exercised against the real device
         * contract, including end-of-device and UINT64_MAX overflow cases.
         */
        {
            uint64_t lba;
            uint8_t block[512];
            struct pros_prosenjitfs_extent invalid_extent;
            if (pfs_lba_add(&device, device.sector_count - 1u, 0u, &lba) !=
                    PROSENJITFS_OK ||
                pfs_lba_add(&device, device.sector_count - 1u, 1u, &lba) !=
                    PROSENJITFS_EINVAL ||
                pfs_lba_add(&device, 0u, UINT64_MAX, &lba) !=
                    PROSENJITFS_EOVERFLOW ||
                pfs_read_block(&device, device.sector_count, block) !=
                    PROSENJITFS_EINVAL ||
                pfs_write_block(&device, device.sector_count, block) !=
                    PROSENJITFS_EINVAL ||
                pfs_inode_block_location(&fs, fs.sb.inode_count,
                                          &lba, (uint32_t *)&done) !=
                    PROSENJITFS_EINVAL ||
                pfs_inode_block_location(&fs, UINT64_MAX, &lba,
                                          (uint32_t *)&done) !=
                    PROSENJITFS_EINVAL ||
                prosenjitfs_read_inode(&fs, fs.sb.inode_count, &inode) !=
                    PROSENJITFS_EINVAL ||
                pfs_read_extent(&fs, fs.sb.total_blocks, &invalid_extent) !=
                    PROSENJITFS_ECORRUPT)
                return pfs_selftest_fail(__LINE__, -15);
        }

        if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE, &file) !=
                PROS_VFS_OK ||
            vfs_file_truncate(&file, UINT64_MAX) != PROS_VFS_EOVERFLOW ||
            vfs_file_truncate(&file, 0x100000000ULL) != PROS_VFS_EOVERFLOW ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        /*
         * Semantic corruption: preserve the inode checksum while replacing
         * its extent head with an impossible filesystem block. Mount can
         * remain structurally valid, but the first data access must fail
         * closed rather than touching the device out of range.
         */
        if (vfs_file_open(&fs.mount, "/corrupt.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, small_data, sizeof(small_data), &done) !=
                PROS_VFS_OK ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/corrupt.bin", &node) !=
                PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -16);
        node_data = (struct pfs_vnode_data *)node->private_data;
        if (!node_data ||
            prosenjitfs_read_inode(&fs, node_data->inode_number, &inode) !=
                PROSENJITFS_OK)
            {
                vfs_vnode_put(node);
                return pfs_selftest_fail(__LINE__, -16);
            }
        inode_before = node_data->inode_number;
        inode.extent_head = fs.sb.total_blocks;
        if (vfs_vnode_put(node) != PROS_VFS_OK ||
            prosenjitfs_write_inode(&fs, inode_before, &inode) !=
                PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -16);

        done = 0u;
        if (vfs_file_open(&fs.mount, "/corrupt.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, data, sizeof(small_data), &done) !=
                PROS_VFS_ECORRUPT ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -16);

        if (pfs_selftest_consistency(&fs) == PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -16);

        /*
         * Restore a clean image before the journal/crash matrix. This keeps
         * the negative corruption fixture isolated from subsequent tests.
         */
        pfs_zero(&disk, sizeof(disk));
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            pfs_selftest_consistency(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -16);
    }

    /*
     * Tiny-image allocation exhaustion.  This uses the real allocator and
     * journal on a minimum-size filesystem, then verifies recovery/reuse.
     */
    {
        struct pfs_test_disk small_disk;
        struct pros_block_device small_device;
        struct pros_prosenjitfs small_fs;
        uint64_t block;
        uint64_t first_reused = 0u;
        uint32_t allocated = 0u;
        uint64_t inode_tmp;

        pfs_zero(&small_disk, sizeof(small_disk));
        if (block_device_init(&small_device, 512u, PFS_MIN_BLOCKS,
                              pfs_test_read, pfs_test_write, &small_disk) !=
                PROS_BLOCK_OK ||
            block_device_set_flush(&small_device, pfs_test_flush) !=
                PROS_BLOCK_OK ||
            prosenjitfs_format(&small_device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&small_fs, &small_device, 0u) !=
                PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -17);

        for (;;) {
            int rc = prosenjitfs_alloc_blocks(&small_fs, 1u, &block);
            if (rc == PROSENJITFS_ENOSPC)
                break;
            if (rc != PROSENJITFS_OK)
                {
                    prosenjitfs_unmount(&small_fs);
                    return pfs_selftest_fail(__LINE__, -17);
                }
            if (++allocated > PFS_MIN_BLOCKS)
                {
                    prosenjitfs_unmount(&small_fs);
                    return pfs_selftest_fail(__LINE__, -17);
                }
        }
        if (allocated == 0u ||
            pfs_selftest_consistency(&small_fs) != PROSENJITFS_OK)
            {
                prosenjitfs_unmount(&small_fs);
                return pfs_selftest_fail(__LINE__, -17);
            }

        if (prosenjitfs_free_blocks(&small_fs, block, 1u) !=
                PROSENJITFS_OK ||
            prosenjitfs_alloc_blocks(&small_fs, 1u, &first_reused) !=
                PROSENJITFS_OK ||
            first_reused != block ||
            prosenjitfs_alloc_inode(&small_fs, &inode_tmp) !=
                PROSENJITFS_OK)
            {
                prosenjitfs_unmount(&small_fs);
                return pfs_selftest_fail(__LINE__, -17);
            }

        {
            uint32_t inode_allocated = 1u;
            for (;;) {
                int rc = prosenjitfs_alloc_inode(&small_fs, &inode_tmp);
                if (rc == PROSENJITFS_ENOSPC)
                    break;
                if (rc != PROSENJITFS_OK ||
                    ++inode_allocated > small_fs.sb.inode_count) {
                    prosenjitfs_unmount(&small_fs);
                    return pfs_selftest_fail(__LINE__, -17);
                }
            }
            if (inode_allocated < small_fs.sb.inode_count - 1u ||
                pfs_selftest_consistency(&small_fs) != PROSENJITFS_OK) {
                prosenjitfs_unmount(&small_fs);
                return pfs_selftest_fail(__LINE__, -17);
            }
        }

        if (prosenjitfs_unmount(&small_fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -17);

        /*
         * A partition view that is one sector shorter than the durable
         * filesystem must be rejected before any filesystem I/O can cross
         * the partition boundary.
         */
        {
            struct pros_partition_device view;
            struct pros_partition part;
            pfs_zero(&view, sizeof(view));
            pfs_zero(&part, sizeof(part));
            part.first_lba = 0u;
            part.sector_count = PFS_MIN_BLOCKS - 1u;
            if (partition_device_init(&view, &small_device, &part) !=
                    PROS_PARTITION_OK ||
                prosenjitfs_mount(&small_fs, &view.device, 0u) !=
                    PROSENJITFS_ECORRUPT) {
                pfs_zero(&small_fs, sizeof(small_fs));
                return pfs_selftest_fail(__LINE__, -17);
            }
        }

        if (prosenjitfs_mount(&small_fs, &small_device, 0u) !=
                PROSENJITFS_OK ||
            pfs_selftest_consistency(&small_fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&small_fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -17);
    }

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
            return pfs_selftest_fail(__LINE__, -12);

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
            return pfs_selftest_fail(__LINE__, -12);
        for (uint32_t i = 0u; i < sizeof(verify); ++i)
            if (verify[i] != image[i])
                return pfs_selftest_fail(__LINE__, -12);

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
            return pfs_selftest_fail(__LINE__, -12);
        for (uint32_t i = 0u; i < sizeof(verify); ++i)
            if (verify[i] != (uint8_t)(0xC3u ^ (uint8_t)i))
                return pfs_selftest_fail(__LINE__, -12);

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
            return pfs_selftest_fail(__LINE__, -12);

        pfs_zero(header, sizeof(header));
        for (uint32_t i = 0u; i < journal_blocks; ++i)
            if (pfs_write_block(&device, journal_start + i, header) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -12);
        if (pfs_flush(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            pfs_read_block(&device, recovery_block, verify) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        for (uint32_t i = 0u; i < sizeof(verify); ++i)
            if (verify[i] != (uint8_t)(0xC3u ^ (uint8_t)i))
                return pfs_selftest_fail(__LINE__, -12);
        if (prosenjitfs_free_blocks(&fs, recovery_block, 1u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
    }
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);

    /*
     * Crash/power-loss transaction matrix.
     *
     * The test disk now has durable media plus a volatile write cache.
     * Each iteration performs a real VFS file overwrite and injects an
     * interruption at one transaction boundary. The crash reset discards
     * unflushed device-cache contents, drops the in-memory filesystem state,
     * and remounts the same durable medium.
     *
     * Stages 1-4 occur before a durable commit and must leave the old data.
     * Stages 5-9 occur after the commit is durable and must recover the new
     * data from the redo journal, including the case where a home write was
     * lost by the simulated power loss.
     */
    {
        static const uint32_t crash_stages[] = {
            1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u
        };
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];

        for (uint32_t stage_index = 0u;
             stage_index < sizeof(crash_stages) / sizeof(crash_stages[0]);
             ++stage_index) {
            uint32_t stage = crash_stages[stage_index];
            struct pros_vfs_file file;
            uint32_t done = 0u;
            int write_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;

            for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
                old_data[i] = (uint8_t)(0x31u ^ (uint8_t)(i * 7u));
                new_data[i] = (uint8_t)(0xD6u ^ (uint8_t)(i * 11u));
            }

            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            if (vfs_file_open(&fs.mount, "/crash.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                              &file) != PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            pfs_crash_inject_stage = stage;
            pfs_crash_injected = 0u;
            done = 0u;
            if (vfs_file_open(&fs.mount, "/crash.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            write_rc = vfs_file_write(&file, new_data, sizeof(new_data),
                                      &done);
            if (vfs_file_close(&file) != PROS_VFS_OK ||
                write_rc != PROS_VFS_EIO ||
                done != 0u ||
                pfs_crash_injected == 0u) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }

            pfs_crash_inject_stage = 0u;
            pfs_test_crash_reset(&fs, &disk);

            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            done = 0u;
            if (vfs_file_open(&fs.mount, "/crash.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(read_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
                uint8_t expected = stage >= 5u ? new_data[i] : old_data[i];
                if (read_data[i] != expected) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -13);
                }
            }

            if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -13);
        }
    }

    /*
     * Interrupted-recovery/idempotence regression.
     *
     * First leave a durable committed journal (stage 9), then interrupt
     * recovery once before the recovery flush and once after the recovery
     * flush but before journal retirement. In both cases the next mount must
     * safely replay the same complete replacement images again.
     */
    {
        static const uint32_t recovery_stages[] = { 10u, 11u };
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];

        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x42u ^ (uint8_t)(i * 5u));
            new_data[i] = (uint8_t)(0x9Bu ^ (uint8_t)(i * 17u));
        }

        for (uint32_t recovery_index = 0u;
             recovery_index < sizeof(recovery_stages) /
                               sizeof(recovery_stages[0]);
             ++recovery_index) {
            struct pros_vfs_file file;
            uint32_t done = 0u;
            int write_rc;
            int mount_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;

            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);

            if (vfs_file_open(&fs.mount, "/recovery.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                              &file) != PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -14);

            pfs_crash_inject_stage = 9u;
            pfs_crash_injected = 0u;
            done = 0u;
            if (vfs_file_open(&fs.mount, "/recovery.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            write_rc = vfs_file_write(&file, new_data, sizeof(new_data),
                                      &done);
            if (vfs_file_close(&file) != PROS_VFS_OK ||
                write_rc != PROS_VFS_EIO ||
                done != 0u ||
                pfs_crash_injected == 0u) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -14);
            }

            pfs_crash_inject_stage = recovery_stages[recovery_index];
            pfs_crash_injected = 0u;
            mount_rc = prosenjitfs_mount(&fs, &device, 0u);
            if (mount_rc != PROSENJITFS_EIO ||
                pfs_crash_injected == 0u) {
                pfs_crash_inject_stage = 0u;
                pfs_zero(&fs, sizeof(fs));
                return pfs_selftest_fail(__LINE__, -14);
            }

            /*
             * prosenjitfs_mount() owns cleanup on recovery failure. Model the
             * subsequent power loss by discarding any remaining volatile
             * device-cache blocks and losing the failed mount's CPU state.
             */
            pfs_crash_inject_stage = 0u;
            pfs_test_power_loss(&disk);
            pfs_zero(&fs, sizeof(fs));

            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);

            done = 0u;
            if (vfs_file_open(&fs.mount, "/recovery.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(read_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -14);

            for (uint32_t i = 0u; i < sizeof(read_data); ++i)
                if (read_data[i] != new_data[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -14);
                }

            if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }
    }

    /* Repeated persistence/reuse regression: 100 complete create/write/
     * unmount/remount/read/unlink/unmount/remount cycles on one image. */
    {
        static const uint32_t cycle_count = 100u;
        static const uint8_t cycle_data[32] = {
            0x00u, 0x01u, 0x7Fu, 0x80u, 0xA5u, 0xD6u, 0xFEu, 0xFFu,
            0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u,
            0x99u, 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xF0u, 0x0Fu,
            0x5Au, 0xC3u, 0x3Cu, 0x96u, 0x69u, 0x12u, 0x24u, 0x48u
        };
        uint8_t cycle_read[sizeof(cycle_data)];
        uint32_t done;

        pfs_zero(&disk, sizeof(disk));
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -16);

        for (uint32_t cycle = 0u; cycle < cycle_count; ++cycle) {
            struct pros_vfs_file file;
            done = 0u;
            if (vfs_file_open(&fs.mount, "/cycle.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) != PROS_VFS_OK ||
                vfs_file_write(&file, cycle_data, sizeof(cycle_data), &done) != PROS_VFS_OK ||
                done != sizeof(cycle_data) || vfs_file_close(&file) != PROS_VFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -16);

            done = 0u;
            if (vfs_file_open(&fs.mount, "/cycle.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, cycle_read, sizeof(cycle_read), &done) != PROS_VFS_OK ||
                done != sizeof(cycle_read) || vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -16);
            for (uint32_t j = 0u; j < sizeof(cycle_data); ++j)
                if (cycle_read[j] != cycle_data[j])
                    return pfs_selftest_fail(__LINE__, -16);

            if (vfs_unlink(&fs.mount, "/cycle.bin") != PROS_VFS_OK ||
                pfs_selftest_consistency(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                pfs_selftest_consistency(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -16);
        }
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -16);
    }

    saved = disk.data[0];
    disk.data[0] ^= 0x01u;
    disk.data[512u] ^= 0x01u;
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_ECORRUPT)
        return pfs_selftest_fail(__LINE__, -9);
    disk.data[0] = saved;
    disk.data[512u] ^= 0x01u;

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -10);
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -11);
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_EINVAL)
        return pfs_selftest_fail(__LINE__, -11);
    return PROSENJITFS_OK;
}
