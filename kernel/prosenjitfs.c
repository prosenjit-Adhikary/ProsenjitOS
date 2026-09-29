        }
    }
    return PROSENJITFS_ENOENT;
}

static int pfs_vnode_lookup(struct pros_vfs_vnode *parent, const char *name,
                            struct pros_vfs_vnode **result)
{
    struct pfs_vnode_data *data;
    if (!parent || !name || !result || !parent->private_data)
        return PROS_VFS_EINVAL;
    *result = 0;
    data = (struct pfs_vnode_data *)parent->private_data;

    if (name[0] == '.' && name[1] == '\0') {
        if (vfs_vnode_get(parent) != PROS_VFS_OK)
            return PROS_VFS_EINVAL;
        *result = parent;
        return PROS_VFS_OK;
    }

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        struct pros_vfs_vnode *p = parent->parent ? parent->parent : parent;