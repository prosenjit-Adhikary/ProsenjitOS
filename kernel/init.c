#include "init.h"
#include "heap.h"
#include "security_exec.h"

int pros_init_load_path(struct pros_vfs_mount *mount,
                        const char *path,
                        struct pros_process *process,
                        const struct pros_file_security_metadata *metadata,
                        struct pros_elf_load_result *result)
{
    struct pros_vfs_file file;
    struct pros_file_admission_record admission;
    uint8_t *image = 0;
    uint64_t length;
    uint32_t total = 0;

    if (!mount || !path || !path[0] || !process || !metadata || !result)
        return PROS_INIT_EINVAL;
    if (!mount->active)
        return PROS_INIT_EIO;

    if (vfs_file_open(mount, path, PROS_VFS_O_READ, &file) != PROS_VFS_OK)
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
        if (rc != PROS_ELF_OK)
            return rc == PROS_ELF_ENOMEM ? PROS_INIT_ENOMEM : PROS_INIT_EFORMAT;
    }

    return PROS_INIT_OK;
}

int pros_init_load(struct pros_vfs_mount *mount,
                   struct pros_process *process,
                   const struct pros_file_security_metadata *metadata,
                   struct pros_elf_load_result *result)
{
    return pros_init_load_path(mount, "/init", process, metadata, result);
}

static int init_reap_failed_child(struct pros_process *parent,
                                  struct pros_process *process)
{
    int32_t ignored_exit_code = 0;

    if (!parent || !process || process->parent != parent)
        return -1;

    if (process_state(process) != PROS_PROCESS_ZOMBIE &&
        process_state(process) != PROS_PROCESS_DEAD) {
        if (process_exit(process, -1) != 0)
            return -1;
    }

    if (process_state(process) == PROS_PROCESS_DEAD)
        return 0;
    return process_wait(parent, process->pid, &ignored_exit_code);
}

int pros_init_launch(struct pros_vfs_mount *mount,
                     const char *path,
                     struct pros_process *parent,
                     struct pros_process *process,
                     struct pros_thread *thread,
                     uint64_t pid,
                     uint64_t tid,
                     uint32_t security_level,
                     const struct pros_file_security_metadata *metadata,
                     uint64_t kernel_stack_bottom,
                     uint64_t kernel_stack_top)
{
    struct pros_elf_load_result loaded = {0};
    int rc;

    if (!mount || !path || !path[0] || !parent || !process ||
        parent == process || !thread || !metadata ||
        pid == 0u || tid == 0u ||
        (security_level != PROS_SECURITY_LIMITED &&
         security_level != PROS_SECURITY_SUPER) ||
        !kernel_stack_bottom || kernel_stack_top <= kernel_stack_bottom ||
        (kernel_stack_bottom & 0xFULL) != 0u ||
        (kernel_stack_top & 0xFULL) != 0u ||
        kernel_stack_top - kernel_stack_bottom < 4096u)
        return PROS_INIT_EINVAL;

    /*
     * Keep launch transactional: the child is not runnable until admission,
     * ELF mapping, the validated CPL3 IRET frame, and queue insertion all
     * succeed. Every post-create failure attempts a full process reap.
     */
    if (process_create(parent, process, pid, security_level) != 0)
        return PROS_INIT_ESTART;

    /*
     * Keep the child registered but non-runnable while the image is admitted,
     * mapped, and its first CPL3 frame is constructed. The process manager's
     * scheduler eligibility predicate rejects NEW, so an interrupt during
     * this transaction cannot expose a partially initialized child to user
     * execution.
     */
    if (process_set_state(process, PROS_PROCESS_NEW) != 0) {
        if (init_reap_failed_child(parent, process) != 0)
            return PROS_INIT_EIO;
        return PROS_INIT_ESTART;
    }

    rc = pros_init_load_path(mount, path, process, metadata, &loaded);
    if (rc != PROS_INIT_OK) {
        if (init_reap_failed_child(parent, process) != 0)
            return PROS_INIT_EIO;
        return rc;
    }

    if (thread_init_user(thread, process, tid, loaded.entry,
                         loaded.stack_top, kernel_stack_bottom,
                         kernel_stack_top) != 0) {
        if (init_reap_failed_child(parent, process) != 0)
            return PROS_INIT_EIO;
        return PROS_INIT_ESTART;
    }

    /* Publish READY only after the complete user frame exists. */
    if (process_set_state(process, PROS_PROCESS_READY) != 0) {
        if (init_reap_failed_child(parent, process) != 0)
            return PROS_INIT_EIO;
        return PROS_INIT_ESTART;
    }

    if (thread_enqueue(thread) != 0) {
        if (init_reap_failed_child(parent, process) != 0)
            return PROS_INIT_EIO;
        return PROS_INIT_ESTART;
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
    if (pros_init_load_path(&inactive, "", process, &metadata, &result) !=
        PROS_INIT_EINVAL)
        return -3;
    inactive.active = 0u;
    if (pros_init_load(&inactive, process, &metadata, &result) != PROS_INIT_EIO)
        return -2;
    return PROS_INIT_OK;
}
