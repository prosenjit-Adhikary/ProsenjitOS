#ifndef PROSENJIT_FAT32_H
#define PROSENJIT_FAT32_H

#include <stdint.h>
#include "vfs.h"

#define PROS_FAT32_OK        0
#define PROS_FAT32_EINVAL   -1
#define PROS_FAT32_EIO      -2
#define PROS_FAT32_ENOTSUP  -3
#define PROS_FAT32_ECORRUPT -4
#define PROS_FAT32_ENOENT   -5

struct pros_fat32 {
    struct pros_vfs_mount mount;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint64_t fat_lba;
    uint64_t data_lba;
    uint32_t cluster_count;
    struct pros_fat32_vfs_node vfs_nodes[PROS_FAT32_VFS_NODE_MAX];
};

struct pros_fat32_dirent {
    char name[261];
    uint8_t attributes;
    uint32_t first_cluster;
    uint32_t size;
};

struct pros_fat32_vfs_node {
    struct pros_vfs_vnode vnode;
    struct pros_fat32_dirent entry;
    uint32_t directory_cluster;
    uint8_t used;
};

#define PROS_FAT32_VFS_NODE_MAX 32u


int fat32_mount(struct pros_fat32 *, struct pros_block_device *);
int fat32_cluster_lba(const struct pros_fat32 *, uint32_t, uint64_t *);
int fat32_read_cluster(const struct pros_fat32 *, uint32_t, void *);
int fat32_next_cluster(const struct pros_fat32 *, uint32_t, uint32_t *);
int fat32_find_83(const struct pros_fat32 *, uint32_t, const char *,
                  struct pros_fat32_dirent *);
int fat32_find(const struct pros_fat32 *, uint32_t, const char *,
               struct pros_fat32_dirent *);
int fat32_read_file(const struct pros_fat32 *, const struct pros_fat32_dirent *,
                    uint32_t, uint32_t, void *);
int fat32_open_83(struct pros_fat32 *, uint32_t, const char *, struct pros_vfs_file *,
                  struct pros_fat32_dirent *);
int fat32_vfs_mount(struct pros_fat32 *);
int fat32_selftest(void);

#endif
