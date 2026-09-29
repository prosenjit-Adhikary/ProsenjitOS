#include "../include/prosenjitfs_format.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PFS_MIN_BLOCKS 256u
#define PFS_MIN_JOURNAL 16u
#define PFS_MAX_JOURNAL 64u
#define PFS_MIN_INODES 64u
#define PFS_MAX_INODES 1024u
#define PFS_UUID_SEED 0x50524F53454E4A49ULL

static void zero_bytes(void *p, size_t n)
{
    memset(p, 0, n);
}

static uint32_t crc32c_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    return crc;
}

static uint32_t crc32c_zero32(const void *data, size_t len, size_t off)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        uint8_t v = (i >= off && i < off + 4u) ? 0u : p[i];
        crc ^= v;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    return ~crc;
}

static int write_block(int fd, uint64_t block, uint32_t block_size,
                       const void *buf)
{
    off_t off;
    ssize_t n;
    if (block > (uint64_t)INT64_MAX / block_size)
        return -1;
    off = (off_t)(block * block_size);
    n = pwrite(fd, buf, block_size, off);
    return n == (ssize_t)block_size ? 0 : -1;
}

static void make_uuid(uint8_t uuid[16], uint64_t blocks, uint32_t block_size)
{
    uint64_t a = PFS_UUID_SEED ^ blocks ^ ((uint64_t)block_size << 32);
    uint64_t b = 0x9E3779B97F4A7C15ULL ^ (blocks << 7) ^ block_size;
    for (unsigned i = 0; i < 16; ++i) {
        a ^= a << 13; a ^= a >> 7; a ^= a << 17;
        b ^= b << 17; b ^= b >> 9; b ^= b << 8;
        uuid[i] = (uint8_t)(a ^ b);
    }
    uuid[6] = (uint8_t)((uuid[6] & 0x0Fu) | 0x40u);
    uuid[8] = (uint8_t)((uuid[8] & 0x3Fu) | 0x80u);
}

static int set_bitmap_range(uint8_t *bitmap, uint32_t bytes,
                            uint64_t start, uint64_t count,
                            uint64_t bitmap_base)
{
    uint64_t bits = (uint64_t)bytes * 8u;
    if (start < bitmap_base || start + count > bitmap_base + bits)
        return -1;
    for (uint64_t b = start; b < start + count; ++b) {
        uint64_t local = b - bitmap_base;
        bitmap[local >> 3] |= (uint8_t)(1u << (local & 7u));
    }
    return 0;
}

static int write_bitmap_region(int fd, uint64_t start, uint32_t blocks,
                               uint64_t region_start, uint64_t region_blocks,
                               uint32_t block_size, uint64_t total_blocks)
{
    uint8_t *bitmap = calloc(1u, block_size);
    uint64_t bits_per_block = (uint64_t)block_size * 8u;
    if (!bitmap)
        return -1;

    for (uint32_t i = 0; i < blocks; ++i) {
        uint64_t base = (uint64_t)i * bits_per_block;
        uint64_t first = region_start > base ? region_start : base;
        uint64_t region_end = region_start + region_blocks;
        uint64_t bitmap_end = base + bits_per_block;
        uint64_t last = region_end < bitmap_end ? region_end : bitmap_end;
        if (first < last &&
            set_bitmap_range(bitmap, block_size, first, last - first, base) != 0) {
            free(bitmap);
            return -1;
        }
        if (write_block(fd, start + i, block_size, bitmap) != 0) {
            free(bitmap);
            return -1;
        }
        zero_bytes(bitmap, block_size);
    }

    (void)total_blocks;
    free(bitmap);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <image> <size-mib> [block-size]\n"
            "  block-size: 512, 1024, 2048 or 4096 (default 512)\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *path;
    uint64_t size_mib;
    uint64_t total_blocks;
    uint32_t block_size = 512u;
    uint32_t journal_blocks;
    uint32_t bitmap_blocks;
    uint32_t inode_bitmap_blocks;
    uint32_t inode_count;
    uint32_t inode_table_blocks;
    uint64_t cursor;
    int fd = -1;
    uint8_t *block = NULL;
    struct pros_prosenjitfs_superblock sb;
    struct pros_prosenjitfs_inode root;
    struct pros_prosenjitfs_dir_index root_index;

    if (argc < 3 || argc > 4) {
        usage(argv[0]);
        return 2;
    }
    path = argv[1];
    errno = 0;
    size_mib = strtoull(argv[2], NULL, 10);
    if (errno || size_mib == 0u ||
        (argc == 4 && (sscanf(argv[3], "%" SCNu32, &block_size) != 1))) {
        usage(argv[0]);
        return 2;
    }
    if (block_size < 512u || block_size > 4096u ||
        (block_size & (block_size - 1u)) != 0u ||
        size_mib > UINT64_MAX / (1024u * 1024u)) {
        fprintf(stderr, "invalid image size or block size\n");
        return 2;
    }

    {
        uint64_t bytes = size_mib * 1024u * 1024u;
        if (bytes % block_size != 0u) {
            fprintf(stderr, "image size must be divisible by block size\n");
            return 2;
        }
        total_blocks = bytes / block_size;
    }
    if (total_blocks < PFS_MIN_BLOCKS) {
        fprintf(stderr, "image must contain at least %" PRIu32 " blocks\n",
                PFS_MIN_BLOCKS);
        return 2;
    }

    journal_blocks = (uint32_t)(total_blocks / 16u);
    if (journal_blocks < PFS_MIN_JOURNAL) journal_blocks = PFS_MIN_JOURNAL;
    if (journal_blocks > PFS_MAX_JOURNAL) journal_blocks = PFS_MAX_JOURNAL;

    bitmap_blocks = (uint32_t)((total_blocks +
                     (uint64_t)block_size * 8u - 1u) /
                    ((uint64_t)block_size * 8u));
    inode_count = (uint32_t)(total_blocks / 32u);
    if (inode_count < PFS_MIN_INODES) inode_count = PFS_MIN_INODES;
    if (inode_count > PFS_MAX_INODES) inode_count = PFS_MAX_INODES;
    inode_bitmap_blocks = (uint32_t)(((uint64_t)inode_count +
                          (uint64_t)block_size * 8u - 1u) /
                         ((uint64_t)block_size * 8u));
    inode_table_blocks = (uint32_t)(((uint64_t)inode_count *
                          PROSENJITFS_INODE_SIZE + block_size - 1u) /
                         block_size);

    cursor = 2u;
    if (cursor + journal_blocks + bitmap_blocks + inode_bitmap_blocks +
        inode_table_blocks + 1u >= total_blocks) {
        fprintf(stderr, "image is too small for ProsenjitFS metadata\n");
        return 2;
    }

    zero_bytes(&sb, sizeof(sb));
    sb.magic[0] = 'P'; sb.magic[1] = 'J'; sb.magic[2] = 'S';
    sb.magic[3] = 'F'; sb.magic[4] = 'S'; sb.magic[5] = '0';
    sb.magic[6] = '1'; sb.magic[7] = 0;
    sb.version = PROSENJITFS_VERSION;
    sb.header_size = sizeof(sb);
    sb.features = PROSENJITFS_FEATURE_CRC32C |
                  PROSENJITFS_FEATURE_JOURNAL |
                  PROSENJITFS_FEATURE_EXTENTS |
                  PROSENJITFS_FEATURE_UTF8;
    make_uuid(sb.uuid, total_blocks, block_size);
    sb.block_size = block_size;
    sb.total_blocks = total_blocks;
    sb.journal_start = cursor;
    sb.journal_blocks = journal_blocks;
    cursor += journal_blocks;
    sb.block_bitmap_start = cursor;
    sb.block_bitmap_blocks = bitmap_blocks;
    cursor += bitmap_blocks;
    sb.inode_bitmap_start = cursor;
    sb.inode_bitmap_blocks = inode_bitmap_blocks;
    cursor += inode_bitmap_blocks;
    sb.inode_table_start = cursor;
    sb.inode_table_blocks = inode_table_blocks;
    cursor += inode_table_blocks;
    sb.data_start = cursor + 1u;
    sb.inode_size = PROSENJITFS_INODE_SIZE;
    sb.root_inode = 0u;
    sb.inode_count = inode_count;
    sb.max_name = PROSENJITFS_MAX_NAME;
    sb.transaction_sequence = 1u;

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    if (ftruncate(fd, (off_t)(total_blocks * block_size)) != 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    block = calloc(1u, block_size);
    if (!block) {
        fprintf(stderr, "out of memory\n");
        close(fd);
        return 1;
    }

    memcpy(block, &sb, sizeof(sb));
    sb.checksum = crc32c_zero32(block, sb.header_size,
                                offsetof(struct pros_prosenjitfs_superblock,
                                         checksum));
    memcpy(block, &sb, sizeof(sb));
    if (write_block(fd, 0u, block_size, block) != 0 ||
        write_block(fd, 1u, block_size, block) != 0) {
        perror("write superblock");
        goto fail;
    }

    zero_bytes(block, block_size);
    if (write_bitmap_region(fd, sb.block_bitmap_start,
                            sb.block_bitmap_blocks, 0u, sb.data_start,
                            block_size, total_blocks) != 0)
        goto fail;

    zero_bytes(block, block_size);
    block[0] = 1u;
    if (write_block(fd, sb.inode_bitmap_start, block_size, block) != 0)
        goto fail;
    for (uint32_t i = 1u; i < sb.inode_bitmap_blocks; ++i)
        if (write_block(fd, sb.inode_bitmap_start + i, block_size,
                        (uint8_t[4096]){0}) != 0)
            goto fail;

    zero_bytes(block, block_size);
    if (write_block(fd, sb.inode_table_start, block_size, block) != 0)
        goto fail;

    zero_bytes(&root, sizeof(root));
    root.type = PROSENJITFS_INODE_DIR;
    root.mode = 0755u;
    root.links = 1u;
    root.generation = 1u;
    root.parent_inode = 0u;
    root.dir_index = sb.data_start - 1u;
    {
        uint8_t *inode_block = calloc(1u, block_size);
        if (!inode_block)
            goto fail;
        memcpy(inode_block, &root, sizeof(root));
        root.checksum = crc32c_zero32(inode_block, sizeof(root),
                                      offsetof(struct pros_prosenjitfs_inode,
                                               checksum));
        memcpy(inode_block, &root, sizeof(root));
        if (write_block(fd, sb.inode_table_start, block_size,
                        inode_block) != 0) {
            free(inode_block);
            goto fail;
        }
        free(inode_block);
    }

    zero_bytes(&root_index, sizeof(root_index));
    root_index.magic = PROSENJITFS_DIR_MAGIC;
    root_index.version = 1u;
    root_index.bucket_count = PROSENJITFS_DIR_BUCKETS;
    root_index.inode = 0u;
    root_index.generation = 1u;
    {
        uint8_t *index_block = calloc(1u, block_size);
        if (!index_block)
            goto fail;
        memcpy(index_block, &root_index, sizeof(root_index));
        root_index.checksum = crc32c_zero32(
            index_block, sizeof(root_index),
            offsetof(struct pros_prosenjitfs_dir_index, checksum));
        memcpy(index_block, &root_index, sizeof(root_index));
        if (write_block(fd, sb.data_start - 1u, block_size,
                        index_block) != 0) {
            free(index_block);
            goto fail;
        }
        free(index_block);
    }

    if (fsync(fd) != 0) {
        perror("fsync");
        goto fail;
    }
    free(block);
    close(fd);
    printf("formatted %s: %" PRIu64 " blocks x %" PRIu32 " bytes\n",
           path, total_blocks, block_size);
    return 0;

fail:
    perror("mkprosenjitfs");
    free(block);
    close(fd);
    return 1;
}
