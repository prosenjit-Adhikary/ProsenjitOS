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
    PROS_FS_PROSENJIT,
    /* Filesystem implemented by a registered compatibility provider. */
    PROS_FS_PROVIDER
};

struct pros_vfs_file;
struct pros_vfs_vnode;
struct pros_vfs_dir;
struct pros_vfs_provider;

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

#define PROS_VFS_ACCESS_EXECUTE 0x01u
#define PROS_VFS_ACCESS_WRITE   0x02u
#define PROS_VFS_ACCESS_READ    0x04u
#define PROS_VFS_METADATA_MODE  0x01u
#define PROS_VFS_METADATA_OWNER 0x02u

struct pros_vfs_metadata {
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint8_t reserved[2];
};

struct pros_vfs_credentials {
    uint32_t uid;
    uint32_t gid;
    uint8_t valid;
    uint8_t privileged;
    uint8_t reserved[2];
};

/*
 * lookup ownership contract: on PROS_VFS_OK, the provider must return a
 * non-NULL vnode and transfer exactly one owned reference through *result.
 * On every error, the provider must leave *result NULL and transfer no
 * reference. The VFS never releases an output pointer returned with an error.
 */
struct pros_vfs_vnode_ops {
    int (*lookup)(struct pros_vfs_vnode *, const char *,
                  struct pros_vfs_vnode **);
    int (*readdir)(struct pros_vfs_vnode *, uint64_t *,
                   struct pros_vfs_dirent *);
    int (*create)(struct pros_vfs_vnode *, const char *,
                  struct pros_vfs_vnode **,
                  const struct pros_vfs_credentials *);
    int (*mkdir)(struct pros_vfs_vnode *, const char *,
                 struct pros_vfs_vnode **,
                 const struct pros_vfs_credentials *);
    int (*rmdir)(struct pros_vfs_vnode *, const char *,
                 const struct pros_vfs_credentials *);
    int (*unlink)(struct pros_vfs_vnode *, const char *,
                  const struct pros_vfs_credentials *);
    int (*rename)(struct pros_vfs_vnode *, const char *,
                  struct pros_vfs_vnode *, const char *,
                  const struct pros_vfs_credentials *);
    void (*release)(struct pros_vfs_vnode *);
    /* Persistent owner/group/mode from the authoritative filesystem inode. */
    int (*get_metadata)(struct pros_vfs_vnode *, struct pros_vfs_metadata *);
    /* Persist a validated metadata update; unsupported providers leave it NULL. */
    int (*set_metadata)(struct pros_vfs_vnode *, const struct pros_vfs_metadata *,
                        uint8_t fields);
};

struct pros_vfs_file_ops {
    int (*read)(struct pros_vfs_file *, uint32_t, uint32_t, void *);
    int (*write)(struct pros_vfs_file *, uint32_t, uint32_t, const void *);
    int (*truncate)(struct pros_vfs_file *, uint64_t);
    int (*close)(struct pros_vfs_file *);
    /* Optional per-open setup and 64-bit offsets for exFAT-sized files. */
    int (*open)(struct pros_vfs_file *);
    int (*read64)(struct pros_vfs_file *, uint64_t, uint32_t, void *);
    int (*write64)(struct pros_vfs_file *, uint64_t, uint32_t, const void *);
};

struct pros_vfs_vnode {
    struct pros_vfs_mount *mount;
    struct pros_vfs_vnode *parent;
    const struct pros_vfs_vnode_ops *ops;
    const struct pros_vfs_file_ops *file_ops;
    void *private_data;
    enum pros_vfs_node_type type;
    uint64_t size;
    /* Access only through atomic VFS retain/release operations. */
    uint32_t refcount;
    /* Trusted metadata copied from the filesystem inode, never from a caller. */
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint8_t metadata_valid;
    uint8_t initialized;
};

/*
 * The operation_busy gate serializes calls made through a live handle object;
 * it is not a reference count for the handle object's storage. The owner must
 * keep this structure allocated and stable until every concurrent API call
 * using it has returned, including callers rejected with EBUSY. The same
 * external object-lifetime rule applies to pros_vfs_dir below. These gates do
 * not promise arbitrary SMP-safe close-vs-free or teardown semantics.
 */
struct pros_vfs_file {
    struct pros_vfs_mount *mount;
    struct pros_vfs_vnode *vnode;
    const struct pros_vfs_file_ops *ops;
    void *private_data;
    uint64_t position;
    uint32_t flags;
    uint8_t open;
    /* Non-blocking serialization for operations sharing this open handle. */
    uint8_t operation_busy;
    /* Trusted open-time identity. Operations also require the same current actor. */
    struct pros_vfs_credentials auth;
};

struct pros_vfs_mount {
    enum pros_filesystem_type type;
    struct pros_vfs_provider *provider;
    struct pros_block_device *device;
    void *private_data;
    uint8_t read_only;
    struct pros_vfs_vnode *root;
    uint8_t active;
    /*
     * ACTIVE bit plus in-flight path-lookup pins. A single atomic word makes
     * lookup admission and unmount draining mutually exclusive.
     */
    uint32_t lifecycle_state;
    /* Fail-fast serialization for namespace-changing VFS transactions. */
    uint8_t namespace_busy;
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
int vfs_mount_deactivate_provider(struct pros_vfs_mount *,
                                  struct pros_vfs_provider *);
int vfs_mount_abort(struct pros_vfs_mount *);
int vfs_mount_abort_provider(struct pros_vfs_mount *,
                             struct pros_vfs_provider *);
int vfs_mount_is_read_only(const struct pros_vfs_mount *);
/*
 * get/put require the caller to already own a live reference, or to hold an
 * external lifetime guard that prevents provider reclamation during the call.
 */
int vfs_vnode_init(struct pros_vfs_vnode *, struct pros_vfs_mount *,
                   struct pros_vfs_vnode *, enum pros_vfs_node_type,
                   const struct pros_vfs_vnode_ops *,
                   const struct pros_vfs_file_ops *, void *, uint64_t);
/* POSIX rwx request bits: 4=read, 2=write, 1=search/execute. */
int vfs_vnode_set_metadata(struct pros_vfs_vnode *, uint16_t mode,
                            uint32_t uid, uint32_t gid);
int vfs_vnode_get(struct pros_vfs_vnode *);
int vfs_vnode_put(struct pros_vfs_vnode *);
int vfs_path_lookup(struct pros_vfs_mount *, const char *,
                    struct pros_vfs_vnode **);
/* Ordinary callers use a kernel-owned process identity; NULL is never root. */
int vfs_get_current_credentials(struct pros_vfs_credentials *);
int vfs_auth_context_validate(const struct pros_vfs_credentials *);
int vfs_vnode_authorize(const struct pros_vfs_vnode *,
                        const struct pros_vfs_credentials *,
                        uint8_t requested);
int vfs_check_access(struct pros_vfs_vnode *, uint32_t required_access);
/* Persistent metadata changes; caller identity is always kernel-derived. */
int vfs_chmod(struct pros_vfs_mount *, const char *, uint16_t mode);
int vfs_chown(struct pros_vfs_mount *, const char *, uint32_t uid, uint32_t gid);
int vfs_kernel_chmod(struct pros_vfs_mount *, const char *, uint16_t mode);
int vfs_kernel_chown(struct pros_vfs_mount *, const char *, uint32_t uid, uint32_t gid);

/* Explicit trusted kernel-only entry points for boot, logs and self-tests. */
int vfs_kernel_path_lookup(struct pros_vfs_mount *, const char *,
                           struct pros_vfs_vnode **);
int vfs_kernel_unlink(struct pros_vfs_mount *, const char *);
int vfs_kernel_mkdir(struct pros_vfs_mount *, const char *);
int vfs_kernel_rmdir(struct pros_vfs_mount *, const char *);
int vfs_kernel_rename(struct pros_vfs_mount *, const char *, const char *);
int vfs_kernel_file_open(struct pros_vfs_mount *, const char *, uint32_t,
                         struct pros_vfs_file *);
int vfs_kernel_dir_open(struct pros_vfs_mount *, const char *,
                        struct pros_vfs_dir *);
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
    /* Non-blocking serialization for directory cursor/close API operations. */
    uint8_t operation_busy;
    struct pros_vfs_credentials auth;
};

int vfs_file_init(struct pros_vfs_file *, struct pros_vfs_mount *,
                  const struct pros_vfs_file_ops *, void *, uint32_t);
int vfs_file_read(struct pros_vfs_file *, void *, uint32_t, uint32_t *);
int vfs_file_write(struct pros_vfs_file *, const void *, uint32_t, uint32_t *);
int vfs_file_truncate(struct pros_vfs_file *, uint64_t);
int vfs_file_seek(struct pros_vfs_file *, uint64_t);
int vfs_file_close(struct pros_vfs_file *);
int vfs_file_64bit_selftest(void);

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
