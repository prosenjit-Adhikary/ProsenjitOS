#ifndef PROSENJIT_VFS_PROVIDER_H
#define PROSENJIT_VFS_PROVIDER_H

#include <stdint.h>

/*
 * Stable boundary for future filesystem providers.
 * Compatibility parsers (FAT32/exFAT/NTFS/etc.) stay outside kernel-core.
 * Provider-private state is opaque and must not cross this ABI.
 */
#define PROSENJIT_VFS_PROVIDER_ABI_VERSION 0x00020000ULL
#define PROSENJIT_VFS_PROVIDER_NAME_MAX 63u
#define PROSENJIT_VFS_PROVIDER_MAX 8u
#define PROS_VFS_PROVIDER_NO_MATCH 1

enum pros_vfs_provider_flags {
    PROS_VFS_PROVIDER_READ_ONLY = 1u << 0,
    PROS_VFS_PROVIDER_PARTITION_AWARE = 1u << 1,
    PROS_VFS_PROVIDER_USERSPACE = 1u << 2
};

struct pros_vfs_provider;
struct pros_vfs_mount;
struct pros_block_device;
struct pros_partition_device;
struct pros_partition;

typedef int (*pros_vfs_provider_probe_fn)(struct pros_block_device *);
typedef int (*pros_vfs_provider_mount_fn)(
    struct pros_vfs_provider *,
    struct pros_vfs_mount *,
    struct pros_block_device *,
    struct pros_partition_device *,
    const struct pros_partition *,
    uint8_t read_only);
typedef int (*pros_vfs_provider_sync_fn)(struct pros_vfs_provider *,
                                         struct pros_vfs_mount *);
typedef int (*pros_vfs_provider_unmount_fn)(struct pros_vfs_provider *,
                                            struct pros_vfs_mount *);

struct pros_vfs_provider_ops {
    pros_vfs_provider_probe_fn probe;
    pros_vfs_provider_mount_fn mount;
    /* Flush durable state before VFS root teardown; failure keeps mount live. */
    pros_vfs_provider_sync_fn sync;
    /* Release provider-private state after VFS has released all root vnodes. */
    pros_vfs_provider_unmount_fn unmount;
};

struct pros_vfs_provider {
    uint64_t abi_version;
    uint32_t flags;
    char name[PROSENJIT_VFS_PROVIDER_NAME_MAX + 1u];
    const struct pros_vfs_provider_ops *ops;
    void *private_data;
    /*
     * Atomic live-mount count. The high bit is reserved internally while
     * unregistering; callers must use mount_acquire/release, never edit it.
     */
    uint32_t active_mounts;
};

/*
 * Pin a registered provider before a mount starts using it; release exactly
 * once after that mount has fully torn down. Registry registration/removal
 * remains an initialization/control-plane operation and must be externally
 * serialized until the registry itself gains a synchronization primitive.
 * Provider objects and their ops tables must outlive every stored provider
 * pointer (currently boot/static lifetime; dynamic module unload is unsupported).
 */
int pros_vfs_provider_mount_acquire(struct pros_vfs_provider *provider);
int pros_vfs_provider_mount_release(struct pros_vfs_provider *provider);
/*
 * Mount callback contract: prepare an initialized, inactive mount with a
 * directory root; do not activate it or set mount->provider. On callback
 * failure, roll back all partial allocations before returning. If a callback
 * nevertheless returns failure with mount state published, the bridge retains
 * the provider pin and association for explicit unmount rollback. The bridge
 * pins the provider and activates the prepared mount.
 *
 * Unmount contract: sync runs before VFS draining and must leave the mount
 * usable on failure. unmount runs only after root vnode teardown (or rollback)
 * and must release provider-private state; it may be retried after failure.
 * Mount objects must be zero-initialized before their first use.
 */
/* Probe returns OK for a match, NO_MATCH for an ordinary format mismatch. */
int pros_vfs_provider_probe_device(struct pros_block_device *,
                                   uint32_t required_flags,
                                   struct pros_vfs_provider **);
int pros_vfs_provider_mount_device(
    struct pros_vfs_provider *provider, struct pros_vfs_mount *mount,
    struct pros_block_device *device, struct pros_partition_device *view,
    const struct pros_partition *partition, uint8_t read_only);
int pros_vfs_provider_unmount_device(struct pros_vfs_mount *mount);
int pros_vfs_provider_register(struct pros_vfs_provider *provider);
int pros_vfs_provider_unregister(struct pros_vfs_provider *provider);
const struct pros_vfs_provider *pros_vfs_provider_find(const char *name);
int pros_vfs_provider_selftest(void);

#endif
