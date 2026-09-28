#include "vfs.h"

int vfs_mount_init(struct pros_vfs_mount *mount,
                   enum pros_filesystem_type type,
                   struct pros_block_device *device,
                   void *private_data,
                   uint8_t read_only)
{
    if (!mount || !device || type == PROS_FS_UNKNOWN)
        return PROS_VFS_EINVAL;
    mount->type = type;
    mount->device = device;
    mount->private_data = private_data;
    mount->read_only = read_only ? 1u : 0u;
    mount->active = 0;
    return PROS_VFS_OK;
}

int vfs_mount_activate(struct pros_vfs_mount *mount)
{
    if (!mount || !mount->device)
        return PROS_VFS_EINVAL;
    if (mount->active)
        return PROS_VFS_EBUSY;
    mount->active = 1;
    return PROS_VFS_OK;
}

int vfs_mount_deactivate(struct pros_vfs_mount *mount)
{
    if (!mount)
        return PROS_VFS_EINVAL;
    mount->active = 0;
    return PROS_VFS_OK;
}

int vfs_mount_is_read_only(const struct pros_vfs_mount *mount)
{
    if (!mount)
        return PROS_VFS_EINVAL;
    return mount->read_only ? 1 : 0;
}
