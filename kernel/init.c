#include "init.h"
#include "heap.h"

int pros_init_load(struct pros_vfs_mount *mount,
                   struct pros_process *process,
                   const struct pros_file_security_metadata *metadata,
                   struct pros_elf_load_result *result)
{
    struct pros_vfs_file file;
    struct pros_file_admission_record admission;
    uint8_t *image = 0;
    uint64_t length;
    uint32_t total = 0;

    if (!mount || !process || !metadata || !result)
        return PROS_INIT_EINVAL;
    if (!mount->active)
        return PROS_INIT_EIO;

    if (vfs_file_open(mount, "/init", PROS_VFS_O_READ, &file) != PROS_VFS_OK)
        return PROS_INIT_ENOENT;

    if (!file.vnode || file.vnode->type != PROS_VFS_NODE_FILE) {
        (void)vfs_file_close(&file);
        return PROS_INIT_EFORMAT;
    }

    length = file.vnode->size;
    if (length == 0 || length > PROS_INIT_IMAGE_MAX ||
        length > UINT32_MAX) {
        (void)vfs_file_close(&file);
        return length > PROS_INIT_IMAGE_MAX ? PROS_INIT_EOVERFLOW
                                             : PROS_INIT_EFORMAT;
    }

    image = (uint8_t *)kmalloc(length);
    if (!image) {
        (void)vfs_file_close(&file);
        return PROS_INIT_ENOMEM;
    }

    while ((uint64_t)total < length) {
        uint32_t want = (uint32_t)((length - total) > 65536u
                                   ? 65536u : (length - total));
        uint32_t got = 0;
        int rc = vfs_file_read(&file, image + total, want, &got);
        if (rc != PROS_VFS_OK || got == 0 || got > want) {
            kfree(image);
            (void)vfs_file_close(&file);
            return PROS_INIT_EIO;
        }
        total += got;
    }

    if (vfs_file_close(&file) != PROS_VFS_OK) {
        kfree(image);
        return PROS_INIT_EIO;
    }

    if (vfs_file_admit(mount, image, length, metadata, &admission) != PROS_VFS_OK ||
        admission.security.action != PROS_FILE_ALLOW) {
        kfree(image);
        return PROS_INIT_ESECURITY;
    }

    if (pros_elf_validate(image, length) != PROS_ELF_OK) {
        kfree(image);
        return PROS_INIT_EFORMAT;
    }

    {
        int rc = pros_elf_load(process, image, length, result);
        kfree(image);
        if (rc != PROS_ELF_OK)
            return rc == PROS_ELF_ENOMEM ? PROS_INIT_ENOMEM : PROS_INIT_EFORMAT;
    }

    return PROS_INIT_OK;
}
