#ifndef PROSENJIT_VFS_H
#define PROSENJIT_VFS_H

#include <stdint.h>
#include "block.h"

#define PROS_VFS_OK       0
#define PROS_VFS_EINVAL  -1
#define PROS_VFS_ENOSYS  -2
#define PROS_VFS_EBUSY   -3

enum pros_filesystem_type {
    PROS_FS_UNKNOWN = 0,
    PROS_FS_FAT32,
    PROS_FS_EXFAT,
    PROS_FS_NTFS,
    PROS_FS_PROSENJIT
};

struct pros_vfs_mount {
    enum pros_filesystem_type type;
    struct pros_block_device *device;
    void *private_data;
    uint8_t read_only;
    uint8_t active;
};

int vfs_mount_init(struct pros_vfs_mount *, enum pros_filesystem_type,
                   struct pros_block_device *, void *, uint8_t);
int vfs_mount_activate(struct pros_vfs_mount *);
int vfs_mount_deactivate(struct pros_vfs_mount *);
int vfs_mount_is_read_only(const struct pros_vfs_mount *);

#endif
