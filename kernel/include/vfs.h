#ifndef PROSENJIT_VFS_H
#define PROSENJIT_VFS_H

#include <stdint.h>
#include "block.h"
#include "security_admission.h"
#include "partition.h"

#define PROS_VFS_OK       0
#define PROS_VFS_EINVAL  -1
#define PROS_VFS_ENOSYS  -2
#define PROS_VFS_EBUSY   -3
#define PROS_VFS_ENOENT  -4
#define PROS_VFS_EIO     -5
#define PROS_VFS_NAME_MAX 255u
#define PROS_VFS_PATH_MAX 4096u
#define PROS_VFS_EOVERFLOW -6
#define PROS_VFS_ENOTDIR -7
#define PROS_VFS_EISDIR -8
#define PROS_VFS_EPERM -9
#define PROS_VFS_ELOOP -10
#define PROS_VFS_EEXIST -11
#define PROS_VFS_ENOTEMPTY -12
#define PROS_VFS_ENOTSUP -13
#define PROS_VFS_ENOSPC -14
#define PROS_VFS_ECORRUPT -15
#define PROS_VFS_ESTALE  -16
#define PROS_VFS_O_READ  (1u << 0)
#define PROS_VFS_O_WRITE (1u << 1)
#define PROS_VFS_O_DIRECTORY (1u << 2)
#define PROS_VFS_O_CREATE    (1u << 3)
#define PROS_VFS_O_APPEND    (1u << 4)

enum pros_filesystem_type {
    PROS_FS_UNKNOWN = 0,
    PROS_FS_PROSENJIT
};

struct pros_vfs_file;
struct pros_vfs_vnode;
struct pros_vfs_dir;

enum pros_vfs_node_type {
    PROS_VFS_NODE_UNKNOWN = 0,
    PROS_VFS_NODE_FILE,
    PROS_VFS_NODE_DIRECTORY
};

struct pros_vfs_dirent {
    char name[PROS_VFS_NAME_MAX + 1u];
    enum pros_vfs_node_type type;
    uint64_t size;
};

struct pros_vfs_vnode_ops {
    int (*lookup)(struct pros_vfs_vnode *, const char *,
                  struct pros_vfs_vnode **);
    int (*readdir)(struct pros_vfs_vnode *, uint64_t *,
                   struct pros_vfs_dirent *);
    int (*create)(struct pros_vfs_vnode *, const char *,
                  struct pros_vfs_vnode **);
    int (*mkdir)(struct pros_vfs_vnode *, const char *,
                 struct pros_vfs_vnode **);
    int (*rmdir)(struct pros_vfs_vnode *, const char *);
    int (*unlink)(struct pros_vfs_vnode *, const char *);
    int (*rename)(struct pros_vfs_vnode *, const char *,
                  struct pros_vfs_vnode *, const char *);
    void (*release)(struct pros_vfs_vnode *);
};

struct pros_vfs_file_ops {
    int (*read)(struct pros_vfs_file *, uint64_t, uint32_t, void *);
    int (*write)(struct pros_vfs_file *, uint64_t, uint32_t, const void *);
    int (*truncate)(struct pros_vfs_file *, uint64_t);
    int (*close)(struct pros_vfs_file *);
};

struct pros_vfs_vnode {
    struct pros_vfs_mount *mount;
    struct pros_vfs_vnode *parent;
    const struct pros_vfs_vnode_ops *ops;
    const struct pros_vfs_file_ops *file_ops;
    void *private_data;
    enum pros_vfs_node_type type;
    uint64_t size;
    uint32_t refcount;
    uint8_t initialized;
};

struct pros_vfs_file {
    struct pros_vfs_mount *mount;
    struct pros_vfs_vnode *vnode;
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
    struct pros_vfs_vnode *root;
    uint8_t active;
};

int vfs_mount_init(struct pros_vfs_mount *, enum pros_filesystem_type,
                   struct pros_block_device *, void *, uint8_t);
int vfs_mount_partition(struct pros_vfs_mount *, enum pros_filesystem_type,
                        struct pros_partition_device *,
                        struct pros_block_device *,
                        const struct pros_partition *, void *, uint8_t);
int vfs_probe_filesystem(struct pros_block_device *, enum pros_filesystem_type *);
int vfs_mount_set_root(struct pros_vfs_mount *, struct pros_vfs_vnode *);
int vfs_mount_activate(struct pros_vfs_mount *);
int vfs_mount_deactivate(struct pros_vfs_mount *);
int vfs_mount_is_read_only(const struct pros_vfs_mount *);
int vfs_vnode_init(struct pros_vfs_vnode *, struct pros_vfs_mount *,
                   struct pros_vfs_vnode *, enum pros_vfs_node_type,
                   const struct pros_vfs_vnode_ops *,
                   const struct pros_vfs_file_ops *, void *, uint64_t);
int vfs_vnode_get(struct pros_vfs_vnode *);
int vfs_vnode_put(struct pros_vfs_vnode *);
int vfs_path_lookup(struct pros_vfs_mount *, const char *,
                    struct pros_vfs_vnode **);
int vfs_unlink(struct pros_vfs_mount *, const char *);
int vfs_mkdir(struct pros_vfs_mount *, const char *);
int vfs_rmdir(struct pros_vfs_mount *, const char *);
int vfs_rename(struct pros_vfs_mount *, const char *, const char *);
int vfs_file_open(struct pros_vfs_mount *, const char *, uint32_t,
                  struct pros_vfs_file *);
int vfs_dir_open(struct pros_vfs_mount *, const char *, struct pros_vfs_dir *);
int vfs_dir_read(struct pros_vfs_dir *, struct pros_vfs_dirent *);
int vfs_dir_close(struct pros_vfs_dir *);

struct pros_vfs_dir {
    struct pros_vfs_vnode *vnode;
    uint64_t cookie;
    uint8_t open;
};

int vfs_file_init(struct pros_vfs_file *, struct pros_vfs_mount *,
                  const struct pros_vfs_file_ops *, void *, uint32_t);
int vfs_file_read(struct pros_vfs_file *, void *, uint32_t, uint32_t *);
int vfs_file_write(struct pros_vfs_file *, const void *, uint32_t, uint32_t *);
int vfs_file_truncate(struct pros_vfs_file *, uint64_t);
int vfs_file_seek(struct pros_vfs_file *, uint64_t);
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

/* Filesystem/VFS observability counters consumed by the health monitor. */
uint64_t vfs_health_mount_count(void);
uint64_t vfs_health_unmount_count(void);
uint64_t vfs_health_read_count(void);
uint64_t vfs_health_write_count(void);
uint64_t vfs_health_truncate_count(void);
uint64_t vfs_health_unlink_count(void);
uint64_t vfs_health_mkdir_count(void);
uint64_t vfs_health_rmdir_count(void);
uint64_t vfs_health_rename_count(void);
uint64_t vfs_health_dir_open_count(void);
uint64_t vfs_health_dir_read_count(void);
uint64_t vfs_health_lookup_error_count(void);
uint64_t vfs_health_io_error_count(void);
uint64_t vfs_health_permission_error_count(void);
uint64_t vfs_health_corruption_error_count(void);

#endif
