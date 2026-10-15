#include "init.h"
#include "heap.h"
#include "security_exec.h"

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

    /* Never expose stale entry/stack addresses after a failed load attempt. */
    *result = (struct pros_elf_load_result){0};
    file = (struct pros_vfs_file){0};
    admission = (struct pros_file_admission_record){0};

    if (!mount->active)
        return PROS_INIT_EIO;

    {
        int open_rc = vfs_file_open(mount, "/init", PROS_VFS_O_READ, &file);
        if (open_rc != PROS_VFS_OK)
            return open_rc == PROS_VFS_ENOENT ? PROS_INIT_ENOENT
                                               : PROS_INIT_EIO;
    }

    if (!file.vnode || file.vnode->type != PROS_VFS_NODE_FILE) {
        (void)vfs_file_close(&file);
        return PROS_INIT_EFORMAT;
    }

    length = file.vnode->size;
    if (length == 0 || length > PROS_INIT_IMAGE_MAX ||
        length > UINT32_MAX) {
        (void)vfs_file_close(&file);
        return length > PROS_INIT_IMAGE_MAX || length > UINT32_MAX
                   ? PROS_INIT_EOVERFLOW : PROS_INIT_EFORMAT;
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

    if (vfs_file_admit(mount, image, length, metadata, &admission) != PROS_VFS_OK) {
        kfree(image);
        return PROS_INIT_ESECURITY;
    }

    /*
     * Filesystem-backed execution must cross the same execution-security
     * boundary as every other executable. Do not turn a filesystem read
     * into a direct ELF load: admission/quarantine/block is authoritative.
     * Timestamp 0 is used here because the init loader has no clock
     * dependency; the quarantine record remains deterministic.
     */
    {
        uint64_t quarantine_id = 0;
        int exec_rc = pros_security_exec_admit(
            process, &admission, 0u, &quarantine_id);
        if (exec_rc != PROS_SECURITY_EXEC_ALLOW) {
            kfree(image);
            return PROS_INIT_ESECURITY;
        }
    }

    if (pros_elf_validate(image, length) != PROS_ELF_OK) {
        kfree(image);
        return PROS_INIT_EFORMAT;
    }

    {
        int rc = pros_elf_load(process, image, length, result);
        kfree(image);
        if (rc != PROS_ELF_OK) {
            *result = (struct pros_elf_load_result){0};
            if (rc == PROS_ELF_ENOMEM)
                return PROS_INIT_ENOMEM;
            if (rc == PROS_ELF_EOVERFLOW || rc == PROS_ELF_EBOUNDS)
                return PROS_INIT_EOVERFLOW;
            if (rc == PROS_ELF_EPERM || rc == PROS_ELF_ECONFLICT)
                return PROS_INIT_ESECURITY;
            return PROS_INIT_EFORMAT;
        }
    }

    return PROS_INIT_OK;
}

int pros_init_selftest(void)
{
    struct pros_vfs_mount inactive = {0};
    struct pros_process dummy_process = {0};
    struct pros_process *process = &dummy_process;
    struct pros_file_security_metadata metadata = {0};
    struct pros_elf_load_result result = {0};

    if (pros_init_load(0, process, &metadata, &result) != PROS_INIT_EINVAL)
        return -1;
    if (pros_init_load(&inactive, 0, &metadata, &result) != PROS_INIT_EINVAL)
        return -4;
    if (pros_init_load(&inactive, process, 0, &result) != PROS_INIT_EINVAL)
        return -5;
    if (pros_init_load(&inactive, process, &metadata, 0) != PROS_INIT_EINVAL)
        return -6;
    inactive.active = 0u;
    result.entry = 0x1111u;
    result.image_start = 0x2222u;
    result.image_end = 0x3333u;
    result.stack_top = 0x4444u;
    result.mapped_pages = 0x5555u;
    if (pros_init_load(&inactive, process, &metadata, &result) != PROS_INIT_EIO)
        return -2;
    if (result.entry != 0u || result.image_start != 0u ||
        result.image_end != 0u || result.stack_top != 0u ||
        result.mapped_pages != 0u)
        return -3;
    return PROS_INIT_OK;
}
