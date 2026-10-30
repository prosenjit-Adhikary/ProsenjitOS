/* Native ProsenjitFS: all on-disk address arithmetic must remain checked. */
#include "prosenjitfs.h"
#include "heap.h"
#include "process.h"
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
                            struct pros_vfs_vnode **,
                            const struct pros_vfs_credentials *);
static int pfs_vnode_mkdir(struct pros_vfs_vnode *, const char *,
                           struct pros_vfs_vnode **,
                           const struct pros_vfs_credentials *);
static int pfs_vnode_rmdir(struct pros_vfs_vnode *, const char *,
                           const struct pros_vfs_credentials *);
static uint32_t pfs_name_hash(const uint8_t *, uint32_t);
static int pfs_utf8_name_valid(const char *, uint32_t *);
static int pfs_read_dir_index(const struct pros_prosenjitfs *,
                              const struct pros_prosenjitfs_inode *,
                              uint64_t,
                              struct pros_prosenjitfs_dir_index *);
static int pfs_find_dir_entry(const struct pros_prosenjitfs *,
                              const struct pros_prosenjitfs_inode *,
                              uint64_t,
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

static int pfs_block_size_valid(uint32_t block_size)
{
    return block_size >= 512u && block_size <= 4096u &&
           (block_size & (block_size - 1u)) == 0u;
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

/* Redundant superblocks must agree byte-for-byte after each copy has
 * independently passed checksum and structural validation. Choosing one of
 * two valid but conflicting layouts could redirect metadata I/O or allocation. */
static int pfs_superblocks_equal(
    const struct pros_prosenjitfs_superblock *a,
    const struct pros_prosenjitfs_superblock *b)
{
    const uint8_t *pa;
    const uint8_t *pb;

    if (!a || !b)
        return 0;
    pa = (const uint8_t *)a;
    pb = (const uint8_t *)b;
    for (uint32_t i = 0u; i < sizeof(*a); ++i) {
        if (pa[i] != pb[i])
            return 0;
    }
    return 1;
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
        !pfs_block_size_valid(sb->block_size) ||
        sb->total_blocks < PFS_MIN_BLOCKS ||
        sb->root_inode != 0u ||
        sb->inode_size != PROSENJITFS_INODE_SIZE ||
        sb->inode_count < PFS_MIN_INODES ||
        sb->inode_count > PFS_MAX_INODES ||
        sb->max_name != PROSENJITFS_MAX_NAME ||
        (sb->features & PROSENJITFS_FEATURE_KNOWN_MASK) !=
            PROSENJITFS_FEATURE_KNOWN_MASK ||
        (sb->features & ~PROSENJITFS_FEATURE_KNOWN_MASK) != 0u)
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
    if (!bitmap || !pfs_block_size_valid(block_size) ||
        bits_per_block == 0u || bitmap_index > UINT64_MAX / bits_per_block ||
        pfs_region_end(region_start, region_blocks, &end) != PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    bitmap_base = (uint64_t)bitmap_index * bits_per_block;
    for (uint64_t b = region_start; b < end;) {
        if (bitmap_base > UINT64_MAX - bits_per_block)
            return PROSENJITFS_EOVERFLOW;
        if (b >= bitmap_base && b < bitmap_base + bits_per_block) {
            uint64_t local = b - bitmap_base;
            if ((local >> 3) >= bytes)
                return PROSENJITFS_EOVERFLOW;
            pfs_mark_bit(bitmap, local);
        }
        /* Avoid wrapping UINT64_MAX when the validated region ends there. */
        if (b == end - 1u)
            break;
        ++b;
    }
    return PROSENJITFS_OK;
}

int prosenjitfs_probe(struct pros_block_device *device)
{
    uint8_t *block;
    struct pros_prosenjitfs_superblock primary;
    struct pros_prosenjitfs_superblock backup;
    int primary_ok;
    int backup_ok;

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
    primary_ok =
        pfs_superblock_valid(block, device->sector_size, &primary) ==
        PROSENJITFS_OK;

    if (pfs_read_block(device, 1u, block) != PROSENJITFS_OK) {
        kfree(block);
        return PROSENJITFS_EIO;
    }
    backup_ok =
        pfs_superblock_valid(block, device->sector_size, &backup) ==
        PROSENJITFS_OK;

    kfree(block);
    if (!primary_ok && !backup_ok)
        return PROSENJITFS_ENOTSUP;

    /*
     * Probe and mount must agree on whether a volume is usable. Two individually
     * valid but conflicting superblocks are corruption, not a supported volume.
     * A single valid copy remains discoverable; mount will expose it read-only.
     */
    if (primary_ok && backup_ok &&
        !pfs_superblocks_equal(&primary, &backup))
        return PROSENJITFS_ECORRUPT;

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
    if (!pfs_block_size_valid(block_size) ||
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
    /* sb.data_start is reserved for the root directory index; the
     * bitmap therefore marks it allocated. Subsequent data blocks are free. */
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

/*
 * Provider callbacks can run in a preemptible context. Never spin while
 * holding an open-file operation gate: a preempted owner may need this CPU to
 * resume and release the lock. Contention is reported as EBUSY instead.
 */
static int pfs_io_lock(struct pros_prosenjitfs *fs)
{
    if (!fs)
        return PROSENJITFS_EINVAL;
    return __atomic_exchange_n(&fs->io_lock, 1u, __ATOMIC_ACQUIRE) == 0u
         ? PROSENJITFS_OK : PROSENJITFS_EBUSY;
}

static void pfs_io_unlock(struct pros_prosenjitfs *fs)
{
    if (fs)
        __atomic_store_n(&fs->io_lock, 0u, __ATOMIC_RELEASE);
}

static int pfs_vnode_validate_inode(struct pfs_vnode_data *data,
                                    struct pros_prosenjitfs_inode *inode)
{
    int rc;
    if (!data || !data->fs || !inode || !data->fs->mount.active)
        return PROSENJITFS_EPERM;
    rc = prosenjitfs_read_inode(data->fs, data->inode_number, inode);
    if (rc == PROSENJITFS_ENOENT)
        return PROSENJITFS_ESTALE;
    if (rc != PROSENJITFS_OK)
        return rc;
    if (inode->generation != data->generation ||
        (inode->type != PROSENJITFS_INODE_FILE &&
         inode->type != PROSENJITFS_INODE_DIR))
        return PROSENJITFS_ESTALE;
    return PROSENJITFS_OK;
}

static int pfs_vnode_get_metadata(struct pros_vfs_vnode *vnode,
                                  struct pros_vfs_metadata *metadata)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    int rc;

    if (!vnode || !metadata || !vnode->private_data)
        return PROS_VFS_EINVAL;
    data = (struct pfs_vnode_data *)vnode->private_data;
    if (!data->fs)
        return PROS_VFS_EINVAL;
    rc = pfs_io_lock(data->fs);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);
    rc = pfs_vnode_validate_inode(data, &inode);
    pfs_io_unlock(data->fs);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);

    metadata->uid = inode.uid;
    metadata->gid = inode.gid;
    metadata->mode = inode.mode;
    metadata->reserved[0] = 0u;
    metadata->reserved[1] = 0u;
    return PROS_VFS_OK;
}

static int pfs_vnode_set_metadata(struct pros_vfs_vnode *vnode,
                                  const struct pros_vfs_metadata *metadata,
                                  uint8_t fields)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint8_t block[4096];
    uint64_t lba;
    uint32_t offset;
    int rc;

    if (!vnode || !metadata || !vnode->private_data ||
        (metadata->mode & (uint16_t)~0777u) != 0u ||
        metadata->reserved[0] != 0u || metadata->reserved[1] != 0u ||
        fields == 0u || (fields & (uint8_t)~(PROS_VFS_METADATA_MODE |
                                            PROS_VFS_METADATA_OWNER)) != 0u)
        return PROS_VFS_EINVAL;
    data = (struct pfs_vnode_data *)vnode->private_data;
    if (!data->fs || data->fs->read_only || data->fs->mount.read_only)
        return PROS_VFS_EPERM;

    rc = pfs_io_lock(data->fs);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);
    rc = pfs_vnode_validate_inode(data, &inode);
    if (rc != PROSENJITFS_OK)
        goto done;
    rc = pfs_inode_block_location(data->fs, data->inode_number,
                                  &lba, &offset);
    if (rc != PROSENJITFS_OK)
        goto done;
    if ((uint64_t)offset + sizeof(inode) > data->fs->sb.block_size) {
        rc = PROSENJITFS_ECORRUPT;
        goto done;
    }
    rc = pfs_read_block(data->fs->mount.device, lba, block);
    if (rc != PROSENJITFS_OK)
        goto done;
    pfs_copy(&inode, block + offset, sizeof(inode));
    if (inode.generation != data->generation ||
        (inode.type != PROSENJITFS_INODE_FILE &&
         inode.type != PROSENJITFS_INODE_DIR) ||
        inode.checksum != pfs_crc_with_zero32(
            block + offset, sizeof(inode),
            (uint32_t)offsetof(struct pros_prosenjitfs_inode, checksum))) {
        rc = PROSENJITFS_ESTALE;
        goto done;
    }

    if (fields & PROS_VFS_METADATA_MODE)
        inode.mode = metadata->mode;
    if (fields & PROS_VFS_METADATA_OWNER) {
        inode.uid = metadata->uid;
        inode.gid = metadata->gid;
    }
    inode.checksum = 0u;
    inode.checksum = pfs_crc_with_zero32(
        (const uint8_t *)&inode, sizeof(inode),
        (uint32_t)offsetof(struct pros_prosenjitfs_inode, checksum));
    pfs_copy(block + offset, &inode, sizeof(inode));

    rc = pfs_tx_reset(data->fs);
    if (rc != PROSENJITFS_OK)
        goto done;
    rc = pfs_tx_add(data->fs, lba, block);
    if (rc != PROSENJITFS_OK) {
        (void)pfs_tx_reset(data->fs);
        goto done;
    }
    rc = pfs_tx_commit(data->fs);
    if (rc == PROSENJITFS_OK)
        (void)vfs_vnode_set_metadata(vnode, inode.mode, inode.uid, inode.gid);

done:
    pfs_io_unlock(data->fs);
    return pfs_vfs_error(rc);
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

    if (!fs || !generation || !pfs_block_size_valid(fs->sb.block_size))
        return PROSENJITFS_EINVAL;
    rc = pfs_inode_block_location(fs, inode_number, &lba, &offset);
    if (rc != PROSENJITFS_OK)
        return rc;
    rc = pfs_read_block(fs->mount.device, lba, block);
    if (rc != PROSENJITFS_OK)
        return rc;
    pfs_copy(&old_inode, block + offset, sizeof(old_inode));
    if (old_inode.type != PROSENJITFS_INODE_FREE)
        return PROSENJITFS_ECORRUPT;
    if (old_inode.generation == 0u && old_inode.checksum == 0u) {
        *generation = 1u;
        return PROSENJITFS_OK;
    }
    if (old_inode.checksum != pfs_crc_with_zero32(
            block + offset, sizeof(old_inode),
            (uint32_t)offsetof(struct pros_prosenjitfs_inode, checksum)))
        return PROSENJITFS_ECORRUPT;
    return pfs_next_generation(old_inode.generation, generation);
}

static int pfs_vnode_lookup(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result);
static int pfs_vnode_unlink(struct pros_vfs_vnode *, const char *,
                            const struct pros_vfs_credentials *);
static int pfs_vnode_rename(struct pros_vfs_vnode *, const char *,
                            struct pros_vfs_vnode *, const char *,
                            const struct pros_vfs_credentials *);
static int __attribute__((unused)) pfs_find_dir_entry_lba(const struct pros_prosenjitfs *,
                                   const struct pros_prosenjitfs_inode *,
                                   uint64_t,
                                   const char *,
                                   struct pros_prosenjitfs_dir_entry *,
                                   uint64_t *,
                                   uint64_t *);
static int pfs_vnode_readdir(struct pros_vfs_vnode *directory, uint64_t *cookie,
                             struct pros_vfs_dirent *entry);
static void pfs_vnode_release(struct pros_vfs_vnode *vnode);
static int pfs_vnode_get_metadata(struct pros_vfs_vnode *,
                                  struct pros_vfs_metadata *);
static int pfs_vnode_set_metadata(struct pros_vfs_vnode *,
                                  const struct pros_vfs_metadata *, uint8_t);

static const struct pros_vfs_vnode_ops pfs_vnode_ops = {
    .lookup = pfs_vnode_lookup,
    .readdir = pfs_vnode_readdir,
    .create = pfs_vnode_create,
    .mkdir = pfs_vnode_mkdir,
    .rmdir = pfs_vnode_rmdir,
    .unlink = pfs_vnode_unlink,
    .rename = pfs_vnode_rename,
    .release = pfs_vnode_release,
    .get_metadata = pfs_vnode_get_metadata,
    .set_metadata = pfs_vnode_set_metadata
};

static int pfs_file_read(struct pros_vfs_file *, uint32_t, uint32_t, void *);
static int pfs_file_write(struct pros_vfs_file *, uint32_t, uint32_t, const void *);
static int pfs_file_truncate(struct pros_vfs_file *, uint64_t);
static int pfs_file_close(struct pros_vfs_file *);

static int pfs_read_extent(const struct pros_prosenjitfs *fs, uint64_t lba,
                           struct pros_prosenjitfs_extent *extent)
{
    uint8_t block[4096];
    if (!fs || !extent || !pfs_block_size_valid(fs->sb.block_size) ||
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
    if (!fs || !block_number || !pfs_block_size_valid(fs->sb.block_size))
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

static int pfs_data_block_is_free(const struct pros_prosenjitfs *fs,
                                      uint64_t block_number)
{
    uint64_t bits_per_block;
    uint64_t bitmap_lba;
    uint8_t bitmap[4096];

    if (!fs || block_number < fs->sb.data_start ||
        block_number >= fs->sb.total_blocks ||
        !pfs_block_size_valid(fs->sb.block_size))
        return PROSENJITFS_EINVAL;

    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    {
        int rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                             block_number / bits_per_block, &bitmap_lba);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    if (pfs_read_block(fs->mount.device, bitmap_lba, bitmap) !=
        PROSENJITFS_OK)
        return PROSENJITFS_EIO;

    return pfs_bitmap_bit(bitmap, block_number % bits_per_block) ? 0 : 1;
}

static int pfs_inode_block_location(const struct pros_prosenjitfs *fs,
                                    uint64_t inode_number,
                                    uint64_t *block_lba,
                                    uint32_t *offset)
{
    uint64_t byte_offset;
    if (!fs || !block_lba || !offset ||
        inode_number >= fs->sb.inode_count ||
        !pfs_block_size_valid(fs->sb.block_size))
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
    case PROSENJITFS_ESTALE:
        return PROS_VFS_ESTALE;
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

static int pfs_file_read_unserialized(struct pros_vfs_file *file, uint32_t offset,
                                    uint32_t length, void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;
    uint32_t done = 0u;

    if (!file || !file->vnode || (!buffer && length != 0u))
        return PROS_VFS_EINVAL;
    if (length == 0u)
        return 0;
    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs || !data->fs->mount.active)
        return PROS_VFS_EPERM;
    /*
     * Validate geometry before inode validation and the read loop divide
     * positions by block_size. A corrupted in-memory superblock must not
     * turn a read request into divide-by-zero or an oversized stack copy.
     */
    if (!pfs_block_size_valid(data->fs->sb.block_size) ||
        data->fs->sb.total_blocks < data->fs->sb.data_start ||
        (data->fs->sb.total_blocks - data->fs->sb.data_start) >
            UINT64_MAX / data->fs->sb.block_size)
        return PROS_VFS_ECORRUPT;
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
        if (rc != PROSENJITFS_OK)
            return done ? (int)done : pfs_vfs_error(rc);
        if (extent_lba == 0u)
            return done ? (int)done : PROS_VFS_ECORRUPT;

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

static int pfs_file_read(struct pros_vfs_file *file, uint32_t offset,
                         uint32_t length, void *buffer)
{
    struct pfs_vnode_data *data = 0;
    struct pros_prosenjitfs *fs = 0;
    int rc;

    if (file && file->vnode)
        data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (data)
        fs = data->fs;
    if (fs) {
        rc = pfs_io_lock(fs);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
    }
    rc = pfs_file_read_unserialized(file, offset, length, buffer);
    if (fs)
        pfs_io_unlock(fs);
    return rc;
}

static int pfs_file_write_unserialized(struct pros_vfs_file *file, uint32_t offset,
                          uint32_t length, const void *buffer)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode inode;

    uint64_t end_position;
    uint64_t max_file_size;

    if (!file || !file->vnode || !file->mount ||
        (!buffer && length != 0u))
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
    if (!pfs_block_size_valid(data->fs->sb.block_size) ||
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
            uint64_t previous_lba = 0u;
            struct pros_prosenjitfs_extent previous_extent;
            uint8_t have_previous = 0u;
            uint8_t coalesce = 0u;

            /*
             * Delayed-allocation equivalent:
             * do not keep an unsafe volatile allocation cache. Instead, choose
             * a physically adjacent free block when a sequential logical
             * write can extend the preceding extent, and publish the data,
             * extent, bitmap and inode atomically in one journal transaction.
             */
            {
                uint64_t current_lba = inode.extent_head;
                struct pros_prosenjitfs_extent current_extent;

                while (current_lba != 0u) {
                    rc = pfs_read_extent(data->fs, current_lba,
                                         &current_extent);
                    if (rc != PROSENJITFS_OK)
                        return pfs_file_write_error(done, rc);
                    if (current_extent.logical_block > logical_block)
                        break;
                    if (current_extent.logical_block <= logical_block &&
                        current_extent.block_count <=
                            UINT64_MAX - current_extent.logical_block &&
                        current_extent.logical_block +
                            current_extent.block_count == logical_block) {
                        previous_lba = current_lba;
                        pfs_copy(&previous_extent, &current_extent,
                                 sizeof(previous_extent));
                        have_previous = 1u;
                    }
                    current_lba = current_extent.next;
                }
            }

            if (have_previous &&
                previous_extent.physical_block <= UINT64_MAX -
                    previous_extent.block_count) {
                uint64_t candidate =
                    previous_extent.physical_block +
                    previous_extent.block_count;
                int free_rc = pfs_data_block_is_free(data->fs, candidate);
                if (free_rc < 0)
                    return pfs_file_write_error(done, free_rc);
                if (free_rc == 1)
                    coalesce = 1u;
            }

            pfs_zero(data_image, data->fs->sb.block_size);
            pfs_zero(extent_image, data->fs->sb.block_size);
            pfs_zero(tail_image, data->fs->sb.block_size);
            pfs_zero(inode_image, data->fs->sb.block_size);

            for (uint32_t i = 0u; i < want; ++i)
                data_image[in_block + i] =
                    ((const uint8_t *)buffer)[done + i];

            if (coalesce) {
                /*
                 * Extend the preceding extent in place. This is the key
                 * coalescing operation: no second extent metadata block is
                 * consumed for consecutive writes.
                 */
                if (previous_extent.block_count == UINT64_MAX ||
                    previous_extent.physical_block >
                        UINT64_MAX - previous_extent.block_count)
                    return pfs_file_write_error(done,
                                                PROSENJITFS_EOVERFLOW);
                physical = previous_extent.physical_block +
                           previous_extent.block_count;
                extent_lba = previous_lba;
                extent = previous_extent;
                extent.block_count++;
                pfs_copy(extent_image, &extent, sizeof(extent));
                pfs_put32(
                    extent_image +
                    offsetof(struct pros_prosenjitfs_extent, checksum),
                    pfs_crc_with_zero32(
                        extent_image, sizeof(extent),
                        (uint32_t)offsetof(
                            struct pros_prosenjitfs_extent, checksum)));
            } else {
                /*
                 * Reserve extent metadata first, then select the data block.
                 * This intentionally leaves the next physical data block free
                 * so a following sequential write can coalesce with this
                 * extent instead of being separated by its metadata block.
                 */
                rc = pfs_find_free_data_block_except(data->fs, 0u,
                                                     &new_extent_lba);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, rc);
                rc = pfs_find_free_data_block_except(data->fs,
                                                     new_extent_lba,
                                                     &physical);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, rc);

                {
                    struct pros_prosenjitfs_extent new_extent;
                    pfs_zero(&new_extent, sizeof(new_extent));
                    new_extent.magic = PROSENJITFS_EXTENT_MAGIC;
                    new_extent.logical_block = logical_block;
                    new_extent.physical_block = physical;
                    new_extent.block_count = 1u;
                    new_extent.next = 0u;

                    /*
                     * Keep the extent chain ordered by logical_block. A sparse
                     * write can fill a hole between two existing extents, so
                     * never append blindly at the tail.
                     */
                    {
                        uint64_t prev_lba = 0u;
                        uint64_t current_lba = inode.extent_head;
                        struct pros_prosenjitfs_extent current_extent;

                        while (current_lba != 0u) {
                            rc = pfs_read_extent(data->fs, current_lba,
                                                 &current_extent);
                            if (rc != PROSENJITFS_OK)
                                return pfs_file_write_error(done, rc);
                            if (logical_block < current_extent.logical_block)
                                break;
                            prev_lba = current_lba;
                            current_lba = current_extent.next;
                        }
                        new_extent.next = current_lba;
                        if (prev_lba != 0u) {
                            rc = pfs_read_block(data->fs->mount.device,
                                                prev_lba, tail_image);
                            if (rc != PROSENJITFS_OK)
                                return pfs_file_write_error(done, rc);
                            {
                                struct pros_prosenjitfs_extent prev_extent;
                                pfs_copy(&prev_extent, tail_image,
                                         sizeof(prev_extent));
                                prev_extent.next = new_extent_lba;
                                pfs_copy(tail_image, &prev_extent,
                                         sizeof(prev_extent));
                                pfs_put32(
                                    tail_image +
                                    offsetof(struct pros_prosenjitfs_extent,
                                              checksum),
                                    pfs_crc_with_zero32(
                                        tail_image, sizeof(prev_extent),
                                        (uint32_t)offsetof(
                                            struct pros_prosenjitfs_extent,
                                            checksum)));
                            }
                            tail_lba = prev_lba;
                        } else {
                            inode.extent_head = new_extent_lba;
                            tail_lba = 0u;
                        }
                    }

                    pfs_copy(extent_image, &new_extent, sizeof(new_extent));
                    pfs_put32(
                        extent_image +
                        offsetof(struct pros_prosenjitfs_extent, checksum),
                        pfs_crc_with_zero32(
                            extent_image, sizeof(new_extent),
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_extent, checksum)));
                }
            }

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
                if (!coalesce && tail_lba == 0u &&
                    staged_extent_head != 0u &&
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
            if (!coalesce) {
                rc = pfs_allocator_stage(data->fs, new_extent_lba, 1u, 1u);
                if (rc != PROSENJITFS_OK)
                    return pfs_file_write_error(done, rc);
            }
            rc = pfs_tx_add(data->fs, physical, data_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            rc = pfs_tx_add(data->fs, extent_lba ? extent_lba :
                             new_extent_lba, extent_image);
            if (rc != PROSENJITFS_OK)
                return pfs_file_write_error(done, rc);
            if (!coalesce && tail_lba != 0u) {
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
    struct pfs_vnode_data *data = 0;
    struct pros_prosenjitfs *fs = 0;
    int rc;

    if (file && file->vnode) {
        data = (struct pfs_vnode_data *)file->vnode->private_data;
        if (data)
            fs = data->fs;
    }
    if (fs) {
        rc = pfs_io_lock(fs);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
    }
    rc = pfs_file_write_unserialized(file, offset, length, buffer);
    if (fs)
        pfs_io_unlock(fs);
    return rc;
}

static int pfs_file_truncate_unserialized(struct pros_vfs_file *file, uint64_t size)
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
    uint64_t tail_zero_lba = 0u;
    uint8_t tail_zero_pending = 0u;
    int rc = PROSENJITFS_EIO;

    if (!file || !file->vnode || !file->mount)
        return PROS_VFS_EINVAL;
    if (size > UINT32_MAX)
        return PROS_VFS_EOVERFLOW;
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
        /*
         * Sparse growth: extending EOF creates a logical hole instead of
         * allocating/journaling every intervening data block. Bytes exposed
         * by the extension must still read as zero.
         */
        uint64_t old_size = inode.size;
        uint64_t bs = data->fs->sb.block_size;
        uint8_t block[4096];

        if (bs == 0u || bs > sizeof(block))
            return PROS_VFS_ECORRUPT;

        /*
         * If EOF is inside an already allocated block, clear the previously
         * unreachable tail before publishing the larger inode size. Later
         * logical blocks remain unallocated holes.
         */
        if ((old_size % bs) != 0u) {
            uint64_t logical_block = old_size / bs;
            struct pros_prosenjitfs_extent extent;
            uint64_t extent_lba = 0u;
            uint64_t tail_lba = 0u;
            int rc = pfs_find_extent(data->fs, &inode, logical_block,
                                     &extent, &extent_lba, &tail_lba);
            if (rc != PROSENJITFS_ENOENT && rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);

            if (rc == PROSENJITFS_OK) {
                uint64_t delta = logical_block - extent.logical_block;
                uint64_t physical = 0u;
                uint32_t start = (uint32_t)(old_size % bs);

                rc = pfs_lba_add(data->fs->mount.device,
                                 extent.physical_block, delta, &physical);
                if (rc != PROSENJITFS_OK)
                    return pfs_vfs_error(rc);
                if (physical < data->fs->sb.data_start ||
                    physical >= data->fs->sb.total_blocks)
                    return PROS_VFS_ECORRUPT;

                rc = pfs_read_block(data->fs->mount.device, physical, block);
                if (rc != PROSENJITFS_OK)
                    return pfs_vfs_error(rc);
                for (uint32_t i = start; i < (uint32_t)bs; ++i)
                    block[i] = 0u;

                rc = pfs_tx_reset(data->fs);
                if (rc == PROSENJITFS_OK)
                    rc = pfs_tx_add(data->fs, physical, block);
                if (rc == PROSENJITFS_OK)
                    rc = pfs_tx_commit(data->fs);
                if (rc != PROSENJITFS_OK) {
                    pfs_tx_reset(data->fs);
                    return pfs_vfs_error(rc);
                }
            }
        }

        /*
         * Publish size only after tail-zeroing. The newly exposed range has
         * no extents and therefore consumes no data blocks. A crash before
         * this inode transaction cannot expose stale bytes past the old EOF.
         */
        inode.size = size;
        inode.flags |= PROSENJITFS_INODE_FLAG_SPARSE;
        {
            uint64_t inode_lba;
            uint32_t inode_offset;
            uint8_t inode_image[4096];
            int rc = pfs_inode_block_location(data->fs, data->inode_number,
                                              &inode_lba, &inode_offset);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);
            rc = pfs_read_block(data->fs->mount.device, inode_lba,
                                inode_image);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);
            pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
            pfs_put32(inode_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(
                          inode_image + inode_offset, sizeof(inode),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_inode, checksum)));
            rc = pfs_tx_reset(data->fs);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_add(data->fs, inode_lba, inode_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_commit(data->fs);
            if (rc != PROSENJITFS_OK) {
                pfs_tx_reset(data->fs);
                return pfs_vfs_error(rc);
            }
        }
        file->vnode->size = size;
        return PROS_VFS_OK;
    }

    if (!pfs_block_size_valid(data->fs->sb.block_size))
        return PROS_VFS_ECORRUPT;

    keep_blocks = size / data->fs->sb.block_size +
                  ((size % data->fs->sb.block_size) != 0u ? 1u : 0u);

    /*
     * Shrinking to an EOF inside an allocated block must zero the
     * unreachable suffix in the same redo transaction as the size update.
     * Resolve the data target before changing the extent chain below.
     */
    if ((size % data->fs->sb.block_size) != 0u) {
        struct pros_prosenjitfs_extent tail_extent;
        uint64_t extent_lba = 0u;
        uint64_t tail_lba = 0u;
        uint64_t logical_block = size / data->fs->sb.block_size;

        rc = pfs_find_extent(data->fs, &inode, logical_block, &tail_extent,
                             &extent_lba, &tail_lba);
        if (rc == PROSENJITFS_OK) {
            uint64_t delta = logical_block - tail_extent.logical_block;
            if (extent_lba == 0u)
                return PROS_VFS_ECORRUPT;
            rc = pfs_lba_add(data->fs->mount.device,
                             tail_extent.physical_block, delta,
                             &tail_zero_lba);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);
            if (tail_zero_lba < data->fs->sb.data_start ||
                tail_zero_lba >= data->fs->sb.total_blocks)
                return PROS_VFS_ECORRUPT;
            tail_zero_pending = 1u;
        } else if (rc != PROSENJITFS_ENOENT) {
            return pfs_vfs_error(rc);
        }
    }

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
    /* Worst-case accounting for v1: each affected extent may need its
     * data bitmap, extent-metadata bitmap and extent image, plus the inode
     * and retained predecessor. Keep the operation below 16 targets before
     * any persistent mutation; actual bitmap sharing can only reduce this. */
    if (affected > 4u)
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

                    {
                        uint64_t next = extent.next;
                        /*
                         * Preserve the metadata block LBA before advancing
                         * through the detached tail.  The truncated extent
                         * image belongs at the current extent's LBA, not at
                         * its successor.  Using the advanced `current`
                         * value here would overwrite the successor (or target
                         * LBA 0 when this was the last extent).
                         */
                        uint64_t extent_lba = current;
                        extent.block_count = keep_in_extent;
                        extent.next = 0u;
                        pfs_copy(extent_image, &extent, sizeof(extent));
                        current = next;

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
                            rc = pfs_tx_add(data->fs, extent_lba, extent_image);
                    if (rc != PROSENJITFS_OK) {
                        pfs_tx_reset(data->fs);
                        return pfs_vfs_error(rc);
                    }
                    while (current != 0u) {
                        struct pros_prosenjitfs_extent tail;
                        uint64_t next;
                        rc = pfs_read_extent(data->fs, current, &tail);
                        if (rc != PROSENJITFS_OK) {
                            pfs_tx_reset(data->fs);
                            return pfs_vfs_error(rc);
                        }
                        next = tail.next;
                        if (tail.block_count > UINT32_MAX ||
                            tail.block_count > UINT64_MAX /
                                data->fs->sb.block_size ||
                            inode.allocated_bytes <
                                tail.block_count * data->fs->sb.block_size) {
                            pfs_tx_reset(data->fs);
                            return PROS_VFS_ECORRUPT;
                        }
                        inode.allocated_bytes -=
                            tail.block_count * data->fs->sb.block_size;
                        rc = pfs_allocator_stage(data->fs,
                                                 tail.physical_block,
                                                 (uint32_t)tail.block_count,
                                                 0u);
                        if (rc == PROSENJITFS_OK)
                            rc = pfs_allocator_stage(data->fs, current,
                                                     1u, 0u);
                        if (rc != PROSENJITFS_OK) {
                            pfs_tx_reset(data->fs);
                            return pfs_vfs_error(rc);
                        }
                        current = next;
                    }
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
            /* Reuse a single block-sized buffer to bound kernel stack use. */
            uint8_t update_image[4096];
            int rc;
            if (affected == 0u) {
                rc = pfs_tx_reset(data->fs);
                if (rc != PROSENJITFS_OK)
                    return pfs_vfs_error(rc);
            }

            if (tail_zero_pending) {
                /* Duplicate targets would replace staged metadata in pfs_tx_add. */
                for (uint32_t i = 0u; i < data->fs->tx.count; ++i) {
                    if (data->fs->tx.targets[i] == tail_zero_lba) {
                        pfs_tx_reset(data->fs);
                        return PROS_VFS_ECORRUPT;
                    }
                }
                rc = pfs_read_block(data->fs->mount.device, tail_zero_lba,
                                    update_image);
                if (rc != PROSENJITFS_OK) {
                    pfs_tx_reset(data->fs);
                    return pfs_vfs_error(rc);
                }
                for (uint32_t i = (uint32_t)(size % data->fs->sb.block_size);
                     i < data->fs->sb.block_size; ++i)
                    update_image[i] = 0u;
                rc = pfs_tx_add(data->fs, tail_zero_lba, update_image);
                if (rc != PROSENJITFS_OK) {
                    pfs_tx_reset(data->fs);
                    return pfs_vfs_error(rc);
                }
            }

            rc = pfs_inode_block_location(data->fs, data->inode_number,
                                          &inode_lba, &inode_offset);
            if (rc != PROSENJITFS_OK) {
                pfs_tx_reset(data->fs);
                return pfs_vfs_error(rc);
            }
            if (tail_zero_pending && inode_lba == tail_zero_lba) {
                pfs_tx_reset(data->fs);
                return PROS_VFS_ECORRUPT;
            }
            rc = pfs_read_block(data->fs->mount.device, inode_lba,
                                update_image);
            if (rc != PROSENJITFS_OK) {
                pfs_tx_reset(data->fs);
                return pfs_vfs_error(rc);
            }
            pfs_copy(update_image + inode_offset, &inode, sizeof(inode));
            pfs_put32(update_image + inode_offset +
                      offsetof(struct pros_prosenjitfs_inode, checksum),
                      pfs_crc_with_zero32(
                          update_image + inode_offset, sizeof(inode),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_inode, checksum)));
            int rc2 = pfs_tx_add(data->fs, inode_lba, update_image);
            if (rc2 != PROSENJITFS_OK) {
                pfs_tx_reset(data->fs);
                return pfs_vfs_error(rc2);
            }
            rc2 = pfs_tx_commit(data->fs);
            if (rc2 != PROSENJITFS_OK)
                return pfs_vfs_error(rc2);
        }
    }
    goto truncate_done;

truncate_abort:
    pfs_tx_reset(data->fs);
    return pfs_vfs_error(rc);

truncate_done:
    ;

    file->vnode->size = size;
    return PROS_VFS_OK;
}

static int pfs_file_truncate(struct pros_vfs_file *file, uint64_t size)
{
    struct pfs_vnode_data *data = 0;
    struct pros_prosenjitfs *fs = 0;
    int rc;

    if (file && file->vnode)
        data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (data)
        fs = data->fs;
    if (fs) {
        rc = pfs_io_lock(fs);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
    }
    rc = pfs_file_truncate_unserialized(file, size);
    if (fs)
        pfs_io_unlock(fs);
    return rc;
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
        return PROSENJITFS_EINVAL;
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
        return PROSENJITFS_ENOTSUP;

    data = (struct pfs_vnode_data *)kmalloc(sizeof(*data));
    if (!data)
        return PROSENJITFS_EIO;
    data->fs = fs;
    data->inode_number = inode_number;
    data->generation = inode.generation;

    *result = (struct pros_vfs_vnode *)kmalloc(sizeof(**result));
    if (!*result) {
        kfree(data);
        return PROSENJITFS_EIO;
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
        return PROSENJITFS_EINVAL;
    }
    if ((inode.mode & (uint16_t)~07777u) != 0u) {
        if (parent)
            vfs_vnode_put(parent);
        kfree(data);
        kfree(*result);
        *result = 0;
        return PROSENJITFS_EIO;
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
        return PROSENJITFS_EINVAL;
    }
    if (vfs_vnode_set_metadata(*result, inode.mode, inode.uid, inode.gid) !=
            PROS_VFS_OK) {
        (*result)->initialized = 0u;
        if (parent)
            vfs_vnode_put(parent);
        kfree(data);
        kfree(*result);
        *result = 0;
        return PROSENJITFS_EIO;
    }
    return PROS_VFS_OK;
}


static int pfs_find_free_inode(const struct pros_prosenjitfs *fs,
                               uint64_t *inode_number)
{
    uint64_t bits_per_block;
    uint8_t *block;

    if (!fs || !inode_number || !pfs_block_size_valid(fs->sb.block_size))
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

    if (!fs || !block_number || !pfs_block_size_valid(fs->sb.block_size))
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

    if (!fs || !block_number || !pfs_block_size_valid(fs->sb.block_size))
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

static int pfs_vnode_mkdir_unserialized(struct pros_vfs_vnode *parent, const char *name,
                           struct pros_vfs_vnode **result,
                           const struct pros_vfs_credentials *auth)
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
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    {
        int access_rc = vfs_vnode_authorize(parent, auth, 3u);
        if (access_rc != PROS_VFS_OK)
            return access_rc;
    }
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK ||
        name_length == 0u || name_length > PROSENJITFS_MAX_NAME ||
        (name_length == 1u && name[0] == '.') ||
        (name_length == 2u && name[0] == '.' && name[1] == '.'))
        return PROS_VFS_EINVAL;
    rc = pfs_vnode_validate_inode(data, &parent_inode);
    if (rc != PROSENJITFS_OK) {
        pfs_mkdir_diag_stage = 2u; pfs_mkdir_diag_rc = rc;
        return pfs_vfs_error(rc);
    }
    if (parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_ENOTDIR;
    rc = pfs_read_dir_index(data->fs, &parent_inode, data->inode_number, &dir_index);
    if (rc != PROSENJITFS_OK) {
        pfs_mkdir_diag_stage = 2u; pfs_mkdir_diag_rc = rc;
        return pfs_vfs_error(rc);
    }
    rc = pfs_find_dir_entry(data->fs, &parent_inode, data->inode_number, name, 0);
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
    child_inode.uid = auth->uid;
    child_inode.gid = auth->gid;
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
        child_index.generation = child_generation;
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

static int pfs_vnode_mkdir(struct pros_vfs_vnode *parent, const char *name, struct pros_vfs_vnode **result, const struct pros_vfs_credentials *auth) { struct pfs_vnode_data *data; int rc; if (!parent || !parent->private_data) return pfs_vnode_mkdir_unserialized(parent,name,result,auth); data=(struct pfs_vnode_data *)parent->private_data; if (!data || !data->fs) return pfs_vnode_mkdir_unserialized(parent,name,result,auth); rc=pfs_io_lock(data->fs); if(rc!=PROSENJITFS_OK) return pfs_vfs_error(rc); rc=pfs_vnode_mkdir_unserialized(parent,name,result,auth); pfs_io_unlock(data->fs); return rc; }
static int pfs_vnode_rmdir_unserialized(struct pros_vfs_vnode *parent, const char *name,
                           const struct pros_vfs_credentials *auth)
{
    struct pfs_vnode_data *data;
    struct pros_prosenjitfs_inode freed_inode;
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
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    {
        int access_rc = vfs_vnode_authorize(parent, auth, 3u);
        if (access_rc != PROS_VFS_OK)
            return access_rc;
    }
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK ||
        name_length == 0u || name_length > PROSENJITFS_MAX_NAME ||
        (name_length == 1u && name[0] == '.') ||
        (name_length == 2u && name[0] == '.' && name[1] == '.'))
        return PROS_VFS_EINVAL;
    rc = pfs_vnode_validate_inode(data, &parent_inode);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);
    if (parent_inode.type != PROSENJITFS_INODE_DIR) {
        rc = PROS_VFS_ENOTDIR;
        goto rmdir_out;
    }
    rc = pfs_read_dir_index(data->fs, &parent_inode, data->inode_number, &dir_index);
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
            rc = PROSENJITFS_ECORRUPT;
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
            entry.name[entry.name_length] != 0u ||
            entry.inode >= data->fs->sb.inode_count ||
            pfs_utf8_name_valid((const char *)entry.name, &name_length) !=
                PROSENJITFS_OK ||
            name_length != entry.name_length ||
            entry.name_hash != pfs_name_hash(entry.name, entry.name_length) ||
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
        rc = pfs_read_dir_index(data->fs, &child_inode, inode_number, &child_dir_index);
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
                        rc = PROSENJITFS_ECORRUPT;
                        goto rmdir_scan_out;
                    }
                    /* This driver funnels filesystem status codes
                     * through pfs_vfs_error() at rmdir_out. Keep the
                     * internal status in the ProsenjitFS error domain here. */
                    rc = PROSENJITFS_ENOTEMPTY;
                    goto rmdir_scan_out;
                }
rmdir_scan_out:
                kfree(scan);
                goto rmdir_out;
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
    {
        uint64_t next_generation;
        rc = pfs_next_generation(child_inode.generation, &next_generation);
        if (rc != PROSENJITFS_OK)
            goto rmdir_out;
        freed_inode.generation = next_generation;
    }
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

static int pfs_vnode_rmdir(struct pros_vfs_vnode *parent, const char *name, const struct pros_vfs_credentials *auth) { struct pfs_vnode_data *data; int rc; if (!parent || !parent->private_data) return pfs_vnode_rmdir_unserialized(parent,name,auth); data=(struct pfs_vnode_data *)parent->private_data; if (!data || !data->fs) return pfs_vnode_rmdir_unserialized(parent,name,auth); rc=pfs_io_lock(data->fs); if(rc!=PROSENJITFS_OK) return pfs_vfs_error(rc); rc=pfs_vnode_rmdir_unserialized(parent,name,auth); pfs_io_unlock(data->fs); return rc; }
static int pfs_vnode_unlink_unserialized(struct pros_vfs_vnode *parent, const char *name,
                            const struct pros_vfs_credentials *auth)
{
    struct pfs_vnode_data *parent_data;
    struct pros_prosenjitfs_inode parent_inode;
    struct pros_prosenjitfs_dir_index dir_index;
    struct pros_prosenjitfs_dir_entry entry;
    struct pros_prosenjitfs_inode child_inode;
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
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    {
        int access_rc = vfs_vnode_authorize(parent, auth, 3u);
        if (access_rc != PROS_VFS_OK)
            return access_rc;
    }
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK ||
        name_length == 0u || name_length > PROSENJITFS_MAX_NAME ||
        (name_length == 1u && name[0] == '.') ||
        (name_length == 2u && name[0] == '.' && name[1] == '.'))
        return PROS_VFS_EINVAL;
    rc = pfs_vnode_validate_inode(parent_data, &parent_inode);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);
    if (parent_inode.type != PROSENJITFS_INODE_DIR)
        return PROS_VFS_ENOTDIR;
    { int dir_read_rc = pfs_read_dir_index(parent_data->fs, &parent_inode, parent_data->inode_number, &dir_index);
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
            current >= parent_data->fs->sb.total_blocks) {
            rc = PROSENJITFS_ECORRUPT;
            goto out;
        }
        rc = pfs_read_block(parent_data->fs->mount.device, current,
                            entry_block);
        if (rc != PROSENJITFS_OK)
            goto out;
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
    {
        uint64_t next_generation;
        rc = pfs_next_generation(child_inode.generation, &next_generation);
        if (rc != PROSENJITFS_OK)
            goto out_reset;
        freed_inode.generation = next_generation;
    }
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
        {
            int extent_rc = pfs_read_extent(parent_data->fs, current,
                                            &extent);
            if (extent_rc != PROSENJITFS_OK) {
                /* Preserve device I/O errors; only malformed extent metadata
                 * should be reported as corruption. */
                rc = extent_rc;
                goto out_reset;
            }
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


static int pfs_vnode_unlink(struct pros_vfs_vnode *parent, const char *name, const struct pros_vfs_credentials *auth) { struct pfs_vnode_data *data; int rc; if (!parent || !parent->private_data) return pfs_vnode_unlink_unserialized(parent,name,auth); data=(struct pfs_vnode_data *)parent->private_data; if (!data || !data->fs) return pfs_vnode_unlink_unserialized(parent,name,auth); rc=pfs_io_lock(data->fs); if(rc!=PROSENJITFS_OK) return pfs_vfs_error(rc); rc=pfs_vnode_unlink_unserialized(parent,name,auth); pfs_io_unlock(data->fs); return rc; }
static int pfs_vnode_create_unserialized(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result,
                            const struct pros_vfs_credentials *auth)
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
    /* Defense in depth: filesystem mutations must honor mount policy even
     * when an internal caller bypasses the VFS open-path check. */
    if (!parent->mount || !parent->mount->active || parent->mount->read_only)
        return PROS_VFS_EPERM;
    if (!data->fs || data->fs->read_only)
        return PROS_VFS_EPERM;
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    {
        int access_rc = vfs_vnode_authorize(parent, auth, 3u);
        if (access_rc != PROS_VFS_OK)
            return access_rc;
    }
    if (pfs_utf8_name_valid(name, &name_length) != PROSENJITFS_OK)
        return PROS_VFS_EINVAL;

    { int inode_read_rc = pfs_vnode_validate_inode(data, &parent_inode);
        if (inode_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(inode_read_rc);
        if (parent_inode.type != PROSENJITFS_INODE_DIR)
            return PROS_VFS_ENOTDIR; }
    rc = pfs_find_dir_entry(data->fs, &parent_inode, data->inode_number, name, 0);
    if (rc == PROSENJITFS_OK)
        return PROS_VFS_EEXIST;
    if (rc != PROSENJITFS_ENOENT)
        return pfs_vfs_error(rc);
    { int dir_read_rc = pfs_read_dir_index(data->fs, &parent_inode, data->inode_number, &dir_index);
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
    child_inode.uid = auth->uid;
    child_inode.gid = auth->gid;
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

    return pfs_vfs_error(pfs_make_vnode(data->fs, inode_number, parent, result));
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

static int pfs_vnode_create(struct pros_vfs_vnode *parent, const char *name, struct pros_vfs_vnode **result, const struct pros_vfs_credentials *auth) { struct pfs_vnode_data *data; int rc; if (!parent || !parent->private_data) return pfs_vnode_create_unserialized(parent,name,result,auth); data=(struct pfs_vnode_data *)parent->private_data; if (!data || !data->fs) return pfs_vnode_create_unserialized(parent,name,result,auth); rc=pfs_io_lock(data->fs); if(rc!=PROSENJITFS_OK) return pfs_vfs_error(rc); rc=pfs_vnode_create_unserialized(parent,name,result,auth); pfs_io_unlock(data->fs); return rc; }
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
                              uint64_t expected_inode_number,
                              struct pros_prosenjitfs_dir_index *index)
{
    uint8_t block[4096];
    uint32_t checksum;
    if (!fs || !inode || !index ||
        expected_inode_number >= fs->sb.inode_count ||
        inode->type != PROSENJITFS_INODE_DIR ||
        inode->dir_index < fs->sb.data_start ||
        inode->dir_index >= fs->sb.total_blocks ||
        !pfs_block_size_valid(fs->sb.block_size))
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
        index->inode >= fs->sb.inode_count ||
        index->inode != expected_inode_number ||
        index->generation != inode->generation)
        return PROSENJITFS_ECORRUPT;
    for (uint32_t bucket = 0u; bucket < PROSENJITFS_DIR_BUCKETS; ++bucket) {
        uint64_t head = index->heads[bucket];
        if (head != 0u &&
            (head < fs->sb.data_start || head >= fs->sb.total_blocks))
            return PROSENJITFS_ECORRUPT;
    }
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
    if (!fs || !entry || !pfs_block_size_valid(fs->sb.block_size) ||
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
    if (entry->name_hash !=
        pfs_name_hash(entry->name, entry->name_length))
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static int pfs_find_dir_entry(const struct pros_prosenjitfs *fs,
                              const struct pros_prosenjitfs_inode *dir_inode,
                              uint64_t dir_inode_number,
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
        int rc = pfs_read_dir_index(fs, dir_inode, dir_inode_number, &index);
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
                                    uint64_t dir_inode_number,
                                    uint64_t ordinal,
                                    struct pros_prosenjitfs_dir_entry *found)
{
    struct pros_prosenjitfs_dir_index index;
    struct pros_prosenjitfs_dir_entry entry;
    uint64_t seen = 0u;
    {
        int rc = pfs_read_dir_index(fs, dir_inode, dir_inode_number, &index);
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
                                   uint64_t dir_inode_number,
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
        int rc = pfs_read_dir_index(fs, dir_inode, dir_inode_number, &index);
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

static int pfs_vnode_lookup_unserialized(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result)
{
    struct pfs_vnode_data *data;
    if (!parent || !name || !result || !parent->private_data ||
        parent->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    *result = 0;
    data = (struct pfs_vnode_data *)parent->private_data;
    {
        struct pros_prosenjitfs_inode current_inode;
        int rc = pfs_vnode_validate_inode(data, &current_inode);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
    }

    if (name[0] == '.' && name[1] == '\0') {
        if (vfs_vnode_get(parent) != PROS_VFS_OK)
            return PROS_VFS_EINVAL;
        *result = parent;
        return PROS_VFS_OK;
    }

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        struct pros_prosenjitfs_inode dir_inode;
        int inode_read_rc = pfs_vnode_validate_inode(data, &dir_inode);
        if (inode_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(inode_read_rc);
        if (dir_inode.parent_inode == 0 || dir_inode.parent_inode == data->inode_number || data->inode_number == data->fs->sb.root_inode) {
            if (vfs_vnode_get(parent) != PROS_VFS_OK)
                return PROS_VFS_EINVAL;
            *result = parent;
            return PROS_VFS_OK;
        }
        return pfs_vfs_error(pfs_make_vnode(
            data->fs, dir_inode.parent_inode,
            parent ? parent->parent : NULL, result));
    }

    {
        struct pros_prosenjitfs_inode dir_inode;
        struct pros_prosenjitfs_dir_entry disk_entry;
        int rc;
        { int inode_read_rc = pfs_vnode_validate_inode(data, &dir_inode);
        if (inode_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(inode_read_rc);
        if (dir_inode.type != PROSENJITFS_INODE_DIR)
            return PROS_VFS_ENOTDIR; }
        rc = pfs_find_dir_entry(data->fs, &dir_inode, data->inode_number, name, &disk_entry);
        if (rc == PROSENJITFS_ENOENT)
            return PROS_VFS_ENOENT;
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);

        /* A valid entry checksum does not prove that its referenced inode is
         * the declared child. Fail closed on dangling or inconsistent links. */
        {
            struct pros_prosenjitfs_inode child_inode;
            rc = prosenjitfs_read_inode(data->fs, disk_entry.inode,
                                        &child_inode);
            if (rc == PROSENJITFS_ENOENT)
                return pfs_vfs_error(PROSENJITFS_ECORRUPT);
            if (rc != PROSENJITFS_OK)
                return pfs_vfs_error(rc);
            if (child_inode.type != disk_entry.type ||
                child_inode.parent_inode != data->inode_number ||
                (child_inode.type == PROSENJITFS_INODE_DIR &&
                 disk_entry.inode == data->inode_number))
                return pfs_vfs_error(PROSENJITFS_ECORRUPT);
        }
        return pfs_vfs_error(pfs_make_vnode(
            data->fs, disk_entry.inode, parent, result));
    }
}

static int pfs_vnode_lookup(struct pros_vfs_vnode *parent, const char *name, struct pros_vfs_vnode **result) { struct pfs_vnode_data *data; int rc; if (!parent || !parent->private_data) return pfs_vnode_lookup_unserialized(parent,name,result); data=(struct pfs_vnode_data *)parent->private_data; if (!data || !data->fs) return pfs_vnode_lookup_unserialized(parent,name,result); rc=pfs_io_lock(data->fs); if(rc!=PROSENJITFS_OK) return pfs_vfs_error(rc); rc=pfs_vnode_lookup_unserialized(parent,name,result); pfs_io_unlock(data->fs); return rc; }
static int pfs_vnode_rename_unserialized(struct pros_vfs_vnode *old_parent,
                                  const char *old_name,
                                  struct pros_vfs_vnode *new_parent,
                                  const char *new_name,
                                  const struct pros_vfs_credentials *auth)
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
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    {
        int access_rc = vfs_vnode_authorize(old_parent, auth, 3u);
        if (access_rc != PROS_VFS_OK)
            return access_rc;
        access_rc = vfs_vnode_authorize(new_parent, auth, 3u);
        if (access_rc != PROS_VFS_OK)
            return access_rc;
    }

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

    rc = pfs_vnode_validate_inode(old_data, &old_dir);
    if (rc != PROSENJITFS_OK || old_dir.type != PROSENJITFS_INODE_DIR)
        return rc == PROSENJITFS_OK ? PROS_VFS_ENOTDIR : pfs_vfs_error(rc);
    rc = pfs_vnode_validate_inode(new_data, &new_dir);
    if (rc != PROSENJITFS_OK || new_dir.type != PROSENJITFS_INODE_DIR)
        return rc == PROSENJITFS_OK ? PROS_VFS_ENOTDIR : pfs_vfs_error(rc);

    rc = pfs_find_dir_entry_lba(old_data->fs, &old_dir, old_data->inode_number, old_name, &entry,
                                 &old_lba, &old_prev);
    if (rc != PROSENJITFS_OK)
        return pfs_vfs_error(rc);

    {
        struct pros_prosenjitfs_dir_entry existing;
        rc = pfs_find_dir_entry(new_data->fs, &new_dir, new_data->inode_number, new_name, &existing);
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
        rc = pfs_read_dir_index(old_data->fs, &old_dir, old_data->inode_number, &old_index);
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

    rc = pfs_read_dir_index(old_data->fs, &old_dir, old_data->inode_number, &old_index);
    if (rc != PROSENJITFS_OK) goto rename_out_error;
    rc = pfs_read_dir_index(new_data->fs, &new_dir, new_data->inode_number, &new_index);
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

static int pfs_vnode_rename(struct pros_vfs_vnode *old_parent, const char *old_name, struct pros_vfs_vnode *new_parent, const char *new_name, const struct pros_vfs_credentials *auth) { struct pfs_vnode_data *a,*b; int rc; if(!old_parent||!new_parent||!old_parent->private_data||!new_parent->private_data) return pfs_vnode_rename_unserialized(old_parent,old_name,new_parent,new_name,auth); a=(struct pfs_vnode_data *)old_parent->private_data; b=(struct pfs_vnode_data *)new_parent->private_data; if(!a||!b||!a->fs||a->fs!=b->fs) return pfs_vnode_rename_unserialized(old_parent,old_name,new_parent,new_name,auth); rc=pfs_io_lock(a->fs); if(rc!=PROSENJITFS_OK) return pfs_vfs_error(rc); rc=pfs_vnode_rename_unserialized(old_parent,old_name,new_parent,new_name,auth); pfs_io_unlock(a->fs); return rc; }
static int pfs_vnode_readdir_unserialized(struct pros_vfs_vnode *directory, uint64_t *cookie,
                             struct pros_vfs_dirent *entry)
{
    if (!directory || !cookie || !entry ||
        directory->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    if (!directory->private_data)
        return PROS_VFS_EINVAL;
    {
        struct pfs_vnode_data *data =
            (struct pfs_vnode_data *)directory->private_data;
        struct pros_prosenjitfs_inode current_inode;
        int rc = pfs_vnode_validate_inode(data, &current_inode);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
    }
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
        { int inode_read_rc = pfs_vnode_validate_inode(data, &dir_inode);
        if (inode_read_rc != PROSENJITFS_OK)
            return pfs_vfs_error(inode_read_rc);
        if (dir_inode.type != PROSENJITFS_INODE_DIR)
            return PROS_VFS_ENOTDIR; }
        rc = pfs_dir_entry_by_ordinal(data->fs, &dir_inode, data->inode_number, ordinal, &disk_entry);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
        rc = prosenjitfs_read_inode(data->fs, disk_entry.inode, &child_inode);
        if (rc == PROSENJITFS_ENOENT)
            return pfs_vfs_error(PROSENJITFS_ECORRUPT);
        if (rc != PROSENJITFS_OK)
            return pfs_vfs_error(rc);
        if (child_inode.type != disk_entry.type ||
            child_inode.parent_inode != data->inode_number ||
            (child_inode.type == PROSENJITFS_INODE_DIR &&
             disk_entry.inode == data->inode_number))
            return pfs_vfs_error(PROSENJITFS_ECORRUPT);
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

static int pfs_vnode_readdir(struct pros_vfs_vnode *directory, uint64_t *cookie, struct pros_vfs_dirent *entry) { struct pfs_vnode_data *data; int rc; if(!directory||!directory->private_data) return pfs_vnode_readdir_unserialized(directory,cookie,entry); data=(struct pfs_vnode_data *)directory->private_data; if(!data||!data->fs) return pfs_vnode_readdir_unserialized(directory,cookie,entry); rc=pfs_io_lock(data->fs); if(rc!=PROSENJITFS_OK) return pfs_vfs_error(rc); rc=pfs_vnode_readdir_unserialized(directory,cookie,entry); pfs_io_unlock(data->fs); return rc; }
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
    if (!fs || !image || target < 2u || target >= fs->sb.total_blocks) {
        if (fs)
            (void)pfs_tx_reset(fs);
        return PROSENJITFS_EINVAL;
    }
    {
        uint64_t journal_end;
        if (pfs_region_end(fs->sb.journal_start, fs->sb.journal_blocks,
                           &journal_end) != PROSENJITFS_OK) {
            (void)pfs_tx_reset(fs);
            return PROSENJITFS_ECORRUPT;
        }
        if (target >= fs->sb.journal_start && target < journal_end) {
            (void)pfs_tx_reset(fs);
            return PROSENJITFS_EINVAL;
        }
    }
    for (uint32_t i = 0; i < fs->tx.count; ++i)
        if (fs->tx.targets[i] == target) {
            pfs_copy(fs->tx.images + (uint64_t)i * fs->sb.block_size,
                     image, fs->sb.block_size);
            return PROSENJITFS_OK;
        }
    if (fs->tx.count >= PROSENJITFS_MAX_TX_BLOCKS) {
        (void)pfs_tx_reset(fs);
        return PROSENJITFS_EOVERFLOW;
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

    if (!fs || !fs->scratch || !fs->mount.device)
        return PROSENJITFS_EINVAL;
    if (!pfs_block_size_valid(fs->sb.block_size) ||
        fs->sb.block_size != fs->mount.device->sector_size)
        return PROSENJITFS_ECORRUPT;

    pfs_zero(fs->scratch, fs->sb.block_size);

    rc = pfs_lba_add(fs->mount.device, fs->sb.journal_start, 0u, &lba);
    if (rc != PROSENJITFS_OK)
        return rc;

    rc = pfs_write_block(fs->mount.device, lba, fs->scratch);
    if (rc != PROSENJITFS_OK)
        return rc;

    rc = pfs_flush(fs->mount.device);
    if (rc != PROSENJITFS_OK)
        return rc;

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
    /*
     * A missing commit marker or a transaction-ID mismatch means the journal
     * does not contain the commit for this header. The commit slot is reused
     * and can still contain the previous transaction's valid record if power
     * fails before the new commit is written, so those cases are discarded.
     * A matching transaction ID with a contradictory record count is instead
     * inconsistent committed metadata and must fail closed.
     */
    if (c.magic != PROSENJITFS_JOURNAL_COMMIT ||
        c.transaction_id != h.transaction_id)
        return pfs_journal_clear(fs);
    if (c.record_count != h.record_count)
        return PROSENJITFS_ECORRUPT;
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
        !pfs_block_size_valid(fs->sb.block_size) ||
        sizeof(struct pros_prosenjitfs_journal_header) > fs->sb.block_size ||
        sizeof(struct pros_prosenjitfs_journal_commit) > fs->sb.block_size)
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
    uint8_t commit_may_be_durable = 0u;
#define PFS_TX_RETURN(value) do { \
    int pfs_tx_return_rc = (value); \
    if (pfs_tx_return_rc != PROSENJITFS_OK && commit_may_be_durable && fs) { \
        fs->read_only = 1u; \
        fs->mount.read_only = 1u; \
    } \
    (void)pfs_tx_reset(fs); \
    return pfs_tx_return_rc; \
} while (0)

    pfs_tx_diag_stage = 1u;
    pfs_tx_diag_count = 0u;
    if (!fs || !fs->scratch || !fs->tx.images || fs->read_only)
        PFS_TX_RETURN(PROSENJITFS_EINVAL);
    {
        int validate_rc = pfs_tx_validate(fs);
        if (validate_rc != PROSENJITFS_OK)
            PFS_TX_RETURN(validate_rc);
    }
    pfs_tx_diag_count = fs->tx.count;
    {
        int crash_rc = pfs_tx_crash_inject(1u);
        if (crash_rc != PROSENJITFS_OK)
            PFS_TX_RETURN(crash_rc);
    }

    header = fs->scratch;
    commit = fs->tx.images + (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                              fs->sb.block_size;
    pfs_zero(header, fs->sb.block_size);
    pfs_zero(commit, fs->sb.block_size);
    /*
     * The transaction ID is currently a mount-local sequence, and the
     * journal reuses commit slots. Retire the selected slot before publishing
     * a new header so a previous mount's valid commit record cannot be
     * mistaken for this transaction after a restart.
     */
    {
        uint64_t commit_lba;
        int rc = pfs_lba_add(fs->mount.device, fs->sb.journal_start,
                             1u + fs->tx.count, &commit_lba);
        if (rc == PROSENJITFS_OK)
            rc = pfs_write_block(fs->mount.device, commit_lba, commit);
        if (rc == PROSENJITFS_OK)
            rc = pfs_flush(fs->mount.device);
        if (rc != PROSENJITFS_OK)
            PFS_TX_RETURN(rc);
    }
    pfs_zero(&h, sizeof(h));
    if (fs->sb.transaction_sequence == UINT64_MAX)
        PFS_TX_RETURN(PROSENJITFS_EOVERFLOW);
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
            PFS_TX_RETURN(rc);
    }
    {
        int crash_rc = pfs_tx_crash_inject(2u);
        if (crash_rc != PROSENJITFS_OK)
            PFS_TX_RETURN(crash_rc);
    }
    pfs_tx_diag_stage = 3u;
    for (uint32_t i = 0; i < fs->tx.count; ++i) {
        uint64_t journal_lba;
        {
            int rc = pfs_lba_add(fs->mount.device, fs->sb.journal_start,
                                 1u + i, &journal_lba);
            if (rc != PROSENJITFS_OK)
                PFS_TX_RETURN(rc);
        }
            {
            int rc = pfs_write_block(fs->mount.device, journal_lba,
                                     fs->tx.images + (uint64_t)i * fs->sb.block_size);
            if (rc != PROSENJITFS_OK)
                PFS_TX_RETURN(rc);
        }
        {
            int crash_rc = pfs_tx_crash_inject(3u);
            if (crash_rc != PROSENJITFS_OK)
                PFS_TX_RETURN(crash_rc);
        }
    }
    pfs_tx_diag_stage = 4u;
    if (pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        PFS_TX_RETURN(PROSENJITFS_EIO);
    {
        int crash_rc = pfs_tx_crash_inject(4u);
        if (crash_rc != PROSENJITFS_OK)
            PFS_TX_RETURN(crash_rc);
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
                PFS_TX_RETURN(rc);
        }
            {
            /*
             * From the first commit-slot write attempt onward, a device error
             * may leave a valid or partially durable commit record. Any
             * failure must force this mount read-only until remount recovery.
             */
            commit_may_be_durable = 1u;
            int rc = pfs_write_block(fs->mount.device, commit_lba, commit);
            if (rc == PROSENJITFS_OK)
                rc = pfs_flush(fs->mount.device);
            if (rc != PROSENJITFS_OK)
                PFS_TX_RETURN(rc);
        }
    }
    {
        int crash_rc = pfs_tx_crash_inject(5u);
        if (crash_rc != PROSENJITFS_OK)
            PFS_TX_RETURN(crash_rc);
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
                PFS_TX_RETURN(rc);
        }
        {
            int crash_rc = pfs_tx_crash_inject(6u);
            if (crash_rc != PROSENJITFS_OK)
                PFS_TX_RETURN(crash_rc);
        }
    }
    pfs_tx_diag_stage = 7u;
    if (pfs_flush(fs->mount.device) != PROSENJITFS_OK)
        PFS_TX_RETURN(PROSENJITFS_EIO);
    {
        int crash_rc = pfs_tx_crash_inject(7u);
        if (crash_rc != PROSENJITFS_OK)
            PFS_TX_RETURN(crash_rc);
    }
    if (pfs_diag.verify_pending) {
        uint8_t verify[4096];
        pfs_tx_diag_stage = 8u;
        if (pfs_read_block(fs->mount.device, pfs_diag.physical_lba,
                           verify) != PROSENJITFS_OK)
            PFS_TX_RETURN(PROSENJITFS_EIO);
        pfs_diag.post_commit_byte = verify[188u];
        if (pfs_diag.post_commit_byte != 0xD6u)
            PFS_TX_RETURN(PROSENJITFS_EIO);
        /* Verify the diagnostic block only for the transaction that created
         * the diagnostic marker. Later transactions may legitimately free
         * and reuse that block (for example during truncate tests). */
        pfs_diag.verify_pending = 0u;
    }

    pfs_tx_diag_stage = 8u;
    {
        int crash_rc = pfs_tx_crash_inject(8u);
        if (crash_rc != PROSENJITFS_OK)
            PFS_TX_RETURN(crash_rc);
    }
    pfs_tx_diag_stage = 9u;
    {
        int crash_rc = pfs_tx_crash_inject(9u);
        if (crash_rc != PROSENJITFS_OK)
            PFS_TX_RETURN(crash_rc);
    }
    pfs_tx_diag_count_before_clear = fs->tx.count;
    pfs_tx_diag_clear_rc = pfs_journal_clear(fs);
    pfs_tx_diag_count_after_clear = fs->tx.count;
    PFS_TX_RETURN(pfs_tx_diag_clear_rc);
}

#undef PFS_TX_RETURN

static int pfs_bitmap_bit(const uint8_t *bitmap, uint64_t bit)
{
    return (bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0;
}

static int pfs_allocator_stage_unchecked(struct pros_prosenjitfs *fs,
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
        !pfs_block_size_valid(fs->sb.block_size))
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

static int pfs_allocator_stage(struct pros_prosenjitfs *fs,
                               uint64_t first, uint32_t count,
                               uint8_t allocate)
{
    int rc = pfs_allocator_stage_unchecked(fs, first, count, allocate);
    if (rc != PROSENJITFS_OK && fs)
        (void)pfs_tx_reset(fs);
    return rc;
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

static int pfs_allocator_range_valid(const struct pros_prosenjitfs *fs,
                                      uint64_t first, uint32_t count)
{
    if (!fs || count == 0u || !pfs_block_size_valid(fs->sb.block_size) ||
        fs->sb.total_blocks < fs->sb.data_start ||
        first < fs->sb.data_start || first >= fs->sb.total_blocks ||
        (uint64_t)count > fs->sb.total_blocks - first)
        return PROSENJITFS_EINVAL;
    if (!fs->mount.active || !fs->mount.device)
        return PROSENJITFS_EPERM;
    if (fs->mount.device->sector_size != fs->sb.block_size ||
        fs->mount.device->sector_count < fs->sb.total_blocks)
        return PROSENJITFS_ECORRUPT;
    return PROSENJITFS_OK;
}

static int pfs_allocator_range_state(const struct pros_prosenjitfs *fs,
                                     uint64_t first, uint32_t count,
                                     uint8_t want_allocated)
{
    uint64_t bits_per_block;
    uint64_t first_bitmap;
    uint64_t last_bitmap;
    uint8_t bitmap[4096];

    if (!fs || want_allocated > 1u || count == 0u ||
        !pfs_block_size_valid(fs->sb.block_size) ||
        first < fs->sb.data_start || first >= fs->sb.total_blocks ||
        (uint64_t)count > fs->sb.total_blocks - first)
        return PROSENJITFS_EINVAL;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;
    first_bitmap = first / bits_per_block;
    last_bitmap = ((uint64_t)first + count - 1u) / bits_per_block;
    if (last_bitmap >= fs->sb.block_bitmap_blocks)
        return PROSENJITFS_ECORRUPT;

    for (uint64_t bi = first_bitmap; bi <= last_bitmap; ++bi) {
        uint64_t lba;
        int rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                             bi, &lba);
        if (rc != PROSENJITFS_OK)
            return rc;
        rc = pfs_read_block(fs->mount.device, lba, bitmap);
        if (rc != PROSENJITFS_OK)
            return rc;

        uint64_t base = bi * bits_per_block;
        uint64_t start = first > base ? first - base : 0u;
        uint64_t end_abs = (uint64_t)first + count - 1u;
        uint64_t bitmap_last = base + bits_per_block - 1u;
        uint64_t end = end_abs < bitmap_last ? end_abs - base
                                             : bits_per_block - 1u;
        for (uint64_t bit = start; bit <= end; ++bit) {
            uint8_t allocated = (uint8_t)pfs_bitmap_bit(bitmap, bit);
            if (allocated != want_allocated)
                return want_allocated ? PROSENJITFS_EINVAL
                                       : PROSENJITFS_ENOSPC;
        }
    }
    return PROSENJITFS_OK;
}

/*
 * Keep-size preallocation: reserve and zero a contiguous logical range
 * without changing inode->size. The reservation is published atomically
 * with the allocation bitmap, extent metadata and inode metadata. The
 * bounded v1 implementation intentionally limits one request so the
 * journal transaction remains representable on every supported geometry.
 */
int prosenjitfs_preallocate_blocks(struct pros_prosenjitfs *fs,
                                   uint64_t inode_number,
                                   uint64_t logical_block,
                                   uint32_t count)
{
    struct pros_prosenjitfs_inode inode;
    struct pros_prosenjitfs_extent tail;
    uint64_t tail_lba = 0u;
    uint64_t max_end = 0u;
    uint64_t run_start = 0u;
    uint64_t run = 0u;
    uint64_t extent_lba = 0u;
    uint64_t inode_lba = 0u;
    uint32_t inode_offset = 0u;
    uint8_t have_tail = 0u;
    uint8_t coalesce = 0u;
    uint8_t bitmap_zero[4096];
    uint8_t extent_image[4096];
    uint8_t inode_image[4096];
    uint64_t bits_per_block;
    int rc;

    if (!fs || fs->read_only || !fs->mount.active || !fs->mount.device ||
        count == 0u || count > 8u ||
        !pfs_block_size_valid(fs->sb.block_size) ||
        inode_number >= fs->sb.inode_count)
        return PROSENJITFS_EINVAL;

    rc = prosenjitfs_read_inode(fs, inode_number, &inode);
    if (rc != PROSENJITFS_OK)
        return rc;
    if (inode.type != PROSENJITFS_INODE_FILE)
        return PROSENJITFS_EINVAL;

    /*
     * Preallocation is append-only in v1: the requested logical range must
     * not overlap an existing extent. This makes keep-size reservations
     * deterministic and avoids creating a second mapping for one block.
     */
    {
        uint64_t current = inode.extent_head;
        uint64_t steps = 0u;
        while (current != 0u) {
            struct pros_prosenjitfs_extent extent;
            if (++steps > fs->sb.total_blocks)
                return PROSENJITFS_ECORRUPT;
            rc = pfs_read_extent(fs, current, &extent);
            if (rc != PROSENJITFS_OK)
                return rc;
            if (extent.logical_block > UINT64_MAX - extent.block_count)
                return PROSENJITFS_ECORRUPT;
            if (extent.logical_block + extent.block_count > max_end)
                max_end = extent.logical_block + extent.block_count;
            if (extent.next == 0u) {
                tail = extent;
                tail_lba = current;
                have_tail = 1u;
            }
            current = extent.next;
        }
    }
    if (logical_block < max_end ||
        logical_block > UINT64_MAX - count)
        return PROSENJITFS_EINVAL;

    bits_per_block = (uint64_t)fs->sb.block_size * 8u;

    /* Find a physically contiguous free data run without skipping bitmap boundaries. */
    {
        uint64_t bitmap_index;
        for (bitmap_index = fs->sb.data_start / bits_per_block;
             bitmap_index <= (fs->sb.total_blocks - 1u) / bits_per_block;
             ++bitmap_index) {
            uint64_t bitmap_lba;
            uint8_t bitbuf[4096];
            uint64_t base = bitmap_index * bits_per_block;
            uint64_t first = fs->sb.data_start > base ? fs->sb.data_start : base;
            uint64_t last = fs->sb.total_blocks - 1u <
                            base + bits_per_block - 1u
                            ? fs->sb.total_blocks - 1u
                            : base + bits_per_block - 1u;

            rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                             bitmap_index, &bitmap_lba);
            if (rc != PROSENJITFS_OK)
                return rc;
            rc = pfs_read_block(fs->mount.device, bitmap_lba, bitbuf);
            if (rc != PROSENJITFS_OK)
                return rc;

            for (uint64_t b = first; b <= last; ++b) {
                if (!pfs_bitmap_bit(bitbuf, b % bits_per_block)) {
                    if (run == 0u)
                        run_start = b;
                    if (run == UINT64_MAX)
                        return PROSENJITFS_EOVERFLOW;
                    ++run;
                    if (run == count)
                        goto run_found;
                } else {
                    run = 0u;
                }
            }
        }
    }
    return PROSENJITFS_ENOSPC;

run_found:
    if (pfs_allocator_range_valid(fs, run_start, count) !=
        PROSENJITFS_OK)
        return PROSENJITFS_ECORRUPT;
    rc = pfs_allocator_range_state(fs, run_start, count, 0u);
    if (rc != PROSENJITFS_OK)
        return rc;

    if (have_tail &&
        tail.logical_block <= UINT64_MAX - tail.block_count &&
        tail.logical_block + tail.block_count == logical_block &&
        tail.physical_block <= UINT64_MAX - tail.block_count &&
        tail.physical_block + tail.block_count == run_start)
        coalesce = 1u;

    if (!coalesce) {
        /* Reserve a separate extent metadata block outside the data run. */
        for (uint64_t b = fs->sb.data_start; b < fs->sb.total_blocks; ++b) {
            if (b >= run_start && b - run_start < count)
                continue;
            int free_rc;

            free_rc = pfs_data_block_is_free(fs, b);
            if (free_rc < 0)
                return free_rc;
            if (free_rc == 1) {
                extent_lba = b;
                break;
            }
        }
        if (extent_lba == 0u)
            return PROSENJITFS_ENOSPC;
    } else {
        extent_lba = tail_lba;
    }

    /*
     * Data blocks are zeroed before metadata publication. If the metadata
     * transaction fails, the blocks remain unallocated and therefore cannot
     * become visible through the filesystem.
     */
    pfs_zero(bitmap_zero, fs->sb.block_size);
    for (uint32_t i = 0u; i < count; ++i) {
        uint64_t lba;
        rc = pfs_lba_add(fs->mount.device, run_start, i, &lba);
        if (rc != PROSENJITFS_OK)
            return rc;
        rc = pfs_write_block(fs->mount.device, lba, bitmap_zero);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    if (!coalesce) {
        rc = pfs_write_block(fs->mount.device, extent_lba, bitmap_zero);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    rc = pfs_flush(fs->mount.device);
    if (rc != PROSENJITFS_OK)
        return rc;

    if (inode.allocated_bytes >
        UINT64_MAX - (uint64_t)count * fs->sb.block_size)
        return PROSENJITFS_EOVERFLOW;
    inode.allocated_bytes += (uint64_t)count * fs->sb.block_size;

    if (coalesce) {
        struct pros_prosenjitfs_extent updated = tail;
        updated.block_count += count;
        pfs_zero(extent_image, fs->sb.block_size);
        pfs_copy(extent_image, &updated, sizeof(updated));
        pfs_put32(extent_image +
                  offsetof(struct pros_prosenjitfs_extent, checksum),
                  pfs_crc_with_zero32(
                      extent_image, sizeof(updated),
                      (uint32_t)offsetof(struct pros_prosenjitfs_extent,
                                         checksum)));
    } else {
        struct pros_prosenjitfs_extent updated;
        pfs_zero(&updated, sizeof(updated));
        updated.magic = PROSENJITFS_EXTENT_MAGIC;
        updated.logical_block = logical_block;
        updated.physical_block = run_start;
        updated.block_count = count;
        updated.next = 0u;
        pfs_zero(extent_image, fs->sb.block_size);
        pfs_copy(extent_image, &updated, sizeof(updated));
        pfs_put32(extent_image +
                  offsetof(struct pros_prosenjitfs_extent, checksum),
                  pfs_crc_with_zero32(
                      extent_image, sizeof(updated),
                      (uint32_t)offsetof(struct pros_prosenjitfs_extent,
                                         checksum)));

        if (have_tail) {
            uint8_t tail_image[4096];
            rc = pfs_read_block(fs->mount.device, tail_lba, tail_image);
            if (rc != PROSENJITFS_OK)
                return rc;
            pfs_copy(&tail, tail_image, sizeof(tail));
            tail.next = extent_lba;
            /* Publish the updated link into the transaction image before
             * recomputing its checksum. Updating only the local struct leaves
             * the on-disk tail.next unchanged and silently disconnects the
             * newly preallocated extent. */
            pfs_copy(tail_image, &tail, sizeof(tail));
            pfs_put32(tail_image +
                      offsetof(struct pros_prosenjitfs_extent, checksum),
                      pfs_crc_with_zero32(
                          tail_image, sizeof(tail),
                          (uint32_t)offsetof(struct pros_prosenjitfs_extent,
                                             checksum)));
            rc = pfs_tx_reset(fs);
            if (rc != PROSENJITFS_OK)
                return rc;
            rc = pfs_allocator_stage(fs, run_start, count, 1u);
            if (rc == PROSENJITFS_OK)
                rc = pfs_allocator_stage(fs, extent_lba, 1u, 1u);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_add(fs, extent_lba, extent_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_add(fs, tail_lba, tail_image);
            if (rc == PROSENJITFS_OK)
                rc = pfs_inode_block_location(fs, inode_number,
                                              &inode_lba, &inode_offset);
            if (rc == PROSENJITFS_OK)
                rc = pfs_read_block(fs->mount.device, inode_lba, inode_image);
            if (rc == PROSENJITFS_OK) {
                pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
                pfs_put32(inode_image + inode_offset +
                          offsetof(struct pros_prosenjitfs_inode, checksum),
                          pfs_crc_with_zero32(
                              inode_image + inode_offset,
                              sizeof(inode),
                              (uint32_t)offsetof(
                                  struct pros_prosenjitfs_inode, checksum)));
                rc = pfs_tx_add(fs, inode_lba, inode_image);
            }
            if (rc == PROSENJITFS_OK)
                rc = pfs_tx_commit(fs);
            return rc;
        }
    }

    rc = pfs_tx_reset(fs);
    if (rc != PROSENJITFS_OK)
        return rc;
    rc = pfs_allocator_stage(fs, run_start, count, 1u);
    if (rc == PROSENJITFS_OK && !coalesce)
        rc = pfs_allocator_stage(fs, extent_lba, 1u, 1u);
    if (rc == PROSENJITFS_OK)
        rc = pfs_tx_add(fs, extent_lba, extent_image);
    if (rc == PROSENJITFS_OK)
        rc = pfs_inode_block_location(fs, inode_number,
                                      &inode_lba, &inode_offset);
    if (rc == PROSENJITFS_OK)
        rc = pfs_read_block(fs->mount.device, inode_lba, inode_image);
    if (rc == PROSENJITFS_OK) {
        pfs_copy(inode_image + inode_offset, &inode, sizeof(inode));
        pfs_put32(inode_image + inode_offset +
                  offsetof(struct pros_prosenjitfs_inode, checksum),
                  pfs_crc_with_zero32(
                      inode_image + inode_offset, sizeof(inode),
                      (uint32_t)offsetof(struct pros_prosenjitfs_inode,
                                         checksum)));
        rc = pfs_tx_add(fs, inode_lba, inode_image);
    }
    if (rc != PROSENJITFS_OK)
        (void)pfs_tx_reset(fs);
    return rc == PROSENJITFS_OK ? pfs_tx_commit(fs) : rc;
}

int prosenjitfs_alloc_blocks(struct pros_prosenjitfs *fs,
                             uint32_t count, uint64_t *first_block)
{
    uint64_t run_start = 0u;
    uint64_t run = 0u;
    uint64_t data_start;
    uint64_t bits_per_block;
    uint64_t previous_bitmap = UINT64_MAX;
    uint8_t bitbuf[4096];

    if (!fs || !first_block || count == 0u || fs->read_only)
        return PROSENJITFS_EINVAL;
    *first_block = 0u;

    {
        int rc = pfs_allocator_range_valid(fs, fs->sb.data_start, 1u);
        if (rc != PROSENJITFS_OK)
            return rc;
    }
    data_start = fs->sb.data_start;
    bits_per_block = (uint64_t)fs->sb.block_size * 8u;

    /*
     * Find a first-fit physically contiguous run. Bitmap blocks are cached
     * for the duration of the scan so large allocations do not reread the
     * same bitmap sector once per data block.
     */
    for (uint64_t b = data_start; b < fs->sb.total_blocks; ++b) {
        uint64_t bitmap_index = b / bits_per_block;
        if (bitmap_index != previous_bitmap) {
            uint64_t bitmap_lba;
            int rc = pfs_lba_add(fs->mount.device, fs->sb.block_bitmap_start,
                                 bitmap_index, &bitmap_lba);
            if (rc != PROSENJITFS_OK)
                return rc;
            rc = pfs_read_block(fs->mount.device, bitmap_lba, bitbuf);
            if (rc != PROSENJITFS_OK)
                return rc;
            previous_bitmap = bitmap_index;
        }

        if (!pfs_bitmap_bit(bitbuf, b % bits_per_block)) {
            if (run == 0u)
                run_start = b;
            if (run == UINT64_MAX)
                return PROSENJITFS_EOVERFLOW;
            ++run;
            if (run >= count) {
                if (pfs_allocator_range_valid(fs, run_start, count) !=
                    PROSENJITFS_OK)
                    return PROSENJITFS_ECORRUPT;

                /*
                 * Revalidate the exact run against the current on-disk
                 * bitmap immediately before staging. This keeps the public
                 * allocator safe if another allocator path changed the
                 * bitmap between the scan and commit preparation.
                 */
                {
                    int rc = pfs_allocator_range_state(fs, run_start, count, 0u);
                    if (rc != PROSENJITFS_OK)
                        return rc;
                }
                {
                    int rc = pfs_allocator_update(fs, run_start, count, 1u);
                    if (rc != PROSENJITFS_OK)
                        return rc;
                }
                *first_block = run_start;
                return PROSENJITFS_OK;
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
    int rc;

    if (!fs || count == 0u || fs->read_only)
        return PROSENJITFS_EINVAL;
    rc = pfs_allocator_range_valid(fs, first_block, count);
    if (rc != PROSENJITFS_OK)
        return rc;

    /*
     * Validate the complete range before modifying any bitmap image. This
     * prevents a mixed allocated/free range from being partially staged and
     * makes double-free detection atomic at the allocator API boundary.
     */
    rc = pfs_allocator_range_state(fs, first_block, count, 1u);
    if (rc != PROSENJITFS_OK)
        return rc;
    return pfs_allocator_update(fs, first_block, count, 0u);
}

static int pfs_inode_bitmap_stage_unchecked(struct pros_prosenjitfs *fs,
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
        !pfs_block_size_valid(fs->sb.block_size))
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

static int pfs_inode_bitmap_stage(struct pros_prosenjitfs *fs,
                                  uint64_t inode_number, uint8_t allocate)
{
    int rc = pfs_inode_bitmap_stage_unchecked(fs, inode_number, allocate);
    if (rc != PROSENJITFS_OK && fs)
        (void)pfs_tx_reset(fs);
    return rc;
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
        !pfs_block_size_valid(fs->sb.block_size))
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
            uint64_t generation;
            int gen_rc = pfs_allocate_generation(fs, inode, &generation);
            if (gen_rc != PROSENJITFS_OK)
                return gen_rc;
            {
                int alloc_rc = pfs_inode_bitmap_update(fs, inode, 1u);
                if (alloc_rc != PROSENJITFS_OK)
                    return alloc_rc;
            }
            *inode_number = inode;
            return PROSENJITFS_OK;
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
        !pfs_block_size_valid(fs->sb.block_size))
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

    /* A checksum authenticates bytes, not their semantic validity. */
    if (inode->type != PROSENJITFS_INODE_FILE &&
        inode->type != PROSENJITFS_INODE_DIR &&
        inode->type != PROSENJITFS_INODE_SYMLINK)
        return PROSENJITFS_ECORRUPT;
    if ((inode->flags & (uint8_t)~PROSENJITFS_INODE_FLAG_SPARSE) != 0u ||
        inode->generation == 0u ||
        inode->parent_inode >= fs->sb.inode_count)
        return PROSENJITFS_ECORRUPT;

    {
        uint64_t capacity_blocks;
        uint64_t size_blocks;
        if (fs->sb.total_blocks < fs->sb.data_start)
            return PROSENJITFS_ECORRUPT;
        capacity_blocks = fs->sb.total_blocks - fs->sb.data_start;
        size_blocks = inode->size / fs->sb.block_size;
        if (size_blocks > capacity_blocks ||
            (size_blocks == capacity_blocks &&
             (inode->size % fs->sb.block_size) != 0u))
            return PROSENJITFS_ECORRUPT;
        /* v1 allocated-byte accounting is expressed in whole data blocks. */
        if ((inode->allocated_bytes % fs->sb.block_size) != 0u ||
            inode->allocated_bytes / fs->sb.block_size > capacity_blocks)
            return PROSENJITFS_ECORRUPT;
    }
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
        !pfs_block_size_valid(fs->sb.block_size))
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
    /* Both copies passed their own checksums, but conflicting metadata layouts
     * are still unsafe. Fail closed rather than arbitrarily trusting the primary. */
    if (primary_ok && backup_ok && !pfs_superblocks_equal(&primary, &backup)) {
        kfree(fs->tx.images); kfree(fs->scratch); fs->tx.images = 0; fs->scratch = 0;
        return PROSENJITFS_ECORRUPT;
    }
    fs->sb = primary_ok ? primary : backup;
    /* Either missing redundant copy is degraded, regardless of which one survived. */
    fs->degraded_superblock = (primary_ok != backup_ok) ? 1u : 0u;
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
        if (rc != PROSENJITFS_OK) {
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
     * Exclude integrity scans and provider callbacks while checking vnode
     * ownership, flushing, and draining the mount. The root reference is
     * sampled atomically; the provider gate makes the teardown decision
     * mutually exclusive with all ProsenjitFS callbacks.
     */
    rc = pfs_io_lock(fs);
    if (rc != PROSENJITFS_OK)
        return rc;
    if (fs->mount.root &&
        __atomic_load_n(&fs->mount.root->refcount, __ATOMIC_ACQUIRE) > 1u) {
        pfs_io_unlock(fs);
        return PROSENJITFS_EBUSY;
    }

    rc = pfs_flush(fs->mount.device);
    if (rc != PROSENJITFS_OK) {
        pfs_io_unlock(fs);
        return rc;
    }
    rc = vfs_mount_deactivate(&fs->mount);
    if (rc != PROS_VFS_OK) {
        pfs_io_unlock(fs);
        return pfs_vfs_error(rc);
    }
    if (fs->tx.images) kfree(fs->tx.images);
    if (fs->scratch) kfree(fs->scratch);
    fs->tx.images = 0;
    fs->scratch = 0;
    pfs_io_unlock(fs);
    return PROSENJITFS_OK;
}


struct pfs_test_disk {
    /* data[] is the durable medium; volatile_data[] models a device write
     * cache. A power-loss reset discards only dirty volatile blocks. */
    uint8_t data[512u * 512u];
    uint8_t volatile_data[512u * 512u];
    uint8_t dirty[512u / 8u];
    /* Fail the selected future block read to test I/O error classification. */
    uint32_t fail_read_countdown;
    /* Fail the selected future block write before any bytes are accepted. */
    uint32_t fail_write_countdown;
    /* Fail one read touching this sector, targeted at journal recovery. */
    uint64_t fail_read_lba;
    uint8_t fail_read_lba_once;
    /* Fail a selected future flush to model a device durability error. */
    uint32_t fail_flush_countdown;
    /* Persist the selected flush, then report an ambiguous I/O failure. */
    uint32_t fail_flush_after_persist_countdown;
    /* Fail the next journal commit-record block write before any bytes land. */
    uint8_t fail_commit_write_once;
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
    if (disk->fail_read_countdown != 0u) {
        --disk->fail_read_countdown;
        if (disk->fail_read_countdown == 0u)
            return PROS_BLOCK_EIO;
    }
    if (disk->fail_read_lba_once &&
        disk->fail_read_lba >= lba &&
        disk->fail_read_lba - lba < count) {
        disk->fail_read_lba_once = 0u;
        return PROS_BLOCK_EIO;
    }
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
    if (disk->fail_write_countdown != 0u) {
        --disk->fail_write_countdown;
        if (disk->fail_write_countdown == 0u)
            return PROS_BLOCK_EIO;
    }
    if (disk->fail_commit_write_once && count == 1u) {
        uint32_t magic = 0u;
        pfs_copy(&magic, buffer, sizeof(magic));
        if (magic == PROSENJITFS_JOURNAL_COMMIT) {
            disk->fail_commit_write_once = 0u;
            return PROS_BLOCK_EIO;
        }
    }
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
    int fail_after_persist = 0;
    if (disk->fail_flush_countdown != 0u) {
        --disk->fail_flush_countdown;
        if (disk->fail_flush_countdown == 0u)
            return PROS_BLOCK_EIO;
    }
    if (disk->fail_flush_after_persist_countdown != 0u) {
        --disk->fail_flush_after_persist_countdown;
        if (disk->fail_flush_after_persist_countdown == 0u)
            fail_after_persist = 1;
    }
    for (uint64_t lba = 0u; lba < 512u; ++lba) {
        if (pfs_test_dirty(disk, lba)) {
            pfs_copy(disk->data + lba * 512u,
                     disk->volatile_data + lba * 512u, 512u);
            pfs_test_clear_dirty(disk, lba);
        }
    }
    return fail_after_persist ? PROS_BLOCK_EIO : PROS_BLOCK_OK;
}

static void pfs_test_power_loss(struct pfs_test_disk *disk)
{
    if (!disk)
        return;

    /*
     * A power loss discards the volatile device cache rather than merely
     * forgetting which cache lines were dirty.  Rebuild the cache from the
     * durable medium so stale pre-crash bytes can never leak into a later
     * write/flush cycle in the test harness.
     */
    pfs_copy(disk->volatile_data, disk->data, sizeof(disk->data));
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

/* Exception diagnostics can observe this state asynchronously while a
 * filesystem self-test is in progress. Keep each checkpoint visible even if
 * the kernel faults before the self-test can return normally. */
volatile uint32_t pfs_selftest_diag_stage;
static volatile int pfs_selftest_diag_rc;
#define PFS_SELFTEST_CHECKPOINT(stage_value) do { \
    __asm__ volatile ("" ::: "memory"); \
    pfs_selftest_diag_stage = (stage_value); \
    __asm__ volatile ("" ::: "memory"); \
} while (0)

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

/*
 * Shared parent-chain validator. Keeping this as a separately testable unit
 * lets the self-test exercise a disconnected cycle without first tripping
 * earlier directory-entry consistency checks.
 */
static int pfs_parent_chain_reaches_root(
    const struct pros_prosenjitfs *fs, uint64_t inode_number)
{
    struct pros_prosenjitfs_inode inode;
    uint64_t current_parent;
    uint64_t steps = 0u;
    int inode_rc;

    if (!fs || !fs->mount.device || inode_number >= fs->sb.inode_count)
        return PROSENJITFS_ECORRUPT;
    if (inode_number == fs->sb.root_inode)
        return PROSENJITFS_OK;

    inode_rc = prosenjitfs_read_inode(fs, inode_number, &inode);
    if (inode_rc != PROSENJITFS_OK)
        return inode_rc == PROSENJITFS_ENOENT
             ? PROSENJITFS_ENOENT
             : inode_rc == PROSENJITFS_EIO
             ? PROSENJITFS_EIO : PROSENJITFS_ECORRUPT;

    current_parent = inode.parent_inode;
    while (current_parent != fs->sb.root_inode) {
        struct pros_prosenjitfs_inode parent_inode;
        if (++steps >= fs->sb.inode_count ||
            current_parent >= fs->sb.inode_count)
            return PROSENJITFS_ECORRUPT;
        inode_rc = prosenjitfs_read_inode(fs, current_parent, &parent_inode);
        if (inode_rc == PROSENJITFS_EIO)
            return PROSENJITFS_EIO;
        if (inode_rc != PROSENJITFS_OK ||
            parent_inode.type != PROSENJITFS_INODE_DIR)
            return PROSENJITFS_ECORRUPT;
        current_parent = parent_inode.parent_inode;
    }
    return PROSENJITFS_OK;
}

static int pfs_selftest_consistency(
    const struct pros_prosenjitfs *fs,
    struct pros_prosenjitfs_integrity_report *report)
{
    uint64_t bits_per_block;
    uint64_t owner_bytes;
    uint8_t *owner = 0;
    uint8_t *dir_refs = 0;
    uint8_t bitmap[4096];
    struct pros_prosenjitfs_inode inode;
    uint32_t name_length = 0u;
    int rc = PROSENJITFS_EIO;
    uint32_t fail_line = 0u;
    uint64_t orphaned_blocks = 0u;
    uint64_t orphaned_inodes = 0u;

    if (report) {
        report->status = PROSENJITFS_EIO;
        report->first_error_location = 0u;
        report->inodes_checked = 0u;
        report->block_bitmap_bits_checked = 0u;
        report->orphaned_blocks_reported = 0u;
        report->first_orphaned_block = 0u;
        report->orphaned_inodes_reported = 0u;
        report->first_orphaned_inode = 0u;
    }

    if (!fs || !fs->mount.device || !fs->mount.active ||
        !pfs_block_size_valid(fs->sb.block_size) ||
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

    if (fs->sb.root_inode >= fs->sb.inode_count)
        return PROSENJITFS_ECORRUPT;
    rc = prosenjitfs_read_inode(fs, fs->sb.root_inode, &inode);
    if (rc != PROSENJITFS_OK)
        return rc == PROSENJITFS_EIO ? PROSENJITFS_EIO
                                     : PROSENJITFS_ECORRUPT;
    if (inode.type != PROSENJITFS_INODE_DIR ||
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
        if (report)
            ++report->inodes_checked;
        uint64_t bi = inode_number / bits_per_block;
        uint64_t within = inode_number % bits_per_block;
        uint64_t lba;
        int allocated;

        if (bi >= fs->sb.inode_bitmap_blocks ||
            pfs_lba_add(fs->mount.device, fs->sb.inode_bitmap_start,
                        bi, &lba) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            fail_line = __LINE__;
            goto consistency_out;
        }
        if (pfs_read_block(fs->mount.device, lba, bitmap) != PROSENJITFS_OK) {
            /* A failed device read is not evidence of on-disk corruption. */
            rc = PROSENJITFS_EIO;
            goto consistency_out;
        }

        allocated = pfs_bitmap_bit(bitmap, within);
        rc = prosenjitfs_read_inode(fs, inode_number, &inode);
        if (rc == PROSENJITFS_EIO)
            goto consistency_out;
        if (!allocated) {
            if (rc != PROSENJITFS_ENOENT) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                        goto consistency_out;
            }
            continue;
        }
        if (rc == PROSENJITFS_ENOENT) {
            /*
             * The inode bitmap claims this record is allocated, but the
             * inode reader sees a free/empty record. Report this as ordinary
             * corruption with a real source location. Do not encode the inode
             * number in the negative return value: the public report wrapper
             * reserves that encoding for source-line diagnostics and would
             * otherwise publish a bogus location (10000 + inode number).
             */
            rc = PROSENJITFS_ECORRUPT;
            fail_line = __LINE__;
            goto consistency_out;
        }
        if (rc != PROSENJITFS_OK ||
            (inode.type != PROSENJITFS_INODE_FILE &&
             inode.type != PROSENJITFS_INODE_DIR &&
             inode.type != PROSENJITFS_INODE_SYMLINK) ||
            inode.parent_inode >= fs->sb.inode_count ||
            inode.size / fs->sb.block_size >
                fs->sb.total_blocks - fs->sb.data_start ||
            (inode.size / fs->sb.block_size ==
                 fs->sb.total_blocks - fs->sb.data_start &&
             inode.size % fs->sb.block_size != 0u)) {
            rc = PROSENJITFS_ECORRUPT;
            fail_line = __LINE__;
                        goto consistency_out;
        }

        if (inode_number != fs->sb.root_inode) {
            struct pros_prosenjitfs_inode parent_inode;
            rc = prosenjitfs_read_inode(fs, inode.parent_inode, &parent_inode);
            if (rc == PROSENJITFS_EIO)
                goto consistency_out;
            if (rc != PROSENJITFS_OK || parent_inode.type != PROSENJITFS_INODE_DIR) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                        goto consistency_out;
            }
        }

        if (inode.type == PROSENJITFS_INODE_DIR) {
            struct pros_prosenjitfs_dir_index index;
            if (inode.extent_head != 0u ||
                inode.dir_index < fs->sb.data_start ||
                inode.dir_index >= fs->sb.total_blocks ||
                pfs_consistency_mark(owner, fs->sb.total_blocks,
                                     inode.dir_index) != PROSENJITFS_OK) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                goto consistency_out;
            }
            rc = pfs_read_dir_index(fs, &inode, inode_number, &index);
            if (rc != PROSENJITFS_OK) {
                if (rc != PROSENJITFS_EIO) {
                    rc = PROSENJITFS_ECORRUPT;
                    fail_line = __LINE__;
                }
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
                                             current) != PROSENJITFS_OK) {
                        rc = PROSENJITFS_ECORRUPT;
                        fail_line = __LINE__;
                        goto consistency_out;
                    }
                    if (pfs_read_block(fs->mount.device, current,
                                       entry_block) != PROSENJITFS_OK) {
                        rc = PROSENJITFS_EIO;
                        goto consistency_out;
                    }
                    pfs_copy(&entry, entry_block, sizeof(entry));
                    if (entry.magic != PROSENJITFS_ENTRY_MAGIC ||
                        entry.name_length == 0u ||
                        entry.name_length > PROSENJITFS_MAX_NAME ||
                        entry.name[entry.name_length] != 0u ||
                        entry.inode >= fs->sb.inode_count ||
                        entry.next >= fs->sb.total_blocks ||
                        (entry.next != 0u &&
                         entry.next < fs->sb.data_start) ||
                        entry.name_hash % PROSENJITFS_DIR_BUCKETS != bucket ||
                        pfs_utf8_name_valid((const char *)entry.name,
                                            &name_length) != PROSENJITFS_OK ||
                        name_length != entry.name_length ||
                        entry.name_hash !=
                            pfs_name_hash(entry.name, entry.name_length) ||
                        pfs_crc_with_zero32(
                            entry_block, sizeof(entry),
                            (uint32_t)offsetof(
                                struct pros_prosenjitfs_dir_entry,
                                checksum)) != entry.checksum) {
                        rc = PROSENJITFS_ECORRUPT;
                        fail_line = __LINE__;
                        goto consistency_out;
                    }
                    /*
                     * A directory bucket is a namespace, not merely a
                     * hash-chain.  A corrupted image containing two entries
                     * with the same name would otherwise pass checksum,
                     * inode-parent, and bitmap ownership checks while making
                     * lookup semantics dependent on chain order.  The normal
                     * insertion path already rejects EEXIST; the consistency
                     * checker must reject the same condition after corruption
                     * or an incomplete recovery.
                     *
                     * This is intentionally a bounded checker-only scan:
                     * every earlier entry in this bucket has already been
                     * validated by this loop, and the chain itself is bounded
                     * by total_blocks.
                     */
                    {
                        uint64_t prior = index.heads[bucket];
                        uint64_t prior_steps = 0u;
                        while (prior != current) {
                            struct pros_prosenjitfs_dir_entry prior_entry;
                            uint8_t prior_block[4096];

                            if (++prior_steps > fs->sb.total_blocks ||
                                prior < fs->sb.data_start ||
                                prior >= fs->sb.total_blocks) {
                                rc = PROSENJITFS_ECORRUPT;
                                fail_line = __LINE__;
                                goto consistency_out;
                            }
                            if (pfs_read_block(fs->mount.device, prior,
                                               prior_block) !=
                                    PROSENJITFS_OK) {
                                rc = PROSENJITFS_EIO;
                                goto consistency_out;
                            }
                            pfs_copy(&prior_entry, prior_block,
                                     sizeof(prior_entry));
                            if (prior_entry.name_hash == entry.name_hash &&
                                prior_entry.name_length == entry.name_length) {
                                uint32_t same = 1u;
                                for (uint32_t ni = 0u;
                                     ni < entry.name_length; ++ni) {
                                    if (prior_entry.name[ni] != entry.name[ni]) {
                                        same = 0u;
                                        break;
                                    }
                                }
                                if (same != 0u) {
                                    rc = PROSENJITFS_ECORRUPT;
                                    fail_line = __LINE__;
                        goto consistency_out;
                                }
                            }
                            prior = prior_entry.next;
                        }
                    }
                    {
                        struct pros_prosenjitfs_inode child;
                        int child_rc = prosenjitfs_read_inode(
                            fs, entry.inode, &child);
                        if (child_rc == PROSENJITFS_EIO) {
                            rc = PROSENJITFS_EIO;
                            goto consistency_out;
                        }
                        if (child_rc != PROSENJITFS_OK ||
                            child.parent_inode != inode_number ||
                            ((entry.type == PROSENJITFS_INODE_DIR) !=
                             (child.type == PROSENJITFS_INODE_DIR)) ||
                            (entry.type != PROSENJITFS_INODE_FILE &&
                             entry.type != PROSENJITFS_INODE_DIR)) {
                            rc = PROSENJITFS_ECORRUPT;
                            fail_line = __LINE__;
                        goto consistency_out;
                        }
                        if (dir_refs[entry.inode] == 0xFFu) {
                            rc = PROSENJITFS_EOVERFLOW;
                            fail_line = __LINE__;
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

            while (current != 0u) {
                struct pros_prosenjitfs_extent extent;
                uint64_t end_logical;
                uint64_t physical;

                if (++steps > fs->sb.total_blocks ||
                    pfs_consistency_mark(owner, fs->sb.total_blocks,
                                         current) != PROSENJITFS_OK) {
                    rc = PROSENJITFS_ECORRUPT;
                    fail_line = __LINE__;
                    goto consistency_out;
                }
                rc = pfs_read_extent(fs, current, &extent);
                if (rc != PROSENJITFS_OK) {
                    if (rc != PROSENJITFS_EIO) {
                        rc = PROSENJITFS_ECORRUPT;
                        fail_line = __LINE__;
                    }
                    goto consistency_out;
                }
                /*
                 * Validate the complete logical and physical ranges before
                 * iterating. Besides preventing integer wrap, this bounds the
                 * work by the actual volume size: a checksum-valid corrupt
                 * block_count must not turn a read-only scan into an enormous
                 * loop. Subtraction-based checks avoid overflowing end values.
                 */
                {
                    uint64_t max_logical_blocks =
                        fs->sb.total_blocks - fs->sb.data_start;
                    if (extent.block_count == 0u ||
                        extent.logical_block < logical_end ||
                        extent.logical_block >= max_logical_blocks ||
                        extent.block_count >
                            max_logical_blocks - extent.logical_block ||
                        extent.physical_block < fs->sb.data_start ||
                        extent.physical_block >= fs->sb.total_blocks ||
                        extent.block_count >
                            fs->sb.total_blocks - extent.physical_block ||
                        extent.block_count >
                            UINT64_MAX / fs->sb.block_size ||
                        allocated_bytes >
                            UINT64_MAX -
                            extent.block_count * fs->sb.block_size) {
                        rc = -10002;
                        fail_line = __LINE__;
                        goto consistency_out;
                    }
                }
                for (uint64_t n = 0u; n < extent.block_count; ++n) {
                    physical = extent.physical_block + n;
                    if (pfs_consistency_mark(owner, fs->sb.total_blocks,
                                             physical) != PROSENJITFS_OK) {
                        rc = -10003;
                        fail_line = __LINE__;
                        goto consistency_out;
                    }
                }
                allocated_bytes +=
                    extent.block_count * fs->sb.block_size;
                end_logical = extent.logical_block + extent.block_count;
                logical_end = end_logical;
                current = extent.next;
            }
            /*
             * Keep-size preallocation may legitimately extend beyond EOF,
             * so EOF is not an extent-allocation boundary. However, no
             * logical extent may exceed the maximum file range representable
             * by the volume's data-block capacity.
             */
            {
                uint64_t max_logical_blocks =
                    fs->sb.total_blocks - fs->sb.data_start;
                if (allocated_bytes != inode.allocated_bytes ||
                    logical_end > max_logical_blocks) {
                    rc = -10004;
                    fail_line = __LINE__;
                    goto consistency_out;
                }
            }
        }
    }

    /* Bits beyond inode_count are permanently invalid and must never
     * survive as allocated state. This catches bitmap tail corruption that
     * cannot be observed by inode lookup alone. */
    for (uint64_t bi = 0u; bi < fs->sb.inode_bitmap_blocks; ++bi) {
        uint64_t lba;
        if (pfs_lba_add(fs->mount.device, fs->sb.inode_bitmap_start,
                        bi, &lba) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            fail_line = __LINE__;
            goto consistency_out;
        }
        if (pfs_read_block(fs->mount.device, lba, bitmap) != PROSENJITFS_OK) {
            rc = PROSENJITFS_EIO;
            goto consistency_out;
        }
        for (uint64_t bit = 0u; bit < bits_per_block; ++bit) {
            uint64_t inode_number = bi * bits_per_block + bit;
            if (inode_number >= fs->sb.inode_count &&
                pfs_bitmap_bit(bitmap, bit)) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                        goto consistency_out;
            }
        }
    }

    if (dir_refs[fs->sb.root_inode] != 0u) {
        rc = PROSENJITFS_ECORRUPT;
        fail_line = __LINE__;
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
                        bi, &lba) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            fail_line = __LINE__;
            goto consistency_out;
        }
        if (pfs_read_block(fs->mount.device, lba, bitmap) != PROSENJITFS_OK) {
            rc = PROSENJITFS_EIO;
            goto consistency_out;
        }
        allocated = pfs_bitmap_bit(bitmap, within);
        if (allocated) {
            int inode_rc = prosenjitfs_read_inode(fs, inode_number, &inode);
            /*
             * This is a second read of allocated inode metadata after the
             * directory-reference count has been built. Do not silently
             * ignore device failures here: otherwise an injected/real I/O
             * error can be reported as a successful integrity scan.
             */
            if (inode_rc == PROSENJITFS_EIO) {
                rc = PROSENJITFS_EIO;
                goto consistency_out;
            }
            if (inode_rc != PROSENJITFS_OK) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                goto consistency_out;
            }
            if (dir_refs[inode_number] > 1u) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                goto consistency_out;
            }
        }
    }

    /*
     * A valid parent pointer and exactly one directory reference do not prove
     * that an inode belongs to the root namespace: a corrupted pair of
     * directories can reference each other while remaining locally valid.
     * Walk every allocated inode's parent chain and require it to terminate
     * at root. The inode_count bound makes cycles fail closed without needing
     * another allocation or a recursive traversal.
     */
    for (uint64_t inode_number = 0u;
         inode_number < fs->sb.inode_count; ++inode_number) {
        int parent_rc;

        if (inode_number == fs->sb.root_inode)
            continue;
        parent_rc = pfs_parent_chain_reaches_root(fs, inode_number);
        if (parent_rc == PROSENJITFS_ENOENT)
            continue;
        if (parent_rc != PROSENJITFS_OK) {
            if (parent_rc == PROSENJITFS_EIO)
                rc = PROSENJITFS_EIO;
            else {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
            }
            goto consistency_out;
        }
        /*
         * Report an allocated inode with no directory-entry reference only
         * after its inode metadata and parent chain have passed validation.
         * This is a conservative candidate, not permission to reclaim it.
         */
        if (dir_refs[inode_number] == 0u) {
            if (!report) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                goto consistency_out;
            }
            if (orphaned_inodes == 0u) {
                report->first_orphaned_inode = inode_number;
                if (fail_line == 0u)
                    fail_line = __LINE__;
            }
            ++orphaned_inodes;
            report->orphaned_inodes_reported = orphaned_inodes;
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
                        bi, &lba) != PROSENJITFS_OK) {
            rc = PROSENJITFS_ECORRUPT;
            fail_line = __LINE__;
            goto consistency_out;
        }
        if (pfs_read_block(fs->mount.device, lba, bitmap) != PROSENJITFS_OK) {
            rc = PROSENJITFS_EIO;
            goto consistency_out;
        }
        for (uint64_t bit = 0u; bit < bits_per_block; ++bit) {
            if (report)
                ++report->block_bitmap_bits_checked;
            uint64_t block = first_bit + bit;
            int set = pfs_bitmap_bit(bitmap, bit);
            int owned = block < fs->sb.total_blocks
                      ? (owner[block >> 3] &
                         (uint8_t)(1u << (block & 7u))) != 0
                      : 0;
            if (block < fs->sb.data_start && !set) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                goto consistency_out;
            }
            if (block >= fs->sb.total_blocks && set) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                goto consistency_out;
            }
            if (block >= fs->sb.data_start &&
                block < fs->sb.total_blocks && set && !owned) {
                /*
                 * An allocated data block with no reachable metadata owner is
                 * an orphan candidate. In report mode, record it and finish
                 * the bounded scan without modifying the volume. The legacy
                 * status-only API retains its fail-fast behavior.
                 */
                if (!report) {
                    rc = PROSENJITFS_ECORRUPT;
                    fail_line = __LINE__;
                    goto consistency_out;
                }
                if (orphaned_blocks == 0u) {
                    report->first_orphaned_block = block;
                    if (fail_line == 0u)
                        fail_line = __LINE__;
                }
                ++orphaned_blocks;
                report->orphaned_blocks_reported = orphaned_blocks;
                continue;
            }
            if (block >= fs->sb.data_start &&
                block < fs->sb.total_blocks && !set && owned) {
                rc = PROSENJITFS_ECORRUPT;
                fail_line = __LINE__;
                goto consistency_out;
            }
        }
    }

    /*
     * Report mode deliberately completes the bounded bitmap scan when orphan
     * candidates exist. Preserve the corruption result after the scan rather
     * than overwriting it with the clean-path success status.
     */
    rc = (orphaned_blocks != 0u || orphaned_inodes != 0u)
       ? PROSENJITFS_ECORRUPT : PROSENJITFS_OK;

consistency_out:
    kfree(owner);
    kfree(dir_refs);
    if (rc != PROSENJITFS_OK && fail_line != 0u)
        return -10000 - (int)fail_line;
    return rc;
}


/*
 * Public kernel-only read-only integrity scan. Mount performs journal replay
 * before exposing an active filesystem, so callers must invoke this only on
 * an active mount and serialize it against concurrent filesystem mutations.
 * The underlying checker only reads on-disk metadata; it never repairs or
 * reclaims blocks. Internal line-coded checker failures are normalized to
 * the stable filesystem error code at this boundary.
 */
int prosenjitfs_check_integrity_report(
    const struct pros_prosenjitfs *fs,
    struct pros_prosenjitfs_integrity_report *report)
{
    int rc;

    if (report) {
        report->status = PROSENJITFS_EINVAL;
        report->first_error_location = 0u;
        report->inodes_checked = 0u;
        report->block_bitmap_bits_checked = 0u;
        report->orphaned_blocks_reported = 0u;
        report->first_orphaned_block = 0u;
        report->orphaned_inodes_reported = 0u;
        report->first_orphaned_inode = 0u;
    }
    if (!fs || !fs->mount.device || !fs->mount.active) {
        if (report)
            report->status = PROSENJITFS_EINVAL;
        return PROSENJITFS_EINVAL;
    }

    /*
     * The checker walks inode, directory and allocation metadata over many
     * block reads. Share the provider I/O gate with file data and metadata
     * callbacks so it cannot observe a torn file-data transaction. Namespace
     * callbacks are separately serialized by the VFS mount mutation gate;
     * callers must still avoid running this scan concurrently with a
     * namespace mutation until provider callbacks share this gate as well.
     */
    rc = pfs_io_lock((struct pros_prosenjitfs *)fs);
    if (rc != PROSENJITFS_OK) {
        if (report)
            report->status = rc;
        return rc;
    }
    rc = pfs_selftest_consistency(fs, report);
    pfs_io_unlock((struct pros_prosenjitfs *)fs);
    if (rc <= -10000) {
        if (report) {
            report->first_error_location = (uint32_t)(-10000 - rc);
            report->status = PROSENJITFS_ECORRUPT;
        }
        return PROSENJITFS_ECORRUPT;
    }
    if (rc != PROSENJITFS_OK && rc != PROSENJITFS_EOVERFLOW &&
        rc != PROSENJITFS_EINVAL && rc != PROSENJITFS_EIO &&
        rc != PROSENJITFS_ECORRUPT)
        rc = PROSENJITFS_ECORRUPT;
    if (report)
        report->status = rc;
    return rc;
}

int prosenjitfs_check_integrity(const struct pros_prosenjitfs *fs)
{
    return prosenjitfs_check_integrity_report(fs, 0);
}

static int pfs_selftest_set_access(struct pros_prosenjitfs *fs,
                                    const char *name,
                                    uint32_t uid, uint32_t gid,
                                    uint16_t mode)
{
    struct pros_prosenjitfs_inode root_inode;
    struct pros_prosenjitfs_inode inode;
    struct pros_prosenjitfs_dir_entry entry;
    uint64_t inode_number;

    if (!fs || prosenjitfs_read_inode(fs, fs->sb.root_inode, &root_inode) !=
                   PROSENJITFS_OK)
        return PROSENJITFS_EINVAL;
    if (!name) {
        inode_number = fs->sb.root_inode;
    } else {
        if (pfs_find_dir_entry(fs, &root_inode, fs->sb.root_inode, name,
                               &entry) != PROSENJITFS_OK)
            return PROSENJITFS_ENOENT;
        inode_number = entry.inode;
    }
    if (prosenjitfs_read_inode(fs, inode_number, &inode) != PROSENJITFS_OK)
        return PROSENJITFS_EIO;
    inode.uid = uid;
    inode.gid = gid;
    inode.mode = mode;
    return prosenjitfs_write_inode(fs, inode_number, &inode);
}

int prosenjitfs_selftest(void)
{
    pfs_selftest_diag_stage = 0xF001u; /* entered */
    pfs_selftest_diag_rc = PROSENJITFS_OK;
    pfs_crash_inject_stage = 0u;
    pfs_crash_injected = 0u;
    pfs_selftest_diag_stage = 0xF002u; /* globals reset */
    static struct pfs_test_disk disk;
    /* Keep the filesystem test's persistent mock device and mount state out
     * of the large self-test stack frame; the test is single-owner. */
    static struct pros_block_device device;
    static struct pros_prosenjitfs fs;
    struct pros_prosenjitfs_inode inode;
    struct pros_prosenjitfs_integrity_report integrity_report;
    uint64_t first_block;
    uint64_t inode_number;
    uint8_t saved;
    uint32_t integrity_durable_crc;
    uint32_t integrity_volatile_crc;
    uint32_t integrity_dirty_crc;

    /* Avoid a compiler-generated aggregate initializer obscuring early
     * crash diagnostics; make this initialization an explicit checkpoint. */
    pfs_zero(&fs, (uint32_t)sizeof(fs));
    if (prosenjitfs_check_integrity(0) != PROSENJITFS_EINVAL ||
        prosenjitfs_check_integrity(&fs) != PROSENJITFS_EINVAL ||
        prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
            PROSENJITFS_EINVAL ||
        integrity_report.status != PROSENJITFS_EINVAL ||
        integrity_report.first_error_location != 0u ||
        integrity_report.inodes_checked != 0u ||
        integrity_report.block_bitmap_bits_checked != 0u)
        return pfs_selftest_fail(__LINE__, -1);
    PFS_SELFTEST_CHECKPOINT(0xF003u); /* local filesystem state zeroed */
    PFS_SELFTEST_CHECKPOINT(0xF004u); /* immediately before block-device init */
    if (block_device_init(&device, 512u, 512u, pfs_test_read,
                          pfs_test_write, &disk) != PROS_BLOCK_OK)
        return pfs_selftest_fail(__LINE__, -1);
    PFS_SELFTEST_CHECKPOINT(0xF005u); /* test block device initialized */
    if (block_device_set_flush(&device, pfs_test_flush) != PROS_BLOCK_OK)
        return pfs_selftest_fail(__LINE__, -2);
    PFS_SELFTEST_CHECKPOINT(0xF006u); /* flush callback installed */
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

    /*
     * Integrity scans must fail fast while a provider operation owns the
     * shared I/O gate; they must never inspect a partially committed image.
     */
    __atomic_store_n(&fs.io_lock, 1u, __ATOMIC_RELEASE);
    if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
            PROSENJITFS_EBUSY ||
        integrity_report.status != PROSENJITFS_EBUSY ||
        integrity_report.inodes_checked != 0u) {
        __atomic_store_n(&fs.io_lock, 0u, __ATOMIC_RELEASE);
        return pfs_selftest_fail(__LINE__, -30);
    }
    __atomic_store_n(&fs.io_lock, 0u, __ATOMIC_RELEASE);
    /*
     * Exercise VFS permission denials against the real ProsenjitFS mock
     * medium. Each rejected operation must leave durable bytes, volatile
     * cache, and dirty-sector tracking unchanged, and the mounted volume
     * must still pass its integrity scan.
     */
    {
        static struct pros_process actor;
        struct pros_process *previous = process_current();
        struct pros_vfs_file setup_file = {0};
        struct pros_vfs_file held_file = {0};
        struct pros_vfs_file held_read_file = {0};
        struct pros_vfs_file denied_file = {0};
        struct pros_vfs_vnode *found = 0;
        struct pros_vfs_dir denied_dir = {0};
        struct pros_vfs_dirent denied_entry;
        uint8_t byte = 0x5Au;
        uint32_t done = 0u;
        uint32_t test_rc = 0u;
        uint8_t actor_initialized = 0u;

        process_set_current(0);
        if (vfs_kernel_file_open(&fs.mount, "/permission-target",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &setup_file) != PROS_VFS_OK ||
            vfs_file_close(&setup_file) != PROS_VFS_OK ||
            vfs_kernel_mkdir(&fs.mount, "/permission-empty") != PROS_VFS_OK ||
            vfs_kernel_mkdir(&fs.mount, "/permission-search") != PROS_VFS_OK ||
            vfs_kernel_mkdir(&fs.mount, "/permission-destination") != PROS_VFS_OK ||
            vfs_kernel_file_open(&fs.mount,
                          "/permission-search/permission-child",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &setup_file) != PROS_VFS_OK ||
            vfs_file_close(&setup_file) != PROS_VFS_OK ||
            0) {
            test_rc = 1u;
            goto pfs_permission_test_cleanup;
        }

        pfs_zero(&actor, (uint32_t)sizeof(actor));
        if (process_init(&actor, 1002u, PROS_SECURITY_LIMITED) != 0) {
            test_rc = 2u;
            goto pfs_permission_test_cleanup;
        }
        actor_initialized = 1u;
        actor.state = PROS_PROCESS_READY;
        actor.gid = 1002u; /* deterministic primary-group credential */
        process_set_current(&actor);
        if (process_current() != &actor) {
            test_rc = 3u;
            goto pfs_permission_test_cleanup;
        }

        /*
         * Open these handles as the unprivileged actor, not through the
         * trusted kernel-only API. Restore root-owned metadata afterward so
         * operation-time checks prove an existing handle cannot bypass a
         * later permission denial.
         */
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-target",
                                    actor.fs_uid, actor.fs_gid, 0600u) !=
                PROSENJITFS_OK) {
            test_rc = 65u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        if (vfs_file_open(&fs.mount, "/permission-target",
                          PROS_VFS_O_WRITE, &held_file) != PROS_VFS_OK ||
            vfs_file_open(&fs.mount, "/permission-target",
                          PROS_VFS_O_READ, &held_read_file) != PROS_VFS_OK) {
            test_rc = 66u;
            goto pfs_permission_test_cleanup;
        }
        /*
         * Provider-level inode serialization is fail-fast too. Simulate a
         * preempted lock owner and verify read/write/truncate return EBUSY
         * rather than spinning; byte-count outputs remain deterministic.
         */
        __atomic_store_n(&fs.io_lock, 1u, __ATOMIC_RELEASE);
        done = 0xA5A5A5A5u;
        if (vfs_file_read(&held_read_file, &byte, 1u, &done) !=
                PROS_VFS_EBUSY || done != 0u) {
            __atomic_store_n(&fs.io_lock, 0u, __ATOMIC_RELEASE);
            test_rc = 68u;
            goto pfs_permission_test_cleanup;
        }
        done = 0xA5A5A5A5u;
        if (vfs_file_write(&held_file, &byte, 1u, &done) !=
                PROS_VFS_EBUSY || done != 0u ||
            vfs_file_truncate(&held_file, 0u) != PROS_VFS_EBUSY) {
            __atomic_store_n(&fs.io_lock, 0u, __ATOMIC_RELEASE);
            test_rc = 69u;
            goto pfs_permission_test_cleanup;
        }
        __atomic_store_n(&fs.io_lock, 0u, __ATOMIC_RELEASE);

        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-target",
                                    0u, 0u, 0644u) != PROSENJITFS_OK) {
            test_rc = 67u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);

#define PFS_EXPECT_DENIAL(operation, expected_rc, failure_code) do { uint32_t durable_before = prosenjitfs_crc32c(disk.data, (uint32_t)sizeof(disk.data)); uint32_t volatile_before = prosenjitfs_crc32c(disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)); uint32_t dirty_before = prosenjitfs_crc32c(disk.dirty, (uint32_t)sizeof(disk.dirty)); int operation_rc = (operation); int integrity_rc = prosenjitfs_check_integrity(&fs); uint32_t detail = operation_rc != (expected_rc) ? 1u : integrity_rc != PROSENJITFS_OK ? 2u : durable_before != prosenjitfs_crc32c(disk.data, (uint32_t)sizeof(disk.data)) ? 3u : volatile_before != prosenjitfs_crc32c(disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ? 4u : dirty_before != prosenjitfs_crc32c(disk.dirty, (uint32_t)sizeof(disk.dirty)) ? 5u : 0u; if (detail != 0u) { test_rc = (failure_code) * 10u + detail; goto pfs_permission_test_cleanup; } } while (0)

        PFS_EXPECT_DENIAL(
            vfs_file_open(&fs.mount, "/permission-target",
                          PROS_VFS_O_WRITE, &denied_file),
            PROS_VFS_EPERM, 4u);
        if (denied_file.open) {
            test_rc = 5u;
            goto pfs_permission_test_cleanup;
        }
        PFS_EXPECT_DENIAL(
            vfs_file_open(&fs.mount, "/permission-created",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &denied_file),
            PROS_VFS_EPERM, 6u);
        if (denied_file.open) {
            test_rc = 7u;
            goto pfs_permission_test_cleanup;
        }
        PFS_EXPECT_DENIAL(vfs_mkdir(&fs.mount, "/permission-created-dir"),
                          PROS_VFS_EPERM, 8u);
        PFS_EXPECT_DENIAL(vfs_unlink(&fs.mount, "/permission-target"),
                          PROS_VFS_EPERM, 9u);
        PFS_EXPECT_DENIAL(vfs_rmdir(&fs.mount, "/permission-empty"),
                          PROS_VFS_EPERM, 10u);
        PFS_EXPECT_DENIAL(vfs_rename(&fs.mount, "/permission-target",
                                     "/permission-renamed"),
                          PROS_VFS_EPERM, 11u);
        PFS_EXPECT_DENIAL(vfs_chmod(&fs.mount, "/permission-target", 0600u),
                          PROS_VFS_EPERM, 48u);
        PFS_EXPECT_DENIAL(vfs_chown(&fs.mount, "/permission-target", 2002u, 2003u),
                          PROS_VFS_EPERM, 49u);

        done = UINT32_MAX;
        PFS_EXPECT_DENIAL(vfs_file_write(&held_file, &byte, 1u, &done),
                          PROS_VFS_EPERM, 12u);
        if (done != 0u) {
            test_rc = 13u;
            goto pfs_permission_test_cleanup;
        }
        PFS_EXPECT_DENIAL(vfs_file_truncate(&held_file, 0u),
                          PROS_VFS_EPERM, 14u);

        process_set_current(0);
        if (vfs_kernel_chmod(&fs.mount, "/permission-target", 0600u) != PROS_VFS_OK ||
            vfs_kernel_chown(&fs.mount, "/permission-target", 2002u, 2003u) != PROS_VFS_OK) {
            test_rc = 50u; goto pfs_permission_test_cleanup;
        }
        {
            struct pros_prosenjitfs_inode ri, ci;
            struct pros_prosenjitfs_dir_entry ce;
            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &ri) != PROSENJITFS_OK ||
                pfs_find_dir_entry(&fs, &ri, fs.sb.root_inode, "permission-target", &ce) != PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, ce.inode, &ci) != PROSENJITFS_OK ||
                ci.uid != 2002u || ci.gid != 2003u || ci.mode != 0600u) {
                test_rc = 51u; goto pfs_permission_test_cleanup;
            }
        }
        if (vfs_kernel_chmod(&fs.mount, "/permission-target", 0644u) != PROS_VFS_OK ||
            vfs_kernel_chown(&fs.mount, "/permission-target", 0u, 0u) != PROS_VFS_OK) {
            test_rc = 52u; goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);

        /* A read handle opened while trusted must not bypass later revocation. */
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-target",
                                    0u, 0u, 0000u) != PROSENJITFS_OK) {
            test_rc = 43u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        done = UINT32_MAX;
        PFS_EXPECT_DENIAL(vfs_file_read(&held_read_file, &byte, 1u, &done),
                          PROS_VFS_EPERM, 44u);
        if (done != 0u) {
            test_rc = 45u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-target",
                                    0u, 0u, 0644u) != PROSENJITFS_OK) {
            test_rc = 46u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);

        /* Exercise all three mode classes against persistent inode
         * metadata. The owner, primary group, and other cases each get one
         * allowed read/write open and one denied read/write open. */
        {
            static const struct {
                uint32_t uid;
                uint32_t gid;
                uint16_t read_mode;
                uint16_t write_mode;
            } classes[] = {
                {1002u, 2000u, 0400u, 0200u},
                {2000u, 1002u, 0040u, 0020u},
                {2000u, 2001u, 0004u, 0002u}
            };

            for (uint32_t i = 0u; i < sizeof(classes) / sizeof(classes[0]); ++i) {
                process_set_current(0);
                if (pfs_selftest_set_access(&fs, "permission-target",
                                            classes[i].uid, classes[i].gid,
                                            classes[i].read_mode) !=
                        PROSENJITFS_OK) {
                    test_rc = 22u;
                    goto pfs_permission_test_cleanup;
                }
                process_set_current(&actor);
                if (vfs_file_open(&fs.mount, "/permission-target",
                                  PROS_VFS_O_READ, &denied_file) !=
                        PROS_VFS_OK ||
                    vfs_file_close(&denied_file) != PROS_VFS_OK) {
                    test_rc = 23u;
                    goto pfs_permission_test_cleanup;
                }
                PFS_EXPECT_DENIAL(
                    vfs_file_open(&fs.mount, "/permission-target",
                                  PROS_VFS_O_WRITE, &denied_file),
                    PROS_VFS_EPERM, 24u);
                process_set_current(0);
                if (pfs_selftest_set_access(&fs, "permission-target",
                                            classes[i].uid, classes[i].gid,
                                            classes[i].write_mode) !=
                        PROSENJITFS_OK) {
                    test_rc = 25u;
                    goto pfs_permission_test_cleanup;
                }
                process_set_current(&actor);
                if (vfs_file_open(&fs.mount, "/permission-target",
                                  PROS_VFS_O_WRITE, &denied_file) !=
                        PROS_VFS_OK ||
                    vfs_file_close(&denied_file) != PROS_VFS_OK) {
                    test_rc = 26u;
                    goto pfs_permission_test_cleanup;
                }
                PFS_EXPECT_DENIAL(
                    vfs_file_open(&fs.mount, "/permission-target",
                                  PROS_VFS_O_READ, &denied_file),
                    PROS_VFS_EPERM, 27u);
            }
        }

        /* Search and listing are distinct: execute-only permits lookup of a
         * child but denies opening/enumerating the directory. Revoke group
         * read after opening to prove directory handles revalidate access. */
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-search",
                                    0u, 0u, 0700u) != PROSENJITFS_OK) {
            test_rc = 28u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        PFS_EXPECT_DENIAL(
            vfs_path_lookup(&fs.mount,
                            "/permission-search/permission-child", &found),
            PROS_VFS_EPERM, 29u);
        if (found) {
            vfs_vnode_put(found);
            found = 0;
            test_rc = 30u;
            goto pfs_permission_test_cleanup;
        }

        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-search",
                                    2000u, actor.gid, 0710u) !=
                PROSENJITFS_OK) {
            test_rc = 31u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        found = 0;
        if (vfs_path_lookup(&fs.mount,
                            "/permission-search/permission-child", &found) !=
                PROS_VFS_OK ||
            !found) {
            test_rc = 32u;
            goto pfs_permission_test_cleanup;
        }
        vfs_vnode_put(found);
        found = 0;
        PFS_EXPECT_DENIAL(vfs_dir_open(&fs.mount, "/permission-search",
                                       &denied_dir),
                          PROS_VFS_EPERM, 33u);

        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-search",
                                    2000u, actor.gid, 0750u) !=
                PROSENJITFS_OK) {
            test_rc = 34u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        if (vfs_dir_open(&fs.mount, "/permission-search", &denied_dir) !=
                PROS_VFS_OK) {
            test_rc = 35u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-search",
                                    2000u, actor.gid, 0710u) !=
                PROSENJITFS_OK) {
            test_rc = 36u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        PFS_EXPECT_DENIAL(vfs_dir_read(&denied_dir, &denied_entry), PROS_VFS_EPERM,
                          37u);
        if (vfs_dir_close(&denied_dir) != PROS_VFS_OK) {
            test_rc = 38u;
            goto pfs_permission_test_cleanup;
        }

        /*
         * Positive namespace cases require writable parents owned by the
         * limited process. Also exercise both sides of cross-directory rename.
         */
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-search",
                                    actor.fs_uid, actor.fs_gid, 0700u) !=
                PROSENJITFS_OK ||
            pfs_selftest_set_access(&fs, "permission-destination",
                                    actor.fs_uid, actor.fs_gid, 0700u) !=
                PROSENJITFS_OK) {
            test_rc = 48u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        if (vfs_file_open(&fs.mount,
                          "/permission-search/actor-file",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &setup_file) != PROS_VFS_OK ||
            vfs_file_close(&setup_file) != PROS_VFS_OK ||
            vfs_mkdir(&fs.mount, "/permission-search/actor-empty") !=
                PROS_VFS_OK) {
            test_rc = 49u;
            goto pfs_permission_test_cleanup;
        }
        {
            struct pros_prosenjitfs_inode search_inode;
            struct pros_prosenjitfs_inode child_inode;
            struct pros_prosenjitfs_dir_entry search_entry;
            struct pros_prosenjitfs_dir_entry child_entry;
            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &search_inode) !=
                    PROSENJITFS_OK ||
                pfs_find_dir_entry(&fs, &search_inode, fs.sb.root_inode,
                                   "permission-search", &search_entry) !=
                    PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, search_entry.inode,
                                       &search_inode) != PROSENJITFS_OK ||
                pfs_find_dir_entry(&fs, &search_inode, search_entry.inode,
                                   "actor-file", &child_entry) !=
                    PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, child_entry.inode,
                                       &child_inode) != PROSENJITFS_OK ||
                child_inode.uid != actor.fs_uid ||
                child_inode.gid != actor.fs_gid ||
                child_inode.mode != 0644u) {
                test_rc = 50u;
                goto pfs_permission_test_cleanup;
            }
        }
        if (vfs_rename(&fs.mount, "/permission-search/actor-file",
                       "/permission-destination/actor-file") != PROS_VFS_OK ||
            vfs_unlink(&fs.mount,
                       "/permission-destination/actor-file") != PROS_VFS_OK ||
            vfs_rmdir(&fs.mount, "/permission-search/actor-empty") !=
                PROS_VFS_OK ||
            vfs_file_open(&fs.mount,
                          "/permission-search/denied-destination",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &setup_file) != PROS_VFS_OK ||
            vfs_file_close(&setup_file) != PROS_VFS_OK) {
            test_rc = 51u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-destination",
                                    actor.fs_uid, actor.fs_gid, 0500u) !=
                PROSENJITFS_OK) {
            test_rc = 52u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        PFS_EXPECT_DENIAL(
            vfs_rename(&fs.mount,
                       "/permission-search/denied-destination",
                       "/permission-destination/denied-destination"),
            PROS_VFS_EPERM, 53u);
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-destination",
                                    actor.fs_uid, actor.fs_gid, 0700u) !=
                PROSENJITFS_OK ||
            pfs_selftest_set_access(&fs, "permission-search",
                                    actor.fs_uid, actor.fs_gid, 0500u) !=
                PROSENJITFS_OK) {
            test_rc = 54u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        PFS_EXPECT_DENIAL(
            vfs_rename(&fs.mount,
                       "/permission-search/denied-destination",
                       "/permission-destination/denied-destination"),
            PROS_VFS_EPERM, 55u);
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-search",
                                    actor.fs_uid, actor.fs_gid, 0700u) !=
                PROSENJITFS_OK ||
            pfs_selftest_set_access(&fs, "permission-target",
                                    actor.fs_uid, actor.fs_gid, 0600u) !=
                PROSENJITFS_OK) {
            test_rc = 56u;
            goto pfs_permission_test_cleanup;
        }
        process_set_current(&actor);
        if (vfs_rename(&fs.mount,
                       "/permission-search/denied-destination",
                       "/permission-destination/denied-destination") !=
                PROS_VFS_OK ||
            vfs_unlink(&fs.mount,
                       "/permission-destination/denied-destination") !=
                PROS_VFS_OK) {
            test_rc = 57u;
            goto pfs_permission_test_cleanup;
        }

        /* A read-only mount rejects mutations even for an authorized owner. */
        fs.mount.read_only = 1u;
        PFS_EXPECT_DENIAL(
            vfs_file_open(&fs.mount, "/permission-target",
                          PROS_VFS_O_WRITE, &denied_file),
            PROS_VFS_EPERM, 58u);
        PFS_EXPECT_DENIAL(vfs_file_open(&fs.mount,
                                       "/permission-search/read-only-create",
                                       PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                                       &denied_file),
                          PROS_VFS_EPERM, 59u);
        PFS_EXPECT_DENIAL(vfs_mkdir(&fs.mount,
                                    "/permission-search/read-only-dir"),
                          PROS_VFS_EPERM, 60u);
        PFS_EXPECT_DENIAL(vfs_unlink(&fs.mount,
                                     "/permission-search/actor-file"),
                          PROS_VFS_EPERM, 61u);
        PFS_EXPECT_DENIAL(vfs_rename(&fs.mount,
                                     "/permission-search/actor-file",
                                     "/permission-destination/actor-file"),
                          PROS_VFS_EPERM, 62u);
        fs.mount.read_only = 0u;
        process_set_current(0);

        /*
         * Verify chmod/chown metadata is durable across a full unmount/remount,
         * not merely visible through the current mount's inode cache.
         */
        process_set_current(0);
        if (vfs_kernel_chmod(&fs.mount, "/permission-target", 0600u) !=
                PROS_VFS_OK ||
            vfs_kernel_chown(&fs.mount, "/permission-target", 2002u, 2003u) !=
                PROS_VFS_OK) {
            test_rc = 70u;
            goto pfs_permission_test_cleanup;
        }
        if ((held_file.open && vfs_file_close(&held_file) != PROS_VFS_OK) ||
            (held_read_file.open &&
             vfs_file_close(&held_read_file) != PROS_VFS_OK)) {
            test_rc = 71u;
            goto pfs_permission_test_cleanup;
        }
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
            test_rc = 72u;
            goto pfs_permission_test_cleanup;
        }
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
            test_rc = 73u;
            goto pfs_permission_test_cleanup;
        }
        {
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_inode target_inode;
            struct pros_prosenjitfs_dir_entry target_entry;
            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                pfs_find_dir_entry(&fs, &root_inode, fs.sb.root_inode,
                                   "permission-target", &target_entry) !=
                    PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, target_entry.inode, &target_inode) !=
                    PROSENJITFS_OK ||
                target_inode.uid != 2002u || target_inode.gid != 2003u ||
                target_inode.mode != 0600u) {
                test_rc = 74u;
                goto pfs_permission_test_cleanup;
            }
        }
        if (vfs_kernel_chmod(&fs.mount, "/permission-target", 0644u) !=
                PROS_VFS_OK ||
            vfs_kernel_chown(&fs.mount, "/permission-target", 0u, 0u) !=
                PROS_VFS_OK) {
            test_rc = 75u;
            goto pfs_permission_test_cleanup;
        }

        /* Restore fixture metadata while trusted, then remove setup artifacts. */
        process_set_current(0);
        if (pfs_selftest_set_access(&fs, "permission-target",
                                    0u, 0u, 0644u) != PROSENJITFS_OK ||
            pfs_selftest_set_access(&fs, "permission-search",
                                    0u, 0u, 0755u) != PROSENJITFS_OK) {
            test_rc = 39u;
            goto pfs_permission_test_cleanup;
        }
#undef PFS_EXPECT_DENIAL

pfs_permission_test_cleanup:
        process_set_current(0);
        if (actor_initialized) {
            if (address_space_destroy(&actor.address_space) != 0 && test_rc == 0u)
                test_rc = 15u;
            if (object_release(&actor.object) != 0 && test_rc == 0u)
                test_rc = 16u;
        }
        if (denied_dir.open && vfs_dir_close(&denied_dir) != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 40u;
        if (held_file.open && vfs_file_close(&held_file) != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 17u;
        if (held_read_file.open && vfs_file_close(&held_read_file) != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 47u;
        if (setup_file.open && vfs_file_close(&setup_file) != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 21u;
        if (vfs_kernel_unlink(&fs.mount,
                       "/permission-search/permission-child") != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 41u;
        if (vfs_kernel_rmdir(&fs.mount, "/permission-destination") != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 64u;
        if (vfs_kernel_rmdir(&fs.mount, "/permission-search") != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 42u;
        if (vfs_kernel_unlink(&fs.mount, "/permission-target") != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 18u;
        if (vfs_kernel_rmdir(&fs.mount, "/permission-empty") != PROS_VFS_OK &&
            test_rc == 0u)
            test_rc = 19u;
        process_set_current(previous);
        if (process_current() != previous && test_rc == 0u)
            test_rc = 20u;
        if (test_rc != 0u)
            return pfs_selftest_fail(__LINE__, -(int)test_rc);
    }

    /*
     * FS-44 persistence regression: metadata changes must survive a complete
     * unmount/remount, not merely update the live vnode or transaction cache.
     */
    {
        struct pros_vfs_file metadata_file = {0};
        struct pros_prosenjitfs_inode root_inode;
        struct pros_prosenjitfs_inode metadata_inode;
        struct pros_prosenjitfs_dir_entry metadata_entry;

        if (vfs_kernel_file_open(&fs.mount, "/metadata-remount",
                                PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                                &metadata_file) != PROS_VFS_OK ||
            vfs_file_close(&metadata_file) != PROS_VFS_OK ||
            vfs_kernel_chmod(&fs.mount, "/metadata-remount", 0640u) !=
                PROS_VFS_OK ||
            vfs_kernel_chown(&fs.mount, "/metadata-remount", 4242u, 4343u) !=
                PROS_VFS_OK ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                PROSENJITFS_OK ||
            pfs_find_dir_entry(&fs, &root_inode, fs.sb.root_inode,
                               "metadata-remount", &metadata_entry) !=
                PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, metadata_entry.inode,
                                   &metadata_inode) != PROSENJITFS_OK ||
            metadata_inode.type != PROSENJITFS_INODE_FILE ||
            metadata_inode.uid != 4242u || metadata_inode.gid != 4343u ||
            metadata_inode.mode != 0640u ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            vfs_kernel_unlink(&fs.mount, "/metadata-remount") != PROS_VFS_OK ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -44);
    }

    /*
     * FS-44 transaction-failure sweep: a failed metadata transaction may
     * recover to the complete old or complete new inode, never a torn mix.
     * Reboot the mock medium after every injected device-write failure.
     */
    {
        uint32_t sweep_finished = 0u;

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            struct pros_vfs_file metadata_file = {0};
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_inode metadata_inode;
            struct pros_prosenjitfs_dir_entry metadata_entry;
            int update_rc;

            /* Tear down any previous iteration's mount before reformatting. */
            pfs_test_crash_reset(&fs, &disk);
            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/metadata-write-sweep",
                                    PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                                    &metadata_file) != PROS_VFS_OK ||
                vfs_file_close(&metadata_file) != PROS_VFS_OK ||
                vfs_kernel_chmod(&fs.mount, "/metadata-write-sweep", 0644u) !=
                    PROS_VFS_OK ||
                vfs_kernel_chown(&fs.mount, "/metadata-write-sweep",
                                 4242u, 4343u) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -45);
            }

            disk.fail_write_countdown = write_position;
            update_rc = vfs_kernel_chmod(&fs.mount, "/metadata-write-sweep",
                                         0600u);
            if (disk.fail_write_countdown != 0u) {
                /* This position is beyond the update's complete write path. */
                disk.fail_write_countdown = 0u;
                if (update_rc != PROS_VFS_OK ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    vfs_kernel_unlink(&fs.mount, "/metadata-write-sweep") !=
                        PROS_VFS_OK ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -45);
                }
                sweep_finished = 1u;
                break;
            }

            if (update_rc == PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -45);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                pfs_find_dir_entry(&fs, &root_inode, fs.sb.root_inode,
                                   "metadata-write-sweep", &metadata_entry) !=
                    PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, metadata_entry.inode,
                                       &metadata_inode) != PROSENJITFS_OK ||
                metadata_inode.type != PROSENJITFS_INODE_FILE ||
                metadata_inode.uid != 4242u || metadata_inode.gid != 4343u ||
                (metadata_inode.mode != 0644u &&
                 metadata_inode.mode != 0600u) ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -45);
            }
        }

        if (!sweep_finished)
            return pfs_selftest_fail(__LINE__, -45);
    }

    /*
     * Directory names are UTF-8 byte strings, bounded by the on-disk v1
     * limit. Exercise Unicode scalar validity and make sure rejected input
     * never begins a metadata transaction.
     */
    {
        static const char invalid_overlong[] = {
            (char)0xC0u, (char)0xAFu, '\0'
        };
        static const char invalid_continuation[] = {(char)0x80u, '\0'};
        static const char invalid_truncated[] = {
            (char)0xE2u, (char)0x82u, '\0'
        };
        static const char invalid_e0_overlong[] = {
            (char)0xE0u, (char)0x80u, (char)0x80u, '\0'
        };
        static const char invalid_f0_overlong[] = {
            (char)0xF0u, (char)0x80u, (char)0x80u, (char)0x80u, '\0'
        };
        static const char invalid_bad_middle[] = {
            (char)0xE2u, '(', (char)0xA1u, '\0'
        };
        static const char invalid_surrogate[] = {
            (char)0xEDu, (char)0xA0u, (char)0x80u, '\0'
        };
        static const char invalid_above_unicode[] = {
            (char)0xF4u, (char)0x90u, (char)0x80u, (char)0x80u, '\0'
        };
        static const char invalid_leading_f5[] = {
            (char)0xF5u, (char)0x80u, (char)0x80u, (char)0x80u, '\0'
        };
        static const char *const invalid_names[] = {
            "", "/", ".", "..", invalid_overlong, invalid_continuation,
            invalid_truncated, invalid_e0_overlong, invalid_f0_overlong,
            invalid_bad_middle, invalid_surrogate, invalid_above_unicode,
            invalid_leading_f5
        };
        static const char *const valid_names[] = {
            "x", "\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x92\xBE",
            "\xF4\x8F\xBF\xBF"
        };
        static const uint32_t valid_lengths[] = {1u, 2u, 3u, 4u, 4u};
        static const char *const valid_paths[] = {
            "/u8-x", "/u8-\xC3\xA9", "/u8-\xE4\xB8\xAD",
            "/u8-\xF0\x9F\x92\xBE", "/u8-\xF4\x8F\xBF\xBF"
        };
        char max_name[PROSENJITFS_MAX_NAME + 1u];
        char too_long_name[PROSENJITFS_MAX_NAME + 2u];
        char max_path[PROSENJITFS_MAX_NAME + 2u];
        char too_long_path[PROSENJITFS_MAX_NAME + 3u];
        char invalid_path[] = {
            '/', 'u', 't', 'f', '-', (char)0xC0u, (char)0xAFu, '\0'
        };
        uint32_t name_length;
        uint32_t durable_before, volatile_before, dirty_before;

        max_path[0] = '/';
        too_long_path[0] = '/';
        for (uint32_t i = 0u; i < PROSENJITFS_MAX_NAME; ++i) {
            max_name[i] = 'a';
            max_path[i + 1u] = 'a';
        }
        max_name[PROSENJITFS_MAX_NAME] = '\0';
        max_path[PROSENJITFS_MAX_NAME + 1u] = '\0';
        for (uint32_t i = 0u; i < PROSENJITFS_MAX_NAME + 1u; ++i) {
            too_long_name[i] = 'b';
            too_long_path[i + 1u] = 'b';
        }
        too_long_name[PROSENJITFS_MAX_NAME + 1u] = '\0';
        too_long_path[PROSENJITFS_MAX_NAME + 2u] = '\0';

        for (uint32_t i = 0u;
             i < sizeof(invalid_names) / sizeof(invalid_names[0]); ++i) {
            name_length = UINT32_MAX;
            if (pfs_utf8_name_valid(invalid_names[i], &name_length) !=
                    PROSENJITFS_EINVAL ||
                name_length != UINT32_MAX)
                return pfs_selftest_fail(__LINE__, -33);
        }
        name_length = UINT32_MAX;
        if (pfs_utf8_name_valid(too_long_name, &name_length) !=
                PROSENJITFS_EINVAL ||
            name_length != UINT32_MAX)
            return pfs_selftest_fail(__LINE__, -33);

        for (uint32_t i = 0u;
             i < sizeof(valid_names) / sizeof(valid_names[0]); ++i) {
            name_length = UINT32_MAX;
            if (pfs_utf8_name_valid(valid_names[i], &name_length) !=
                    PROSENJITFS_OK ||
                name_length != valid_lengths[i])
                return pfs_selftest_fail(__LINE__, -33);
        }
        name_length = UINT32_MAX;
        if (pfs_utf8_name_valid(max_name, &name_length) != PROSENJITFS_OK ||
            name_length != PROSENJITFS_MAX_NAME)
            return pfs_selftest_fail(__LINE__, -33);

        /* Exercise valid UTF-8 through the public VFS path, not only the
         * private validator. Create/remove each entry and check consistency. */
        for (uint32_t i = 0u;
             i < sizeof(valid_paths) / sizeof(valid_paths[0]); ++i) {
            if (vfs_kernel_mkdir(&fs.mount, valid_paths[i]) != PROS_VFS_OK ||
                vfs_kernel_rmdir(&fs.mount, valid_paths[i]) != PROS_VFS_OK ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -33);
        }

        /* The 255-byte boundary must work through VFS; 256 bytes must fail
         * before any metadata transaction begins. */
        if (vfs_kernel_mkdir(&fs.mount, max_path) != PROS_VFS_OK ||
            vfs_kernel_rmdir(&fs.mount, max_path) != PROS_VFS_OK ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -33);

        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));
        if (vfs_kernel_mkdir(&fs.mount, invalid_path) != PROS_VFS_EINVAL ||
            vfs_kernel_mkdir(&fs.mount, too_long_path) != PROS_VFS_EINVAL ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty)) ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -33);
    }

    integrity_durable_crc = prosenjitfs_crc32c(
        disk.data, (uint32_t)sizeof(disk.data));
    integrity_volatile_crc = prosenjitfs_crc32c(
        disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
    integrity_dirty_crc = prosenjitfs_crc32c(
        disk.dirty, (uint32_t)sizeof(disk.dirty));
    if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
            PROSENJITFS_OK ||
        integrity_report.status != PROSENJITFS_OK ||
        integrity_report.first_error_location != 0u ||
        integrity_report.inodes_checked != fs.sb.inode_count ||
        integrity_report.block_bitmap_bits_checked == 0u ||
        integrity_report.orphaned_blocks_reported != 0u ||
        integrity_report.first_orphaned_block != 0u ||
        integrity_durable_crc != prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data)) ||
        integrity_volatile_crc != prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
        integrity_dirty_crc != prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty)))
        return pfs_selftest_fail(__LINE__, -4);
    /*
     * I/O classification and immutability acceptance regression. Walk every
     * block-device read position reached by a clean integrity scan and inject
     * a failure at that exact read. Each injected failure must propagate as
     * EIO (never be collapsed into corruption) and must not mutate durable,
     * volatile or dirty media state. The first countdown not reached marks
     * the end of the scan's read sequence.
     */
    {
        uint32_t durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        uint32_t volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        uint32_t dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));
        uint32_t injected_read_failures = 0u;
        uint32_t read_position;

        for (read_position = 1u; read_position <= 512u; ++read_position) {
            int scan_rc;
            disk.fail_read_countdown = read_position;
            scan_rc = prosenjitfs_check_integrity_report(&fs,
                                                          &integrity_report);
            if (disk.fail_read_countdown != 0u) {
                /* The selected read was beyond the scan's final read. */
                if (scan_rc != PROSENJITFS_OK ||
                    integrity_report.status != PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -4);
                break;
            }
            if (scan_rc != PROSENJITFS_EIO ||
                integrity_report.status != PROSENJITFS_EIO ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)))
                return pfs_selftest_fail(__LINE__, -4);
            ++injected_read_failures;
        }
        disk.fail_read_countdown = 0u;
        if (read_position > 512u || injected_read_failures < 2u ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty)))
            return pfs_selftest_fail(__LINE__, -4);
    }

    /*
     * Report-only orphan regression: flip one free data-block allocation bit
     * directly on the durable mock medium. The scan must report the candidate,
     * return corruption, and leave all medium/cache state untouched.
     */
    {
        uint64_t orphan_block = fs.sb.data_start;
        uint64_t bitmap_lba;
        uint64_t byte_offset;
        uint8_t mask = 0u;
        uint8_t original_byte;
        uint32_t durable_before;
        uint32_t volatile_before;
        uint32_t dirty_before;

        /*
         * Pick a genuinely free data block rather than assuming a fixed
         * offset is unowned; the root index and other metadata can occupy
         * early data blocks depending on the formatted layout.
         */
        for (; orphan_block < fs.sb.total_blocks; ++orphan_block) {
            bitmap_lba = fs.sb.block_bitmap_start +
                         orphan_block / ((uint64_t)fs.sb.block_size * 8u);
            byte_offset = bitmap_lba * fs.sb.block_size +
                          (orphan_block % ((uint64_t)fs.sb.block_size * 8u)) / 8u;
            if (byte_offset >= sizeof(disk.data))
                return pfs_selftest_fail(__LINE__, -4);
            mask = (uint8_t)(1u << (orphan_block & 7u));
            if ((disk.data[byte_offset] & mask) == 0u)
                break;
        }
        if (orphan_block >= fs.sb.total_blocks)
            return pfs_selftest_fail(__LINE__, -4);
        original_byte = disk.data[byte_offset];
        {
            uint8_t original_volatile_byte = disk.volatile_data[byte_offset];

            /*
             * The mock device reads dirty sectors from volatile_data and
             * clean sectors from durable data. Inject the same bitmap bit in
             * both views so the test is independent of cache/dirty state.
             */
            disk.data[byte_offset] = (uint8_t)(original_byte | mask);
            disk.volatile_data[byte_offset] =
                (uint8_t)(original_volatile_byte | mask);
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                integrity_report.orphaned_blocks_reported != 1u ||
                integrity_report.first_orphaned_block != orphan_block ||
                integrity_report.first_error_location == 0u ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                disk.data[byte_offset] = original_byte;
                disk.volatile_data[byte_offset] = original_volatile_byte;
                return pfs_selftest_fail(__LINE__, -4);
            }
            disk.data[byte_offset] = original_byte;
            disk.volatile_data[byte_offset] = original_volatile_byte;
        }
        if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_OK ||
            integrity_report.orphaned_blocks_reported != 0u ||
            integrity_report.first_orphaned_block != 0u)
            return pfs_selftest_fail(__LINE__, -4);
    }
    /*
     * Report-only orphan inode regression: allocate a valid, empty inode whose
     * parent chain reaches root but which has no directory-entry reference.
     * The report must identify it without changing durable or cached media.
     */
    {
        struct pros_prosenjitfs_inode orphan_inode;
        uint64_t orphan_inode_number = 0u;
        uint64_t generation = 0u;
        uint32_t durable_before;
        uint32_t volatile_before;
        uint32_t dirty_before;

        if (prosenjitfs_alloc_inode(&fs, &orphan_inode_number) !=
                PROSENJITFS_OK ||
            pfs_allocate_generation(&fs, orphan_inode_number, &generation) !=
                PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -4);
        pfs_zero(&orphan_inode, sizeof(orphan_inode));
        orphan_inode.type = PROSENJITFS_INODE_FILE;
        orphan_inode.mode = 0644u;
        orphan_inode.links = 1u;
        orphan_inode.generation = generation;
        orphan_inode.parent_inode = fs.sb.root_inode;
        if (prosenjitfs_write_inode(&fs, orphan_inode_number,
                                    &orphan_inode) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -4);

        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));
        if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_ECORRUPT ||
            integrity_report.status != PROSENJITFS_ECORRUPT ||
            integrity_report.orphaned_inodes_reported != 1u ||
            integrity_report.first_orphaned_inode != orphan_inode_number ||
            integrity_report.orphaned_blocks_reported != 0u ||
            integrity_report.first_error_location == 0u ||
            /* Repeated report scans must be deterministic and read-only. */
            prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_ECORRUPT ||
            integrity_report.status != PROSENJITFS_ECORRUPT ||
            integrity_report.orphaned_inodes_reported != 1u ||
            integrity_report.first_orphaned_inode != orphan_inode_number ||
            integrity_report.orphaned_blocks_reported != 0u ||
            integrity_report.first_error_location == 0u ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_ECORRUPT ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty))) {
            pfs_zero(&orphan_inode, sizeof(orphan_inode));
            orphan_inode.type = PROSENJITFS_INODE_FREE;
            (void)prosenjitfs_write_inode(&fs, orphan_inode_number,
                                          &orphan_inode);
            (void)pfs_inode_bitmap_update(&fs, orphan_inode_number, 0u);
            return pfs_selftest_fail(__LINE__, -4);
        }

        /*
         * An allocated inode bitmap bit with a FREE inode record is corruption,
         * not an orphan candidate. Its report must expose a source-line
         * location rather than a synthetic 10000 + inode-number value.
         */
        {
            struct pros_prosenjitfs_inode empty_record = orphan_inode;
            empty_record.type = PROSENJITFS_INODE_FREE;
            if (prosenjitfs_write_inode(&fs, orphan_inode_number,
                                        &empty_record) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -4);
            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                integrity_report.first_error_location == 0u ||
                integrity_report.first_error_location >= 10000u ||
                integrity_report.orphaned_inodes_reported != 0u ||
                integrity_report.first_orphaned_inode != 0u) {
                (void)prosenjitfs_write_inode(&fs, orphan_inode_number,
                                              &orphan_inode);
                pfs_zero(&orphan_inode, sizeof(orphan_inode));
                orphan_inode.type = PROSENJITFS_INODE_FREE;
                (void)prosenjitfs_write_inode(&fs, orphan_inode_number,
                                              &orphan_inode);
                (void)pfs_inode_bitmap_update(&fs, orphan_inode_number, 0u);
                return pfs_selftest_fail(__LINE__, -4);
            }
            if (prosenjitfs_write_inode(&fs, orphan_inode_number,
                                        &orphan_inode) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -4);
        }

        /*
         * A malformed parent must fail closed, not be counted as a valid
         * orphan candidate. Use a self-parented file inode, snapshot media
         * after deliberately injecting it, and prove the scan itself is
         * still read-only before restoring the valid parent.
         */
        orphan_inode.parent_inode = orphan_inode_number;
        if (prosenjitfs_write_inode(&fs, orphan_inode_number,
                                    &orphan_inode) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -4);
        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));
        if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_ECORRUPT ||
            integrity_report.status != PROSENJITFS_ECORRUPT ||
            integrity_report.orphaned_inodes_reported != 0u ||
            integrity_report.first_orphaned_inode != 0u ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty))) {
            orphan_inode.parent_inode = fs.sb.root_inode;
            (void)prosenjitfs_write_inode(&fs, orphan_inode_number,
                                          &orphan_inode);
            pfs_zero(&orphan_inode, sizeof(orphan_inode));
            orphan_inode.type = PROSENJITFS_INODE_FREE;
            (void)prosenjitfs_write_inode(&fs, orphan_inode_number,
                                          &orphan_inode);
            (void)pfs_inode_bitmap_update(&fs, orphan_inode_number, 0u);
            return pfs_selftest_fail(__LINE__, -4);
        }
        orphan_inode.parent_inode = fs.sb.root_inode;
        if (prosenjitfs_write_inode(&fs, orphan_inode_number,
                                    &orphan_inode) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -4);

        pfs_zero(&orphan_inode, sizeof(orphan_inode));
        orphan_inode.type = PROSENJITFS_INODE_FREE;
        if (prosenjitfs_write_inode(&fs, orphan_inode_number, &orphan_inode) !=
                PROSENJITFS_OK ||
            pfs_inode_bitmap_update(&fs, orphan_inode_number, 0u) !=
                PROSENJITFS_OK ||
            prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_OK ||
            integrity_report.orphaned_inodes_reported != 0u ||
            integrity_report.first_orphaned_inode != 0u)
            return pfs_selftest_fail(__LINE__, -4);
    }
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
        if (vfs_kernel_path_lookup(&fs.mount, "/", &root) != PROS_VFS_OK ||
            root != fs.mount.root ||
            vfs_vnode_put(root) != PROS_VFS_OK ||
            vfs_kernel_dir_open(&fs.mount, "/", &dir) != PROS_VFS_OK ||
            vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
            entry.name[0] != '.' ||
            vfs_dir_close(&dir) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -5);
    }
    /* A directory index must identify the inode that owns it. A valid CRC
     * and an in-range but wrong inode number must not alias another dir. */
    {
        struct pros_prosenjitfs_inode root_inode;
        struct pros_prosenjitfs_dir_index damaged_index, observed_index;
        uint8_t original_block[4096], damaged_block[4096];
        uint64_t wrong_owner = fs.sb.root_inode == 0u ? 1u : 0u;
        int inject_rc, read_rc, restore_rc;

        if (wrong_owner >= fs.sb.inode_count ||
            prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                PROSENJITFS_OK ||
            pfs_read_dir_index(&fs, &root_inode, fs.sb.root_inode,
                              &observed_index) != PROSENJITFS_OK ||
            pfs_read_block(fs.mount.device, root_inode.dir_index,
                           original_block) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -171);
        pfs_copy(damaged_block, original_block, fs.sb.block_size);
        pfs_copy(&damaged_index, original_block, sizeof(damaged_index));
        damaged_index.inode = wrong_owner;
        pfs_copy(damaged_block, &damaged_index, sizeof(damaged_index));
        pfs_put32(damaged_block + offsetof(struct pros_prosenjitfs_dir_index,
                                           checksum),
                  pfs_crc_with_zero32(
                      damaged_block, sizeof(damaged_index),
                      (uint32_t)offsetof(struct pros_prosenjitfs_dir_index,
                                         checksum)));
        inject_rc = pfs_tx_reset(&fs);
        if (inject_rc == PROSENJITFS_OK)
            inject_rc = pfs_tx_add(&fs, root_inode.dir_index, damaged_block);
        if (inject_rc == PROSENJITFS_OK)
            inject_rc = pfs_tx_commit(&fs);
        read_rc = inject_rc == PROSENJITFS_OK
                ? pfs_read_dir_index(&fs, &root_inode, fs.sb.root_inode,
                                     &observed_index)
                : inject_rc;
        /* Restore even if the checksum-valid corruption write failed. */
        restore_rc = pfs_tx_reset(&fs);
        if (restore_rc == PROSENJITFS_OK)
            restore_rc = pfs_tx_add(&fs, root_inode.dir_index, original_block);
        if (restore_rc == PROSENJITFS_OK)
            restore_rc = pfs_tx_commit(&fs);
        if (inject_rc != PROSENJITFS_OK ||
            read_rc != PROSENJITFS_ECORRUPT ||
            restore_rc != PROSENJITFS_OK ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -171);
    }

    {
        struct pros_vfs_file file;
        uint8_t write_data[700];
        uint8_t read_data[700];
        uint32_t done = 0u;
        for (uint32_t i = 0u; i < sizeof(write_data); ++i)
            write_data[i] = (uint8_t)((i * 13u) ^ 0x5Au);

        if (vfs_kernel_file_open(&fs.mount, "/hello.bin",
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


        /*
         * End-to-end authorization denial on a real ProsenjitFS mock-media
         * mount. Capture durable, volatile and dirty views separately around
         * every denied mutation. No denial may reach journal/allocator staging.
         */
        {
            static struct pros_process auth_actor;
            struct pros_process *previous_process = process_current();
            struct pros_vfs_file auth_file = {0};
            uint8_t auth_byte = 0u;
            uint32_t auth_done = 0u;
            uint32_t durable_before = 0u;
            uint32_t volatile_before = 0u;
            uint32_t dirty_before = 0u;
            uint8_t actor_initialized = 0u;
            uint8_t file_fixture_created = 0u;
            uint8_t dir_fixture_created = 0u;
            int auth_rc = 0;

            if (vfs_kernel_mkdir(&fs.mount, "/auth-denied-dir") !=
                    PROS_VFS_OK) {
                auth_rc = -180;
                goto pfs_auth_cleanup;
            }
            dir_fixture_created = 1u;
            if (vfs_kernel_file_open(&fs.mount, "/auth-denied-file.bin",
                    PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &auth_file) !=
                    PROS_VFS_OK ||
                vfs_file_close(&auth_file) != PROS_VFS_OK) {
                auth_rc = -181;
                goto pfs_auth_cleanup;
            }
            file_fixture_created = 1u;

            if (process_init(&auth_actor, 500u, PROS_SECURITY_LIMITED) != 0) {
                auth_rc = -182;
                goto pfs_auth_cleanup;
            }
            actor_initialized = 1u;
            auth_actor.state = PROS_PROCESS_READY;
            process_set_current(&auth_actor);
            if (process_current() != &auth_actor) {
                auth_rc = -183;
                goto pfs_auth_cleanup;
            }

            /* Other-class read of a root-owned 0644 file is allowed. */
            if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ,
                              &auth_file) != PROS_VFS_OK ||
                vfs_file_read(&auth_file, &auth_byte, 1u, &auth_done) !=
                    PROS_VFS_OK ||
                auth_done != 1u || auth_byte != write_data[0] ||
                vfs_file_close(&auth_file) != PROS_VFS_OK) {
                auth_rc = -184;
                goto pfs_auth_cleanup;
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (vfs_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE,
                              &auth_file) != PROS_VFS_EPERM ||
                auth_file.open ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                auth_rc = -185;
                goto pfs_auth_cleanup;
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (vfs_file_open(&fs.mount, "/auth-denied-create.bin",
                    PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &auth_file) !=
                    PROS_VFS_EPERM ||
                auth_file.open ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                auth_rc = -186;
                goto pfs_auth_cleanup;
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (vfs_mkdir(&fs.mount, "/auth-denied-mkdir") != PROS_VFS_EPERM ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                auth_rc = -187;
                goto pfs_auth_cleanup;
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (vfs_unlink(&fs.mount, "/auth-denied-file.bin") !=
                    PROS_VFS_EPERM ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                auth_rc = -188;
                goto pfs_auth_cleanup;
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (vfs_rmdir(&fs.mount, "/auth-denied-dir") != PROS_VFS_EPERM ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                auth_rc = -189;
                goto pfs_auth_cleanup;
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (vfs_rename(&fs.mount, "/auth-denied-file.bin",
                           "/auth-denied-renamed.bin") != PROS_VFS_EPERM ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                auth_rc = -190;
                goto pfs_auth_cleanup;
            }

pfs_auth_cleanup:
            if (auth_file.open)
                (void)vfs_file_close(&auth_file);
            process_set_current(previous_process);
            if (process_current() != previous_process) {
                process_set_current(0);
                if (process_current() != 0 && auth_rc == 0)
                    auth_rc = -191;
            }
            if (actor_initialized) {
                if (address_space_destroy(&auth_actor.address_space) != 0 &&
                    auth_rc == 0)
                    auth_rc = -192;
                if (object_release(&auth_actor.object) != 0 && auth_rc == 0)
                    auth_rc = -193;
            }
            if (file_fixture_created &&
                vfs_kernel_unlink(&fs.mount, "/auth-denied-file.bin") !=
                    PROS_VFS_OK && auth_rc == 0)
                auth_rc = -194;
            if (dir_fixture_created &&
                vfs_kernel_rmdir(&fs.mount, "/auth-denied-dir") !=
                    PROS_VFS_OK && auth_rc == 0)
                auth_rc = -195;
            if (auth_rc != 0)
                return pfs_selftest_fail(__LINE__, auth_rc);
        }

        /* A directory entry's type must match its referenced inode type. */
        {
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_dir_entry saved_entry, damaged_entry;
            uint8_t original_block[4096], damaged_block[4096];
            uint64_t entry_lba = 0u, previous_lba = 0u;
            struct pros_vfs_vnode *lookup_result = 0;
            struct pros_vfs_dir listing;
            struct pros_vfs_dirent listed_entry;
            int inject_rc, lookup_rc, enum_rc, enum_close_rc, restore_rc;

            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                pfs_find_dir_entry_lba(&fs, &root_inode, fs.sb.root_inode,
                                       "hello.bin", &saved_entry, &entry_lba,
                                       &previous_lba) != PROSENJITFS_OK ||
                entry_lba == 0u ||
                pfs_read_block(fs.mount.device, entry_lba, original_block) !=
                    PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -172);

            pfs_copy(damaged_block, original_block, fs.sb.block_size);
            pfs_copy(&damaged_entry, original_block, sizeof(damaged_entry));
            damaged_entry.type = PROSENJITFS_INODE_DIR;
            pfs_copy(damaged_block, &damaged_entry, sizeof(damaged_entry));
            pfs_put32(damaged_block + offsetof(struct pros_prosenjitfs_dir_entry,
                                               checksum),
                      pfs_crc_with_zero32(
                          damaged_block, sizeof(damaged_entry),
                          (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry,
                                             checksum)));
            inject_rc = pfs_tx_reset(&fs);
            if (inject_rc == PROSENJITFS_OK)
                inject_rc = pfs_tx_add(&fs, entry_lba, damaged_block);
            if (inject_rc == PROSENJITFS_OK)
                inject_rc = pfs_tx_commit(&fs);
            lookup_rc = inject_rc == PROSENJITFS_OK
                      ? vfs_kernel_path_lookup(&fs.mount, "/hello.bin", &lookup_result)
                      : inject_rc;
            if (lookup_result) {
                (void)vfs_vnode_put(lookup_result);
                lookup_result = 0;
            }
            /* Listing must fail on the same corrupt child reference. */
            enum_rc = vfs_kernel_dir_open(&fs.mount, "/", &listing);
            enum_close_rc = enum_rc;
            if (enum_rc == PROS_VFS_OK) {
                enum_rc = PROS_VFS_OK;
                for (uint32_t attempt = 0u;
                     attempt < fs.sb.inode_count + 2u &&
                     enum_rc == PROS_VFS_OK; ++attempt)
                    enum_rc = vfs_dir_read(&listing, &listed_entry);
                enum_close_rc = vfs_dir_close(&listing);
            }
            restore_rc = pfs_tx_reset(&fs);
            if (restore_rc == PROSENJITFS_OK)
                restore_rc = pfs_tx_add(&fs, entry_lba, original_block);
            if (restore_rc == PROSENJITFS_OK)
                restore_rc = pfs_tx_commit(&fs);
            if (inject_rc != PROSENJITFS_OK || lookup_rc != PROS_VFS_ECORRUPT ||
                enum_rc != PROS_VFS_ECORRUPT || enum_close_rc != PROS_VFS_OK ||
                restore_rc != PROSENJITFS_OK ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -172);
        }

        /* Parent IDs must agree with the containing directory as well. */
        {
            struct pros_prosenjitfs_inode root_inode, valid_inode, damaged_inode;
            struct pros_prosenjitfs_dir_entry entry;
            struct pros_vfs_vnode *lookup_result = 0;
            int inject_rc, lookup_rc, restore_rc;

            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                pfs_find_dir_entry(&fs, &root_inode, fs.sb.root_inode,
                                   "hello.bin", &entry) != PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, entry.inode, &valid_inode) !=
                    PROSENJITFS_OK || entry.inode == fs.sb.root_inode)
                return pfs_selftest_fail(__LINE__, -172);
            damaged_inode = valid_inode;
            damaged_inode.parent_inode = fs.sb.inode_count - 1u;
            if (damaged_inode.parent_inode == fs.sb.root_inode)
                damaged_inode.parent_inode = fs.sb.root_inode + 1u;
            inject_rc = prosenjitfs_write_inode(&fs, entry.inode, &damaged_inode);
            lookup_rc = inject_rc == PROSENJITFS_OK
                      ? vfs_kernel_path_lookup(&fs.mount, "/hello.bin", &lookup_result)
                      : inject_rc;
            if (lookup_result) {
                (void)vfs_vnode_put(lookup_result);
                lookup_result = 0;
            }
            restore_rc = prosenjitfs_write_inode(&fs, entry.inode, &valid_inode);
            if (inject_rc != PROSENJITFS_OK || lookup_rc != PROS_VFS_ECORRUPT ||
                restore_rc != PROSENJITFS_OK ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -172);
        }

        if (vfs_kernel_file_open(&fs.mount, "/hello.bin",
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

    pfs_selftest_diag_stage = __LINE__;
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

        if (vfs_kernel_file_open(&fs.mount, "/lifecycle.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, &value, 1u, &done) != PROS_VFS_OK ||
            done != 1u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -17);

        if (vfs_kernel_file_open(&fs.mount, "/lifecycle.bin",
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

    pfs_selftest_diag_stage = __LINE__;
    /* Append semantics must use the current EOF even after an explicit seek. */
    {
        struct pros_vfs_file file;
        static const uint8_t first[] = { 'a', 'b', 'c' };
        static const uint8_t second[] = { 'D', 'E', 'F' };
        uint8_t combined[6];
        uint32_t done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) != PROS_VFS_OK ||
            vfs_file_write(&file, first, sizeof(first), &done) != PROS_VFS_OK ||
            done != sizeof(first) || vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        if (vfs_kernel_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_APPEND, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 0u) != PROS_VFS_OK ||
            vfs_file_write(&file, second, sizeof(second), &done) != PROS_VFS_OK ||
            done != sizeof(second) || file.position != 6u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        if (vfs_kernel_file_open(&fs.mount, "/append.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, combined, sizeof(combined), &done) != PROS_VFS_OK ||
            done != sizeof(combined) || vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        for (uint32_t i = 0u; i < 6u; ++i)
            if (combined[i] != (i < 3u ? first[i] : second[i - 3u]))
                return pfs_selftest_fail(__LINE__, -13);

        pfs_selftest_diag_stage = __LINE__;
        /*
         * Two independent append handles must each re-read authoritative EOF.
         * The provider serializes the full append transaction, so the second
         * append cannot reuse the first handle's stale vnode size.
         */
        {
            struct pros_vfs_file a, b;
            uint8_t tail_a = 'A', tail_b = 'B';
            uint8_t appended[8];
            if (vfs_kernel_file_open(&fs.mount, "/append.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_APPEND, &a) !=
                    PROS_VFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/append.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_APPEND, &b) !=
                    PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);
            done = 0u;
            if (vfs_file_write(&a, &tail_a, 1u, &done) != PROS_VFS_OK ||
                done != 1u)
                return pfs_selftest_fail(__LINE__, -13);
            done = 0u;
            if (vfs_file_write(&b, &tail_b, 1u, &done) != PROS_VFS_OK ||
                done != 1u ||
                vfs_file_close(&a) != PROS_VFS_OK ||
                vfs_file_close(&b) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);
            if (vfs_kernel_file_open(&fs.mount, "/append.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, appended, sizeof(appended), &done) !=
                    PROS_VFS_OK ||
                done != 8u ||
                appended[6] != 'A' || appended[7] != 'B' ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);
        }

    pfs_selftest_diag_stage = __LINE__;
    /*
     * VFS negative/error-contract matrix: invalid flags, missing paths,
     * directory-as-file, overflow seek/truncate, and read-only writes must
     * remain distinguishable instead of collapsing into generic I/O errors.
     */
    {
        struct pros_vfs_file file;
        uint8_t byte = 0x5Au;
        uint32_t done = 0u;

        if (vfs_kernel_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_APPEND, &file) != PROS_VFS_EINVAL ||
            vfs_kernel_file_open(&fs.mount, "/does-not-exist", PROS_VFS_O_READ,
                          &file) != PROS_VFS_ENOENT ||
            vfs_kernel_file_open(&fs.mount, "/", PROS_VFS_O_READ, &file) !=
                PROS_VFS_EISDIR)
            return pfs_selftest_fail(__LINE__, -18);

        if (vfs_kernel_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_write(&file, &byte, 1u, &done) != PROS_VFS_EPERM ||
            vfs_file_seek(&file, UINT64_MAX) != PROS_VFS_EOVERFLOW ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -18);

        if (vfs_kernel_file_open(&fs.mount, "/append.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
            vfs_file_truncate(&file, UINT64_MAX) != PROS_VFS_EOVERFLOW ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -18);
    }

    pfs_selftest_diag_stage = __LINE__;
    /* Sparse logical-offset regression: seek beyond EOF, write one byte, and
     * verify the unwritten interval reads as zeros. */
    {
        struct pros_vfs_file file;
        uint8_t marker = 0xD6u;
        uint8_t hole[512];
        uint32_t done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/sparse.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 4096u) != PROS_VFS_OK ||
            vfs_file_write(&file, &marker, 1u, &done) != PROS_VFS_OK ||
            done != 1u || file.position != 4097u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);
        if (vfs_kernel_file_open(&fs.mount, "/sparse.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
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

    pfs_selftest_diag_stage = __LINE__;
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
            vfs_kernel_file_open(&fs.mount, "/sparse-order.bin",
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

        if (vfs_kernel_file_open(&fs.mount, "/sparse-order.bin",
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

    pfs_selftest_diag_stage = __LINE__;
    /*
     * Sparse truncate-growth regression: extending EOF must preserve the
     * already allocated prefix, expose only zeros through the new hole, and
     * consume no additional data blocks.
     */
    {
        struct pros_vfs_file file;
        struct pros_prosenjitfs_dir_entry entry;
        struct pros_prosenjitfs_inode sparse_inode;
        uint8_t marker = 0x7Cu;
        uint8_t hole[1536];
        uint32_t done = 0u;
        uint64_t old_allocated;
        uint64_t new_size = (uint64_t)fs.sb.block_size * 3u + 1u;

        if (fs.sb.block_size != 512u ||
            vfs_kernel_file_open(&fs.mount, "/sparse-truncate.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, &marker, 1u, &done) != PROS_VFS_OK ||
            done != 1u ||
            vfs_file_truncate(&file, new_size) != PROS_VFS_OK ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);

        if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &inode) !=
                PROSENJITFS_OK ||
            pfs_find_dir_entry(&fs, &inode, fs.sb.root_inode, "sparse-truncate.bin",
                               &entry) != PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, entry.inode, &sparse_inode) !=
                PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -14);

        old_allocated = fs.sb.block_size;
        if (sparse_inode.size != new_size ||
            sparse_inode.allocated_bytes != old_allocated ||
            (sparse_inode.flags & PROSENJITFS_INODE_FLAG_SPARSE) == 0u)
            return pfs_selftest_fail(__LINE__, -14);

        if (vfs_kernel_file_open(&fs.mount, "/sparse-truncate.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, hole, sizeof(hole), &done) != PROS_VFS_OK ||
            done != sizeof(hole) || hole[0] != marker)
        {
            vfs_file_close(&file);
            return pfs_selftest_fail(__LINE__, -14);
        }
        for (uint32_t i = 1u; i < sizeof(hole); ++i) {
            if (hole[i] != 0u) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -14);
            }
        }
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);
    }

    pfs_selftest_diag_stage = __LINE__;
    /*
     * Delayed-allocation regression: two consecutive full-block writes must
     * reuse physically adjacent space and coalesce into one 2-block extent.
     * Allocation remains journaled immediately; no unsafe volatile cache is
     * introduced, so crash recovery sees a complete old-or-new transaction.
     */
    {
        struct pros_vfs_file file;
        struct pros_prosenjitfs_dir_entry entry;
        struct pros_prosenjitfs_inode delayed_inode;
        struct pros_prosenjitfs_extent delayed_extent;
        uint8_t block_a[512];
        uint8_t block_b[512];
        uint32_t done = 0u;
        uint64_t extent_lba;
        uint64_t second_physical;

        for (uint32_t i = 0u; i < sizeof(block_a); ++i) {
            block_a[i] = (uint8_t)(0x31u ^ (uint8_t)i);
            block_b[i] = (uint8_t)(0xC7u ^ (uint8_t)(i * 3u));
        }

        if (fs.sb.block_size != 512u ||
            vfs_kernel_file_open(&fs.mount, "/delalloc.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, block_a, sizeof(block_a), &done) !=
                PROS_VFS_OK ||
            done != sizeof(block_a) ||
            vfs_file_write(&file, block_b, sizeof(block_b), &done) !=
                PROS_VFS_OK ||
            done != sizeof(block_b) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);

        if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &inode) !=
                PROSENJITFS_OK ||
            pfs_find_dir_entry(&fs, &inode, fs.sb.root_inode, "delalloc.bin", &entry) !=
                PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, entry.inode, &delayed_inode) !=
                PROSENJITFS_OK ||
            delayed_inode.size != 1024u ||
            delayed_inode.allocated_bytes != 1024u ||
            delayed_inode.extent_head == 0u)
            return pfs_selftest_fail(__LINE__, -14);

        extent_lba = delayed_inode.extent_head;
        if (pfs_read_extent(&fs, extent_lba, &delayed_extent) !=
                PROSENJITFS_OK ||
            delayed_extent.logical_block != 0u ||
            delayed_extent.block_count != 2u ||
            delayed_extent.next != 0u ||
            delayed_extent.physical_block < fs.sb.data_start ||
            delayed_extent.physical_block + 1u >= fs.sb.total_blocks)
            return pfs_selftest_fail(__LINE__, -14);

        second_physical = delayed_extent.physical_block + 1u;
        if (vfs_kernel_file_open(&fs.mount, "/delalloc.bin", PROS_VFS_O_READ,
                          &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, 512u) != PROS_VFS_OK ||
            vfs_file_read(&file, block_b, sizeof(block_b), &done) !=
                PROS_VFS_OK ||
            done != sizeof(block_b) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);

        for (uint32_t i = 0u; i < sizeof(block_b); ++i)
            if (block_b[i] != (uint8_t)(0xC7u ^ (uint8_t)(i * 3u)))
                return pfs_selftest_fail(__LINE__, -14);

        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -14);

        if (prosenjitfs_read_inode(&fs, entry.inode, &delayed_inode) !=
                PROSENJITFS_OK ||
            delayed_inode.allocated_bytes != 1024u ||
            pfs_read_extent(&fs, delayed_inode.extent_head,
                            &delayed_extent) != PROSENJITFS_OK ||
            delayed_extent.block_count != 2u ||
            delayed_extent.physical_block > UINT64_MAX - 1u ||
            delayed_extent.physical_block + 1u != second_physical)
            return pfs_selftest_fail(__LINE__, -14);

        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -14);
    }

    pfs_selftest_diag_stage = __LINE__;
    /* The preceding persistence test unmounts; remount for the next test. */
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -15);

    pfs_selftest_diag_stage = __LINE__;
    /*
     * Sparse + delayed-allocation cross-regression: create a high logical
     * extent first, then fill a lower hole and finally write the immediately
     * following logical block. The lower two blocks must coalesce without
     * disturbing the higher extent or over-counting allocated_bytes.
     */
    {
        struct pros_vfs_file file;
        struct pros_prosenjitfs_dir_entry entry;
        struct pros_prosenjitfs_inode cross_inode;
        struct pros_prosenjitfs_extent first_extent;
        struct pros_prosenjitfs_extent second_extent;
        uint8_t low_a = 0x5Au;
        uint8_t low_b = 0xA5u;
        uint8_t high = 0xE7u;
        uint32_t done = 0u;
        uint32_t bs = fs.sb.block_size;
        uint64_t high_block = 4u;
        uint64_t low_block = 2u;
        uint64_t high_lba = 0u;

        if (bs != 512u)
            return pfs_selftest_fail(__LINE__, -141);
        {
            int open_rc = vfs_kernel_file_open(&fs.mount,
                                        "/sparse-delalloc-cross.bin",
                                        PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                                        &file);
            if (open_rc != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -200 + open_rc);
        }
        if (vfs_file_seek(&file, high_block * bs) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -143);
        done = 0u;
        if (vfs_file_write(&file, &high, 1u, &done) != PROS_VFS_OK ||
            done != 1u)
            return pfs_selftest_fail(__LINE__, -144);
        if (vfs_file_seek(&file, low_block * bs) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -145);
        done = 0u;
        if (vfs_file_write(&file, &low_a, 1u, &done) != PROS_VFS_OK ||
            done != 1u)
            return pfs_selftest_fail(__LINE__, -146);
        if (vfs_file_seek(&file, (low_block + 1u) * bs) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -147);
        done = 0u;
        if (vfs_file_write(&file, &low_b, 1u, &done) != PROS_VFS_OK ||
            done != 1u)
            return pfs_selftest_fail(__LINE__, -149);
        if (vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -148);

        if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &inode) !=
                PROSENJITFS_OK ||
            pfs_find_dir_entry(&fs, &inode, fs.sb.root_inode, "sparse-delalloc-cross.bin",
                               &entry) != PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, entry.inode, &cross_inode) !=
                PROSENJITFS_OK ||
            cross_inode.size != (high_block * bs) + 1u ||
            cross_inode.allocated_bytes != bs * 3u ||
            cross_inode.extent_head == 0u)
            return pfs_selftest_fail(__LINE__, -14);

        /*
         * Directory-chain cycle regression: make a checksum-valid entry point
         * back to itself. The checker must reject the repeated metadata block
         * without mutating any mock-media view, then pass after restoration.
         */
        {
            static const char target_name[] = "sparse-delalloc-cross.bin";
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_dir_index root_index;
            struct pros_prosenjitfs_dir_entry candidate;
            uint8_t original_entry_block[4096];
            uint8_t corrupt_entry_block[4096];
            uint64_t current;
            uint64_t target_lba = 0u;
            uint64_t steps = 0u;
            uint32_t bucket;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                pfs_read_dir_index(&fs, &root_inode, fs.sb.root_inode, &root_index) !=
                    PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            bucket = pfs_name_hash((const uint8_t *)target_name,
                                   (uint32_t)(sizeof(target_name) - 1u)) %
                     PROSENJITFS_DIR_BUCKETS;
            current = root_index.heads[bucket];
            while (current != 0u && ++steps <= fs.sb.total_blocks) {
                uint8_t entry_block[4096];
                uint32_t same = 0u;
                if (current < fs.sb.data_start ||
                    current >= fs.sb.total_blocks ||
                    pfs_read_block(fs.mount.device, current, entry_block) !=
                        PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -14);
                pfs_copy(&candidate, entry_block, sizeof(candidate));
                if (candidate.name_length == sizeof(target_name) - 1u) {
                    same = 1u;
                    for (uint32_t ni = 0u;
                         ni < sizeof(target_name) - 1u; ++ni) {
                        if (candidate.name[ni] != (uint8_t)target_name[ni]) {
                            same = 0u;
                            break;
                        }
                    }
                }
                if (same != 0u) {
                    target_lba = current;
                    pfs_copy(original_entry_block, entry_block,
                             fs.sb.block_size);
                    break;
                }
                current = candidate.next;
            }
            if (target_lba == 0u)
                return pfs_selftest_fail(__LINE__, -14);

            pfs_copy(corrupt_entry_block, original_entry_block,
                     fs.sb.block_size);
            pfs_copy(&candidate, corrupt_entry_block, sizeof(candidate));
            candidate.next = target_lba;
            candidate.checksum = 0u;
            pfs_copy(corrupt_entry_block, &candidate, sizeof(candidate));
            candidate.checksum = pfs_crc_with_zero32(
                corrupt_entry_block, sizeof(candidate),
                (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
            pfs_copy(corrupt_entry_block, &candidate, sizeof(candidate));
            if (pfs_write_block(fs.mount.device, target_lba,
                                corrupt_entry_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, target_lba,
                                      original_entry_block);
                return pfs_selftest_fail(__LINE__, -14);
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, target_lba,
                                      original_entry_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, target_lba,
                                original_entry_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Directory hash-bucket membership regression: move a valid entry's
         * name/hash to match an entry stored in another bucket. The checker
         * must reject the misplaced entry without mutating mock media.
         */
        {
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_dir_index root_index;
            struct pros_prosenjitfs_dir_entry first_entry;
            struct pros_prosenjitfs_dir_entry second_entry;
            uint8_t first_entry_block[4096];
            uint8_t original_second_block[4096];
            uint8_t corrupt_second_block[4096];
            uint64_t first_lba = 0u;
            uint64_t second_lba = 0u;
            uint32_t first_bucket = 0u;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                pfs_read_dir_index(&fs, &root_inode, fs.sb.root_inode, &root_index) !=
                    PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);

            for (uint32_t bucket = 0u;
                 bucket < PROSENJITFS_DIR_BUCKETS && second_lba == 0u;
                 ++bucket) {
                uint64_t current = root_index.heads[bucket];
                if (current == 0u)
                    continue;
                if (current < fs.sb.data_start ||
                    current >= fs.sb.total_blocks)
                    return pfs_selftest_fail(__LINE__, -14);
                if (first_lba == 0u) {
                    first_lba = current;
                    first_bucket = bucket;
                    if (pfs_read_block(fs.mount.device, first_lba,
                                       first_entry_block) != PROSENJITFS_OK)
                        return pfs_selftest_fail(__LINE__, -14);
                    pfs_copy(&first_entry, first_entry_block,
                             sizeof(first_entry));
                    continue;
                }
                if (bucket == first_bucket)
                    continue;
                second_lba = current;
                if (pfs_read_block(fs.mount.device, second_lba,
                                   original_second_block) != PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -14);
                pfs_copy(&second_entry, original_second_block,
                         sizeof(second_entry));
            }
            if (first_lba == 0u || second_lba == 0u ||
                first_entry.name_length == 0u ||
                first_entry.name_length > PROSENJITFS_MAX_NAME)
                return pfs_selftest_fail(__LINE__, -14);

            pfs_copy(corrupt_second_block, original_second_block,
                     fs.sb.block_size);
            pfs_zero(second_entry.name, sizeof(second_entry.name));
            pfs_copy(second_entry.name, first_entry.name,
                     first_entry.name_length);
            second_entry.name[first_entry.name_length] = 0u;
            second_entry.name_length = first_entry.name_length;
            second_entry.name_hash = first_entry.name_hash;
            second_entry.checksum = 0u;
            pfs_copy(corrupt_second_block, &second_entry,
                     sizeof(second_entry));
            second_entry.checksum = pfs_crc_with_zero32(
                corrupt_second_block, sizeof(second_entry),
                (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
            pfs_copy(corrupt_second_block, &second_entry,
                     sizeof(second_entry));

            if (pfs_write_block(fs.mount.device, second_lba,
                                corrupt_second_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, second_lba,
                                      original_second_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, second_lba,
                                      original_second_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, second_lba,
                                original_second_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        if (pfs_read_extent(&fs, cross_inode.extent_head, &first_extent) !=
                PROSENJITFS_OK ||
            first_extent.logical_block != low_block ||
            first_extent.block_count != 2u ||
            first_extent.next == 0u)
            return pfs_selftest_fail(__LINE__, -14);

        if (pfs_read_extent(&fs, first_extent.next, &second_extent) !=
                PROSENJITFS_OK ||
            second_extent.logical_block != high_block ||
            second_extent.block_count != 1u ||
            second_extent.next != 0u)
            return pfs_selftest_fail(__LINE__, -14);
        high_lba = second_extent.physical_block;

        /*
         * Cross-extent ownership regression: make the later logical extent
         * alias the first extent's physical data block while keeping its
         * checksum valid. The checker must reject the duplicate owner and
         * must not modify any mock-media view during the scan.
         */
        {
            uint8_t original_extent_block[4096];
            uint8_t corrupt_extent_block[4096];
            struct pros_prosenjitfs_extent corrupt_extent;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;
            uint64_t extent_lba = first_extent.next;

            if (pfs_read_block(fs.mount.device, extent_lba,
                               original_extent_block) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_extent_block, original_extent_block,
                     fs.sb.block_size);
            pfs_copy(&corrupt_extent, corrupt_extent_block,
                     sizeof(corrupt_extent));
            if (first_extent.physical_block == second_extent.physical_block)
                return pfs_selftest_fail(__LINE__, -14);
            corrupt_extent.physical_block = first_extent.physical_block;
            corrupt_extent.checksum = 0u;
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));
            corrupt_extent.checksum = pfs_crc_with_zero32(
                corrupt_extent_block, sizeof(corrupt_extent),
                (uint32_t)offsetof(struct pros_prosenjitfs_extent, checksum));
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));

            if (pfs_write_block(fs.mount.device, extent_lba,
                                corrupt_extent_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, extent_lba,
                                original_extent_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Logical extent overlap regression: move the second extent's logical
         * start into the first extent's covered range while preserving a valid
         * checksum. The scanner must reject overlapping logical ownership
         * without changing any mock-media view.
         */
        {
            uint8_t original_extent_block[4096];
            uint8_t corrupt_extent_block[4096];
            struct pros_prosenjitfs_extent corrupt_extent;
            uint64_t extent_lba = first_extent.next;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (extent_lba == 0u ||
                pfs_read_block(fs.mount.device, extent_lba,
                               original_extent_block) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_extent_block, original_extent_block,
                     fs.sb.block_size);
            pfs_copy(&corrupt_extent, corrupt_extent_block,
                     sizeof(corrupt_extent));
            if (first_extent.block_count < 2u ||
                first_extent.logical_block == UINT64_MAX)
                return pfs_selftest_fail(__LINE__, -14);
            corrupt_extent.logical_block = first_extent.logical_block + 1u;
            corrupt_extent.checksum = 0u;
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));
            corrupt_extent.checksum = pfs_crc_with_zero32(
                corrupt_extent_block, sizeof(corrupt_extent),
                (uint32_t)offsetof(struct pros_prosenjitfs_extent, checksum));
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));

            if (pfs_write_block(fs.mount.device, extent_lba,
                                corrupt_extent_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, extent_lba,
                                original_extent_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Extent-chain cycle regression: make the second extent point to
         * itself with a valid checksum. The ownership scan must detect the
         * repeated extent-metadata block and remain read-only.
         */
        {
            uint8_t original_extent_block[4096];
            uint8_t corrupt_extent_block[4096];
            struct pros_prosenjitfs_extent corrupt_extent;
            uint64_t extent_lba = first_extent.next;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (extent_lba == 0u ||
                pfs_read_block(fs.mount.device, extent_lba,
                               original_extent_block) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_extent_block, original_extent_block,
                     fs.sb.block_size);
            pfs_copy(&corrupt_extent, corrupt_extent_block,
                     sizeof(corrupt_extent));
            corrupt_extent.next = extent_lba;
            corrupt_extent.checksum = 0u;
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));
            corrupt_extent.checksum = pfs_crc_with_zero32(
                corrupt_extent_block, sizeof(corrupt_extent),
                (uint32_t)offsetof(struct pros_prosenjitfs_extent, checksum));
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));

            if (pfs_write_block(fs.mount.device, extent_lba,
                                corrupt_extent_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, extent_lba,
                                original_extent_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Block-bitmap tail regression: set the first bit beyond total_blocks.
         * Such bits are never valid allocation state and must be rejected
         * without the report-only checker changing the media.
         */
        {
            uint64_t bits_per_block = (uint64_t)fs.sb.block_size * 8u;
            uint64_t bi = fs.sb.total_blocks / bits_per_block;
            uint64_t within = fs.sb.total_blocks % bits_per_block;
            uint64_t lba;
            uint8_t original_bitmap[4096];
            uint8_t corrupt_bitmap[4096];
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (within == 0u || bi >= fs.sb.block_bitmap_blocks ||
                pfs_lba_add(fs.mount.device, fs.sb.block_bitmap_start,
                            bi, &lba) != PROSENJITFS_OK ||
                pfs_read_block(fs.mount.device, lba,
                               original_bitmap) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_bitmap, original_bitmap, fs.sb.block_size);
            corrupt_bitmap[within >> 3] |=
                (uint8_t)(1u << (within & 7u));
            if (pfs_write_block(fs.mount.device, lba,
                                corrupt_bitmap) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, lba, original_bitmap);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, lba, original_bitmap);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, lba, original_bitmap) !=
                    PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Inode-bitmap tail regression: set the first bit beyond inode_count.
         * The integrity checker must reject the invalid inode allocation bit
         * without mutating the medium, then pass after the bit is restored.
         */
        {
            uint64_t bits_per_block = (uint64_t)fs.sb.block_size * 8u;
            uint64_t bi = (uint64_t)fs.sb.inode_count / bits_per_block;
            uint64_t within = (uint64_t)fs.sb.inode_count % bits_per_block;
            uint64_t lba;
            uint8_t original_bitmap[4096];
            uint8_t corrupt_bitmap[4096];
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (within == 0u || bi >= fs.sb.inode_bitmap_blocks ||
                pfs_lba_add(fs.mount.device, fs.sb.inode_bitmap_start,
                            bi, &lba) != PROSENJITFS_OK ||
                pfs_read_block(fs.mount.device, lba,
                               original_bitmap) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_bitmap, original_bitmap, fs.sb.block_size);
            corrupt_bitmap[within >> 3] |=
                (uint8_t)(1u << (within & 7u));
            if (pfs_write_block(fs.mount.device, lba,
                                corrupt_bitmap) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, lba, original_bitmap);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, lba, original_bitmap);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, lba, original_bitmap) !=
                    PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Duplicate-name regression: create two distinct names that hash to
         * the same directory bucket, then rewrite the second entry to the
         * first name with a valid checksum. The checker must reject the
         * ambiguous namespace and leave mock media unchanged while scanning.
         */
        {
            static const char first_name[] = "bucket-collision-1";
            static const char second_name[] = "bucket-collision-10";
            static const char first_path[] = "/bucket-collision-1";
            static const char second_path[] = "/bucket-collision-10";
            struct pros_vfs_file create_file;
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_dir_index root_index;
            struct pros_prosenjitfs_dir_entry first_entry;
            struct pros_prosenjitfs_dir_entry second_entry;
            uint8_t entry_block[4096];
            uint8_t original_second_block[4096];
            uint8_t corrupt_second_block[4096];
            uint64_t first_lba = 0u;
            uint64_t second_lba = 0u;
            uint64_t current;
            uint64_t steps = 0u;
            uint32_t bucket;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (pfs_name_hash((const uint8_t *)first_name,
                              (uint32_t)(sizeof(first_name) - 1u)) %
                    PROSENJITFS_DIR_BUCKETS !=
                pfs_name_hash((const uint8_t *)second_name,
                              (uint32_t)(sizeof(second_name) - 1u)) %
                    PROSENJITFS_DIR_BUCKETS)
                return pfs_selftest_fail(__LINE__, -14);
            if (vfs_kernel_file_open(&fs.mount, first_path,
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                              &create_file) != PROS_VFS_OK ||
                vfs_file_close(&create_file) != PROS_VFS_OK ||
                vfs_kernel_file_open(&fs.mount, second_path,
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                              &create_file) != PROS_VFS_OK ||
                vfs_file_close(&create_file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -14);

            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                pfs_read_dir_index(&fs, &root_inode, fs.sb.root_inode, &root_index) !=
                    PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            bucket = pfs_name_hash((const uint8_t *)first_name,
                                   (uint32_t)(sizeof(first_name) - 1u)) %
                     PROSENJITFS_DIR_BUCKETS;
            current = root_index.heads[bucket];
            while (current != 0u && ++steps <= fs.sb.total_blocks) {
                uint32_t is_first = 1u;
                uint32_t is_second = 1u;
                if (current < fs.sb.data_start ||
                    current >= fs.sb.total_blocks ||
                    pfs_read_block(fs.mount.device, current, entry_block) !=
                        PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -14);
                pfs_copy(&first_entry, entry_block, sizeof(first_entry));
                if (first_entry.name_length == sizeof(first_name) - 1u) {
                    for (uint32_t ni = 0u;
                         ni < sizeof(first_name) - 1u; ++ni) {
                        if (first_entry.name[ni] != (uint8_t)first_name[ni]) {
                            is_first = 0u;
                            break;
                        }
                    }
                    if (is_first != 0u)
                        first_lba = current;
                } else {
                    is_first = 0u;
                }
                if (first_entry.name_length == sizeof(second_name) - 1u) {
                    for (uint32_t ni = 0u;
                         ni < sizeof(second_name) - 1u; ++ni) {
                        if (first_entry.name[ni] != (uint8_t)second_name[ni]) {
                            is_second = 0u;
                            break;
                        }
                    }
                    if (is_second != 0u) {
                        second_lba = current;
                        pfs_copy(original_second_block, entry_block,
                                 fs.sb.block_size);
                        second_entry = first_entry;
                    }
                }
                current = first_entry.next;
            }
            if (first_lba == 0u || second_lba == 0u ||
                first_lba == second_lba)
                return pfs_selftest_fail(__LINE__, -14);

            pfs_copy(corrupt_second_block, original_second_block,
                     fs.sb.block_size);
            pfs_zero(second_entry.name, sizeof(second_entry.name));
            pfs_copy(second_entry.name, first_name,
                     (uint32_t)(sizeof(first_name) - 1u));
            second_entry.name[sizeof(first_name) - 1u] = 0u;
            second_entry.name_length = (uint32_t)(sizeof(first_name) - 1u);
            second_entry.name_hash = pfs_name_hash(
                (const uint8_t *)first_name,
                (uint32_t)(sizeof(first_name) - 1u));
            second_entry.checksum = 0u;
            pfs_copy(corrupt_second_block, &second_entry,
                     sizeof(second_entry));
            second_entry.checksum = pfs_crc_with_zero32(
                corrupt_second_block, sizeof(second_entry),
                (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
            pfs_copy(corrupt_second_block, &second_entry,
                     sizeof(second_entry));

            if (pfs_write_block(fs.mount.device, second_lba,
                                corrupt_second_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, second_lba,
                                      original_second_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, second_lba,
                                      original_second_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, second_lba,
                                original_second_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Cross-inode ownership regression: create a second file, then point
         * its checksum-valid extent at a block already owned by
         * sparse-delalloc-cross.bin. The global owner map must reject aliasing
         * across inode boundaries, without scan-side media mutation.
         */
        {
            static const char alias_name[] = "/cross-inode-alias.bin";
            struct pros_vfs_file alias_file;
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_dir_entry alias_entry;
            struct pros_prosenjitfs_inode alias_inode;
            struct pros_prosenjitfs_extent alias_extent;
            uint8_t payload = 0x6Du;
            uint8_t original_extent_block[4096];
            uint8_t corrupt_extent_block[4096];
            uint32_t done = 0u;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;
            uint64_t extent_lba;

            if (vfs_kernel_file_open(&fs.mount, alias_name,
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                              &alias_file) != PROS_VFS_OK ||
                vfs_file_write(&alias_file, &payload, 1u, &done) !=
                    PROS_VFS_OK ||
                done != 1u ||
                vfs_file_close(&alias_file) != PROS_VFS_OK ||
                prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                pfs_find_dir_entry(&fs, &root_inode, fs.sb.root_inode, "cross-inode-alias.bin",
                                   &alias_entry) != PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, alias_entry.inode, &alias_inode) !=
                    PROSENJITFS_OK ||
                alias_inode.type != PROSENJITFS_INODE_FILE ||
                alias_inode.extent_head == 0u ||
                pfs_read_extent(&fs, alias_inode.extent_head, &alias_extent) !=
                    PROSENJITFS_OK ||
                alias_extent.physical_block == first_extent.physical_block)
                return pfs_selftest_fail(__LINE__, -14);

            extent_lba = alias_inode.extent_head;
            if (pfs_read_block(fs.mount.device, extent_lba,
                               original_extent_block) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_extent_block, original_extent_block,
                     fs.sb.block_size);
            alias_extent.physical_block = first_extent.physical_block;
            alias_extent.checksum = 0u;
            pfs_copy(corrupt_extent_block, &alias_extent,
                     sizeof(alias_extent));
            alias_extent.checksum = pfs_crc_with_zero32(
                corrupt_extent_block, sizeof(alias_extent),
                (uint32_t)offsetof(struct pros_prosenjitfs_extent, checksum));
            pfs_copy(corrupt_extent_block, &alias_extent,
                     sizeof(alias_extent));

            if (pfs_write_block(fs.mount.device, extent_lba,
                                corrupt_extent_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, extent_lba,
                                original_extent_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Integrity regression: a checksum-valid extent outside the volume's
         * maximum logical file range must be rejected. Keep-size preallocation
         * beyond EOF remains valid. Preserve the original extent block and
         * compare all mock-media/cache views around the read-only scan.
         */
        {
            uint8_t original_extent_block[4096];
            uint8_t corrupt_extent_block[4096];
            struct pros_prosenjitfs_extent corrupt_extent;
            uint64_t extent_lba = first_extent.next;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (pfs_read_block(fs.mount.device, extent_lba,
                               original_extent_block) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_extent_block, original_extent_block,
                     fs.sb.block_size);
            pfs_copy(&corrupt_extent, corrupt_extent_block,
                     sizeof(corrupt_extent));
            corrupt_extent.logical_block =
                fs.sb.total_blocks - fs.sb.data_start;
            corrupt_extent.checksum = 0u;
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));
            corrupt_extent.checksum = pfs_crc_with_zero32(
                corrupt_extent_block, sizeof(corrupt_extent),
                (uint32_t)offsetof(struct pros_prosenjitfs_extent, checksum));
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));

            if (pfs_write_block(fs.mount.device, extent_lba,
                                corrupt_extent_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, extent_lba,
                                original_extent_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * A checksum-valid extent with a block count larger than the remaining
         * logical volume range must fail before the checker iterates over the
         * claimed blocks. This is both a bounds check and a denial-of-service
         * regression: malformed counts must not drive volume-sized work.
         */
        {
            uint8_t original_extent_block[4096];
            uint8_t corrupt_extent_block[4096];
            struct pros_prosenjitfs_extent corrupt_extent;
            uint64_t extent_lba = first_extent.next;
            uint64_t max_logical_blocks =
                fs.sb.total_blocks - fs.sb.data_start;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (pfs_read_block(fs.mount.device, extent_lba,
                               original_extent_block) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_extent_block, original_extent_block,
                     fs.sb.block_size);
            pfs_copy(&corrupt_extent, corrupt_extent_block,
                     sizeof(corrupt_extent));
            if (corrupt_extent.logical_block >= max_logical_blocks) {
                return pfs_selftest_fail(__LINE__, -14);
            }
            corrupt_extent.block_count =
                max_logical_blocks - corrupt_extent.logical_block + 1u;
            corrupt_extent.checksum = 0u;
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));
            corrupt_extent.checksum = pfs_crc_with_zero32(
                corrupt_extent_block, sizeof(corrupt_extent),
                (uint32_t)offsetof(struct pros_prosenjitfs_extent, checksum));
            pfs_copy(corrupt_extent_block, &corrupt_extent,
                     sizeof(corrupt_extent));

            if (pfs_write_block(fs.mount.device, extent_lba,
                                corrupt_extent_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, extent_lba,
                                      original_extent_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, extent_lba,
                                original_extent_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        /*
         * Exercise the other explicit extent-range guards with checksum-valid
         * metadata: a zero-length extent and a physical start at total_blocks.
         * Both must be rejected by the scan without any scan-side media writes.
         */
        {
            uint8_t original_extent_block[4096];
            uint8_t corrupt_extent_block[4096];
            struct pros_prosenjitfs_extent original_extent;
            uint64_t extent_lba = first_extent.next;

            if (pfs_read_block(fs.mount.device, extent_lba,
                               original_extent_block) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(&original_extent, original_extent_block,
                     sizeof(original_extent));

            for (uint32_t variant = 0u; variant < 2u; ++variant) {
                struct pros_prosenjitfs_extent corrupt_extent = original_extent;
                uint32_t durable_before;
                uint32_t volatile_before;
                uint32_t dirty_before;

                if (variant == 0u)
                    corrupt_extent.block_count = 0u;
                else
                    corrupt_extent.physical_block = fs.sb.total_blocks;

                pfs_copy(corrupt_extent_block, original_extent_block,
                         fs.sb.block_size);
                corrupt_extent.checksum = 0u;
                pfs_copy(corrupt_extent_block, &corrupt_extent,
                         sizeof(corrupt_extent));
                corrupt_extent.checksum = pfs_crc_with_zero32(
                    corrupt_extent_block, sizeof(corrupt_extent),
                    (uint32_t)offsetof(struct pros_prosenjitfs_extent,
                                       checksum));
                pfs_copy(corrupt_extent_block, &corrupt_extent,
                         sizeof(corrupt_extent));

                if (pfs_write_block(fs.mount.device, extent_lba,
                                    corrupt_extent_block) != PROSENJITFS_OK) {
                    (void)pfs_write_block(fs.mount.device, extent_lba,
                                          original_extent_block);
                    return pfs_selftest_fail(__LINE__, -14);
                }
                durable_before = prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data));
                volatile_before = prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
                dirty_before = prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty));

                if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                        PROSENJITFS_ECORRUPT ||
                    integrity_report.status != PROSENJITFS_ECORRUPT ||
                    durable_before != prosenjitfs_crc32c(
                        disk.data, (uint32_t)sizeof(disk.data)) ||
                    volatile_before != prosenjitfs_crc32c(
                        disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                    dirty_before != prosenjitfs_crc32c(
                        disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                    (void)pfs_write_block(fs.mount.device, extent_lba,
                                          original_extent_block);
                    return pfs_selftest_fail(__LINE__, -14);
                }
                if (pfs_write_block(fs.mount.device, extent_lba,
                                    original_extent_block) != PROSENJITFS_OK ||
                    prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                        PROSENJITFS_OK ||
                    integrity_report.status != PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -14);
            }
        }

        /*
         * A checksum-valid inode size beyond the volume's maximum file range
         * is corruption even if every individual extent is in range. Inject
         * this malformed inode and prove the scanner itself remains read-only.
         */
        {
            uint8_t original_inode_block[4096];
            uint8_t corrupt_inode_block[4096];
            struct pros_prosenjitfs_inode corrupt_inode;
            uint64_t inode_lba = 0u;
            uint32_t inode_offset = 0u;
            uint64_t max_logical_blocks =
                fs.sb.total_blocks - fs.sb.data_start;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;

            if (pfs_inode_block_location(&fs, entry.inode, &inode_lba,
                                         &inode_offset) != PROSENJITFS_OK ||
                pfs_read_block(fs.mount.device, inode_lba,
                               original_inode_block) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(corrupt_inode_block, original_inode_block, fs.sb.block_size);
            pfs_copy(&corrupt_inode,
                     original_inode_block + inode_offset,
                     sizeof(corrupt_inode));
            corrupt_inode.size = max_logical_blocks * fs.sb.block_size + 1u;
            corrupt_inode.checksum = 0u;
            pfs_copy(corrupt_inode_block + inode_offset, &corrupt_inode,
                     sizeof(corrupt_inode));
            corrupt_inode.checksum = pfs_crc_with_zero32(
                corrupt_inode_block + inode_offset, sizeof(corrupt_inode),
                (uint32_t)offsetof(struct pros_prosenjitfs_inode, checksum));
            pfs_copy(corrupt_inode_block + inode_offset, &corrupt_inode,
                     sizeof(corrupt_inode));

            if (pfs_write_block(fs.mount.device, inode_lba,
                                corrupt_inode_block) != PROSENJITFS_OK) {
                (void)pfs_write_block(fs.mount.device, inode_lba,
                                      original_inode_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_ECORRUPT ||
                integrity_report.status != PROSENJITFS_ECORRUPT ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)pfs_write_block(fs.mount.device, inode_lba,
                                      original_inode_block);
                return pfs_selftest_fail(__LINE__, -14);
            }
            if (pfs_write_block(fs.mount.device, inode_lba,
                                original_inode_block) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                    PROSENJITFS_OK ||
                integrity_report.status != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }

        if (vfs_kernel_file_open(&fs.mount, "/sparse-delalloc-cross.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_seek(&file, low_block * bs) != PROS_VFS_OK ||
            vfs_file_read(&file, &low_a, 1u, &done) != PROS_VFS_OK ||
            done != 1u || low_a != 0x5Au ||
            vfs_file_seek(&file, (low_block + 1u) * bs) != PROS_VFS_OK ||
            vfs_file_read(&file, &low_b, 1u, &done) != PROS_VFS_OK ||
            done != 1u || low_b != 0xA5u ||
            vfs_file_seek(&file, high_block * bs) != PROS_VFS_OK ||
            vfs_file_read(&file, &high, 1u, &done) != PROS_VFS_OK ||
            done != 1u || high != 0xE7u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -14);

        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, entry.inode, &cross_inode) !=
                PROSENJITFS_OK ||
            cross_inode.allocated_bytes != bs * 3u ||
            pfs_read_extent(&fs, cross_inode.extent_head, &first_extent) !=
                PROSENJITFS_OK ||
            first_extent.logical_block != low_block ||
            first_extent.block_count != 2u ||
            pfs_read_extent(&fs, first_extent.next, &second_extent) !=
                PROSENJITFS_OK ||
            second_extent.logical_block != high_block ||
            second_extent.physical_block != high_lba)
            return pfs_selftest_fail(__LINE__, -14);

        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -14);
    }

    /*
     * Exhaust the inode bitmap in a bounded test volume. Snapshot both media
     * views and mutate the mock disk directly so the test itself does not
     * change the dirty map or hide cache-state regressions.
     */
    if (fs.sb.block_size == 512u && fs.sb.inode_bitmap_blocks == 1u &&
        fs.sb.inode_count > 1u &&
        fs.sb.inode_count <= (uint64_t)fs.sb.block_size * 8u) {
        uint8_t saved_durable[512];
        uint8_t saved_volatile[512];
        uint64_t bitmap_lba;
        uint64_t byte_offset;
        uint64_t candidate = UINT64_MAX;
        uint32_t durable_before;
        uint32_t volatile_before;
        uint32_t dirty_before;
        int exhausted_rc;

        if (pfs_lba_add(fs.mount.device, fs.sb.inode_bitmap_start, 0u,
                        &bitmap_lba) != PROSENJITFS_OK ||
            bitmap_lba >= 512u)
            return pfs_selftest_fail(__LINE__, -28);
        byte_offset = bitmap_lba * 512u;
        pfs_copy(saved_durable, disk.data + byte_offset, 512u);
        pfs_copy(saved_volatile, disk.volatile_data + byte_offset, 512u);
        for (uint64_t bit = 0u; bit < fs.sb.inode_count; ++bit) {
            uint8_t mask = (uint8_t)(1u << (bit & 7u));
            disk.data[byte_offset + (bit >> 3)] |= mask;
            disk.volatile_data[byte_offset + (bit >> 3)] |= mask;
        }
        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));

        exhausted_rc = pfs_find_free_inode(&fs, &candidate);
        if (exhausted_rc != PROSENJITFS_ENOSPC || candidate != UINT64_MAX ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty))) {
            pfs_copy(disk.data + byte_offset, saved_durable, 512u);
            pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
            return pfs_selftest_fail(__LINE__, -28);
        }
        /*
         * With every valid inode bit set, clear only the final valid inode.
         * The finder must reach that boundary without reading past it.
         */
        {
            uint64_t last_inode = fs.sb.inode_count - 1u;
            uint64_t bit_byte = byte_offset + (last_inode >> 3);
            uint8_t mask = (uint8_t)(1u << (last_inode & 7u));
            disk.data[bit_byte] &= (uint8_t)~mask;
            disk.volatile_data[bit_byte] &= (uint8_t)~mask;
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            candidate = UINT64_MAX;
            exhausted_rc = pfs_find_free_inode(&fs, &candidate);
            if (exhausted_rc != PROSENJITFS_OK || candidate != last_inode ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                pfs_copy(disk.data + byte_offset, saved_durable, 512u);
                pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
                return pfs_selftest_fail(__LINE__, -28);
            }
        }
        pfs_copy(disk.data + byte_offset, saved_durable, 512u);
        pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
    }

    /*
     * Exhaust the data-block bitmap in the same bounded fixture and verify
     * ENOSPC leaves the result sentinel and all mock-media views unchanged.
     */
    if (fs.sb.block_size == 512u && fs.sb.block_bitmap_blocks == 1u &&
        fs.sb.total_blocks <= (uint64_t)fs.sb.block_size * 8u) {
        uint8_t saved_durable[512];
        uint8_t saved_volatile[512];
        uint64_t bitmap_lba;
        uint64_t byte_offset;
        uint64_t candidate = UINT64_MAX;
        uint32_t durable_before;
        uint32_t volatile_before;
        uint32_t dirty_before;
        int exhausted_rc;

        if (pfs_lba_add(fs.mount.device, fs.sb.block_bitmap_start, 0u,
                        &bitmap_lba) != PROSENJITFS_OK ||
            bitmap_lba >= 512u)
            return pfs_selftest_fail(__LINE__, -29);
        byte_offset = bitmap_lba * 512u;
        pfs_copy(saved_durable, disk.data + byte_offset, 512u);
        pfs_copy(saved_volatile, disk.volatile_data + byte_offset, 512u);
        for (uint64_t bit = fs.sb.data_start; bit < fs.sb.total_blocks; ++bit) {
            uint8_t mask = (uint8_t)(1u << (bit & 7u));
            disk.data[byte_offset + (bit >> 3)] |= mask;
            disk.volatile_data[byte_offset + (bit >> 3)] |= mask;
        }
        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));

        exhausted_rc = pfs_find_free_data_block(&fs, &candidate);
        if (exhausted_rc != PROSENJITFS_ENOSPC || candidate != UINT64_MAX ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty))) {
            pfs_copy(disk.data + byte_offset, saved_durable, 512u);
            pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
            return pfs_selftest_fail(__LINE__, -29);
        }
        /*
         * Clear only the final valid data bit after the exhaustion check.
         * This verifies the allocator's inclusive lower/exclusive upper bound.
         */
        if (fs.sb.total_blocks > fs.sb.data_start) {
            uint64_t last_block = fs.sb.total_blocks - 1u;
            uint64_t bit_byte = byte_offset + (last_block >> 3);
            uint8_t mask = (uint8_t)(1u << (last_block & 7u));
            disk.data[bit_byte] &= (uint8_t)~mask;
            disk.volatile_data[bit_byte] &= (uint8_t)~mask;
            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            candidate = UINT64_MAX;
            exhausted_rc = pfs_find_free_data_block(&fs, &candidate);
            if (exhausted_rc != PROSENJITFS_OK || candidate != last_block ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                pfs_copy(disk.data + byte_offset, saved_durable, 512u);
                pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
                return pfs_selftest_fail(__LINE__, -29);
            }
            /*
             * Excluding a different block must still find the sole free final
             * block. Then excluding that final block must report ENOSPC and
             * preserve the caller's sentinel.
             */
            candidate = UINT64_MAX;
            if (pfs_find_free_data_block_except(
                    &fs, last_block - 1u, &candidate) != PROSENJITFS_OK ||
                candidate != last_block) {
                pfs_copy(disk.data + byte_offset, saved_durable, 512u);
                pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
                return pfs_selftest_fail(__LINE__, -29);
            }
            candidate = UINT64_MAX;
            if (pfs_find_free_data_block_excluding(
                    &fs, last_block - 1u, &candidate) != PROSENJITFS_OK ||
                candidate != last_block ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                pfs_copy(disk.data + byte_offset, saved_durable, 512u);
                pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
                return pfs_selftest_fail(__LINE__, -29);
            }
            candidate = UINT64_MAX;
            if (pfs_find_free_data_block_except(&fs, last_block, &candidate) !=
                    PROSENJITFS_ENOSPC || candidate != UINT64_MAX ||
                pfs_find_free_data_block_excluding(
                    &fs, last_block, &candidate) != PROSENJITFS_ENOSPC ||
                candidate != UINT64_MAX ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                pfs_copy(disk.data + byte_offset, saved_durable, 512u);
                pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
                return pfs_selftest_fail(__LINE__, -29);
            }
        }
        pfs_copy(disk.data + byte_offset, saved_durable, 512u);
        pfs_copy(disk.volatile_data + byte_offset, saved_volatile, 512u);
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
    pfs_selftest_diag_stage = __LINE__;
    /* Allocation alone is a temporary reservation; release it after this
     * boundary check so later consistency scans see no allocated empty inode. */
    if (pfs_inode_bitmap_update(&fs, inode_number, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -8);
    if (prosenjitfs_free_blocks(&fs, fs.sb.total_blocks, 1u) != PROSENJITFS_EINVAL)
        return pfs_selftest_fail(__LINE__, -9);
    if (prosenjitfs_free_blocks(&fs, first_block, 4u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -10);
    if (prosenjitfs_free_blocks(&fs, first_block, 1u) != PROSENJITFS_EINVAL)
        return pfs_selftest_fail(__LINE__, -10);

    /*
     * Bitmap read faults must propagate through both private finders and
     * public allocator APIs without leaving stale output values, changing
     * media/cache state, or forcing an otherwise healthy mount read-only.
     */
    {
        uint64_t candidate = UINT64_MAX;
        uint32_t durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        uint32_t volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        uint32_t dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));

        disk.fail_read_countdown = 1u;
        if (pfs_find_free_inode(&fs, &candidate) != PROSENJITFS_EIO ||
            disk.fail_read_countdown != 0u || candidate != UINT64_MAX ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -31);

        disk.fail_read_countdown = 1u;
        candidate = UINT64_MAX;
        if (prosenjitfs_alloc_inode(&fs, &candidate) != PROSENJITFS_EIO ||
            disk.fail_read_countdown != 0u || candidate != UINT64_MAX ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -31);

        /*
         * A write failure in inode-bitmap reservation must not publish the
         * candidate inode number to the caller when allocation fails.
         * Fail the first (pre-commit) journal write, before media can change.
         */
        disk.fail_write_countdown = 1u;
        candidate = UINT64_MAX;
        if (prosenjitfs_alloc_inode(&fs, &candidate) != PROSENJITFS_EIO ||
            disk.fail_write_countdown != 0u || candidate != UINT64_MAX ||
            fs.read_only || fs.mount.read_only ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty)))
            return pfs_selftest_fail(__LINE__, -33);

        if (durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty)) ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -31);
    }

    /*
     * Data-bitmap read faults are independent: exercise each finder/query
     * separately and make sure a failed scan does not publish a candidate.
     */
    {
        uint64_t candidate = UINT64_MAX;
        uint32_t durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        uint32_t volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        uint32_t dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));

        disk.fail_read_countdown = 1u;
        if (pfs_find_free_data_block(&fs, &candidate) != PROSENJITFS_EIO ||
            disk.fail_read_countdown != 0u || candidate != UINT64_MAX ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -32);

        disk.fail_read_countdown = 1u;
        candidate = UINT64_MAX;
        if (pfs_find_free_data_block_except(
                &fs, UINT64_MAX, &candidate) != PROSENJITFS_EIO ||
            disk.fail_read_countdown != 0u || candidate != UINT64_MAX ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -32);

        disk.fail_read_countdown = 1u;
        candidate = UINT64_MAX;
        if (pfs_find_free_data_block_excluding(
                &fs, UINT64_MAX, &candidate) != PROSENJITFS_EIO ||
            disk.fail_read_countdown != 0u || candidate != UINT64_MAX ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -32);

        disk.fail_read_countdown = 1u;
        if (pfs_data_block_is_free(&fs, fs.sb.data_start) !=
                PROSENJITFS_EIO ||
            disk.fail_read_countdown != 0u ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -32);

        if (durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty)) ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -32);
    }

    /*
     * Preallocation must not turn an I/O failure during extent-metadata
     * block selection into a successful allocation. For an empty file the
     * first four reads are: inode record, bitmap scan, allocation range-state
     * validation, then pfs_data_block_is_free(data_start). The last is the
     * one-shot fault; no data or metadata writes should have happened yet.
     */
    {
        struct pros_vfs_file fault_file;
        struct pros_prosenjitfs_inode root_inode;
        struct pros_prosenjitfs_inode file_inode;
        struct pros_prosenjitfs_dir_entry fault_entry;
        uint32_t durable_before;
        uint32_t volatile_before;
        uint32_t dirty_before;
        int prealloc_rc;

        if (vfs_kernel_file_open(&fs.mount, "/prealloc-read-error.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &fault_file) != PROS_VFS_OK ||
            vfs_file_close(&fault_file) != PROS_VFS_OK ||
            prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                PROSENJITFS_OK ||
            pfs_find_dir_entry(&fs, &root_inode, fs.sb.root_inode,
                               "prealloc-read-error.bin", &fault_entry) !=
                PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, fault_entry.inode, &file_inode) !=
                PROSENJITFS_OK ||
            file_inode.type != PROSENJITFS_INODE_FILE ||
            file_inode.extent_head != 0u ||
            file_inode.allocated_bytes != 0u)
            return pfs_selftest_fail(__LINE__, -36);

        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));

        disk.fail_read_countdown = 4u;
        prealloc_rc = prosenjitfs_preallocate_blocks(
            &fs, fault_entry.inode, 1u, 2u);
        if (prealloc_rc != PROSENJITFS_EIO ||
            disk.fail_read_countdown != 0u ||
            fs.read_only || fs.mount.read_only ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty)) ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -36);
    }



    pfs_selftest_diag_stage = __LINE__;
    /* Keep-size preallocation regression: reserve future blocks without
     * changing EOF, verify zero initialization, persistence, and subsequent
     * in-place write reuse. */
    {
        struct pros_prosenjitfs_inode root_inode;
        struct pros_prosenjitfs_inode before;
        struct pros_prosenjitfs_inode after;
        struct pros_prosenjitfs_dir_entry entry;
        struct pros_prosenjitfs_extent extent;
        uint64_t extent_found_lba = 0u;
        uint64_t extent_tail_lba = 0u;
        struct pros_vfs_file file;
        uint8_t zero[512];
        uint8_t value = 0xA7u;
        uint8_t readback = 0u;
        uint32_t done = 0u;

        pfs_zero(zero, sizeof(zero));
        if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                PROSENJITFS_OK ||
            pfs_find_dir_entry(&fs, &root_inode, fs.sb.root_inode, "append.bin", &entry) !=
                PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, entry.inode, &before) !=
                PROSENJITFS_OK ||
            before.type != PROSENJITFS_INODE_FILE ||
            prosenjitfs_preallocate_blocks(&fs, entry.inode, 1u, 2u) !=
                PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, entry.inode, &after) !=
                PROSENJITFS_OK ||
            after.size != before.size ||
            after.allocated_bytes !=
                before.allocated_bytes + 2u * fs.sb.block_size)
            return pfs_selftest_fail(__LINE__, -10);

        {
            int extent_rc = pfs_find_extent(&fs, &after, 1u, &extent,
                                            &extent_found_lba,
                                            &extent_tail_lba);
            if (extent_rc != PROSENJITFS_OK) {
                if (after.extent_head == 0u)
                    return pfs_selftest_fail(__LINE__, -161);
                if (pfs_read_extent(&fs, after.extent_head, &extent) !=
                    PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -162);
                if (extent.logical_block == 0u && extent.block_count == 1u &&
                    extent.next == 0u)
                    return pfs_selftest_fail(__LINE__, -163);
                if (extent.logical_block == 0u && extent.block_count == 3u)
                    return pfs_selftest_fail(__LINE__, -164);
                if (extent.logical_block == 1u && extent.block_count == 2u)
                    return pfs_selftest_fail(__LINE__, -165);
                return pfs_selftest_fail(__LINE__, -166);
            }
            if (extent.logical_block > 1u ||
                pfs_lba_add(fs.mount.device, extent.physical_block,
                            1u - extent.logical_block, &first_block) !=
                    PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -152);
            if (pfs_read_block(&device, first_block, zero) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -153);
        }

        for (uint32_t i = 0u; i < sizeof(zero); ++i)
            if (zero[i] != 0u)
                return pfs_selftest_fail(__LINE__, -10);

        if (vfs_kernel_file_open(&fs.mount, "/append.bin", PROS_VFS_O_WRITE, &file) !=
                PROS_VFS_OK ||
            vfs_file_seek(&file, fs.sb.block_size) != PROS_VFS_OK ||
            vfs_file_write(&file, &value, 1u, &done) != PROS_VFS_OK ||
            done != 1u ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/append.bin", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK ||
            vfs_file_seek(&file, fs.sb.block_size) != PROS_VFS_OK ||
            vfs_file_read(&file, &readback, 1u, &done) != PROS_VFS_OK ||
            done != 1u || readback != value ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -10);

        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -10);
        if (vfs_kernel_file_open(&fs.mount, "/append.bin", PROS_VFS_O_READ, &file) !=
                PROS_VFS_OK ||
            vfs_file_seek(&file, fs.sb.block_size) != PROS_VFS_OK ||
            vfs_file_read(&file, &readback, 1u, &done) != PROS_VFS_OK ||
            done != 1u || readback != value ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -10);
    }

    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -11);

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    pfs_selftest_diag_stage = __LINE__;
    /* Persistence checks for append and sparse files after remount. */
    {
        struct pros_vfs_file file;
        uint8_t append_data[6];
        uint8_t sparse_data[513];
        uint32_t done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/append.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, append_data, sizeof(append_data), &done) != PROS_VFS_OK ||
            done != sizeof(append_data) || vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);
        if (append_data[0] != 'a' || append_data[1] != 'b' || append_data[2] != 'c' ||
            append_data[3] != 'D' || append_data[4] != 'E' || append_data[5] != 'F')
            return pfs_selftest_fail(__LINE__, -15);
        if (vfs_kernel_file_open(&fs.mount, "/sparse.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
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
        if (vfs_kernel_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
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
        if (vfs_kernel_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE, &file) !=
                PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -12);

        pfs_selftest_diag_stage = __LINE__;
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


        if (vfs_kernel_file_open(&fs.mount, "/hello.bin",
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

        pfs_selftest_diag_stage = __LINE__;
        /*
         * Explicitly exercise seek/read/write across the 512-byte block
         * boundary. The file is 700 bytes, so offset 500 + 200 bytes
         * crosses from logical block 0 into logical block 1.
         */
        if (vfs_kernel_file_open(&fs.mount, "/hello.bin",
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

        pfs_selftest_diag_stage = __LINE__;
        /*
         * A seek beyond the 32-bit VFS offset limit must fail without
         * changing the current position. Seeking past EOF itself is valid
         * for sparse-file writes and is tested above.
         */
        if (file.position != 700u ||
            vfs_file_seek(&file, 0x100000000ULL) != PROS_VFS_EOVERFLOW ||
            file.position != 700u) {
            vfs_file_close(&file);
            return pfs_selftest_fail(__LINE__, -12);
        }

        pfs_selftest_diag_stage = __LINE__;
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

        pfs_selftest_diag_stage = __LINE__;
        /*
         * Partial write starting at offset 500 must cross the 512-byte
         * boundary while preserving the file size.
         */
        if (vfs_kernel_file_open(&fs.mount, "/hello.bin",
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

        pfs_selftest_diag_stage = __LINE__;
        /*
         * Verify the cross-boundary write immediately through the VFS.
         */
        if (vfs_kernel_file_open(&fs.mount, "/hello.bin",
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

        if (vfs_kernel_file_open(&fs.mount, "/hello.bin",
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

        /* Shrink must not expose stale bytes when a later write extends EOF. */
        {
            uint8_t marker = 0xE7u;
            uint8_t gap[101];
            uint32_t write_done = 0u;
            uint32_t read_done = 0u;

            if (vfs_kernel_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE,
                              &file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -12);
            if (vfs_file_seek(&file, 200u) != PROS_VFS_OK ||
                vfs_file_write(&file, &marker, 1u, &write_done) !=
                    PROS_VFS_OK ||
                write_done != 1u || file.vnode->size != 201u) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }
            if (vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -12);

            if (vfs_kernel_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ,
                              &file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -12);
            if (vfs_file_seek(&file, 100u) != PROS_VFS_OK ||
                vfs_file_read(&file, gap, sizeof(gap), &read_done) !=
                    PROS_VFS_OK ||
                read_done != sizeof(gap) || gap[100] != marker) {
                vfs_file_close(&file);
                return pfs_selftest_fail(__LINE__, -12);
            }
            for (uint32_t i = 0u; i < 100u; ++i) {
                if (gap[i] != 0u) {
                    vfs_file_close(&file);
                    return pfs_selftest_fail(__LINE__, -12);
                }
            }
            if (vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -12);
        }
    }
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);

    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    {
        struct pros_vfs_file file;
        uint8_t read_data[100];
        uint32_t done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
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
        if (vfs_kernel_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_WRITE, &file) !=
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
        if (vfs_kernel_file_open(&fs.mount, "/hello.bin", PROS_VFS_O_READ, &file) !=
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
            vfs_kernel_rename(&fs.mount, "/hello.bin", "/.hello.bin") !=
                PROS_VFS_OK ||
            vfs_kernel_path_lookup(&fs.mount, "/hello.bin", &hidden) !=
                PROS_VFS_ENOENT ||
            hidden != 0)
            return pfs_selftest_fail(__LINE__, -12);
        if (vfs_kernel_path_lookup(&fs.mount, "/.hello.bin", &hidden) !=
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
            rc = vfs_kernel_path_lookup(&fs.mount, "/hello.bin", &hidden);
            if (rc != PROS_VFS_ENOENT)
                return pfs_selftest_fail(41733u, -12);

            if (hidden != 0)
                return pfs_selftest_fail(41734u, -12);

            rc = vfs_kernel_path_lookup(&fs.mount, "/.hello.bin", &hidden);
            if (rc != PROS_VFS_OK)
                return pfs_selftest_fail(41735u, -12);

            if (!hidden)
                return pfs_selftest_fail(41736u, -12);

            rc = vfs_vnode_put(hidden);
            if (rc != PROS_VFS_OK)
                return pfs_selftest_fail(41737u, -12);

            rc = vfs_kernel_rename(&fs.mount, "/.hello.bin", "/hello.bin");
            if (rc != PROS_VFS_OK)
                return pfs_selftest_fail(41738u, -12);
        }
    }
    pfs_selftest_diag_stage = __LINE__;
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

        if (vfs_kernel_file_open(&fs.mount, "/stale.bin",
                          PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                          PROS_VFS_O_CREATE, &stale) != PROS_VFS_OK ||
            vfs_file_write(&stale, &value, 1u, &done) != PROS_VFS_OK ||
            done != 1u)
            return pfs_selftest_fail(__LINE__, -13);

        if (vfs_kernel_unlink(&fs.mount, "/stale.bin") != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_file_read(&stale, &verify, 1u, &done) != PROS_VFS_ESTALE ||
            vfs_file_write(&stale, &value, 1u, &done) != PROS_VFS_ESTALE ||
            vfs_file_truncate(&stale, 0u) != PROS_VFS_ESTALE)
            return pfs_selftest_fail(__LINE__, -13);

        if (vfs_kernel_file_open(&fs.mount, "/stale.bin",
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
    if (vfs_kernel_unlink(&fs.mount, "/hello.bin") != PROS_VFS_OK)
        return pfs_selftest_fail(__LINE__, -12);
    {
        struct pros_vfs_vnode *deleted = 0;
        if (vfs_kernel_path_lookup(&fs.mount, "/hello.bin", &deleted) !=
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

    pfs_selftest_diag_stage = __LINE__;
    /*
     * Transaction hygiene regression: rejected staging must not leak a
     * partially prepared transaction into the next filesystem operation.
     */
    {
        uint8_t tx_image[512];
        pfs_zero(tx_image, sizeof(tx_image));
        if (pfs_tx_reset(&fs) != PROSENJITFS_OK ||
            pfs_tx_add(&fs, fs.sb.journal_start, tx_image) !=
                PROSENJITFS_EINVAL ||
            fs.tx.count != 0u)
            return pfs_selftest_fail(__LINE__, -12);
        if (pfs_tx_reset(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -12);
        for (uint32_t n = 0u; n < PROSENJITFS_MAX_TX_BLOCKS; ++n) {
            if (pfs_tx_add(&fs, fs.sb.data_start + n, tx_image) !=
                    PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -12);
        }
        if (pfs_tx_add(&fs, fs.sb.data_start + PROSENJITFS_MAX_TX_BLOCKS,
                       tx_image) != PROSENJITFS_EOVERFLOW ||
            fs.tx.count != 0u)
            return pfs_selftest_fail(__LINE__, -12);
    }


    pfs_selftest_diag_stage = __LINE__;
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

        if (vfs_kernel_mkdir(&fs.mount, "/persist") != PROS_VFS_OK ||
            vfs_kernel_mkdir(&fs.mount, "/persist/config") != PROS_VFS_OK ||
            vfs_kernel_mkdir(&fs.mount, "/persist/config/sub") != PROS_VFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/persist/config/sub/tree.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, data, sizeof(data), &done) != PROS_VFS_OK ||
            done != sizeof(data) ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/persist/config/rename.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, small_data, sizeof(small_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(small_data) ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_kernel_path_lookup(&fs.mount, "/persist/config/sub/tree.bin",
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
            vfs_kernel_path_lookup(&fs.mount, "/persist/config/rename.bin",
                            &node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);
        node_data = (struct pfs_vnode_data *)node->private_data;
        if (!node_data) {
            vfs_vnode_put(node);
            return pfs_selftest_fail(__LINE__, -15);
        }
        rename_inode_before = node_data->inode_number;
        if (vfs_vnode_put(node) != PROS_VFS_OK ||
            vfs_kernel_rename(&fs.mount, "/persist/config/rename.bin",
                       "/persist/config/sub/renamed.bin") != PROS_VFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/persist/config/sub/tree.bin",
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

        /*
         * Repeat the integrity-scan read-failure sweep with a populated
         * namespace, a multi-block file, and a renamed entry. The clean-format
         * sweep above cannot exercise all directory, extent and parent-chain
         * reads; this pass ensures those paths also preserve EIO and that the
         * report-only scan leaves every mock-media view unchanged.
         */
        {
            uint32_t durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            uint32_t volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            uint32_t dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            uint32_t injected = 0u;
            uint32_t read_position;

            for (read_position = 1u; read_position <= 512u;
                 ++read_position) {
                int scan_rc;
                disk.fail_read_countdown = read_position;
                scan_rc = prosenjitfs_check_integrity_report(
                    &fs, &integrity_report);
                if (disk.fail_read_countdown != 0u) {
                    if (scan_rc != PROSENJITFS_OK ||
                        integrity_report.status != PROSENJITFS_OK)
                        return pfs_selftest_fail(__LINE__, -203);
                    break;
                }
                /*
                 * Keep each failure mode distinguishable in the boot log.
                 * This sweep covers a populated namespace and can otherwise
                 * report only the common -203 code, hiding whether the scan
                 * lost EIO propagation or mutated a media/cache view.
                 */
                if (scan_rc != PROSENJITFS_EIO)
                    return pfs_selftest_fail(__LINE__, -3000 + scan_rc);
                if (integrity_report.status != PROSENJITFS_EIO)
                    return pfs_selftest_fail(__LINE__, -205);
                if (durable_before != prosenjitfs_crc32c(
                        disk.data, (uint32_t)sizeof(disk.data)))
                    return pfs_selftest_fail(__LINE__, -206);
                if (volatile_before != prosenjitfs_crc32c(
                        disk.volatile_data,
                        (uint32_t)sizeof(disk.volatile_data)))
                    return pfs_selftest_fail(__LINE__, -207);
                if (dirty_before != prosenjitfs_crc32c(
                        disk.dirty, (uint32_t)sizeof(disk.dirty)))
                    return pfs_selftest_fail(__LINE__, -208);
                ++injected;
            }
            disk.fail_read_countdown = 0u;
            if (read_position > 512u || injected < 1u ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)))
                return pfs_selftest_fail(__LINE__, -203);
        }

        {
            int inode_rc = prosenjitfs_read_inode(&fs, inode_before, &inode);
            if (inode_rc != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -201);
            inode_rc = prosenjitfs_read_inode(&fs, rename_inode_before, &inode);
            if (inode_rc != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -202);
            struct pros_prosenjitfs_integrity_report consistency_report;
            int consistency_rc = prosenjitfs_check_integrity_report(
                &fs, &consistency_report);
            if (consistency_rc != PROSENJITFS_OK)
                return pfs_selftest_fail(
                    __LINE__,
                    consistency_report.first_error_location != 0u
                        ? -(int)consistency_report.first_error_location
                        : consistency_rc);
        }
        if (vfs_kernel_path_lookup(&fs.mount, "/persist/config/sub/tree.bin",
                            &node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -152);
        if (node->size != 513u + sizeof(small_data)) {
            (void)vfs_vnode_put(node);
            return pfs_selftest_fail(__LINE__, -153);
        }
        if (vfs_vnode_put(node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -154);
        if (vfs_kernel_path_lookup(&fs.mount, "/persist/config/sub/renamed.bin",
                            &node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -155);
        node_data = (struct pfs_vnode_data *)node->private_data;
        if (!node_data || node_data->inode_number != rename_inode_before ||
            node_data->inode_number == inode_before ||
            vfs_vnode_put(node) != PROS_VFS_OK ||
            vfs_kernel_path_lookup(&fs.mount, "/persist/config",
                            &node) != PROS_VFS_OK ||
            vfs_vnode_put(node) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        /*
         * Cross-directory directory rename regression: preserve descendants,
         * reject moving a directory below itself, and verify parent metadata
         * survives a clean unmount/remount.
         */
        {
            struct pros_vfs_vnode *moved_node = 0;
            if (vfs_kernel_mkdir(&fs.mount, "/persist/move_src") != PROS_VFS_OK ||
                vfs_kernel_mkdir(&fs.mount, "/persist/move_src/child") != PROS_VFS_OK ||
                vfs_kernel_mkdir(&fs.mount, "/persist/move_dst") != PROS_VFS_OK ||
                vfs_kernel_rename(&fs.mount, "/persist/move_src",
                           "/persist/move_dst/moved") != PROS_VFS_OK ||
                vfs_kernel_rename(&fs.mount, "/persist/move_dst/moved",
                           "/persist/move_dst/moved/child/loop") !=
                    PROS_VFS_EINVAL ||
                vfs_kernel_path_lookup(&fs.mount, "/persist/move_dst/moved/child",
                                &moved_node) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -211);
            if (vfs_vnode_put(moved_node) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -212);
            if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_path_lookup(&fs.mount, "/persist/move_dst/moved/child",
                                &moved_node) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -213);
            if (vfs_vnode_put(moved_node) != PROS_VFS_OK ||
                vfs_kernel_rmdir(&fs.mount, "/persist/move_dst/moved/child") !=
                    PROS_VFS_OK ||
                vfs_kernel_rmdir(&fs.mount, "/persist/move_dst/moved") !=
                    PROS_VFS_OK ||
                vfs_kernel_rmdir(&fs.mount, "/persist/move_dst") != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -214);
        }

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/persist/config/sub/tree.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, data, sizeof(data), &done) != PROS_VFS_OK ||
            done != 545u ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        pfs_selftest_diag_stage = __LINE__;
        {
            int unlink_rc = vfs_kernel_unlink(&fs.mount,
                                       "/persist/config/sub/renamed.bin");
            if (unlink_rc != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -151);
            int rmdir_rc = vfs_kernel_rmdir(&fs.mount, "/persist/config/sub");
            if (rmdir_rc != PROS_VFS_ENOTEMPTY)
                return pfs_selftest_fail(__LINE__, rmdir_rc);
        }

        pfs_selftest_diag_stage = __LINE__;
        if (vfs_kernel_unlink(&fs.mount, "/persist/config/sub/tree.bin") !=
                PROS_VFS_OK ||
            vfs_kernel_rmdir(&fs.mount, "/persist/config/sub") != PROS_VFS_OK ||
            vfs_kernel_rmdir(&fs.mount, "/persist/config") != PROS_VFS_OK ||
            vfs_kernel_rmdir(&fs.mount, "/persist") != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);


        pfs_selftest_diag_stage = __LINE__;
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

                if (vfs_kernel_file_open(&fs.mount, "/sizes.bin",
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
                if (vfs_kernel_file_open(&fs.mount, "/sizes.bin",
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
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -15);
            }
        }

        /* An allocated inode is a deliberate temporary reservation until
         * its caller writes the inode image. Do not run the global consistency
         * checker while that reservation is intentionally uninitialized. */
        if (prosenjitfs_alloc_blocks(&fs, 2u, &allocated_block) !=
                PROSENJITFS_OK ||
            prosenjitfs_alloc_inode(&fs, &allocated_inode) !=
                PROSENJITFS_OK ||
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
            pfs_inode_bitmap_update(&fs, allocated_inode, 0u) !=
                PROSENJITFS_OK ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
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
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_read_inode(&fs, allocated_inode, &inode) !=
                PROSENJITFS_ENOENT)
            return pfs_selftest_fail(__LINE__, -15);

        /*
         * The legacy allocator helper must reject invalid block sizes before
         * deriving bits_per_block, avoiding a divide-by-zero on corrupt state.
         */
        {
            struct pros_prosenjitfs invalid_fs = fs;
            uint64_t candidate = UINT64_MAX;
            uint32_t durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            uint32_t volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            uint32_t dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            /*
             * Every allocator entry point must reject zero, sub-sector, and
             * non-power-of-two geometry before division or bitmap indexing.
             */
            {
                const uint32_t invalid_sizes[] = {0u, 511u, 513u, 4097u, 8192u};
                uint8_t bitmap_probe[512];
                for (uint32_t bi = 0u; bi < sizeof(bitmap_probe); ++bi)
                    bitmap_probe[bi] = 0xA5u;
                for (uint32_t i = 0u;
                     i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++i) {
                    uint64_t block_lba = UINT64_MAX;
                    uint64_t inode_candidate = UINT64_MAX;
                    uint32_t offset = UINT32_MAX;
                    uint32_t bitmap_before = prosenjitfs_crc32c(
                        bitmap_probe, (uint32_t)sizeof(bitmap_probe));
                    invalid_fs.sb.block_size = invalid_sizes[i];
                    if (pfs_bitmap_region_build(
                            bitmap_probe, sizeof(bitmap_probe), 0u, 1u, 0u,
                            invalid_sizes[i]) != PROSENJITFS_EINVAL ||
                        bitmap_before != prosenjitfs_crc32c(
                            bitmap_probe, (uint32_t)sizeof(bitmap_probe)) ||
                        pfs_find_free_inode(&invalid_fs, &candidate) !=
                            PROSENJITFS_EINVAL || candidate != UINT64_MAX ||
                        prosenjitfs_alloc_inode(&invalid_fs, &inode_candidate) !=
                            PROSENJITFS_EINVAL || inode_candidate != UINT64_MAX ||
                        pfs_inode_bitmap_stage_unchecked(&invalid_fs, 1u, 1u) !=
                            PROSENJITFS_EINVAL ||
                        pfs_allocator_stage_unchecked(
                            &invalid_fs, invalid_fs.sb.data_start, 1u, 1u) !=
                            PROSENJITFS_EINVAL ||
                        pfs_allocator_range_state(
                            &invalid_fs, invalid_fs.sb.data_start, 1u, 0u) !=
                            PROSENJITFS_EINVAL ||
                        prosenjitfs_check_integrity_report(
                            &invalid_fs, &integrity_report) !=
                            PROSENJITFS_ECORRUPT ||
                        prosenjitfs_free_blocks(&invalid_fs,
                            invalid_fs.sb.data_start, 1u) != PROSENJITFS_EINVAL ||
                        prosenjitfs_preallocate_blocks(
                            &invalid_fs, invalid_fs.sb.root_inode, 0u, 1u) !=
                            PROSENJITFS_EINVAL ||
                        pfs_find_free_data_block(&invalid_fs, &candidate) !=
                            PROSENJITFS_EINVAL || candidate != UINT64_MAX ||
                        pfs_find_free_data_block_except(
                            &invalid_fs, UINT64_MAX, &candidate) !=
                            PROSENJITFS_EINVAL || candidate != UINT64_MAX ||
                        pfs_find_free_data_block_excluding(
                            &invalid_fs, UINT64_MAX, &candidate) !=
                            PROSENJITFS_EINVAL || candidate != UINT64_MAX ||
                        pfs_inode_block_location(
                            &invalid_fs, 0u, &block_lba, &offset) !=
                            PROSENJITFS_EINVAL ||
                        block_lba != UINT64_MAX || offset != UINT32_MAX ||
                        pfs_data_block_is_free(
                            &invalid_fs, invalid_fs.sb.data_start) !=
                            PROSENJITFS_EINVAL ||
                        prosenjitfs_read_inode(&invalid_fs, 0u, &inode) !=
                            PROSENJITFS_EINVAL ||
                        prosenjitfs_write_inode(&invalid_fs, 0u, &inode) !=
                            PROSENJITFS_EINVAL)
                        return pfs_selftest_fail(__LINE__, -27);
                }
            }
            if (durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)))
                return pfs_selftest_fail(__LINE__, -27);
        }

        pfs_selftest_diag_stage = __LINE__;
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
                pfs_lba_add(&device, 1u, UINT64_MAX, &lba) !=
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

        /* CRC-valid inode records must still obey the v1 semantic contract.
         * Inject each malformed root with write_inode, reject it via
         * read_inode, and restore the valid root before checking the result. */
        {
            struct pros_prosenjitfs_inode valid_root;
            struct pros_prosenjitfs_inode invalid_root;
            struct pros_prosenjitfs_inode readback;
            uint64_t capacity_blocks;
            int write_rc, read_rc, restore_rc;
            if (prosenjitfs_read_inode(&fs, fs.sb.root_inode, &valid_root) !=
                PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -170);
            if (fs.sb.total_blocks < fs.sb.data_start)
                return pfs_selftest_fail(__LINE__, -170);
            capacity_blocks = fs.sb.total_blocks - fs.sb.data_start;
            if (capacity_blocks >= UINT64_MAX / fs.sb.block_size)
                return pfs_selftest_fail(__LINE__, -170);
            for (uint32_t invalid_case = 0u; invalid_case < 7u; ++invalid_case) {
                invalid_root = valid_root;
                switch (invalid_case) {
                case 0u: invalid_root.type = 0x7Fu; break;
                case 1u: invalid_root.flags = (uint8_t)(1u << 1); break;
                case 2u: invalid_root.generation = 0u; break;
                case 3u: invalid_root.parent_inode = fs.sb.inode_count; break;
                case 4u: invalid_root.size = UINT64_MAX; break;
                case 5u: invalid_root.allocated_bytes = fs.sb.block_size + 1u; break;
                default:
                    invalid_root.allocated_bytes = (capacity_blocks + 1u) * fs.sb.block_size;
                    break;
                }
                write_rc = prosenjitfs_write_inode(&fs, fs.sb.root_inode, &invalid_root);
                read_rc = (write_rc == PROSENJITFS_OK)
                        ? prosenjitfs_read_inode(&fs, fs.sb.root_inode, &readback)
                        : write_rc;
                /* Always attempt restoration, even if the injected write failed. */
                restore_rc = prosenjitfs_write_inode(&fs, fs.sb.root_inode, &valid_root);
                if (write_rc != PROSENJITFS_OK || restore_rc != PROSENJITFS_OK ||
                    read_rc != PROSENJITFS_ECORRUPT)
                    return pfs_selftest_fail(__LINE__, -170);
            }
            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -170);
        }
        /* Use a dedicated fixture: /hello.bin was deliberately unlinked
         * earlier, and other persistence tests may remount or reset paths. */
        if (vfs_kernel_file_open(&fs.mount, "/truncate-overflow.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_truncate(&file, UINT64_MAX) != PROS_VFS_EOVERFLOW ||
            vfs_file_truncate(&file, 0x100000000ULL) != PROS_VFS_EOVERFLOW ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -15);

        pfs_selftest_diag_stage = __LINE__;
        /*
         * Bounded-truncate regression: eight independent extents exceed the
         * v1 transaction budget. The operation must fail before changing EOF
         * or allocation metadata.
         */
        {
            struct pros_vfs_file file;
            uint8_t marker_byte = 0xE1u;
            uint32_t done = 0u;
            uint64_t before_size;
            struct pros_prosenjitfs_inode before_inode;
            struct pfs_vnode_data *file_data;
            uint64_t inode_number;

            if (vfs_kernel_file_open(&fs.mount, "/truncate-bound.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -15);
            for (uint32_t n = 0u; n < 8u; ++n) {
                /* Leave a logical hole between writes so each byte owns
                 * a distinct extent; adjacent blocks may coalesce. */
                if (vfs_file_seek(&file, (uint64_t)n * 2u * fs.sb.block_size) !=
                        PROS_VFS_OK ||
                    vfs_file_write(&file, &marker_byte, 1u, &done) !=
                        PROS_VFS_OK ||
                    done != 1u)
                    return pfs_selftest_fail(__LINE__, -15);
            }
            if (vfs_file_close(&file) != PROS_VFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/truncate-bound.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -15);
            file_data = (struct pfs_vnode_data *)file.vnode->private_data;
            if (!file_data)
                return pfs_selftest_fail(__LINE__, -15);
            inode_number = file_data->inode_number;
            if (prosenjitfs_read_inode(&fs, inode_number,
                                       &before_inode) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -15);
            before_size = before_inode.size;
            if (vfs_file_truncate(&file, 0u) != PROS_VFS_EOVERFLOW ||
                vfs_file_close(&file) != PROS_VFS_OK ||
                prosenjitfs_read_inode(&fs, file_data->inode_number,
                                       &before_inode) != PROSENJITFS_OK ||
                before_inode.size != before_size ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                vfs_kernel_unlink(&fs.mount, "/truncate-bound.bin") != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -15);
        }

        pfs_selftest_diag_stage = __LINE__;
        /*
         * Build a locally consistent but root-disconnected directory cycle.
         * Normal VFS mutations correctly forbid this state, so repurpose the
         * just-created root->A entry block as B->A, remove A from the root
         * index, retain A->B, and update both checksummed parent pointers.
         * This keeps directory references, checksums, and block ownership
         * valid until the parent-chain reachability check runs.
         */
        {
            struct pros_vfs_vnode *a_node = 0;
            struct pros_vfs_vnode *b_node = 0;
            struct pfs_vnode_data *a_data;
            struct pfs_vnode_data *b_data;
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_inode a_inode;
            struct pros_prosenjitfs_inode b_inode;
            struct pros_prosenjitfs_dir_index root_index;
            struct pros_prosenjitfs_dir_index b_index;
            struct pros_prosenjitfs_dir_entry root_entry;
            uint8_t root_index_block[4096];
            uint8_t b_index_block[4096];
            uint8_t entry_block[4096];
            uint64_t a_number;
            uint64_t b_number;
            uint64_t entry_lba = 0u;
            uint64_t previous_lba = 0u;
            uint32_t root_bucket;
            uint32_t cycle_bucket;

            if (vfs_kernel_mkdir(&fs.mount, "/parent-cycle-a") != PROS_VFS_OK ||
                vfs_kernel_mkdir(&fs.mount, "/parent-cycle-a/b") != PROS_VFS_OK ||
                vfs_kernel_path_lookup(&fs.mount, "/parent-cycle-a", &a_node) !=
                    PROS_VFS_OK ||
                vfs_kernel_path_lookup(&fs.mount, "/parent-cycle-a/b", &b_node) !=
                    PROS_VFS_OK) {
                if (a_node)
                    vfs_vnode_put(a_node);
                if (b_node)
                    vfs_vnode_put(b_node);
                return pfs_selftest_fail(__LINE__, -161);
            }
            a_data = (struct pfs_vnode_data *)a_node->private_data;
            b_data = (struct pfs_vnode_data *)b_node->private_data;
            if (!a_data || !b_data) {
                vfs_vnode_put(a_node);
                vfs_vnode_put(b_node);
                return pfs_selftest_fail(__LINE__, -161);
            }
            a_number = a_data->inode_number;
            b_number = b_data->inode_number;
            if (vfs_vnode_put(a_node) != PROS_VFS_OK ||
                vfs_vnode_put(b_node) != PROS_VFS_OK ||
                prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, a_number, &a_inode) !=
                    PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, b_number, &b_inode) !=
                    PROSENJITFS_OK ||
                a_inode.type != PROSENJITFS_INODE_DIR ||
                b_inode.type != PROSENJITFS_INODE_DIR ||
                pfs_read_dir_index(&fs, &root_inode, fs.sb.root_inode, &root_index) !=
                    PROSENJITFS_OK ||
                pfs_read_dir_index(&fs, &b_inode, b_number, &b_index) !=
                    PROSENJITFS_OK ||
                pfs_find_dir_entry_lba(&fs, &root_inode, fs.sb.root_inode, "parent-cycle-a",
                                       &root_entry, &entry_lba,
                                       &previous_lba) != PROSENJITFS_OK ||
                previous_lba != 0u ||
                pfs_read_block(&device, root_inode.dir_index,
                               root_index_block) != PROSENJITFS_OK ||
                pfs_read_block(&device, b_inode.dir_index,
                               b_index_block) != PROSENJITFS_OK ||
                pfs_read_block(&device, entry_lba, entry_block) !=
                    PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -161);

            root_bucket = root_entry.name_hash % PROSENJITFS_DIR_BUCKETS;
            cycle_bucket = pfs_name_hash((const uint8_t *)"a", 1u) %
                           PROSENJITFS_DIR_BUCKETS;
            if (root_index.heads[root_bucket] != entry_lba ||
                b_index.heads[cycle_bucket] != 0u ||
                root_entry.inode != a_number ||
                root_entry.type != PROSENJITFS_INODE_DIR ||
                root_entry.next >= fs.sb.total_blocks)
                return pfs_selftest_fail(__LINE__, -161);

            /* Detach A from root while preserving the rest of that bucket. */
            root_index.heads[root_bucket] = root_entry.next;
            pfs_copy(root_index_block, &root_index, sizeof(root_index));
            pfs_put32(root_index_block +
                          offsetof(struct pros_prosenjitfs_dir_index, checksum),
                      pfs_crc_with_zero32(
                          root_index_block, sizeof(root_index),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_dir_index, checksum)));

            /* Reuse the detached entry block as B -> A, preserving ownership. */
            pfs_zero(root_entry.name, sizeof(root_entry.name));
            root_entry.name[0] = (uint8_t)'a';
            root_entry.name[1] = 0u;
            root_entry.name_length = 1u;
            root_entry.name_hash = pfs_name_hash(
                (const uint8_t *)root_entry.name, root_entry.name_length);
            root_entry.inode = a_number;
            root_entry.type = PROSENJITFS_INODE_DIR;
            root_entry.next = b_index.heads[cycle_bucket];
            root_entry.checksum = 0u;
            b_index.heads[cycle_bucket] = entry_lba;
            pfs_copy(b_index_block, &b_index, sizeof(b_index));
            pfs_put32(b_index_block +
                          offsetof(struct pros_prosenjitfs_dir_index, checksum),
                      pfs_crc_with_zero32(
                          b_index_block, sizeof(b_index),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_dir_index, checksum)));
            pfs_copy(entry_block, &root_entry, sizeof(root_entry));
            pfs_put32(entry_block +
                          offsetof(struct pros_prosenjitfs_dir_entry, checksum),
                      pfs_crc_with_zero32(
                          entry_block, sizeof(root_entry),
                          (uint32_t)offsetof(
                              struct pros_prosenjitfs_dir_entry, checksum)));

            if (pfs_tx_reset(&fs) != PROSENJITFS_OK ||
                pfs_tx_add(&fs, root_inode.dir_index, root_index_block) !=
                    PROSENJITFS_OK ||
                pfs_tx_add(&fs, b_inode.dir_index, b_index_block) !=
                    PROSENJITFS_OK ||
                pfs_tx_add(&fs, entry_lba, entry_block) != PROSENJITFS_OK ||
                pfs_tx_commit(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -161);

            a_inode.parent_inode = b_number;
            b_inode.parent_inode = a_number;
            if (prosenjitfs_write_inode(&fs, a_number, &a_inode) !=
                    PROSENJITFS_OK ||
                prosenjitfs_write_inode(&fs, b_number, &b_inode) !=
                    PROSENJITFS_OK ||
                pfs_parent_chain_reaches_root(&fs, a_number) !=
                    PROSENJITFS_ECORRUPT ||
                prosenjitfs_check_integrity(&fs) == PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -161);

            /* Reset the mock medium so later corruption tests start clean. */
            if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -161);
            pfs_zero(&disk, sizeof(disk));
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -161);
        }

        pfs_selftest_diag_stage = __LINE__;
        /*
         * Semantic corruption: preserve the inode checksum while replacing
         * its extent head with an impossible filesystem block. Mount can
         * remain structurally valid, but the first data access must fail
         * closed rather than touching the device out of range.
         */
        if (vfs_kernel_file_open(&fs.mount, "/corrupt.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, small_data, sizeof(small_data), &done) !=
                PROS_VFS_OK ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            vfs_kernel_path_lookup(&fs.mount, "/corrupt.bin", &node) !=
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
        if (vfs_kernel_file_open(&fs.mount, "/corrupt.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, data, sizeof(small_data), &done) !=
                PROS_VFS_ECORRUPT ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -16);

        integrity_durable_crc = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        integrity_volatile_crc = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        integrity_dirty_crc = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));
        if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_ECORRUPT ||
            integrity_report.status != PROSENJITFS_ECORRUPT ||
            integrity_report.first_error_location == 0u ||
            integrity_report.inodes_checked == 0u ||
            integrity_durable_crc != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            integrity_volatile_crc != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            integrity_dirty_crc != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty)))
            return pfs_selftest_fail(__LINE__, -16);

        /*
         * Restore a clean image before the journal/crash matrix. The
         * corruption fixture is still mounted, so deactivate it before
         * replacing the backing medium and mounting the clean image.
         */
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -16);
        pfs_zero(&disk, sizeof(disk));
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -16);
    }

    pfs_selftest_diag_stage = __LINE__;
    /*
     * Tiny-image allocation exhaustion.  This uses the real allocator and
     * journal on a minimum-size filesystem, then verifies recovery/reuse.
     */
    {
        /* The mock medium is 512 KiB; keep it in static storage rather than
         * overflowing the 128 KiB bootstrap kernel stack during self-test. */
        static struct pfs_test_disk small_disk;
        struct pros_block_device small_device;
        struct pros_prosenjitfs small_fs;
        uint64_t block;
        uint64_t first_reused = 0u;
        uint32_t allocated = 0u;
        uint64_t inode_tmp;

        pfs_zero(&small_disk, sizeof(small_disk));
        /* prosenjitfs_mount() checks mount.active before clearing fs. */
        pfs_zero(&small_fs, sizeof(small_fs));
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
            uint64_t candidate = 0u;
            int rc = prosenjitfs_alloc_blocks(&small_fs, 1u, &candidate);
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
            /* Failed allocations clear their output; retain only success. */
            block = candidate;
        }
        /*
         * These blocks are deliberately allocated directly, without inode
         * extents owning them. The global consistency checker correctly flags
         * such reservations as unowned, so validate allocator exhaustion via
         * the authoritative free-block counter instead.
         */
        if (allocated == 0u)
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
            /*
             * This loop reserves inode numbers directly without linking
             * them into the directory tree. The consistency checker should
             * reject those deliberately unowned inodes, so keep this test
             * focused on allocator exhaustion only.
             */
            if (inode_allocated < small_fs.sb.inode_count - 1u) {
                prosenjitfs_unmount(&small_fs);
                return pfs_selftest_fail(__LINE__, -17);
            }
        }

        if (prosenjitfs_unmount(&small_fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -17);

        /*
         * The exhaustion checks deliberately reserve blocks and inodes
         * without filesystem ownership. Reformat the medium before the
         * independent partition-boundary and clean-remount checks.
         */
        pfs_zero(&small_disk, sizeof(small_disk));
        if (prosenjitfs_format(&small_device) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -17);

        pfs_selftest_diag_stage = __LINE__;
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
            prosenjitfs_check_integrity(&small_fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&small_fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -17);
    }

    pfs_selftest_diag_stage = __LINE__;
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

        /*
         * A valid commit marker with a matching transaction ID but a
         * contradictory record count must fail closed, even with a checksum
         * that matches the bytes on disk. A stale transaction ID is covered
         * by the torn-transaction case above because commit slots are reused.
         */
        pfs_zero(header, sizeof(header));
        pfs_zero(commit, sizeof(commit));
        pfs_zero(&h, sizeof(h));
        h.magic = PROSENJITFS_JOURNAL_MAGIC;
        h.record_count = 1u;
        h.transaction_id = 0xAABBCCDD12344321ULL;
        h.targets[0] = recovery_block;
        pfs_copy(header, &h, sizeof(h));
        h.checksum = pfs_crc_with_zero32(
            header, sizeof(header),
            (uint32_t)offsetof(
                struct pros_prosenjitfs_journal_header, checksum));
        pfs_copy(header, &h, sizeof(h));

        pfs_zero(&jc, sizeof(jc));
        jc.magic = PROSENJITFS_JOURNAL_COMMIT;
        jc.transaction_id = h.transaction_id;
        jc.record_count = 2u;
        pfs_copy(commit, &jc, sizeof(jc));
        jc.checksum = pfs_journal_checksum(
            header, image, 512u, h.record_count, commit);
        pfs_copy(commit, &jc, sizeof(jc));

        if (pfs_write_block(&device, journal_start, header) !=
                PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 1u, image) !=
                PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 2u, commit) !=
                PROSENJITFS_OK ||
            pfs_flush(&device) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_ECORRUPT)
            return pfs_selftest_fail(__LINE__, -12);

        pfs_zero(header, sizeof(header));
        for (uint32_t i = 0u; i < journal_blocks; ++i)
            if (pfs_write_block(&device, journal_start + i, header) !=
                PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -12);
        if (pfs_flush(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            pfs_read_block(&device, recovery_block, verify) !=
                PROSENJITFS_OK)
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

    pfs_selftest_diag_stage = __LINE__;
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

            if (vfs_kernel_file_open(&fs.mount, "/crash.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                              &file) != PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            /*
             * Simulate a reboot before this overwrite, which resets the
             * mount-local transaction sequence. For the pre-commit-flush
             * case, seed every possible commit slot with a valid stale record
             * for transaction 1. The new transaction must invalidate its
             * selected slot before writing a durable header/images; otherwise
             * recovery could mistake the stale record for this transaction.
             */
            if (stage == 4u) {
                struct pros_prosenjitfs_journal_header stale_header;
                struct pros_prosenjitfs_journal_commit stale_commit;
                uint8_t *stale_header_block = fs.scratch;
                uint8_t *stale_images = fs.tx.images;
                uint8_t *stale_commit_block =
                    fs.tx.images + (uint64_t)PROSENJITFS_MAX_TX_BLOCKS *
                                    fs.sb.block_size;

                if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -13);

                for (uint32_t count = 1u;
                     count <= PROSENJITFS_MAX_TX_BLOCKS &&
                     (uint64_t)count + 2u <= fs.sb.journal_blocks;
                     ++count) {
                    uint64_t commit_lba;
                    pfs_zero(stale_header_block, fs.sb.block_size);
                    pfs_zero(stale_images,
                             PROSENJITFS_MAX_TX_BLOCKS * fs.sb.block_size);
                    pfs_zero(stale_commit_block, fs.sb.block_size);
                    pfs_zero(&stale_header, sizeof(stale_header));
                    stale_header.magic = PROSENJITFS_JOURNAL_MAGIC;
                    stale_header.record_count = count;
                    stale_header.transaction_id = 1u;
                    stale_header.targets[0] = fs.sb.data_start;
                    pfs_copy(stale_header_block, &stale_header,
                             sizeof(stale_header));
                    stale_header.checksum = pfs_crc_with_zero32(
                        stale_header_block, fs.sb.block_size,
                        (uint32_t)offsetof(
                            struct pros_prosenjitfs_journal_header, checksum));
                    pfs_copy(stale_header_block, &stale_header,
                             sizeof(stale_header));

                    pfs_zero(&stale_commit, sizeof(stale_commit));
                    stale_commit.magic = PROSENJITFS_JOURNAL_COMMIT;
                    stale_commit.record_count = count;
                    stale_commit.transaction_id = 1u;
                    pfs_copy(stale_commit_block, &stale_commit,
                             sizeof(stale_commit));
                    stale_commit.checksum = pfs_journal_checksum(
                        stale_header_block, stale_images, fs.sb.block_size,
                        count, stale_commit_block);
                    pfs_copy(stale_commit_block, &stale_commit,
                             sizeof(stale_commit));
                    if (pfs_lba_add(&device, fs.sb.journal_start,
                                    1u + count, &commit_lba) !=
                            PROSENJITFS_OK ||
                        pfs_write_block(&device, commit_lba,
                                        stale_commit_block) !=
                            PROSENJITFS_OK)
                        return pfs_selftest_fail(__LINE__, -13);
                }
                if (pfs_flush(&device) != PROSENJITFS_OK ||
                    fs.sb.transaction_sequence != 1u)
                    return pfs_selftest_fail(__LINE__, -13);
            }

            pfs_crash_inject_stage = stage;
            pfs_crash_injected = 0u;
            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/crash.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            write_rc = vfs_file_write(&file, new_data, sizeof(new_data),
                                      &done);
            if (vfs_file_close(&file) != PROS_VFS_OK ||
                write_rc != PROS_VFS_EIO ||
                done != 0u ||
                pfs_crash_injected == 0u ||
                (stage >= 5u &&
                 (!fs.read_only || !fs.mount.read_only)) ||
                (stage < 5u &&
                 (fs.read_only || fs.mount.read_only))) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }

            /*
             * Once the commit write may have reached durable media, reject
             * further writes on this mount until remount recovery. Verify
             * that a rejected retry leaves every mock-media view unchanged.
             */
            if (stage >= 5u) {
                uint32_t durable_crc = prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data));
                uint32_t volatile_crc = prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
                uint32_t dirty_crc = prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty));
                int retry_rc = vfs_kernel_file_open(
                    &fs.mount, "/crash.bin", PROS_VFS_O_WRITE, &file);

                done = 0u;
                if (retry_rc == PROS_VFS_OK) {
                    retry_rc = vfs_file_write(
                        &file, old_data, sizeof(old_data), &done);
                    if (vfs_file_close(&file) != PROS_VFS_OK) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -13);
                    }
                }
                if (retry_rc == PROS_VFS_OK || done != 0u ||
                    durable_crc != prosenjitfs_crc32c(
                        disk.data, (uint32_t)sizeof(disk.data)) ||
                    volatile_crc != prosenjitfs_crc32c(
                        disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                    dirty_crc != prosenjitfs_crc32c(
                        disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -13);
                }
            }

            pfs_crash_inject_stage = 0u;
            pfs_test_crash_reset(&fs, &disk);

            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/crash.bin",
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
     * Recovery itself must be restartable. Interrupt mount recovery after one
     * home-block replay (stage 10) and after the home-block flush (stage 11),
     * then power-cycle and recover again. Both cases must converge to the
     * fully committed new file contents without requiring metadata repair.
     */
    {
        static const uint32_t recovery_stages[] = { 10u, 11u };
        struct pros_vfs_file file;
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];

        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x42u ^ (uint8_t)(i * 3u));
            new_data[i] = (uint8_t)(0xB1u ^ (uint8_t)(i * 19u));
        }

        for (uint32_t stage_index = 0u;
             stage_index < sizeof(recovery_stages) /
                           sizeof(recovery_stages[0]);
             ++stage_index) {
            uint32_t recovery_stage = recovery_stages[stage_index];
            uint32_t done = 0u;
            int write_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/recovery-restart.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);

            /* Stage 5 is after the commit record is durably flushed. */
            pfs_crash_inject_stage = 5u;
            pfs_crash_injected = 0u;
            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/recovery-restart.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -13);
            write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
            if (vfs_file_close(&file) != PROS_VFS_OK ||
                write_rc != PROS_VFS_EIO || done != 0u ||
                pfs_crash_injected == 0u ||
                !fs.read_only || !fs.mount.read_only)
                return pfs_selftest_fail(__LINE__, -13);

            pfs_crash_inject_stage = recovery_stage;
            pfs_crash_injected = 0u;
            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) == PROSENJITFS_OK ||
                pfs_crash_injected == 0u) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }

            /* Recovery was interrupted; a second power loss must be safe. */
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/recovery-restart.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(read_data) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
            for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
                if (read_data[i] != new_data[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -13);
                }
            }
            if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -13);
        }
    }

    /*
     * Unlink must preserve device I/O errors while reading a directory entry.
     * The third read in this root-directory path is the selected entry block;
     * a failed read must not be mislabeled as metadata corruption or mutate
     * any durable, volatile, or dirty mock-media state.
     */
    {
        struct pros_vfs_file file;
        uint8_t data[64];
        uint8_t read_data[64];
        uint32_t done = 0u;
        uint32_t durable_before, volatile_before, dirty_before;
        int unlink_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(data); ++i)
            data[i] = (uint8_t)(0x5Cu ^ (uint8_t)(i * 29u));
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/unlink-read-error.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, data, sizeof(data), &done) != PROS_VFS_OK ||
            done != sizeof(data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));

        disk.fail_read_countdown = 3u;
        unlink_rc = vfs_kernel_unlink(&fs.mount, "/unlink-read-error.bin");
        if (unlink_rc != PROS_VFS_EIO ||
            disk.fail_read_countdown != 0u ||
            fs.read_only || fs.mount.read_only ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty))) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/unlink-read-error.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }
        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            if (read_data[i] != data[i]) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        }
        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * Sweep read failures across unlink's path lookup and directory-entry
     * resolution. Every reached read failure must propagate as I/O error and
     * leave the namespace/media untouched; the first not-reached position
     * proves the complete read sequence was covered.
     */
    {
        struct pros_vfs_file file;
        uint8_t payload[64];
        uint8_t readback[64];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(payload); ++i)
            payload[i] = (uint8_t)(0x3Bu ^ (uint8_t)(i * 23u));

        for (uint32_t read_position = 1u; read_position <= 64u;
             ++read_position) {
            uint32_t done = 0u;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;
            int unlink_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/unlink-read-sweep.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, payload, sizeof(payload), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(payload) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -23);

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            disk.fail_read_countdown = read_position;
            unlink_rc = vfs_kernel_unlink(&fs.mount, "/unlink-read-sweep.bin");
            if (disk.fail_read_countdown != 0u) {
                /*
                 * Unlink completed before the selected read position. Do not
                 * let integrity/unmount consume this operation-scoped fault.
                 */
                disk.fail_read_countdown = 0u;
                if (unlink_rc != PROS_VFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -23);
                }
                {
                    int lookup_rc = vfs_kernel_file_open(
                        &fs.mount, "/unlink-read-sweep.bin",
                        PROS_VFS_O_READ, &file);
                    if (lookup_rc == PROS_VFS_OK) {
                        (void)vfs_file_close(&file);
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -23);
                    }
                    if (lookup_rc != PROS_VFS_ENOENT) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -23);
                    }
                }
                if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -23);
                }
                sweep_finished = 1u;
                break;
            }

            if (unlink_rc != PROS_VFS_EIO ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data,
                    (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)) ||
                fs.read_only || fs.mount.read_only) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -23);
            }

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/unlink-read-sweep.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, readback, sizeof(readback), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(readback) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -23);
            }
            for (uint32_t i = 0u; i < sizeof(readback); ++i) {
                if (readback[i] != payload[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -23);
                }
            }
            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -23);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -23);
    }

    /*
     * Sweep block-write failures across unlink's transaction. A failure may
     * occur before commit or after the durable commit record, so remount may
     * validly expose the original file or the completed unlink; it must never
     * expose corrupt metadata or altered payload bytes.
     */
    {
        struct pros_vfs_file file;
        uint8_t payload[64];
        uint8_t readback[64];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(payload); ++i)
            payload[i] = (uint8_t)(0xE1u ^ (uint8_t)(i * 13u));

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            uint32_t done = 0u;
            int unlink_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/unlink-write-sweep.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, payload, sizeof(payload), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(payload) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -24);

            disk.fail_write_countdown = write_position;
            unlink_rc = vfs_kernel_unlink(&fs.mount, "/unlink-write-sweep.bin");
            if (disk.fail_write_countdown != 0u) {
                /* No write at this position occurred during unlink. */
                disk.fail_write_countdown = 0u;
                if (unlink_rc != PROS_VFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -24);
                }
                {
                    int lookup_rc = vfs_kernel_file_open(
                        &fs.mount, "/unlink-write-sweep.bin",
                        PROS_VFS_O_READ, &file);
                    if (lookup_rc == PROS_VFS_OK) {
                        (void)vfs_file_close(&file);
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -24);
                    }
                    if (lookup_rc != PROS_VFS_ENOENT) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -24);
                    }
                }
                if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -24);
                }
                sweep_finished = 1u;
                break;
            }

            if (unlink_rc == PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -24);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -24);
            }

            {
                int lookup_rc = vfs_kernel_file_open(
                    &fs.mount, "/unlink-write-sweep.bin",
                    PROS_VFS_O_READ, &file);
                if (lookup_rc == PROS_VFS_OK) {
                    done = 0u;
                    if (vfs_file_read(&file, readback, sizeof(readback),
                                      &done) != PROS_VFS_OK ||
                        done != sizeof(readback) ||
                        vfs_file_close(&file) != PROS_VFS_OK) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -24);
                    }
                    for (uint32_t i = 0u; i < sizeof(readback); ++i) {
                        if (readback[i] != payload[i]) {
                            pfs_test_crash_reset(&fs, &disk);
                            return pfs_selftest_fail(__LINE__, -24);
                        }
                    }
                } else if (lookup_rc != PROS_VFS_ENOENT) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -24);
                }
            }

            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -24);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -24);
    }

    /*
     * Sweep read failures during append-at-EOF. A metadata/data read fault
     * must not change the existing file or any mock-media view. The first
     * countdown not reached proves the operation's read sequence is covered.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[64];
        uint8_t append_data[64];
        uint8_t read_data[128];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x47u ^ (uint8_t)(i * 9u));
            append_data[i] = (uint8_t)(0xA5u + (uint8_t)(i * 17u));
        }

        for (uint32_t read_position = 1u; read_position <= 64u;
             ++read_position) {
            uint32_t done = 0u;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;
            int operation_rc = PROS_VFS_OK;
            int open_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/append-read-sweep.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -27);

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            done = 0u;
            disk.fail_read_countdown = read_position;
            open_rc = vfs_kernel_file_open(&fs.mount, "/append-read-sweep.bin",
                                    PROS_VFS_O_WRITE, &file);
            if (open_rc != PROS_VFS_OK) {
                operation_rc = open_rc;
            } else {
                operation_rc = vfs_file_seek(&file, sizeof(old_data));
                if (operation_rc == PROS_VFS_OK)
                    operation_rc = vfs_file_write(
                        &file, append_data, sizeof(append_data), &done);
                if (vfs_file_close(&file) != PROS_VFS_OK &&
                    operation_rc == PROS_VFS_OK)
                    operation_rc = PROS_VFS_EIO;
            }

            if (disk.fail_read_countdown != 0u) {
                /* Do not let an append-specific fault leak into verification. */
                disk.fail_read_countdown = 0u;
                if (operation_rc != PROS_VFS_OK ||
                    done != sizeof(append_data)) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -27);
                }

                done = 0u;
                if (vfs_kernel_file_open(&fs.mount, "/append-read-sweep.bin",
                                  PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                    vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                        PROS_VFS_OK ||
                    done != sizeof(read_data) ||
                    vfs_file_close(&file) != PROS_VFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -27);
                }
                for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
                    if (read_data[i] != old_data[i]) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -27);
                    }
                }
                for (uint32_t i = 0u; i < sizeof(append_data); ++i) {
                    if (read_data[sizeof(old_data) + i] != append_data[i]) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -27);
                    }
                }
                if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -27);
                }
                sweep_finished = 1u;
                break;
            }

            if (operation_rc != PROS_VFS_EIO) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -271);
            }
            if (durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data))) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -272);
            }
            if (volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data))) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -273);
            }
            if (dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -274);
            }
            if (fs.read_only) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -275);
            }
            if (fs.mount.read_only) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -276);
            }

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/append-read-sweep.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -27);
            }
            for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
                if (read_data[i] != old_data[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -27);
                }
            }
            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -27);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -27);
    }

    /*
     * Sweep append-at-EOF write failures. After simulated power loss, the
     * file must retain its original payload or expose the entire appended
     * payload; partial logical append and changed original bytes are invalid.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[64];
        uint8_t append_data[64];
        uint8_t read_data[128];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x39u + (uint8_t)(i * 7u));
            append_data[i] = (uint8_t)(0xD1u ^ (uint8_t)(i * 13u));
        }

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            uint32_t done = 0u;
            int write_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/append-write-sweep.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -26);

            disk.fail_write_countdown = write_position;
            if (vfs_kernel_file_open(&fs.mount, "/append-write-sweep.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
                vfs_file_seek(&file, sizeof(old_data)) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }
            write_rc = vfs_file_write(&file, append_data,
                                      sizeof(append_data), &done);
            if (vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }

            if (disk.fail_write_countdown != 0u) {
                disk.fail_write_countdown = 0u;
                if (write_rc != PROS_VFS_OK || done != sizeof(append_data) ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -26);
                }
                sweep_finished = 1u;
                break;
            }

            if (write_rc == PROS_VFS_OK && done == sizeof(append_data)) {
                /* A reached injected failure cannot report a full success. */
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/append-write-sweep.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }

            if (done != sizeof(old_data) &&
                done != sizeof(old_data) + sizeof(append_data)) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }
            for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
                if (read_data[i] != old_data[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -26);
                }
            }
            if (done == sizeof(old_data) + sizeof(append_data)) {
                for (uint32_t i = 0u; i < sizeof(append_data); ++i) {
                    if (read_data[sizeof(old_data) + i] != append_data[i]) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -26);
                    }
                }
            }

            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -26);
    }

    /*
     * Sweep read failures across truncate's complete operation, including
     * opening the file. A reached read fault must leave the old file and all
     * mock-media views unchanged; a not-reached fault is disarmed before the
     * successful truncate's verification reads.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[64];
        uint8_t read_data[64];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(old_data); ++i)
            old_data[i] = (uint8_t)(0x6Du + (uint8_t)(i * 19u));

        for (uint32_t read_position = 1u; read_position <= 64u;
             ++read_position) {
            uint32_t done = 0u;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;
            int open_rc;
            int truncate_rc = PROS_VFS_OK;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/truncate-read-sweep.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -28);

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            disk.fail_read_countdown = read_position;
            open_rc = vfs_kernel_file_open(&fs.mount, "/truncate-read-sweep.bin",
                                    PROS_VFS_O_WRITE, &file);
            if (open_rc != PROS_VFS_OK) {
                truncate_rc = open_rc;
            } else {
                truncate_rc = vfs_file_truncate(&file, 0u);
                if (vfs_file_close(&file) != PROS_VFS_OK &&
                    truncate_rc == PROS_VFS_OK)
                    truncate_rc = PROS_VFS_EIO;
            }

            if (disk.fail_read_countdown != 0u) {
                /* Truncate completed before this read position was reached. */
                disk.fail_read_countdown = 0u;
                if (truncate_rc != PROS_VFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -28);
                }
                done = 0u;
                if (vfs_kernel_file_open(&fs.mount, "/truncate-read-sweep.bin",
                                  PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                    vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                        PROS_VFS_OK ||
                    done != 0u ||
                    vfs_file_close(&file) != PROS_VFS_OK ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -28);
                }
                sweep_finished = 1u;
                break;
            }

            if (truncate_rc != PROS_VFS_EIO ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)) ||
                fs.read_only || fs.mount.read_only) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/truncate-read-sweep.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }
            for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
                if (read_data[i] != old_data[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -28);
                }
            }
            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -28);
    }

    /*
     * Sweep block-write failures across truncate's complete transaction,
     * not only its commit-record write. After simulated power loss, the file
     * must be either the original complete payload or fully truncated; an
     * intermediate EOF, altered payload, or corrupt namespace is forbidden.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[64];
        uint8_t read_data[64];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(old_data); ++i)
            old_data[i] = (uint8_t)(0xB3u ^ (uint8_t)(i * 23u));

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            uint32_t done = 0u;
            int truncate_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/truncate-write-sweep.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -25);

            disk.fail_write_countdown = write_position;
            if (vfs_kernel_file_open(&fs.mount, "/truncate-write-sweep.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -25);
            }
            truncate_rc = vfs_file_truncate(&file, 0u);
            if (vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -25);
            }

            if (disk.fail_write_countdown != 0u) {
                /* Truncate completed before this write position was reached. */
                disk.fail_write_countdown = 0u;
                if (truncate_rc != PROS_VFS_OK ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -25);
                }
                sweep_finished = 1u;
                break;
            }

            if (truncate_rc == PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -25);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -25);
            }

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/truncate-write-sweep.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -25);
            }

            if (done == sizeof(old_data)) {
                for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
                    if (read_data[i] != old_data[i]) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -25);
                    }
                }
            } else if (done != 0u) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -25);
            }

            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -25);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -25);
    }

    /*
     * Sweep the directory lookup reads preceding unlink publication. A read
     * failure at any of these positions must preserve the namespace, file
     * bytes, writable state, and all mock-media views.
     */
    {
        uint8_t payload[64];
        uint8_t readback[64];
        uint32_t completed = 0u;

        for (uint32_t i = 0u; i < sizeof(payload); ++i)
            payload[i] = (uint8_t)(0xA7u ^ (uint8_t)(i * 11u));

        for (uint32_t read_position = 1u; read_position <= 3u;
             ++read_position) {
            struct pros_vfs_file file;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;
            uint32_t done = 0u;
            int unlink_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/unlink-read-sweep.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, payload, sizeof(payload), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(payload) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -24);
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            disk.fail_read_countdown = read_position;
            unlink_rc = vfs_kernel_unlink(&fs.mount, "/unlink-read-sweep.bin");
            if (unlink_rc != PROS_VFS_EIO ||
                disk.fail_read_countdown != 0u ||
                fs.read_only || fs.mount.read_only ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -24);
            }

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/unlink-read-sweep.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, readback, sizeof(readback), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(readback) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -24);
            }
            for (uint32_t i = 0u; i < sizeof(readback); ++i) {
                if (readback[i] != payload[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -24);
                }
            }
            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -24);
            }
            ++completed;
        }

        if (completed != 3u)
            return pfs_selftest_fail(__LINE__, -24);
    }


    /*
     * Truncation is a metadata transaction too. If its commit record cannot
     * be written, power-loss remount must retain the pre-truncate EOF, extent,
     * and bytes rather than exposing a partially applied shrink.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[64];
        uint8_t read_data[64];
        uint32_t done = 0u;
        int truncate_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i)
            old_data[i] = (uint8_t)(0xC7u ^ (uint8_t)(i * 31u));

        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/truncate-atomic.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_commit_write_once = 1u;
        if (vfs_kernel_file_open(&fs.mount, "/truncate-atomic.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }
        truncate_rc = vfs_file_truncate(&file, 0u);
        if (vfs_file_close(&file) != PROS_VFS_OK ||
            truncate_rc != PROS_VFS_EIO ||
            disk.fail_commit_write_once != 0u ||
            !fs.read_only || !fs.mount.read_only) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/truncate-atomic.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }
        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            if (read_data[i] != old_data[i]) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        }
        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * mkdir spans several metadata images (inode allocation, child inode,
     * directory entry/index). A failed journal commit must leave no reachable
     * half-created directory after a simulated power loss and remount.
     */
    {
        struct pros_vfs_dir directory;
        int mkdir_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_commit_write_once = 1u;
        mkdir_rc = vfs_kernel_mkdir(&fs.mount, "/mkdir-atomic-probe");
        if (mkdir_rc != PROS_VFS_EIO ||
            disk.fail_commit_write_once != 0u ||
            !fs.read_only || !fs.mount.read_only) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        {
            int directory_rc = vfs_kernel_dir_open(
                &fs.mount, "/mkdir-atomic-probe", &directory);
            if (directory_rc == PROS_VFS_OK) {
                (void)vfs_dir_close(&directory);
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
            if (directory_rc != PROS_VFS_ENOENT) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        }

        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * A failed rmdir commit must not remove the directory's namespace entry
     * or leave a partially freed child inode after simulated power loss.
     */
    {
        struct pros_vfs_dir directory;
        int rmdir_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_mkdir(&fs.mount, "/rmdir-atomic-probe") != PROS_VFS_OK ||
            vfs_kernel_dir_open(&fs.mount, "/rmdir-atomic-probe", &directory) !=
                PROS_VFS_OK ||
            vfs_dir_close(&directory) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_commit_write_once = 1u;
        rmdir_rc = vfs_kernel_rmdir(&fs.mount, "/rmdir-atomic-probe");
        if (rmdir_rc != PROS_VFS_EIO ||
            disk.fail_commit_write_once != 0u ||
            !fs.read_only || !fs.mount.read_only) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        if (vfs_kernel_dir_open(&fs.mount, "/rmdir-atomic-probe", &directory) !=
                PROS_VFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }
        if (vfs_dir_close(&directory) != PROS_VFS_OK ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * File creation must also be atomic at the namespace boundary. If the
     * journal commit record cannot be written, remount must not expose a
     * partially initialized inode or a directory entry for the new file.
     */
    {
        struct pros_vfs_file file;
        int create_rc;
        int lookup_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_commit_write_once = 1u;
        create_rc = vfs_kernel_file_open(
            &fs.mount, "/create-atomic-probe.bin",
            PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file);
        if (create_rc != PROS_VFS_EIO ||
            disk.fail_commit_write_once != 0u ||
            !fs.read_only || !fs.mount.read_only) {
            if (create_rc == PROS_VFS_OK)
                (void)vfs_file_close(&file);
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        lookup_rc = vfs_kernel_file_open(&fs.mount, "/create-atomic-probe.bin",
                                  PROS_VFS_O_READ, &file);
        if (lookup_rc == PROS_VFS_OK) {
            (void)vfs_file_close(&file);
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }
        if (lookup_rc != PROS_VFS_ENOENT ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * A failure before the journal header is published is retryable. Fail
     * the first transaction flush (commit-slot invalidation), verify the
     * mount remains writable and no bytes are reported written, then retry
     * successfully on the same mount and verify the new contents.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];
        uint32_t done = 0u;
        int write_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x24u ^ (uint8_t)(i * 5u));
            new_data[i] = (uint8_t)(0xC2u ^ (uint8_t)(i * 7u));
        }
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/precommit-flush.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_flush_countdown = 1u;
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/precommit-flush.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
        if (vfs_file_close(&file) != PROS_VFS_OK ||
            write_rc == PROS_VFS_OK || done != 0u ||
            disk.fail_flush_countdown != 0u ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/precommit-flush.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
            vfs_file_write(&file, new_data, sizeof(new_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(new_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/precommit-flush.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        for (uint32_t i = 0u; i < sizeof(read_data); ++i)
            if (read_data[i] != new_data[i])
                return pfs_selftest_fail(__LINE__, -13);
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * Failure while flushing the journal header/images (the second transaction
     * flush) occurs before the commit record is published. The mount should
     * remain writable, and a subsequent transaction should replace the
     * uncommitted journal contents and succeed.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];
        uint32_t done = 0u;
        int write_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x42u ^ (uint8_t)(i * 3u));
            new_data[i] = (uint8_t)(0xB1u ^ (uint8_t)(i * 19u));
        }
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/journal-image-flush.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_flush_countdown = 2u;
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/journal-image-flush.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
        if (vfs_file_close(&file) != PROS_VFS_OK ||
            write_rc == PROS_VFS_OK || done != 0u ||
            disk.fail_flush_countdown != 0u ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/journal-image-flush.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
            vfs_file_write(&file, new_data, sizeof(new_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(new_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/journal-image-flush.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        for (uint32_t i = 0u; i < sizeof(read_data); ++i)
            if (read_data[i] != new_data[i])
                return pfs_selftest_fail(__LINE__, -13);
        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * Commit-record write failure: the device rejects the commit block before
     * accepting any bytes. Since a partial device write is still possible on
     * real hardware, the conservative policy freezes this mount read-only;
     * power-loss/remount must preserve the old committed contents.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];
        uint32_t done = 0u;
        int write_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x31u ^ (uint8_t)(i * 11u));
            new_data[i] = (uint8_t)(0xD4u ^ (uint8_t)(i * 17u));
        }
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/commit-write-failure.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_commit_write_once = 1u;
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/commit-write-failure.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
        if (vfs_file_close(&file) != PROS_VFS_OK ||
            write_rc == PROS_VFS_OK || done != 0u ||
            disk.fail_commit_write_once != 0u ||
            !fs.read_only || !fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -13);

        {
            uint32_t durable_crc = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            uint32_t volatile_crc = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            uint32_t dirty_crc = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            int retry_rc = vfs_kernel_file_open(
                &fs.mount, "/commit-write-failure.bin",
                PROS_VFS_O_WRITE, &file);

            done = 0u;
            if (retry_rc == PROS_VFS_OK) {
                retry_rc = vfs_file_write(
                    &file, new_data, sizeof(new_data), &done);
                if (vfs_file_close(&file) != PROS_VFS_OK)
                    return pfs_selftest_fail(__LINE__, -13);
            }
            if (retry_rc == PROS_VFS_OK || done != 0u ||
                durable_crc != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_crc != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_crc != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)))
                return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/commit-write-failure.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        for (uint32_t i = 0u; i < sizeof(read_data); ++i)
            if (read_data[i] != old_data[i]) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * A failed journal commit must not publish a namespace rename. Inject a
     * commit-record write failure after creating the source file, discard all
     * volatile state, and prove remount sees the original name and contents
     * while the destination remains absent.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[64];
        uint8_t read_data[64];
        uint32_t done = 0u;
        int rename_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i)
            old_data[i] = (uint8_t)(0x39u ^ (uint8_t)(i * 23u));

        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/rename-atomic-before.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_commit_write_once = 1u;
        rename_rc = vfs_kernel_rename(&fs.mount, "/rename-atomic-before.bin",
                               "/rename-atomic-after.bin");
        if (rename_rc == PROS_VFS_OK ||
            disk.fail_commit_write_once != 0u ||
            !fs.read_only || !fs.mount.read_only) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/rename-atomic-before.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }
        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            if (read_data[i] != old_data[i]) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        }

        {
            int destination_rc = vfs_kernel_file_open(
                &fs.mount, "/rename-atomic-after.bin", PROS_VFS_O_READ,
                &file);
            if (destination_rc == PROS_VFS_OK) {
                (void)vfs_file_close(&file);
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
            if (destination_rc != PROS_VFS_ENOENT) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        }

        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * Rename read-failure sweep: each read position reached while resolving
     * and staging a rename must fail closed without publishing the new name or
     * mutating any mock-media view. The first position not reached proves the
     * sweep passed the complete read sequence for this namespace.
     */
    {
        struct pros_vfs_file file;
        uint8_t payload[96];
        uint8_t readback[96];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(payload); ++i)
            payload[i] = (uint8_t)(0x6Du ^ (uint8_t)(i * 17u));

        for (uint32_t read_position = 1u; read_position <= 64u;
             ++read_position) {
            uint32_t done = 0u;
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;
            int rename_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/rename-read-before.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, payload, sizeof(payload), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(payload) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -21);

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            disk.fail_read_countdown = read_position;
            rename_rc = vfs_kernel_rename(&fs.mount, "/rename-read-before.bin",
                                   "/rename-read-after.bin");
            if (disk.fail_read_countdown != 0u) {
                /*
                 * The requested read position was beyond rename's read
                 * sequence. Disarm the one-shot fault before integrity scan
                 * and unmount; those operations must not consume a fault
                 * intended exclusively for the rename operation.
                 */
                disk.fail_read_countdown = 0u;
                if (rename_rc != PROS_VFS_OK ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -21);
                }
                sweep_finished = 1u;
                break;
            }

            if (rename_rc == PROS_VFS_OK ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)) ||
                fs.read_only || fs.mount.read_only) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -21);
            }

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/rename-read-before.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, readback, sizeof(readback), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(readback) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -21);
            }
            for (uint32_t i = 0u; i < sizeof(readback); ++i) {
                if (readback[i] != payload[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -21);
                }
            }

            {
                int destination_rc = vfs_kernel_file_open(
                    &fs.mount, "/rename-read-after.bin", PROS_VFS_O_READ,
                    &file);
                if (destination_rc == PROS_VFS_OK) {
                    (void)vfs_file_close(&file);
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -21);
                }
                if (destination_rc != PROS_VFS_ENOENT) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -21);
                }
            }

            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -21);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -21);
    }

    /*
     * Sweep failures across every block write issued by rename. After a
     * simulated power loss, recovery must expose exactly one namespace path
     * (old or new) with the original payload, never a missing or duplicate
     * name. A countdown not reached marks the end of rename's write sequence.
     */
    {
        struct pros_vfs_file file;
        uint8_t payload[96];
        uint8_t readback[96];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(payload); ++i)
            payload[i] = (uint8_t)(0xA7u ^ (uint8_t)(i * 19u));

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            uint32_t done = 0u;
            int rename_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/rename-write-before.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, payload, sizeof(payload), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(payload) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -22);

            disk.fail_write_countdown = write_position;
            rename_rc = vfs_kernel_rename(&fs.mount, "/rename-write-before.bin",
                                   "/rename-write-after.bin");
            if (disk.fail_write_countdown != 0u) {
                /*
                 * Rename completed before this write position was reached.
                 * Disarm the test-only fault before integrity scan/unmount.
                 */
                disk.fail_write_countdown = 0u;
                if (rename_rc != PROS_VFS_OK ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -22);
                }
                sweep_finished = 1u;
                break;
            }

            if (rename_rc == PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -22);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -22);
            }

            {
                int source_rc = vfs_kernel_file_open(
                    &fs.mount, "/rename-write-before.bin",
                    PROS_VFS_O_READ, &file);
                int source_exists = source_rc == PROS_VFS_OK;
                if (source_exists) {
                    done = 0u;
                    if (vfs_file_read(&file, readback, sizeof(readback),
                                      &done) != PROS_VFS_OK ||
                        done != sizeof(readback) ||
                        vfs_file_close(&file) != PROS_VFS_OK) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -22);
                    }
                } else if (source_rc != PROS_VFS_ENOENT) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -22);
                }

                int destination_rc = vfs_kernel_file_open(
                    &fs.mount, "/rename-write-after.bin",
                    PROS_VFS_O_READ, &file);
                int destination_exists = destination_rc == PROS_VFS_OK;
                if (destination_exists) {
                    done = 0u;
                    if (vfs_file_read(&file, readback, sizeof(readback),
                                      &done) != PROS_VFS_OK ||
                        done != sizeof(readback) ||
                        vfs_file_close(&file) != PROS_VFS_OK) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -22);
                    }
                } else if (destination_rc != PROS_VFS_ENOENT) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -22);
                }

                if (source_exists == destination_exists) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -22);
                }
            }

            for (uint32_t i = 0u; i < sizeof(readback); ++i) {
                if (readback[i] != payload[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -22);
                }
            }
            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -22);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -22);
    }

    /*
     * A CRC-valid journal header with duplicate home-block targets is still
     * structurally corrupt. Construct a valid committed transaction directly
     * on the mock device, then duplicate a target and recompute the header CRC.
     * Recovery must reject the malformed target list before replaying it.
     */
    {
        struct pros_prosenjitfs_journal_header header;
        struct pros_prosenjitfs_journal_commit commit;
        uint8_t header_block[512];
        uint8_t images[2u * 512u];
        uint8_t commit_block[512];
        uint64_t target0;
        uint64_t journal_start;
        uint64_t total_blocks;
        uint32_t block_size;
        int mount_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        block_size = fs.sb.block_size;
        journal_start = fs.sb.journal_start;
        total_blocks = fs.sb.total_blocks;
        if (block_size != sizeof(header_block) ||
            fs.sb.journal_blocks < 4u ||
            prosenjitfs_alloc_blocks(&fs, 2u, &target0) != PROSENJITFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        for (uint32_t i = 0u; i < sizeof(images); ++i)
            images[i] = (uint8_t)(0x6Du ^ (uint8_t)(i * 13u));
        pfs_zero(header_block, sizeof(header_block));
        pfs_zero(&header, sizeof(header));
        header.magic = PROSENJITFS_JOURNAL_MAGIC;
        header.record_count = 2u;
        header.transaction_id = 0x46533237544A4E4Cull;
        header.targets[0] = target0;
        header.targets[1] = target0 + 1u;
        pfs_copy(header_block, &header, sizeof(header));
        header.checksum = pfs_crc_with_zero32(
            header_block, block_size,
            (uint32_t)offsetof(struct pros_prosenjitfs_journal_header,
                               checksum));
        pfs_copy(header_block, &header, sizeof(header));

        pfs_zero(commit_block, sizeof(commit_block));
        pfs_zero(commit_block, sizeof(commit_block));
        pfs_zero(&commit, sizeof(commit));
        commit.magic = PROSENJITFS_JOURNAL_COMMIT;
        commit.record_count = 2u;
        commit.transaction_id = header.transaction_id;
        pfs_copy(commit_block, &commit, sizeof(commit));
        commit.checksum = pfs_journal_checksum(
            header_block, images, block_size, 2u, commit_block);
        pfs_copy(commit_block, &commit, sizeof(commit));

        if (pfs_write_block(&device, journal_start, header_block) !=
                PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 1u, images) !=
                PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 2u,
                            images + block_size) != PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 3u, commit_block) !=
                PROSENJITFS_OK ||
            pfs_flush(&device) != PROSENJITFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        /* Simulate power loss: normal unmount would retire the journal. */
        pfs_test_crash_reset(&fs, &disk);

        header.targets[1] = header.targets[0];
        header.checksum = 0u;
        pfs_zero(header_block, sizeof(header_block));
        pfs_copy(header_block, &header, sizeof(header));
        header.checksum = pfs_crc_with_zero32(
            header_block, block_size,
            (uint32_t)offsetof(struct pros_prosenjitfs_journal_header,
                               checksum));
        pfs_copy(header_block, &header, sizeof(header));

        /* Keep the commit checksum valid so duplicate-target validation,
         * rather than checksum rejection, is what makes recovery fail. */
        pfs_zero(&commit, sizeof(commit));
        commit.magic = PROSENJITFS_JOURNAL_COMMIT;
        commit.record_count = 2u;
        commit.transaction_id = header.transaction_id;
        pfs_copy(commit_block, &commit, sizeof(commit));
        commit.checksum = pfs_journal_checksum(
            header_block, images, block_size, 2u, commit_block);
        pfs_copy(commit_block, &commit, sizeof(commit));

        if (pfs_write_block(&device, journal_start, header_block) !=
                PROSENJITFS_OK ||
            pfs_write_block(&device, journal_start + 3u, commit_block) !=
                PROSENJITFS_OK ||
            pfs_flush(&device) != PROSENJITFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        mount_rc = prosenjitfs_mount(&fs, &device, 0u);
        if (mount_rc != PROSENJITFS_ECORRUPT) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }
        pfs_test_crash_reset(&fs, &disk);

        /*
         * FS-32: reject invalid home-block geometry even when both checksums
         * are valid. Each mount attempt must fail before replay and leave the
         * mock medium/cache state unchanged.
         */
        {
            /* Block 0/1 are reserved for the disk/partition boot
             * structures; total_blocks is outside the volume, and
             * journal_start is inside the journal. All four fixtures have
             * valid header/commit checksums so target admission is tested. */
            const uint64_t invalid_targets[4] = {
                0u,
                1u,
                total_blocks,
                journal_start
            };
            for (uint32_t invalid_index = 0u;
                 invalid_index < sizeof(invalid_targets) /
                                 sizeof(invalid_targets[0]);
                 ++invalid_index) {
                uint32_t durable_before;
                uint32_t volatile_before;
                uint32_t dirty_before;

                header.targets[1] = invalid_targets[invalid_index];
                header.checksum = 0u;
                pfs_zero(header_block, sizeof(header_block));
                pfs_copy(header_block, &header, sizeof(header));
                header.checksum = pfs_crc_with_zero32(
                    header_block, block_size,
                    (uint32_t)offsetof(
                        struct pros_prosenjitfs_journal_header, checksum));
                pfs_copy(header_block, &header, sizeof(header));

                pfs_zero(commit_block, sizeof(commit_block));
                pfs_zero(&commit, sizeof(commit));
                commit.magic = PROSENJITFS_JOURNAL_COMMIT;
                commit.record_count = 2u;
                commit.transaction_id = header.transaction_id;
                pfs_copy(commit_block, &commit, sizeof(commit));
                commit.checksum = pfs_journal_checksum(
                    header_block, images, block_size, 2u, commit_block);
                pfs_copy(commit_block, &commit, sizeof(commit));

                if (pfs_write_block(&device, journal_start, header_block) !=
                        PROSENJITFS_OK ||
                    pfs_write_block(&device, journal_start + 3u,
                                    commit_block) != PROSENJITFS_OK ||
                    pfs_flush(&device) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -13);
                }

                durable_before = prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data));
                volatile_before = prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
                dirty_before = prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty));
                mount_rc = prosenjitfs_mount(&fs, &device, 0u);
                if (mount_rc != PROSENJITFS_ECORRUPT ||
                    durable_before != prosenjitfs_crc32c(
                        disk.data, (uint32_t)sizeof(disk.data)) ||
                    volatile_before != prosenjitfs_crc32c(
                        disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                    dirty_before != prosenjitfs_crc32c(
                        disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -13);
                }
                pfs_test_crash_reset(&fs, &disk);
            }
        }
    }

    /*
     * A failed journal commit must not publish an unlink. Keep a populated
     * file, inject failure at the commit-record write, simulate power loss,
     * then prove remount retains the old name and bytes and the scan is clean.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[64];
        uint8_t read_data[64];
        uint32_t done = 0u;
        int unlink_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i)
            old_data[i] = (uint8_t)(0xA6u ^ (uint8_t)(i * 13u));

        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/unlink-atomic.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_commit_write_once = 1u;
        unlink_rc = vfs_kernel_unlink(&fs.mount, "/unlink-atomic.bin");
        if (unlink_rc == PROS_VFS_OK ||
            disk.fail_commit_write_once != 0u ||
            !fs.read_only || !fs.mount.read_only) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/unlink-atomic.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -13);
        }
        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            if (read_data[i] != old_data[i]) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        }

        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * Device-flush failure at the commit durability boundary.
     *
     * The first two transaction flushes retire the reused commit slot and
     * persist the journal header/images. Fail the third flush, which is the
     * commit-record flush. The mount must become read-only, reject retries
     * without changing media, and after power loss recover the old contents
     * because the failed flush did not make the commit durable.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];
        uint32_t done = 0u;
        int write_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x57u ^ (uint8_t)(i * 3u));
            new_data[i] = (uint8_t)(0xA9u ^ (uint8_t)(i * 13u));
        }
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/flush-failure.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_flush_countdown = 3u;
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/flush-failure.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
        if (vfs_file_close(&file) != PROS_VFS_OK ||
            write_rc == PROS_VFS_OK || done != 0u ||
            disk.fail_flush_countdown != 0u ||
            !fs.read_only || !fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -13);

        {
            uint32_t durable_crc = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            uint32_t volatile_crc = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            uint32_t dirty_crc = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            int retry_rc = vfs_kernel_file_open(
                &fs.mount, "/flush-failure.bin", PROS_VFS_O_WRITE, &file);

            done = 0u;
            if (retry_rc == PROS_VFS_OK) {
                retry_rc = vfs_file_write(
                    &file, old_data, sizeof(old_data), &done);
                if (vfs_file_close(&file) != PROS_VFS_OK)
                    return pfs_selftest_fail(__LINE__, -13);
            }
            if (retry_rc == PROS_VFS_OK || done != 0u ||
                durable_crc != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_crc != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_crc != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)))
                return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/flush-failure.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        for (uint32_t i = 0u; i < sizeof(read_data); ++i)
            if (read_data[i] != old_data[i]) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * Ambiguous commit flush: the device persists the commit record but
     * reports an I/O error. The mount must still freeze writes; after power
     * loss, remount must replay the durable committed transaction.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];
        uint32_t done = 0u;
        int write_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x68u ^ (uint8_t)(i * 9u));
            new_data[i] = (uint8_t)(0xB7u ^ (uint8_t)(i * 15u));
        }
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/ambiguous-flush.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        /*
         * Flush #2 may persist the journal header/images while returning EIO.
         * No commit record has been published, so recovery must retain old data.
         */
        disk.fail_flush_after_persist_countdown = 2u;
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/ambiguous-flush.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
        if (vfs_file_close(&file) != PROS_VFS_OK ||
            write_rc == PROS_VFS_OK || done != 0u ||
            disk.fail_flush_after_persist_countdown != 0u ||
            fs.read_only || fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -13);

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/ambiguous-flush.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            if (read_data[i] != old_data[i])
                return pfs_selftest_fail(__LINE__, -13);
        }
        if (prosenjitfs_check_integrity_report(&fs, 0) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);

        disk.fail_flush_after_persist_countdown = 3u;
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/ambiguous-flush.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
        if (vfs_file_close(&file) != PROS_VFS_OK ||
            write_rc == PROS_VFS_OK || done != 0u ||
            disk.fail_flush_after_persist_countdown != 0u ||
            !fs.read_only || !fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -13);

        {
            uint32_t durable_crc = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            uint32_t volatile_crc = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            uint32_t dirty_crc = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            int retry_rc = vfs_kernel_file_open(
                &fs.mount, "/ambiguous-flush.bin", PROS_VFS_O_WRITE, &file);

            done = 0u;
            if (retry_rc == PROS_VFS_OK) {
                retry_rc = vfs_file_write(
                    &file, old_data, sizeof(old_data), &done);
                if (vfs_file_close(&file) != PROS_VFS_OK)
                    return pfs_selftest_fail(__LINE__, -13);
            }
            if (retry_rc == PROS_VFS_OK || done != 0u ||
                durable_crc != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_crc != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_crc != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)))
                return pfs_selftest_fail(__LINE__, -13);
        }

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/ambiguous-flush.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
        for (uint32_t i = 0u; i < sizeof(read_data); ++i)
            if (read_data[i] != new_data[i]) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -13);
            }
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -13);
    }

    /*
     * Home-block flush failure: the commit record has already been flushed
     * successfully, but persisting the home-block updates fails. The current
     * mount must become read-only; after simulated power loss, recovery must
     * replay the committed transaction and expose the complete new payload.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];
        uint32_t done = 0u;
        int write_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x57u ^ (uint8_t)(i * 5u));
            new_data[i] = (uint8_t)(0xA9u ^ (uint8_t)(i * 13u));
        }
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/home-flush-failure.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -30);

        /*
         * A single overwrite transaction flushes: commit-slot invalidation,
         * journal header/images, commit record, then home blocks.
         */
        disk.fail_flush_countdown = 4u;
        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/home-flush-failure.bin",
                          PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -30);
        write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
        if (vfs_file_close(&file) != PROS_VFS_OK ||
            write_rc == PROS_VFS_OK || done != 0u ||
            disk.fail_flush_countdown != 0u ||
            !fs.read_only || !fs.mount.read_only)
            return pfs_selftest_fail(__LINE__, -30);

        pfs_test_crash_reset(&fs, &disk);
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -30);

        done = 0u;
        if (vfs_kernel_file_open(&fs.mount, "/home-flush-failure.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
            vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(read_data) ||
            vfs_file_close(&file) != PROS_VFS_OK)
            return pfs_selftest_fail(__LINE__, -30);
        for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
            if (read_data[i] != new_data[i]) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -30);
            }
        }
        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -30);
    }

    /*
     * Recovery must preserve device I/O errors while reading each journal
     * component. Fail reads of the durable header, first image, and commit
     * record separately; each failed mount must be retryable after power loss
     * and must converge to the committed replacement bytes.
     */
    {
        static const uint32_t target_kinds[] = { 0u, 1u, 2u };
        uint8_t old_data[512];
        uint8_t new_data[512];
        uint8_t read_data[512];

        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x2Du ^ (uint8_t)(i * 7u));
            new_data[i] = (uint8_t)(0xD3u ^ (uint8_t)(i * 13u));
        }

        for (uint32_t target_index = 0u;
             target_index < sizeof(target_kinds) / sizeof(target_kinds[0]);
             ++target_index) {
            struct pros_vfs_file file;
            struct pros_prosenjitfs_journal_header header;
            uint8_t journal_block[4096];
            uint64_t journal_start;
            uint64_t target_block;
            uint64_t target_sector;
            uint32_t block_sectors;
            uint32_t done = 0u;
            int write_rc;
            int mount_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/recovery-read-error.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -14);

            pfs_crash_inject_stage = 9u;
            pfs_crash_injected = 0u;
            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/recovery-read-error.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
            if (vfs_file_close(&file) != PROS_VFS_OK ||
                write_rc != PROS_VFS_EIO || done != 0u ||
                pfs_crash_injected == 0u)
                return pfs_selftest_fail(__LINE__, -14);

            journal_start = fs.sb.journal_start;
            block_sectors = fs.sb.block_size / device.sector_size;
            if (block_sectors == 0u ||
                fs.sb.block_size > sizeof(journal_block) ||
                pfs_read_block(&device, journal_start, journal_block) !=
                    PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
            pfs_copy(&header, journal_block, sizeof(header));
            if (header.magic != PROSENJITFS_JOURNAL_MAGIC ||
                header.record_count == 0u ||
                header.record_count > PROSENJITFS_MAX_TX_BLOCKS ||
                target_kinds[target_index] > 2u)
                return pfs_selftest_fail(__LINE__, -14);

            target_block = journal_start;
            if (target_kinds[target_index] == 1u)
                target_block = journal_start + 1u;
            else if (target_kinds[target_index] == 2u)
                target_block = journal_start + 1u + header.record_count;
            if (target_block > UINT64_MAX / block_sectors)
                return pfs_selftest_fail(__LINE__, -14);
            target_sector = target_block * block_sectors;

            pfs_test_crash_reset(&fs, &disk);
            disk.fail_read_lba = target_sector;
            disk.fail_read_lba_once = 1u;
            mount_rc = prosenjitfs_mount(&fs, &device, 0u);
            if (mount_rc != PROSENJITFS_EIO ||
                disk.fail_read_lba_once != 0u) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -14);
            }

            /* A read error during recovery is not corruption. Retry from the
             * same durable journal after dropping volatile and CPU state. */
            pfs_test_power_loss(&disk);
            pfs_zero(&fs, sizeof(fs));
            disk.fail_read_lba_once = 0u;
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -14);
            }
            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/recovery-read-error.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(read_data) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -14);
            }
            for (uint32_t i = 0u; i < sizeof(read_data); ++i)
                if (read_data[i] != new_data[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -14);
                }
            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -14);
        }
    }

    /*
     * Journal retirement must preserve the underlying I/O error during mount
     * recovery. Stage 4 leaves a durable header/images with no new commit.
     * Fail the journal-clear write and flush separately, then retry mount.
     */
    {
        uint8_t old_data[64];
        uint8_t new_data[64];
        uint8_t read_data[64];

        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x31u ^ (uint8_t)(i * 7u));
            new_data[i] = (uint8_t)(0xC7u ^ (uint8_t)(i * 19u));
        }

        for (uint32_t failure_kind = 1u; failure_kind <= 2u;
             ++failure_kind) {
            struct pros_vfs_file file;
            uint32_t done = 0u;
            int write_rc;
            int mount_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/journal-clear-error.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -34);
            }

            pfs_crash_inject_stage = 4u;
            pfs_crash_injected = 0u;
            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/journal-clear-error.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -34);
            }
            write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
            if (vfs_file_close(&file) != PROS_VFS_OK ||
                write_rc != PROS_VFS_EIO || done != 0u ||
                pfs_crash_injected == 0u) {
                pfs_crash_inject_stage = 0u;
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -34);
            }

            pfs_crash_inject_stage = 0u;
            pfs_test_crash_reset(&fs, &disk);
            if (failure_kind == 1u)
                disk.fail_write_countdown = 1u;
            else
                disk.fail_flush_countdown = 1u;

            mount_rc = prosenjitfs_mount(&fs, &device, 0u);
            if (mount_rc != PROSENJITFS_EIO ||
                (failure_kind == 1u && disk.fail_write_countdown != 0u) ||
                (failure_kind == 2u && disk.fail_flush_countdown != 0u)) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -34);
            }

            /* Failed mount owns its allocations; drop only device volatile state. */
            pfs_test_power_loss(&disk);
            pfs_zero(&fs, sizeof(fs));
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -34);
            }
            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/journal-clear-error.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -34);
            }
            for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
                if (read_data[i] != old_data[i]) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -34);
                }
            }
            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -34);
            }
        }
    }

    /*
     * A block-write error during committed-journal replay must not retire the
     * journal. Simulate a crash after commit publication but before home-block
     * application, fail the second replay write, then crash and retry recovery.
     */
    {
        struct pros_vfs_file file;
        struct pros_prosenjitfs_superblock sb;
        struct pros_prosenjitfs_journal_header h;
        struct pros_prosenjitfs_journal_commit c;
        uint8_t old_data[64];
        uint32_t done = 0u;
        int unlink_rc;

        for (uint32_t i = 0u; i < sizeof(old_data); ++i)
            old_data[i] = (uint8_t)(0x5Au ^ (uint8_t)(i * 13u));

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/journal-replay-write-error.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                PROS_VFS_OK ||
            done != sizeof(old_data) ||
            vfs_file_close(&file) != PROS_VFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }

        pfs_crash_inject_stage = 5u;
        pfs_crash_injected = 0u;
        unlink_rc = vfs_kernel_unlink(&fs.mount, "/journal-replay-write-error.bin");
        if (unlink_rc != PROS_VFS_EIO || pfs_crash_injected == 0u) {
            pfs_crash_inject_stage = 0u;
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }

        pfs_crash_inject_stage = 0u;
        pfs_test_crash_reset(&fs, &disk);
        pfs_copy(&sb, disk.data, sizeof(sb));
        if (sb.block_size != device.sector_size ||
            sb.journal_start >= device.sector_count) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }
        pfs_copy(&h, disk.data + sb.journal_start * sb.block_size,
                 sizeof(h));
        if (h.magic != PROSENJITFS_JOURNAL_MAGIC ||
            h.record_count < 2u ||
            h.record_count > PROSENJITFS_MAX_TX_BLOCKS ||
            h.record_count + 2u > sb.journal_blocks) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }
        pfs_copy(&c, disk.data +
                 (sb.journal_start + 1u + h.record_count) * sb.block_size,
                 sizeof(c));
        if (c.magic != PROSENJITFS_JOURNAL_COMMIT ||
            c.transaction_id != h.transaction_id ||
            c.record_count != h.record_count) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }

        /* First replay write is accepted; the second fails before flush. */
        disk.fail_write_countdown = 2u;
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_EIO ||
            disk.fail_write_countdown != 0u) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }

        /* Lose the partial, unflushed replay and repeat the durable journal. */
        pfs_test_power_loss(&disk);
        pfs_zero(&fs, sizeof(fs));
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }
        if (vfs_kernel_file_open(&fs.mount, "/journal-replay-write-error.bin",
                          PROS_VFS_O_READ, &file) != PROS_VFS_ENOENT) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }
        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -35);
        }
    }

    pfs_selftest_diag_stage = __LINE__;
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

            if (vfs_kernel_file_open(&fs.mount, "/recovery.bin",
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
            if (vfs_kernel_file_open(&fs.mount, "/recovery.bin",
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

            /*
             * Stage 9 interrupted the write transaction while this original
             * mount still owns volatile state. Simulate the power loss and
             * discard that mount before testing an interruption during replay.
             */
            pfs_test_crash_reset(&fs, &disk);
            pfs_crash_inject_stage = recovery_stages[recovery_index];
            pfs_crash_injected = 0u;
            mount_rc = prosenjitfs_mount(&fs, &device, 0u);
            if (mount_rc != PROSENJITFS_EIO ||
                pfs_crash_injected == 0u) {
                pfs_crash_inject_stage = 0u;
                pfs_test_crash_reset(&fs, &disk);
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
            if (vfs_kernel_file_open(&fs.mount, "/recovery.bin",
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

    pfs_selftest_diag_stage = __LINE__;
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
            if (vfs_kernel_file_open(&fs.mount, "/cycle.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) != PROS_VFS_OK ||
                vfs_file_write(&file, cycle_data, sizeof(cycle_data), &done) != PROS_VFS_OK ||
                done != sizeof(cycle_data) || vfs_file_close(&file) != PROS_VFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -16);

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/cycle.bin", PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, cycle_read, sizeof(cycle_read), &done) != PROS_VFS_OK ||
                done != sizeof(cycle_read) || vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -16);
            for (uint32_t j = 0u; j < sizeof(cycle_data); ++j)
                if (cycle_read[j] != cycle_data[j])
                    return pfs_selftest_fail(__LINE__, -16);

            if (vfs_kernel_unlink(&fs.mount, "/cycle.bin") != PROS_VFS_OK ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -16);
        }
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -16);
    }

    /*
     * Sweep read failures across rmdir's parent lookup, child metadata checks,
     * and transaction preparation. Reached faults must propagate as EIO,
     * leave media unchanged and preserve the directory; the first not-reached
     * countdown marks the end of the operation's read sequence.
     */
    {
        struct pros_vfs_dir directory;
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t read_position = 1u; read_position <= 64u;
             ++read_position) {
            uint32_t durable_before;
            uint32_t volatile_before;
            uint32_t dirty_before;
            int rmdir_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_mkdir(&fs.mount, "/rmdir-read-sweep") != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }

            durable_before = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            volatile_before = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            dirty_before = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));

            disk.fail_read_countdown = read_position;
            rmdir_rc = vfs_kernel_rmdir(&fs.mount, "/rmdir-read-sweep");
            if (disk.fail_read_countdown != 0u) {
                /* This read position was beyond rmdir's complete read path. */
                disk.fail_read_countdown = 0u;
                if (rmdir_rc != PROS_VFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -26);
                }
                {
                    int lookup_rc = vfs_kernel_dir_open(
                        &fs.mount, "/rmdir-read-sweep", &directory);
                    if (lookup_rc == PROS_VFS_OK) {
                        (void)vfs_dir_close(&directory);
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -26);
                    }
                    if (lookup_rc != PROS_VFS_ENOENT) {
                        pfs_test_crash_reset(&fs, &disk);
                        return pfs_selftest_fail(__LINE__, -26);
                    }
                }
                if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -26);
                }
                sweep_finished = 1u;
                break;
            }

            if (rmdir_rc != PROS_VFS_EIO ||
                durable_before != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_before != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_before != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty)) ||
                fs.read_only || fs.mount.read_only) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }

            if (vfs_kernel_dir_open(&fs.mount, "/rmdir-read-sweep", &directory) !=
                    PROS_VFS_OK ||
                vfs_dir_close(&directory) != PROS_VFS_OK ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -26);
    }

    /*
     * A cyclic parent-directory bucket chain is malformed metadata, not a
     * device I/O failure. Exercise the VFS rmdir lookup against a checksum-
     * valid self-cycle and require corruption classification without media
     * writes; restore the entry before running the integrity scan.
     */
    {
        static const char anchor_name[] = "rmdir-cycle-aa";
        static const char anchor_path[] = "/rmdir-cycle-aa";
        char missing_name[] = "rmdir-cycle-aa";
        char missing_path[] = "/rmdir-cycle-aa";
        struct pros_prosenjitfs_inode root_inode;
        struct pros_prosenjitfs_dir_entry anchor_entry;
        uint8_t saved_block[4096];
        uint8_t corrupt_block[4096];
        uint64_t anchor_lba = 0u;
        uint64_t previous_lba = 0u;
        uint32_t bucket;
        uint32_t found_missing = 0u;
        uint32_t media_size;
        uint32_t durable_before;
        uint32_t volatile_before;
        uint32_t dirty_before;
        int rmdir_rc;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_mkdir(&fs.mount, anchor_path) != PROS_VFS_OK ||
            prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                PROSENJITFS_OK ||
            pfs_find_dir_entry_lba(&fs, &root_inode, fs.sb.root_inode, anchor_name,
                                   &anchor_entry, &anchor_lba,
                                   &previous_lba) != PROSENJITFS_OK ||
            anchor_lba == 0u ||
            pfs_read_block(fs.mount.device, anchor_lba, saved_block) !=
                PROSENJITFS_OK) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -25);
        }

        bucket = pfs_name_hash((const uint8_t *)anchor_name,
                               (uint32_t)(sizeof(anchor_name) - 1u)) %
                 PROSENJITFS_DIR_BUCKETS;
        for (uint32_t a = 0u; a < 26u && !found_missing; ++a) {
            for (uint32_t b = 0u; b < 26u; ++b) {
                missing_name[12] = (char)('a' + a);
                missing_name[13] = (char)('a' + b);
                missing_name[14] = '\0';
                if ((a != 0u || b != 0u) &&
                    pfs_name_hash((const uint8_t *)missing_name, 14u) %
                        PROSENJITFS_DIR_BUCKETS == bucket) {
                    found_missing = 1u;
                    break;
                }
            }
        }
        if (!found_missing) {
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -25);
        }

        pfs_copy(corrupt_block, saved_block, fs.sb.block_size);
        anchor_entry.next = anchor_lba;
        pfs_copy(corrupt_block, &anchor_entry, sizeof(anchor_entry));
        pfs_put32(corrupt_block +
                      offsetof(struct pros_prosenjitfs_dir_entry, checksum),
                  pfs_crc_with_zero32(
                      corrupt_block, sizeof(anchor_entry),
                      (uint32_t)offsetof(
                          struct pros_prosenjitfs_dir_entry, checksum)));
        media_size = (uint32_t)sizeof(disk.data);
        pfs_copy(disk.data + anchor_lba * fs.sb.block_size,
                 corrupt_block, fs.sb.block_size);
        pfs_copy(disk.volatile_data + anchor_lba * fs.sb.block_size,
                 corrupt_block, fs.sb.block_size);

        durable_before = prosenjitfs_crc32c(disk.data, media_size);
        volatile_before = prosenjitfs_crc32c(disk.volatile_data, media_size);
        dirty_before = prosenjitfs_crc32c(disk.dirty,
                                           (uint32_t)sizeof(disk.dirty));
        missing_path[13] = missing_name[12];
        missing_path[14] = missing_name[13];
        rmdir_rc = vfs_kernel_rmdir(&fs.mount, missing_path);
        if (rmdir_rc != PROS_VFS_ECORRUPT ||
            durable_before != prosenjitfs_crc32c(disk.data, media_size) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, media_size) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty))) {
            pfs_copy(disk.data + anchor_lba * fs.sb.block_size,
                     saved_block, fs.sb.block_size);
            pfs_copy(disk.volatile_data + anchor_lba * fs.sb.block_size,
                     saved_block, fs.sb.block_size);
            pfs_test_crash_reset(&fs, &disk);
            return pfs_selftest_fail(__LINE__, -25);
        }

        pfs_copy(disk.data + anchor_lba * fs.sb.block_size,
                 saved_block, fs.sb.block_size);
        pfs_copy(disk.volatile_data + anchor_lba * fs.sb.block_size,
                 saved_block, fs.sb.block_size);
        if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            vfs_kernel_rmdir(&fs.mount, anchor_path) != PROS_VFS_OK ||
            prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -25);
    }

    /*
     * Unknown v1 feature bits must fail closed even when both superblocks
     * agree and each has a valid checksum. Restore the fixture before the
     * following independent redundant-superblock consistency regression.
     */
    {
        uint32_t feature_offset =
            (uint32_t)offsetof(struct pros_prosenjitfs_superblock, features);
        uint32_t checksum_offset =
            (uint32_t)offsetof(struct pros_prosenjitfs_superblock, checksum);
        uint8_t saved_primary = disk.data[feature_offset + 3u];
        uint8_t saved_backup = disk.data[512u + feature_offset + 3u];

        disk.data[feature_offset + 3u] |= 0x80u;
        disk.data[512u + feature_offset + 3u] |= 0x80u;
        pfs_put32(disk.data + checksum_offset,
                  pfs_crc_with_zero32(
                      disk.data,
                      (uint32_t)sizeof(struct pros_prosenjitfs_superblock),
                      checksum_offset));
        pfs_put32(disk.data + 512u + checksum_offset,
                  pfs_crc_with_zero32(
                      disk.data + 512u,
                      (uint32_t)sizeof(struct pros_prosenjitfs_superblock),
                      checksum_offset));
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_ECORRUPT)
            return pfs_selftest_fail(__LINE__, -20);
        disk.data[feature_offset + 3u] = saved_primary;
        disk.data[512u + feature_offset + 3u] = saved_backup;
        pfs_put32(disk.data + checksum_offset,
                  pfs_crc_with_zero32(
                      disk.data,
                      (uint32_t)sizeof(struct pros_prosenjitfs_superblock),
                      checksum_offset));
        pfs_put32(disk.data + 512u + checksum_offset,
                  pfs_crc_with_zero32(
                      disk.data + 512u,
                      (uint32_t)sizeof(struct pros_prosenjitfs_superblock),
                      checksum_offset));
    }

    /*
     * Both superblocks remain individually valid (including their CRCs), but
     * a different UUID in the backup must be rejected as conflicting metadata.
     * Restore the original byte and checksum before the remaining corruption
     * regression so this test leaves the durable image unchanged.
     */
    saved = disk.data[512u + offsetof(struct pros_prosenjitfs_superblock, uuid)];
    disk.data[512u + offsetof(struct pros_prosenjitfs_superblock, uuid)] ^= 0x01u;
    pfs_put32(disk.data + 512u +
              offsetof(struct pros_prosenjitfs_superblock, checksum),
              pfs_crc_with_zero32(
                  disk.data + 512u,
                  (uint32_t)sizeof(struct pros_prosenjitfs_superblock),
                  (uint32_t)offsetof(struct pros_prosenjitfs_superblock, checksum)));
    if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_ECORRUPT ||
        prosenjitfs_probe(&device) != PROSENJITFS_ECORRUPT)
        return pfs_selftest_fail(__LINE__, -17);
    disk.data[512u + offsetof(struct pros_prosenjitfs_superblock, uuid)] = saved;
    pfs_put32(disk.data + 512u +
              offsetof(struct pros_prosenjitfs_superblock, checksum),
              pfs_crc_with_zero32(
                  disk.data + 512u,
                  (uint32_t)sizeof(struct pros_prosenjitfs_superblock),
                  (uint32_t)offsetof(struct pros_prosenjitfs_superblock, checksum)));

    /*
     * Seed a file on a clean mount so degraded-mode tests can exercise write,
     * unlink and rename rejection against an existing namespace entry.
     */
    {
        struct pros_vfs_file file;
        uint8_t anchor[16];
        uint32_t done = 0u;
        for (uint32_t i = 0u; i < sizeof(anchor); ++i)
            anchor[i] = (uint8_t)(0x5Du ^ (uint8_t)(i * 7u));
        if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/degraded-anchor.bin",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                PROS_VFS_OK ||
            vfs_file_write(&file, anchor, sizeof(anchor), &done) !=
                PROS_VFS_OK ||
            done != sizeof(anchor) ||
            vfs_file_close(&file) != PROS_VFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -18);
    }

    /*
     * A single surviving superblock is a degraded mount in either direction.
     * It must be exposed read-only and the mount must not rewrite either copy.
     */
    {
        uint32_t checksum_offset =
            (uint32_t)offsetof(struct pros_prosenjitfs_superblock, checksum);
        uint8_t saved_checksum = disk.data[checksum_offset];

        /* Primary invalid, backup valid: recover from backup read-only. */
        disk.data[checksum_offset] ^= 0x01u;
        if (prosenjitfs_probe(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -18);
        if (!fs.read_only || !fs.degraded_superblock) {
            (void)prosenjitfs_unmount(&fs);
            return pfs_selftest_fail(__LINE__, -18);
        }
        /*
         * Degraded recovery is read-only, not merely labelled read-only.
         * Mutating VFS operations must fail before changing any media/cache
         * state, for either surviving-superblock direction.
         */
        {
            struct pros_vfs_file rejected_file;
            uint32_t durable_crc = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            uint32_t volatile_crc = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            uint32_t dirty_crc = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            int write_open_rc = vfs_kernel_file_open(
                &fs.mount, "/degraded-anchor.bin", PROS_VFS_O_WRITE,
                &rejected_file);
            if (write_open_rc == PROS_VFS_OK)
                (void)vfs_file_close(&rejected_file);
            int create_rc = vfs_kernel_file_open(
                &fs.mount, "/degraded-create-probe",
                PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &rejected_file);
            int mkdir_rc = vfs_kernel_mkdir(&fs.mount, "/degraded-mkdir-probe");
            int unlink_rc = vfs_kernel_unlink(&fs.mount, "/degraded-anchor.bin");
            int rename_rc = vfs_kernel_rename(&fs.mount, "/degraded-anchor.bin",
                                        "/degraded-anchor-renamed.bin");
            int rmdir_rc = vfs_kernel_rmdir(&fs.mount, "/degraded-dir-probe");

            if (create_rc == PROS_VFS_OK) {
                (void)vfs_file_close(&rejected_file);
                (void)prosenjitfs_unmount(&fs);
                return pfs_selftest_fail(__LINE__, -18);
            }
            if (write_open_rc != PROS_VFS_EPERM ||
                create_rc != PROS_VFS_EPERM ||
                mkdir_rc != PROS_VFS_EPERM ||
                unlink_rc != PROS_VFS_EPERM ||
                rename_rc != PROS_VFS_EPERM ||
                rmdir_rc != PROS_VFS_EPERM ||
                durable_crc != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_crc != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_crc != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)prosenjitfs_unmount(&fs);
                return pfs_selftest_fail(__LINE__, -18);
            }
        }
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -18);
        disk.data[checksum_offset] = saved_checksum;

        /* Backup invalid, primary valid: this must also be read-only. */
        saved_checksum = disk.data[512u + checksum_offset];
        disk.data[512u + checksum_offset] ^= 0x01u;
        if (prosenjitfs_probe(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -19);
        if (!fs.read_only || !fs.degraded_superblock) {
            (void)prosenjitfs_unmount(&fs);
            return pfs_selftest_fail(__LINE__, -19);
        }
        {
            struct pros_vfs_file rejected_file;
            uint32_t durable_crc = prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data));
            uint32_t volatile_crc = prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
            uint32_t dirty_crc = prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty));
            int write_open_rc = vfs_kernel_file_open(
                &fs.mount, "/degraded-anchor.bin", PROS_VFS_O_WRITE,
                &rejected_file);
            if (write_open_rc == PROS_VFS_OK)
                (void)vfs_file_close(&rejected_file);
            int create_rc = vfs_kernel_file_open(
                &fs.mount, "/degraded-create-probe",
                PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &rejected_file);
            int mkdir_rc = vfs_kernel_mkdir(&fs.mount, "/degraded-mkdir-probe");
            int unlink_rc = vfs_kernel_unlink(&fs.mount, "/degraded-anchor.bin");
            int rename_rc = vfs_kernel_rename(&fs.mount, "/degraded-anchor.bin",
                                        "/degraded-anchor-renamed.bin");
            int rmdir_rc = vfs_kernel_rmdir(&fs.mount, "/degraded-dir-probe");

            if (create_rc == PROS_VFS_OK) {
                (void)vfs_file_close(&rejected_file);
                (void)prosenjitfs_unmount(&fs);
                return pfs_selftest_fail(__LINE__, -19);
            }
            if (write_open_rc != PROS_VFS_EPERM ||
                create_rc != PROS_VFS_EPERM ||
                mkdir_rc != PROS_VFS_EPERM ||
                unlink_rc != PROS_VFS_EPERM ||
                rename_rc != PROS_VFS_EPERM ||
                rmdir_rc != PROS_VFS_EPERM ||
                durable_crc != prosenjitfs_crc32c(
                    disk.data, (uint32_t)sizeof(disk.data)) ||
                volatile_crc != prosenjitfs_crc32c(
                    disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
                dirty_crc != prosenjitfs_crc32c(
                    disk.dirty, (uint32_t)sizeof(disk.dirty))) {
                (void)prosenjitfs_unmount(&fs);
                return pfs_selftest_fail(__LINE__, -19);
            }
        }
        if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -19);
        disk.data[512u + checksum_offset] = saved_checksum;
    }

    /*
     * Adversarial namespace regression: two distinct directory entries must
     * not reference the same inode. Keep both entries' names/bucket placement
     * valid and recompute the entry checksum so the scanner must catch the
     * repeated inode reference itself. The report-only scan must not mutate
     * durable, volatile, or dirty media; restoring the entry must rescan clean.
     */
    {
        struct pros_vfs_file first_file;
        struct pros_vfs_file second_file;
        struct pros_prosenjitfs_inode root_inode;
        struct pros_prosenjitfs_dir_index root_index;
        struct pros_prosenjitfs_dir_entry first_entry = { 0 };
        struct pros_prosenjitfs_dir_entry second_entry = { 0 };
        static uint8_t original_first_block[4096];
        static uint8_t original_second_block[4096];
        static uint8_t corrupt_second_block[4096];
        uint64_t first_lba = 0u;
        uint64_t second_lba = 0u;
        uint32_t durable_before;
        uint32_t volatile_before;
        uint32_t dirty_before;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/repeat-ref-a",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &first_file) != PROS_VFS_OK ||
            vfs_file_close(&first_file) != PROS_VFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/repeat-ref-b",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &second_file) != PROS_VFS_OK ||
            vfs_file_close(&second_file) != PROS_VFS_OK ||
            prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                PROSENJITFS_OK ||
            pfs_read_dir_index(&fs, &root_inode, fs.sb.root_inode, &root_index) !=
                PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -20);

        for (uint32_t bucket = 0u;
             bucket < PROSENJITFS_DIR_BUCKETS && second_lba == 0u;
             ++bucket) {
            uint64_t current = root_index.heads[bucket];
            uint64_t traversed = 0u;
            while (current != 0u && second_lba == 0u) {
                struct pros_prosenjitfs_dir_entry entry;
                uint8_t block[4096];
                if (current < fs.sb.data_start ||
                    current >= fs.sb.total_blocks ||
                    ++traversed > fs.sb.total_blocks ||
                    pfs_read_block(fs.mount.device, current, block) !=
                        PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -20);
                pfs_copy(&entry, block, sizeof(entry));
                if (first_lba == 0u) {
                    first_lba = current;
                    pfs_copy(original_first_block, block, fs.sb.block_size);
                    first_entry = entry;
                } else {
                    second_lba = current;
                    pfs_copy(original_second_block, block, fs.sb.block_size);
                    second_entry = entry;
                }
                current = entry.next;
            }
        }
        if (first_lba == 0u || second_lba == 0u ||
            first_entry.inode == second_entry.inode ||
            first_entry.name_length == 0u ||
            second_entry.name_length == 0u)
            return pfs_selftest_fail(__LINE__, -20);

        pfs_copy(corrupt_second_block, original_second_block,
                 fs.sb.block_size);
        second_entry.inode = first_entry.inode;
        second_entry.checksum = 0u;
        pfs_copy(corrupt_second_block, &second_entry, sizeof(second_entry));
        second_entry.checksum = pfs_crc_with_zero32(
            corrupt_second_block, sizeof(second_entry),
            (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
        pfs_copy(corrupt_second_block, &second_entry, sizeof(second_entry));
        if (pfs_write_block(fs.mount.device, second_lba,
                            corrupt_second_block) != PROSENJITFS_OK) {
            (void)pfs_write_block(fs.mount.device, second_lba,
                                  original_second_block);
            return pfs_selftest_fail(__LINE__, -20);
        }
        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));
        if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_ECORRUPT ||
            integrity_report.status != PROSENJITFS_ECORRUPT ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty))) {
            (void)pfs_write_block(fs.mount.device, second_lba,
                                  original_second_block);
            return pfs_selftest_fail(__LINE__, -20);
        }
        if (pfs_write_block(fs.mount.device, second_lba,
                            original_second_block) != PROSENJITFS_OK ||
            prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_OK ||
            integrity_report.status != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -20);
    }


    /*
     * Adversarial namespace regression: two distinct inodes in the same hash
     * bucket are rewritten to carry the same valid name. Recompute the name
     * hash and entry checksum so the integrity checker must detect namespace
     * ambiguity itself, not a malformed checksum or misplaced bucket entry.
     */
    {
        struct pros_vfs_file first_file;
        struct pros_vfs_file second_file;
        struct pros_prosenjitfs_inode root_inode;
        struct pros_prosenjitfs_dir_index root_index;
        struct pros_prosenjitfs_dir_entry first_entry = { 0 };
        struct pros_prosenjitfs_dir_entry second_entry = { 0 };
        static uint8_t original_second_block[4096];
        static uint8_t corrupt_second_block[4096];
        uint64_t first_lba = 0u;
        uint64_t second_lba = 0u;
        uint32_t durable_before;
        uint32_t volatile_before;
        uint32_t dirty_before;

        pfs_zero(&disk, sizeof(disk));
        pfs_crash_inject_stage = 0u;
        pfs_crash_injected = 0u;
        if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
            prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/dupname-6",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &first_file) != PROS_VFS_OK ||
            vfs_file_close(&first_file) != PROS_VFS_OK ||
            vfs_kernel_file_open(&fs.mount, "/dupname-20",
                          PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                          &second_file) != PROS_VFS_OK ||
            vfs_file_close(&second_file) != PROS_VFS_OK ||
            prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                PROSENJITFS_OK ||
            pfs_read_dir_index(&fs, &root_inode, fs.sb.root_inode, &root_index) !=
                PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -20);

        for (uint32_t bucket = 0u;
             bucket < PROSENJITFS_DIR_BUCKETS &&
                 (first_lba == 0u || second_lba == 0u);
             ++bucket) {
            uint64_t current = root_index.heads[bucket];
            uint64_t traversed = 0u;
            while (current != 0u &&
                   (first_lba == 0u || second_lba == 0u)) {
                struct pros_prosenjitfs_dir_entry entry = { 0 };
                uint8_t block[4096];
                uint32_t is_first = 0u;
                uint32_t is_second = 0u;
                if (current < fs.sb.data_start ||
                    current >= fs.sb.total_blocks ||
                    ++traversed > fs.sb.total_blocks ||
                    pfs_read_block(fs.mount.device, current, block) !=
                        PROSENJITFS_OK)
                    return pfs_selftest_fail(__LINE__, -20);
                pfs_copy(&entry, block, sizeof(entry));
                if (entry.name_length == sizeof("dupname-6") - 1u) {
                    is_first = 1u;
                    for (uint32_t ni = 0u; ni < entry.name_length; ++ni)
                        if (entry.name[ni] != (uint8_t)"dupname-6"[ni])
                            is_first = 0u;
                }
                if (entry.name_length == sizeof("dupname-20") - 1u) {
                    is_second = 1u;
                    for (uint32_t ni = 0u; ni < entry.name_length; ++ni)
                        if (entry.name[ni] != (uint8_t)"dupname-20"[ni])
                            is_second = 0u;
                }
                if (is_first) {
                    first_lba = current;
                    first_entry = entry;
                } else if (is_second) {
                    second_lba = current;
                    second_entry = entry;
                    pfs_copy(original_second_block, block, fs.sb.block_size);
                }
                current = entry.next;
            }
        }
        if (first_lba == 0u || second_lba == 0u ||
            first_entry.inode == second_entry.inode ||
            first_entry.name_hash % PROSENJITFS_DIR_BUCKETS !=
                second_entry.name_hash % PROSENJITFS_DIR_BUCKETS ||
            first_entry.name_length == 0u ||
            second_entry.name_length == 0u)
            return pfs_selftest_fail(__LINE__, -20);

        pfs_copy(corrupt_second_block, original_second_block, fs.sb.block_size);
        pfs_zero(second_entry.name, sizeof(second_entry.name));
        pfs_copy(second_entry.name, first_entry.name, first_entry.name_length);
        second_entry.name_length = first_entry.name_length;
        second_entry.name_hash = first_entry.name_hash;
        second_entry.checksum = 0u;
        pfs_copy(corrupt_second_block, &second_entry, sizeof(second_entry));
        second_entry.checksum = pfs_crc_with_zero32(
            corrupt_second_block, sizeof(second_entry),
            (uint32_t)offsetof(struct pros_prosenjitfs_dir_entry, checksum));
        pfs_copy(corrupt_second_block, &second_entry, sizeof(second_entry));
        if (pfs_write_block(fs.mount.device, second_lba,
                            corrupt_second_block) != PROSENJITFS_OK) {
            (void)pfs_write_block(fs.mount.device, second_lba,
                                  original_second_block);
            return pfs_selftest_fail(__LINE__, -20);
        }

        durable_before = prosenjitfs_crc32c(
            disk.data, (uint32_t)sizeof(disk.data));
        volatile_before = prosenjitfs_crc32c(
            disk.volatile_data, (uint32_t)sizeof(disk.volatile_data));
        dirty_before = prosenjitfs_crc32c(
            disk.dirty, (uint32_t)sizeof(disk.dirty));
        if (prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_ECORRUPT ||
            integrity_report.status != PROSENJITFS_ECORRUPT ||
            durable_before != prosenjitfs_crc32c(
                disk.data, (uint32_t)sizeof(disk.data)) ||
            volatile_before != prosenjitfs_crc32c(
                disk.volatile_data, (uint32_t)sizeof(disk.volatile_data)) ||
            dirty_before != prosenjitfs_crc32c(
                disk.dirty, (uint32_t)sizeof(disk.dirty))) {
            (void)pfs_write_block(fs.mount.device, second_lba,
                                  original_second_block);
            return pfs_selftest_fail(__LINE__, -20);
        }
        if (pfs_write_block(fs.mount.device, second_lba,
                            original_second_block) != PROSENJITFS_OK ||
            prosenjitfs_check_integrity_report(&fs, &integrity_report) !=
                PROSENJITFS_OK ||
            integrity_report.status != PROSENJITFS_OK ||
            prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
            return pfs_selftest_fail(__LINE__, -20);
    }


    /*
     * Sweep block-write failures across mkdir's multi-metadata transaction.
     * After simulated power loss, either the complete directory is present
     * or it is absent; integrity must never reveal a partially published
     * namespace or leaked allocation.
     */
    {
        struct pros_prosenjitfs_inode root_inode;
        struct pros_prosenjitfs_inode created_inode;
        struct pros_prosenjitfs_dir_entry dir_entry;
        uint64_t dir_lba = 0u;
        uint64_t previous_lba = 0u;
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;
        const char *dir_name = "mkdir-write-sweep";

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            int mkdir_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -26);

            disk.fail_write_countdown = write_position;
            mkdir_rc = vfs_kernel_mkdir(&fs.mount, "/mkdir-write-sweep");
            if (disk.fail_write_countdown != 0u) {
                /* The requested failure position was beyond mkdir's writes. */
                disk.fail_write_countdown = 0u;
                if (mkdir_rc != PROS_VFS_OK ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -26);
                }
                sweep_finished = 1u;
                break;
            }

            if (mkdir_rc == PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }

            dir_lba = 0u;
            previous_lba = 0u;
            {
                int lookup_rc = pfs_find_dir_entry_lba(
                    &fs, &root_inode, fs.sb.root_inode, dir_name, &dir_entry, &dir_lba,
                    &previous_lba);
                if (lookup_rc != PROSENJITFS_OK &&
                    lookup_rc != PROSENJITFS_ENOENT) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -26);
                }
                if (lookup_rc == PROSENJITFS_OK &&
                    (dir_lba == 0u ||
                     prosenjitfs_read_inode(&fs, dir_entry.inode,
                                            &created_inode) != PROSENJITFS_OK ||
                     created_inode.type != PROSENJITFS_INODE_DIR)) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -26);
                }
            }

            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -26);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -26);
    }

    /*
     * Sweep create-open block-write failures separately from mkdir. A failed
     * create may recover as absent or as a fully initialized empty file, but
     * the namespace/inode/bitmap graph must always pass integrity validation.
     */
    {
        const char *file_name = "create-write-sweep.bin";
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_inode created_inode;
            struct pros_prosenjitfs_dir_entry dir_entry;
            struct pros_vfs_file file;
            uint64_t found_lba = 0u;
            uint64_t previous_lba = 0u;
            int lookup_rc;
            int open_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK)
                return pfs_selftest_fail(__LINE__, -27);

            disk.fail_write_countdown = write_position;
            open_rc = vfs_kernel_file_open(
                &fs.mount, "/create-write-sweep.bin",
                PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file);
            if (open_rc == PROS_VFS_OK &&
                vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -27);
            }

            if (disk.fail_write_countdown != 0u) {
                /* Create completed before the selected write position. */
                disk.fail_write_countdown = 0u;
                if (open_rc != PROS_VFS_OK ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -27);
                }
                sweep_finished = 1u;
                break;
            }

            if (open_rc == PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -27);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -27);
            }

            found_lba = 0u;
            previous_lba = 0u;
            lookup_rc = pfs_find_dir_entry_lba(
                &fs, &root_inode, fs.sb.root_inode, file_name, &dir_entry, &found_lba,
                &previous_lba);
            if (lookup_rc != PROSENJITFS_OK &&
                lookup_rc != PROSENJITFS_ENOENT) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -27);
            }
            if (lookup_rc == PROSENJITFS_OK &&
                (found_lba == 0u ||
                 prosenjitfs_read_inode(&fs, dir_entry.inode,
                                        &created_inode) != PROSENJITFS_OK ||
                 created_inode.type != PROSENJITFS_INODE_FILE ||
                 created_inode.size != 0u)) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -27);
            }

            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -27);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -27);
    }

    /*
     * Sweep failures while overwriting an existing file without changing EOF.
     * Recovery must preserve either the entire old block image or the entire
     * new image; a torn mixture is never an acceptable committed result.
     */
    {
        struct pros_vfs_file file;
        uint8_t old_data[64];
        uint8_t new_data[64];
        uint8_t read_data[64];
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t i = 0u; i < sizeof(old_data); ++i) {
            old_data[i] = (uint8_t)(0x47u ^ (uint8_t)(i * 11u));
            new_data[i] = (uint8_t)(0xA9u + (uint8_t)(i * 17u));
        }

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            uint32_t done = 0u;
            int write_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_file_open(&fs.mount, "/overwrite-write-sweep.bin",
                              PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
                    PROS_VFS_OK ||
                vfs_file_write(&file, old_data, sizeof(old_data), &done) !=
                    PROS_VFS_OK ||
                done != sizeof(old_data) ||
                vfs_file_close(&file) != PROS_VFS_OK)
                return pfs_selftest_fail(__LINE__, -28);

            if (vfs_kernel_file_open(&fs.mount, "/overwrite-write-sweep.bin",
                              PROS_VFS_O_WRITE, &file) != PROS_VFS_OK ||
                vfs_file_seek(&file, 0u) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }
            disk.fail_write_countdown = write_position;
            write_rc = vfs_file_write(&file, new_data, sizeof(new_data), &done);
            if (vfs_file_close(&file) != PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }

            if (disk.fail_write_countdown != 0u) {
                disk.fail_write_countdown = 0u;
                if (write_rc != PROS_VFS_OK || done != sizeof(new_data) ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -28);
                }
                sweep_finished = 1u;
                break;
            }

            if (write_rc == PROS_VFS_OK && done == sizeof(new_data)) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }

            done = 0u;
            if (vfs_kernel_file_open(&fs.mount, "/overwrite-write-sweep.bin",
                              PROS_VFS_O_READ, &file) != PROS_VFS_OK ||
                vfs_file_read(&file, read_data, sizeof(read_data), &done) !=
                    PROS_VFS_OK ||
                vfs_file_close(&file) != PROS_VFS_OK ||
                done != sizeof(old_data)) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }

            {
                uint32_t matches_old = 1u;
                uint32_t matches_new = 1u;
                for (uint32_t i = 0u; i < sizeof(read_data); ++i) {
                    if (read_data[i] != old_data[i])
                        matches_old = 0u;
                    if (read_data[i] != new_data[i])
                        matches_new = 0u;
                }
                if (!matches_old && !matches_new) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -28);
                }
            }

            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -28);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -28);
    }

    /*
     * Sweep block-write failures through rmdir's metadata transaction.
     * After simulated power loss, the empty directory may be present or
     * fully removed, but the namespace and allocation graph must stay valid.
     */
    {
        const char *dir_name = "rmdir-write-sweep";
        uint32_t completed_failures = 0u;
        uint32_t sweep_finished = 0u;

        for (uint32_t write_position = 1u; write_position <= 64u;
             ++write_position) {
            struct pros_prosenjitfs_inode root_inode;
            struct pros_prosenjitfs_inode child_inode;
            struct pros_prosenjitfs_dir_entry dir_entry;
            uint64_t found_lba = 0u;
            uint64_t previous_lba = 0u;
            int rmdir_rc;
            int lookup_rc;

            pfs_zero(&disk, sizeof(disk));
            pfs_crash_inject_stage = 0u;
            pfs_crash_injected = 0u;
            if (prosenjitfs_format(&device) != PROSENJITFS_OK ||
                prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                vfs_kernel_mkdir(&fs.mount, "/rmdir-write-sweep") != PROS_VFS_OK ||
                prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -29);
            }

            disk.fail_write_countdown = write_position;
            rmdir_rc = vfs_kernel_rmdir(&fs.mount, "/rmdir-write-sweep");
            if (disk.fail_write_countdown != 0u) {
                /* The selected failure position was beyond rmdir's writes. */
                disk.fail_write_countdown = 0u;
                if (rmdir_rc != PROS_VFS_OK ||
                    prosenjitfs_read_inode(&fs, fs.sb.root_inode,
                                           &root_inode) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -29);
                }
                lookup_rc = pfs_find_dir_entry_lba(
                    &fs, &root_inode, fs.sb.root_inode, dir_name, &dir_entry, &found_lba,
                    &previous_lba);
                if (lookup_rc != PROSENJITFS_ENOENT ||
                    prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                    prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                    pfs_test_crash_reset(&fs, &disk);
                    return pfs_selftest_fail(__LINE__, -29);
                }
                sweep_finished = 1u;
                break;
            }

            if (rmdir_rc == PROS_VFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -29);
            }

            pfs_test_crash_reset(&fs, &disk);
            if (prosenjitfs_mount(&fs, &device, 0u) != PROSENJITFS_OK ||
                prosenjitfs_read_inode(&fs, fs.sb.root_inode, &root_inode) !=
                    PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -29);
            }

            found_lba = 0u;
            previous_lba = 0u;
            lookup_rc = pfs_find_dir_entry_lba(
                &fs, &root_inode, fs.sb.root_inode, dir_name, &dir_entry, &found_lba,
                &previous_lba);
            if (lookup_rc != PROSENJITFS_OK &&
                lookup_rc != PROSENJITFS_ENOENT) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -29);
            }
            if (lookup_rc == PROSENJITFS_OK &&
                (found_lba == 0u ||
                 prosenjitfs_read_inode(&fs, dir_entry.inode, &child_inode) !=
                    PROSENJITFS_OK ||
                 child_inode.type != PROSENJITFS_INODE_DIR ||
                 child_inode.parent_inode != fs.sb.root_inode)) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -29);
            }

            if (prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK ||
                (found_lba != 0u &&
                 (vfs_kernel_rmdir(&fs.mount, "/rmdir-write-sweep") != PROS_VFS_OK ||
                  prosenjitfs_check_integrity(&fs) != PROSENJITFS_OK)) ||
                prosenjitfs_unmount(&fs) != PROSENJITFS_OK) {
                pfs_test_crash_reset(&fs, &disk);
                return pfs_selftest_fail(__LINE__, -29);
            }
            ++completed_failures;
        }

        if (sweep_finished == 0u || completed_failures == 0u)
            return pfs_selftest_fail(__LINE__, -29);
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
    __atomic_store_n(&fs.io_lock, 1u, __ATOMIC_RELEASE);
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_EBUSY) {
        __atomic_store_n(&fs.io_lock, 0u, __ATOMIC_RELEASE);
        return pfs_selftest_fail(__LINE__, -31);
    }
    __atomic_store_n(&fs.io_lock, 0u, __ATOMIC_RELEASE);
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_OK)
        return pfs_selftest_fail(__LINE__, -11);
    if (prosenjitfs_unmount(&fs) != PROSENJITFS_EINVAL)
        return pfs_selftest_fail(__LINE__, -11);
    return PROSENJITFS_OK;
}
}
