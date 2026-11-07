#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "block.h"
#include "heap.h"
#include "prosenjitfs.h"

#define SECTOR_SIZE 512u
#define EXPECTED_PFS_LBA 2048u
#define EXPECTED_PFS_SECTORS 512u
#define MBR_PARTITION_TABLE_OFFSET 446u
#define MBR_PARTITION_ENTRY_SIZE 16u

struct image_context {
    int fd;
    uint64_t first_lba;
    uint64_t sector_count;
};

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int transfer_image(struct pros_block_device *device, uint64_t lba,
                          uint32_t count, void *buffer, int is_write)
{
    struct image_context *ctx;
    uint64_t absolute_lba;
    uint64_t offset;
    size_t bytes;
    size_t done = 0u;

    if (!device || !buffer || count == 0u || !device->private_data)
        return PROS_BLOCK_EINVAL;
    ctx = (struct image_context *)device->private_data;
    if (lba >= ctx->sector_count || (uint64_t)count > ctx->sector_count - lba ||
        ctx->first_lba > UINT64_MAX - lba)
        return PROS_BLOCK_EINVAL;
    absolute_lba = ctx->first_lba + lba;
    if (absolute_lba > (uint64_t)INT64_MAX / SECTOR_SIZE)
        return PROS_BLOCK_EINVAL;
    offset = absolute_lba * SECTOR_SIZE;
    bytes = (size_t)count * SECTOR_SIZE;
    if ((uint64_t)bytes > (uint64_t)INT64_MAX - offset)
        return PROS_BLOCK_EINVAL;

    while (done < bytes) {
        ssize_t n = is_write
            ? pwrite(ctx->fd, (const uint8_t *)buffer + done, bytes - done,
                     (off_t)(offset + done))
            : pread(ctx->fd, (uint8_t *)buffer + done, bytes - done,
                    (off_t)(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return PROS_BLOCK_EIO;
        done += (size_t)n;
    }
    return PROS_BLOCK_OK;
}

static int image_read(struct pros_block_device *device, uint64_t lba,
                      uint32_t count, void *buffer)
{
    return transfer_image(device, lba, count, buffer, 0);
}

static int image_write(struct pros_block_device *device, uint64_t lba,
                       uint32_t count, const void *buffer)
{
    return transfer_image(device, lba, count, (void *)buffer, 1);
}

static int image_flush(struct pros_block_device *device)
{
    struct image_context *ctx;
    if (!device || !device->private_data)
        return PROS_BLOCK_EINVAL;
    ctx = (struct image_context *)device->private_data;
    return fsync(ctx->fd) == 0 ? PROS_BLOCK_OK : PROS_BLOCK_EIO;
}

int block_device_init(struct pros_block_device *device, uint32_t sector_size,
                      uint64_t sector_count, pros_block_read_fn read_fn,
                      pros_block_write_fn write_fn, void *private_data)
{
    if (!device || sector_size != SECTOR_SIZE || sector_count == 0u ||
        !read_fn)
        return PROS_BLOCK_EINVAL;
    device->sector_size = sector_size;
    device->sector_count = sector_count;
    device->read = read_fn;
    device->write = write_fn;
    device->flush = 0;
    device->private_data = private_data;
    return PROS_BLOCK_OK;
}

int block_device_read(struct pros_block_device *device, uint64_t lba,
                      uint32_t count, void *buffer)
{
    if (!device || !device->read || !buffer || count == 0u ||
        lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba)
        return PROS_BLOCK_EINVAL;
    return device->read(device, lba, count, buffer);
}

int block_device_write(struct pros_block_device *device, uint64_t lba,
                       uint32_t count, const void *buffer)
{
    if (!device || !device->write || !buffer || count == 0u ||
        lba >= device->sector_count ||
        (uint64_t)count > device->sector_count - lba)
        return PROS_BLOCK_EINVAL;
    return device->write(device, lba, count, buffer);
}

int block_device_set_flush(struct pros_block_device *device,
                           pros_block_flush_fn flush_fn)
{
    if (!device || !flush_fn)
        return PROS_BLOCK_EINVAL;
    device->flush = flush_fn;
    return PROS_BLOCK_OK;
}

int block_device_flush(struct pros_block_device *device)
{
    if (!device)
        return PROS_BLOCK_EINVAL;
    return device->flush ? device->flush(device) : PROS_BLOCK_OK;
}

int block_device_lba_add(const struct pros_block_device *device, uint64_t base,
                         uint64_t offset, uint64_t *result)
{
    if (!device || !result || base >= device->sector_count ||
        offset > UINT64_MAX - base || base + offset >= device->sector_count)
        return PROS_BLOCK_EINVAL;
    *result = base + offset;
    return PROS_BLOCK_OK;
}

void *kmalloc(uint64_t size)
{
    return malloc(size ? (size_t)size : 1u);
}

void kfree(void *address)
{
    free(address);
}

int main(int argc, char **argv)
{
    int fd;
    struct stat path_st;
    struct stat st;
    uint8_t mbr[SECTOR_SIZE];
    struct image_context ctx;
    struct pros_block_device partition;
    uint64_t image_sectors;
    int found = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s native-disk-image\n", argv[0]);
        return 2;
    }
    /*
     * This host utility is image-only. Reject special files before open,
     * then recheck the opened descriptor to defend against path races.
     */
    if (lstat(argv[1], &path_st) != 0) {
        perror("stat native disk image");
        return 1;
    }
    if (!S_ISREG(path_st.st_mode)) {
        fprintf(stderr, "refusing to format a non-regular file; use a disposable disk image\n");
        return 1;
    }
    fd = open(argv[1], O_RDWR | O_NOFOLLOW);
    if (fd < 0) {
        perror("open native disk image");
        return 1;
    }
    if (fstat(fd, &st) != 0 || st.st_size < 0 ||
        ((uint64_t)st.st_size % SECTOR_SIZE) != 0u) {
        fprintf(stderr, "invalid native disk image size\n");
        close(fd);
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "refusing to format a non-regular file; use a disposable disk image\n");
        close(fd);
        return 1;
    }
    image_sectors = (uint64_t)st.st_size / SECTOR_SIZE;
    if (image_sectors < EXPECTED_PFS_LBA + EXPECTED_PFS_SECTORS ||
        pread(fd, mbr, sizeof(mbr), 0) != (ssize_t)sizeof(mbr) ||
        mbr[510] != 0x55u || mbr[511] != 0xAAu) {
        fprintf(stderr, "native disk has no valid MBR boot sector\n");
        close(fd);
        return 1;
    }

    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint8_t *entry = &mbr[MBR_PARTITION_TABLE_OFFSET +
                                    i * MBR_PARTITION_ENTRY_SIZE];
        uint8_t status = entry[0];
        uint8_t type = entry[4];
        uint32_t first_lba = read_le32(entry + 8u);
        uint32_t sectors = read_le32(entry + 12u);
        uint64_t partition_end;

        if (type == 0u && first_lba == 0u && sectors == 0u)
            continue;
        if ((status != 0u && status != 0x80u) || type == 0u ||
            first_lba == 0u || sectors == 0u) {
            fprintf(stderr, "malformed MBR partition entry %u\n", i);
            close(fd);
            return 1;
        }
        partition_end = (uint64_t)first_lba + sectors;
        if (partition_end > image_sectors) {
            fprintf(stderr, "MBR partition entry %u exceeds the image\n", i);
            close(fd);
            return 1;
        }

        if (type == 0xDAu) {
            if (first_lba != EXPECTED_PFS_LBA ||
                sectors != EXPECTED_PFS_SECTORS || found) {
                fprintf(stderr, "unexpected or duplicate ProsenjitFS partition\n");
                close(fd);
                return 1;
            }
            found = 1;
            continue;
        }

        /* Never format a ProsenjitFS range that aliases another partition. */
        if ((uint64_t)first_lba < EXPECTED_PFS_LBA + EXPECTED_PFS_SECTORS &&
            partition_end > EXPECTED_PFS_LBA) {
            fprintf(stderr, "MBR partition entry %u overlaps the ProsenjitFS target\n", i);
            close(fd);
            return 1;
        }
    }
    if (!found) {
        fprintf(stderr, "expected ProsenjitFS partition (type 0xDA, LBA %u, %u sectors) missing\n",
                EXPECTED_PFS_LBA, EXPECTED_PFS_SECTORS);
        close(fd);
        return 1;
    }

    ctx.fd = fd;
    ctx.first_lba = EXPECTED_PFS_LBA;
    ctx.sector_count = EXPECTED_PFS_SECTORS;
    if (block_device_init(&partition, SECTOR_SIZE, ctx.sector_count,
                          image_read, image_write, &ctx) != PROS_BLOCK_OK ||
        block_device_set_flush(&partition, image_flush) != PROS_BLOCK_OK) {
        fprintf(stderr, "failed to initialize native image block view\n");
        close(fd);
        return 1;
    }

    int rc = prosenjitfs_format(&partition);
    if (rc != PROSENJITFS_OK) {
        fprintf(stderr, "ProsenjitFS format failed: %d\n", rc);
        close(fd);
        return 1;
    }

    struct pros_prosenjitfs_superblock sb;
    if (block_device_read(&partition, 0u, 1u, mbr) != PROS_BLOCK_OK) {
        fprintf(stderr, "cannot read formatted ProsenjitFS superblock\n");
        close(fd);
        return 1;
    }
    memcpy(&sb, mbr, sizeof(sb));
    if (memcmp(sb.magic, PROSENJITFS_MAGIC, PROSENJITFS_MAGIC_LEN) != 0 ||
        sb.version != PROSENJITFS_VERSION ||
        sb.block_size != SECTOR_SIZE ||
        sb.total_blocks != EXPECTED_PFS_SECTORS ||
        sb.root_inode != 0u) {
        fprintf(stderr, "formatted ProsenjitFS superblock validation failed\n");
        close(fd);
        return 1;
    }
    if (fsync(fd) != 0) {
        perror("fsync native disk image");
        close(fd);
        return 1;
    }
    printf("Formatted native ProsenjitFS partition: LBA %u + %u sectors\n",
           EXPECTED_PFS_LBA, EXPECTED_PFS_SECTORS);
    return close(fd) == 0 ? 0 : 1;
}
