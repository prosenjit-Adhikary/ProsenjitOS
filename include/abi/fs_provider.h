#ifndef PROSENJIT_FS_PROVIDER_ABI_H
#define PROSENJIT_FS_PROVIDER_ABI_H

#include <stdint.h>

/*
 * External filesystem-provider wire ABI.
 *
 * This header contains only fixed-width, endian-neutral protocol values.
 * It intentionally contains no kernel pointers, function pointers, vnode
 * layouts, allocator state, or filesystem-specific structures.
 *
 * Provider implementations run outside kernel-core. The kernel-side VFS
 * adapter may communicate with a provider through a future capability-backed
 * transport without importing a filesystem parser into the kernel image.
 */
#define PROS_FS_PROVIDER_ABI_VERSION 0x00010000ULL
#define PROS_FS_PROVIDER_NAME_MAX 32u
#define PROS_FS_PROVIDER_MAX_MESSAGE 64u

enum pros_fs_provider_type {
    PROS_FS_PROVIDER_UNKNOWN = 0,
    PROS_FS_PROVIDER_FAT32 = 1,
    PROS_FS_PROVIDER_EXFAT = 2,
    PROS_FS_PROVIDER_NTFS = 3
};

enum pros_fs_provider_probe_result {
    PROS_FS_PROVIDER_PROBE_ERROR = -1,
    PROS_FS_PROVIDER_PROBE_NO_MATCH = 0,
    PROS_FS_PROVIDER_PROBE_MATCH = 1
};

enum pros_fs_provider_opcode {
    PROS_FS_PROVIDER_OP_PROBE = 1,
    PROS_FS_PROVIDER_OP_MOUNT = 2,
    PROS_FS_PROVIDER_OP_UNMOUNT = 3,
    PROS_FS_PROVIDER_OP_LOOKUP = 4,
    PROS_FS_PROVIDER_OP_READDIR = 5,
    PROS_FS_PROVIDER_OP_CREATE = 6,
    PROS_FS_PROVIDER_OP_MKDIR = 7,
    PROS_FS_PROVIDER_OP_UNLINK = 8,
    PROS_FS_PROVIDER_OP_RMDIR = 9,
    PROS_FS_PROVIDER_OP_RENAME = 10,
    PROS_FS_PROVIDER_OP_READ = 11,
    PROS_FS_PROVIDER_OP_WRITE = 12,
    PROS_FS_PROVIDER_OP_STAT = 13,
    PROS_FS_PROVIDER_OP_SYNC = 14,
    PROS_FS_PROVIDER_OP_RELEASE = 15
};

enum pros_fs_provider_status {
    PROS_FS_PROVIDER_OK = 0,
    PROS_FS_PROVIDER_EINVAL = -1,
    PROS_FS_PROVIDER_ENOSYS = -2,
    PROS_FS_PROVIDER_EIO = -3,
    PROS_FS_PROVIDER_ECORRUPT = -4,
    PROS_FS_PROVIDER_ENOENT = -5,
    PROS_FS_PROVIDER_EEXIST = -6,
    PROS_FS_PROVIDER_ENOTDIR = -7,
    PROS_FS_PROVIDER_EISDIR = -8,
    PROS_FS_PROVIDER_ENOTEMPTY = -9,
    PROS_FS_PROVIDER_EOVERFLOW = -10,
    PROS_FS_PROVIDER_ENOTSUP = -11,
    PROS_FS_PROVIDER_EROFS = -12,
    PROS_FS_PROVIDER_EBUSY = -13
};

/*
 * Capability bits describe what the provider actually implements. A provider
 * must not advertise an operation merely because the wire opcode exists.
 */
#define PROS_FS_PROVIDER_CAP_READ       (1ULL << 0)
#define PROS_FS_PROVIDER_CAP_WRITE      (1ULL << 1)
#define PROS_FS_PROVIDER_CAP_CREATE     (1ULL << 2)
#define PROS_FS_PROVIDER_CAP_MKDIR      (1ULL << 3)
#define PROS_FS_PROVIDER_CAP_UNLINK     (1ULL << 4)
#define PROS_FS_PROVIDER_CAP_RMDIR      (1ULL << 5)
#define PROS_FS_PROVIDER_CAP_RENAME     (1ULL << 6)
#define PROS_FS_PROVIDER_CAP_READDIR    (1ULL << 7)
#define PROS_FS_PROVIDER_CAP_STAT       (1ULL << 8)
#define PROS_FS_PROVIDER_CAP_SYNC       (1ULL << 9)

struct pros_fs_provider_descriptor {
    uint64_t abi_version;
    uint32_t provider_type;
    uint32_t descriptor_size;
    uint64_t capabilities;
    char name[PROS_FS_PROVIDER_NAME_MAX];
};

struct pros_fs_provider_request {
    uint32_t opcode;
    uint32_t size;
    uint64_t request_id;
    uint64_t object_id;
    uint64_t offset;
    uint64_t length;
    uint64_t data_ref;
    uint64_t aux_ref;
};

struct pros_fs_provider_response {
    int32_t status;
    uint32_t size;
    uint64_t request_id;
    uint64_t object_id;
    uint64_t value0;
    uint64_t value1;
    uint64_t data_ref;
};

_Static_assert(sizeof(struct pros_fs_provider_descriptor) == 56,
               "provider descriptor ABI size changed");
_Static_assert(sizeof(struct pros_fs_provider_request) == 64,
               "provider request ABI size changed");
_Static_assert(sizeof(struct pros_fs_provider_response) == 48,
               "provider response ABI size changed");

#endif
