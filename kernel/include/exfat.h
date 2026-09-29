#ifndef PROSENJIT_EXFAT_H
#define PROSENJIT_EXFAT_H

#include <stdint.h>
#include "vfs.h"

#define PROS_EXFAT_OK        0
#define PROS_EXFAT_EINVAL   -1
#define PROS_EXFAT_EIO      -2
#define PROS_EXFAT_ENOTSUP  -3
#define PROS_EXFAT_ECORRUPT -4
#define PROS_EXFAT_ENOENT   -5

struct pros_exfat_dirent {
    char name[256];
    uint8_t attributes;
    uint8_t no_fat_chain;
    uint32_t first_cluster;
    uint64_t valid_data_length;
    uint64_t size;
};

struct pros_exfat {
    struct pros_vfs_mount mount;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint64_t volume_length;
};

int exfat_mount(struct pros_exfat *, struct pros_block_device *);
int exfat_cluster_lba(const struct pros_exfat *, uint32_t, uint64_t *);
int exfat_read_cluster(const struct pros_exfat *, uint32_t, void *);

/*
 * Read the active FAT entry for a cluster.  The returned value is either
 * another valid cluster, 0xFFFFFFF7 (bad cluster), or 0xFFFFFFFF (EOC).
 */
int exfat_read_fat_entry(const struct pros_exfat *, uint32_t, uint32_t *);

/*
 * Read a file data stream without allocating a filesystem-sized buffer.
 * no_fat_chain follows the exFAT Stream Extension NoFatChain flag.
 * Bytes beyond valid_data_length are returned as zeroes.
 */
int exfat_file_read(const struct pros_exfat *, uint32_t first_cluster,
                    uint8_t no_fat_chain, uint64_t data_length,
                    uint64_t valid_data_length, uint64_t offset,
                    void *buffer, uint32_t length, uint32_t *done);

int exfat_find(const struct pros_exfat *, uint32_t, const char *,
               struct pros_exfat_dirent *);
int exfat_selftest(void);

#endif
