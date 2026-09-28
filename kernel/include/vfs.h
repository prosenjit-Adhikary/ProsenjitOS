#ifndef PROSENJIT_VFS_H
#define PROSENJIT_VFS_H

#include <stdint.h>
#include "block.h"
#include "security_admission.h"

#define PROS_VFS_OK       0
#define PROS_VFS_EINVAL  -1
#define PROS_VFS_ENOSYS  -2
#define PROS_VFS_EBUSY   -3
#define PROS_VFS_ENOENT  -4
#define PROS_VFS_EIO     -5

enum pros_filesystem_type {
    PROS_FS_UNKNOWN = 0,
    PROS_FS_FAT32,
    PROS_FS_EXFAT,
    PROS_FS_NTFS,
    PROS_FS_PROSENJIT
};

struct pros_vfs_file;

struct pros_vfs_file_ops {
    int (*read)(struct pros_vfs_file *, uint32_t, uint32_t, void *);
    int (*close)(struct pros_vfs_file *);
};

struct pros_vfs_file {
    struct pros_vfs_mount *mount;
    const struct pros_vfs_file_ops *ops;
    void *private_data;
    uint64_t position;
    uint32_t flags;
    uint8_t open;
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
int vfs_file_init(struct pros_vfs_file *, struct pros_vfs_mount *,
                  const struct pros_vfs_file_ops *, void *, uint32_t);
int vfs_file_read(struct pros_vfs_file *, void *, uint32_t, uint32_t *);
int vfs_file_close(struct pros_vfs_file *);

/*
 * Filesystem-neutral security boundary.
 *
 * A filesystem driver supplies the bytes and evidence it has established;
 * the VFS routes them through the kernel admission broker. This keeps the
 * security policy independent of FAT32/exFAT/NTFS/native filesystem code
 * without inventing filesystem-specific file APIs before those drivers exist.
 */
int vfs_file_admit(const struct pros_vfs_mount *,
                   const uint8_t *data,
                   uint64_t length,
                   const struct pros_file_security_metadata *,
                   struct pros_file_admission_record *);

int vfs_security_selftest(void);

#endif
