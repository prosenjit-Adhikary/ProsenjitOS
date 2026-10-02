            vfs_rename(&fs.mount, "/hello.bin", "/.hello.bin") !=
                PROS_VFS_OK ||
            vfs_path_lookup(&fs.mount, "/hello.bin", &hidden) !=
                PROS_VFS_ENOENT ||
            hidden != 0)
            return -(120000 + __LINE__);
        if (vfs_path_lookup(&fs.mount, "/.hello.bin", &hidden) != PROS_VFS_OK)
            return -(130000 + __LINE__);
        if (!hidden)
            return -(130000 + __LINE__);
        if (!hidden->initialized || hidden->refcount == 0u ||
            hidden->mount != &fs.mount || hidden->ops != &pfs_vnode_ops)
            return -(150000 + (int)(hidden->initialized * 10000u) +
                     (int)(hidden->refcount > 9999u ? 9999u : hidden->refcount));
        if (vfs_vnode_put(hidden) != PROS_VFS_OK)
            return -(160000 + (int)(hidden->initialized * 10000u) +
                     (int)(hidden->refcount > 9999u ? 9999u : hidden->refcount));
        if (prosenjitfs_read_inode(&fs, 1u, &after) != PROSENJITFS_OK)
            return -(130000 + __LINE__);
        if (after.type != before.type)