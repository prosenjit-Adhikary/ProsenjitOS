#ifndef PROSENJIT_VFS_PROVIDER_H
#define PROSENJIT_VFS_PROVIDER_H

#include <stdint.h>

/*
 * Stable boundary for future filesystem providers.
 * Compatibility parsers (FAT32/exFAT/NTFS/etc.) stay outside kernel-core.
 * Provider-private state is opaque and must not cross this ABI.
 */
#define PROSENJIT_VFS_PROVIDER_ABI_VERSION 0x00010000ULL
#define PROSENJIT_VFS_PROVIDER_NAME_MAX 63u
#define PROSENJIT_VFS_PROVIDER_MAX 8u

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
typedef int (*pros_vfs_provider_unmount_fn)(struct pros_vfs_provider *,
                                            struct pros_vfs_mount *);

struct pros_vfs_provider_ops {
    pros_vfs_provider_probe_fn probe;
    pros_vfs_provider_mount_fn mount;
    pros_vfs_provider_unmount_fn unmount;
};

struct pros_vfs_provider {
    uint64_t abi_version;
    uint32_t flags;
    char name[PROSENJIT_VFS_PROVIDER_NAME_MAX + 1u];
    const struct pros_vfs_provider_ops *ops;
    void *private_data;
};

#endif
