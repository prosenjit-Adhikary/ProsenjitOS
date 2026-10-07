    uint64_t old_size, keep_blocks, retained_blocks = 0u;
    uint64_t current, previous = 0u;
    uint64_t steps = 0u;

    if (!file || !file->vnode || !file->mount)
        return PROS_VFS_EINVAL;
    if (size > UINT32_MAX)
        return PROS_VFS_EOVERFLOW;
    if (file->mount->read_only || !file->mount->active)
        return PROS_VFS_EPERM;
    data = (struct pfs_vnode_data *)file->vnode->private_data;
    if (!data || !data->fs)