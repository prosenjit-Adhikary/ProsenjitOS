#include "vfs.h"
#include "vfs_provider.h"
#include "partition.h"
#include "prosenjitfs.h"
#include "health.h"
#include "process.h"

#define PROS_VFS_PROBE_SECTOR_MAX 4096u

static uint64_t health_mount_count;
static uint64_t health_unmount_count;
static uint64_t health_read_count;
static uint64_t health_write_count;
static uint64_t health_truncate_count;
static uint64_t health_lookup_error_count;
static uint64_t health_io_error_count;
static uint64_t health_permission_error_count;
static uint64_t health_corruption_error_count;
static uint64_t health_unlink_count;
static uint64_t health_mkdir_count;
static uint64_t health_rmdir_count;
static uint64_t health_rename_count;
static uint64_t health_dir_open_count;
static uint64_t health_dir_read_count;
static uint64_t vfs_selftest_read_calls;
static uint64_t vfs_selftest_write_calls;
static uint64_t vfs_selftest_truncate_calls;
/* Re-entry probes exercise the real operation gates from inside callbacks. */
static struct pros_vfs_mount *vfs_selftest_mount_probe;
static uint8_t vfs_selftest_mount_probe_enabled;
static int vfs_selftest_mount_probe_result;
static struct pros_vfs_file *vfs_selftest_file_probe;
static uint8_t vfs_selftest_file_probe_enabled;
static int vfs_selftest_file_probe_read_result;
static int vfs_selftest_file_probe_seek_result;
static int vfs_selftest_file_probe_write_result;
static int vfs_selftest_file_probe_truncate_result;
static int vfs_selftest_file_probe_close_result;
static struct pros_vfs_dir *vfs_selftest_dir_probe;
static uint8_t vfs_selftest_dir_probe_enabled;
static int vfs_selftest_dir_probe_close_result;

#define PROS_VFS_ACTIVE_HANDLE_MAX 256u
#define PROS_VFS_HANDLE_OPENING 1u
#define PROS_VFS_HANDLE_ACTIVE 2u
#define PROS_VFS_HANDLE_CLOSING 3u
struct vfs_active_handle_entry {
    void *handle;
    uint8_t state;
};
static struct vfs_active_handle_entry
    vfs_active_file_handles[PROS_VFS_ACTIVE_HANDLE_MAX];
static struct vfs_active_handle_entry
    vfs_active_dir_handles[PROS_VFS_ACTIVE_HANDLE_MAX];

/* Pointer-only handle ownership registry. IRQ exclusion makes each bounded
 * state transition atomic on the current single-CPU VFS implementation
 * without dereferencing an uninitialized caller-provided output object. */
static uint64_t vfs_handle_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void vfs_handle_irq_restore(uint64_t flags)
{
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static int vfs_active_handle_claim(
    void *handle, struct vfs_active_handle_entry *entries)
{
    uint64_t flags;
    uint32_t free_slot = PROS_VFS_ACTIVE_HANDLE_MAX;
    if (!handle || !entries)
        return PROS_VFS_EINVAL;

    flags = vfs_handle_irq_save();
    for (uint32_t i = 0u; i < PROS_VFS_ACTIVE_HANDLE_MAX; ++i) {
        if (entries[i].handle == handle) {
            vfs_handle_irq_restore(flags);
            return PROS_VFS_EBUSY;
        }
        if (!entries[i].handle && free_slot == PROS_VFS_ACTIVE_HANDLE_MAX)
            free_slot = i;
    }
    if (free_slot == PROS_VFS_ACTIVE_HANDLE_MAX) {
        vfs_handle_irq_restore(flags);
        return PROS_VFS_EBUSY;
    }
    entries[free_slot].handle = handle;
    entries[free_slot].state = PROS_VFS_HANDLE_OPENING;
    vfs_handle_irq_restore(flags);
    return PROS_VFS_OK;
}

static int vfs_active_handle_set_state(
    void *handle, struct vfs_active_handle_entry *entries,
    uint8_t expected_state, uint8_t new_state)
{
    uint64_t flags;
    int rc = PROS_VFS_EINVAL;
    if (!handle || !entries)
        return PROS_VFS_EINVAL;
    flags = vfs_handle_irq_save();
    for (uint32_t i = 0u; i < PROS_VFS_ACTIVE_HANDLE_MAX; ++i) {
        if (entries[i].handle != handle)
            continue;
        rc = entries[i].state == expected_state
            ? PROS_VFS_OK : PROS_VFS_EBUSY;
        if (rc == PROS_VFS_OK)
            entries[i].state = new_state;
        break;
    }
    vfs_handle_irq_restore(flags);
    return rc;
}

static int vfs_active_handle_require_active(
    void *handle, struct vfs_active_handle_entry *entries)
{
    uint64_t flags;
    int rc = PROS_VFS_EINVAL;
    if (!handle || !entries)
        return PROS_VFS_EINVAL;
    flags = vfs_handle_irq_save();
    for (uint32_t i = 0u; i < PROS_VFS_ACTIVE_HANDLE_MAX; ++i) {
        if (entries[i].handle != handle)
            continue;
        rc = entries[i].state == PROS_VFS_HANDLE_ACTIVE
            ? PROS_VFS_OK : PROS_VFS_EBUSY;
        break;
    }
    vfs_handle_irq_restore(flags);
    return rc;
}

static void vfs_active_handle_release(
    void *handle, struct vfs_active_handle_entry *entries)
{
    uint64_t flags;
    if (!handle || !entries)
        return;
    flags = vfs_handle_irq_save();
    for (uint32_t i = 0u; i < PROS_VFS_ACTIVE_HANDLE_MAX; ++i) {
        if (entries[i].handle == handle) {
            entries[i].handle = 0;
            entries[i].state = 0u;
            break;
        }
    }
    vfs_handle_irq_restore(flags);
}

static void vfs_health_error(int rc)
{
    uint32_t code = 0u;
    if (rc == PROS_VFS_EIO) {
        ++health_io_error_count;
        code = 1u;
    }
    else if (rc == PROS_VFS_EPERM) {
        ++health_permission_error_count;
        code = 2u;
    }
    else if (rc == PROS_VFS_ENOENT || rc == PROS_VFS_ENOTDIR ||
             rc == PROS_VFS_ELOOP)
        ++health_lookup_error_count;
    /* Size/path overflow is a validation error, not proof of corruption. */
    if (code != 0u)
        (void)pros_health_record_filesystem(0x56465348u, code,
                                            (uint64_t)(uint32_t)(-rc));
}

/*
 * Filesystem identities are kernel-owned process attributes.  Kernel-only
 * maintenance/self-tests execute with no current process and use the trusted
 * kernel identity; an invalid or quarantined process never inherits that
 * privilege.  User processes with an unmapped identity fail closed.
 */
static const struct pros_vfs_credentials vfs_kernel_credentials = {
    .uid = 0u, .gid = 0u, .valid = 1u, .privileged = 1u,
    .reserved = {0u, 0u}
};

int vfs_auth_context_validate(const struct pros_vfs_credentials *credentials)
{
    if (!credentials || credentials->valid != 1u ||
        credentials->reserved[0] != 0u ||
        credentials->reserved[1] != 0u ||
        credentials->privileged > 1u ||
        (credentials->privileged &&
         (credentials->uid != 0u || credentials->gid != 0u)) ||
        (!credentials->privileged &&
         (credentials->uid == UINT32_MAX ||
          credentials->gid == UINT32_MAX)))
        return PROS_VFS_EPERM;
    return PROS_VFS_OK;
}

int vfs_get_current_credentials(struct pros_vfs_credentials *credentials)
{
    struct pros_process *process;

    if (!credentials)
        return PROS_VFS_EINVAL;
    credentials->uid = UINT32_MAX;
    credentials->gid = UINT32_MAX;
    credentials->valid = 0u;
    credentials->privileged = 0u;
    credentials->reserved[0] = 0u;
    credentials->reserved[1] = 0u;

    process = process_current();
    /* NULL is not an implicit kernel/root context. Internal callers use
     * the explicit vfs_kernel_* entry points. */
    if (!process || process->quarantined ||
        (process->state != PROS_PROCESS_READY &&
         process->state != PROS_PROCESS_RUNNING) ||
        process->fs_identity_valid != 1u ||
        process->fs_uid == UINT32_MAX || process->fs_gid == UINT32_MAX)
        return PROS_VFS_EPERM;

    credentials->uid = process->fs_uid;
    credentials->gid = process->fs_gid;
    credentials->valid = 1u;
    return vfs_auth_context_validate(credentials);
}

static int vfs_check_metadata_access(
    const struct pros_vfs_credentials *credentials,
    const struct pros_vfs_metadata *metadata,
    uint32_t required_access)
{
    uint32_t granted;

    if (vfs_auth_context_validate(credentials) != PROS_VFS_OK ||
        !metadata || required_access == 0u ||
        (required_access & ~(PROS_VFS_ACCESS_READ |
                             PROS_VFS_ACCESS_WRITE |
                             PROS_VFS_ACCESS_EXECUTE)) != 0u)
        return PROS_VFS_EPERM;
    if ((metadata->mode & (uint16_t)~0777u) != 0u)
        return PROS_VFS_ECORRUPT;
    if (credentials->privileged)
        return PROS_VFS_OK;

    if (credentials->uid == metadata->uid)
        granted = (metadata->mode >> 6) & 7u;
    else if (credentials->gid == metadata->gid)
        granted = (metadata->mode >> 3) & 7u;
    else
        granted = metadata->mode & 7u;

    return ((granted & required_access) == required_access)
         ? PROS_VFS_OK : PROS_VFS_EPERM;
}

int vfs_vnode_authorize(const struct pros_vfs_vnode *vnode,
                        const struct pros_vfs_credentials *credentials,
                        uint8_t requested)
{
    struct pros_vfs_metadata metadata;
    int rc;

    if (!vnode || !vnode->initialized || !vnode->mount ||
        !vnode->mount->active ||
        vfs_auth_context_validate(credentials) != PROS_VFS_OK ||
        requested == 0u || (requested & (uint8_t)~7u) != 0u)
        return PROS_VFS_EPERM;

    /* Prefer authoritative metadata from the provider on every operation;
     * the cached vnode copy is only a fallback for providers without a
     * metadata callback. Validate the on-disk mode even for kernel callers. */
    if (vnode->ops && vnode->ops->get_metadata) {
        rc = vnode->ops->get_metadata((struct pros_vfs_vnode *)vnode,
                                      &metadata);
        if (rc != PROS_VFS_OK)
            return rc < 0 ? rc : PROS_VFS_EIO;
    } else {
        if (!vnode->metadata_valid)
            return credentials->privileged ? PROS_VFS_OK : PROS_VFS_EPERM;
        metadata.uid = vnode->uid;
        metadata.gid = vnode->gid;
        metadata.mode = vnode->mode;
        metadata.reserved[0] = 0u;
        metadata.reserved[1] = 0u;
    }
    if ((metadata.mode & (uint16_t)~0777u) != 0u)
        return PROS_VFS_ECORRUPT;
    if (credentials->privileged)
        return PROS_VFS_OK;
    return vfs_check_metadata_access(credentials, &metadata, requested);
}

static int vfs_permission_policy_selftest(void)
{
    struct pros_vfs_credentials credentials = {
        .uid = 1001u, .gid = 10u, .valid = 1u, .privileged = 0u,
        .reserved = {0u, 0u}
    };
    struct pros_vfs_metadata metadata = {
        .uid = 1001u, .gid = 20u, .mode = 0640u,
        .reserved = {0u, 0u}
    };

    if (vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_READ) != PROS_VFS_OK ||
        vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_WRITE) != PROS_VFS_OK ||
        vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_EXECUTE) != PROS_VFS_EPERM)
        return -1;

    credentials.uid = 1002u;
    credentials.gid = 20u;
    if (vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_READ) != PROS_VFS_OK ||
        vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_WRITE) != PROS_VFS_EPERM)
        return -2;

    credentials.uid = 1003u;
    credentials.gid = 30u;
    if (vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_READ) != PROS_VFS_EPERM)
        return -3;

    credentials.uid = UINT32_MAX;
    credentials.gid = UINT32_MAX;
    credentials.valid = 0u;
    if (vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_READ) != PROS_VFS_EPERM)
        return -4;

    credentials.uid = 0u;
    credentials.gid = 0u;
    credentials.valid = 1u;
    credentials.privileged = 1u;
    metadata.mode = 0000u;
    if (vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_READ |
                                  PROS_VFS_ACCESS_WRITE |
                                  PROS_VFS_ACCESS_EXECUTE) != PROS_VFS_OK)
        return -5;
    metadata.mode = 01000u;
    if (vfs_check_metadata_access(&credentials, &metadata,
                                  PROS_VFS_ACCESS_READ) != PROS_VFS_ECORRUPT)
        return -6;
    return 0;
}

static int vfs_selftest_read(struct pros_block_device *device,
                             uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    return PROS_BLOCK_OK;
}

static int vfs_probe_selftest(void)
{
    struct pros_block_device device;
    enum pros_filesystem_type type;

    if (vfs_probe_filesystem(0, &type) != PROS_VFS_EINVAL)
        return -1;
    if (vfs_probe_filesystem(&device, 0) != PROS_VFS_EINVAL)
        return -2;

    if (block_device_init(&device, 512u, 1u,
                          vfs_selftest_read, 0, 0) != PROS_BLOCK_OK)
        return -3;

    if (vfs_probe_filesystem(&device, &type) != PROS_VFS_EINVAL ||
        type != PROS_FS_UNKNOWN)
        return -4;

    if (block_device_init(&device, 512u, 256u,
                          vfs_selftest_read, 0, 0) != PROS_BLOCK_OK)
        return -5;

    if (vfs_probe_filesystem(&device, &type) != PROS_VFS_ENOSYS ||
        type != PROS_FS_UNKNOWN)
        return -6;

    return 0;
}

#define VFS_MOUNT_ACTIVE_BIT 0x80000000u
#define VFS_MOUNT_DRAINING_BIT 0x40000000u
#define VFS_MOUNT_OPERATION_MASK 0x3FFFFFFFu

static int vfs_mount_operation_enter(struct pros_vfs_mount *mount)
{
    uint32_t state;
    if (!mount)
        return PROS_VFS_EINVAL;
    state = __atomic_load_n(&mount->lifecycle_state, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((state & VFS_MOUNT_ACTIVE_BIT) == 0u)
            return PROS_VFS_EINVAL;
        if ((state & VFS_MOUNT_OPERATION_MASK) == VFS_MOUNT_OPERATION_MASK)
            return PROS_VFS_EOVERFLOW;
        if (__atomic_compare_exchange_n(&mount->lifecycle_state, &state,
                                        state + 1u, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return PROS_VFS_OK;
    }
}

static void vfs_mount_operation_leave(struct pros_vfs_mount *mount)
{
    (void)__atomic_fetch_sub(&mount->lifecycle_state, 1u, __ATOMIC_RELEASE);
}

static int vfs_namespace_operation_enter(struct pros_vfs_mount *mount)
{
    int rc;

    if (!mount)
        return PROS_VFS_EINVAL;

    /*
     * Hold a lifecycle pin for the complete namespace operation, not only
     * during path lookup. This closes the lookup-to-provider-callback gap:
     * unmount cannot detach the mount while a mutator still owns parent
     * vnode references or is executing a provider namespace callback.
     */
    rc = vfs_mount_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;

    {
        uint8_t expected = 0u;
        /* Preserve unexpected non-zero/corrupt state on failed admission. */
        if (!__atomic_compare_exchange_n(&mount->namespace_busy, &expected,
                                         1u, 0, __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED)) {
            vfs_mount_operation_leave(mount);
            return PROS_VFS_EBUSY;
        }
    }

    return PROS_VFS_OK;
}

static void vfs_namespace_operation_leave(struct pros_vfs_mount *mount)
{
    __atomic_clear(&mount->namespace_busy, __ATOMIC_RELEASE);
    vfs_mount_operation_leave(mount);
}

int vfs_mount_init(struct pros_vfs_mount *mount,
                   enum pros_filesystem_type type,
                   struct pros_block_device *device,
                   void *private_data,
                   uint8_t read_only)
{
    if (!mount || !device || type == PROS_FS_UNKNOWN)
        return PROS_VFS_EINVAL;
    /*
     * Reinitializing a live mount would discard its root/device/private
     * ownership without running the filesystem unmount path first.
     */
    if (mount->active || mount->root || mount->provider ||
        mount->namespace_busy ||
        __atomic_load_n(&mount->lifecycle_state, __ATOMIC_ACQUIRE) != 0u)
        return PROS_VFS_EBUSY;
    mount->type = type;
    mount->provider = 0;
    mount->device = device;
    mount->private_data = private_data;
    mount->root = 0;
    mount->read_only = read_only ? 1u : 0u;
    mount->active = 0;
    mount->namespace_busy = 0u;
    __atomic_store_n(&mount->lifecycle_state, 0u, __ATOMIC_RELEASE);
    return PROS_VFS_OK;
}

int vfs_mount_partition(struct pros_vfs_mount *mount,
                        enum pros_filesystem_type type,
                        struct pros_partition_device *view,
                        struct pros_block_device *parent,
                        const struct pros_partition *partition,
                        void *private_data, uint8_t read_only)
{
    if (!mount || !view || !parent || !partition ||
        type == PROS_FS_UNKNOWN)
        return PROS_VFS_EINVAL;
    /* Validate mount ownership state before mutating the partition view. */
    if (mount->active || mount->root || mount->provider ||
        mount->namespace_busy ||
        __atomic_load_n(&mount->lifecycle_state, __ATOMIC_ACQUIRE) != 0u)
        return PROS_VFS_EBUSY;
    {
        int rc = partition_device_init(view, parent, partition);
        if (rc != PROS_PARTITION_OK)
            return rc == PROS_PARTITION_EIO ? PROS_VFS_EIO :
                   rc == PROS_PARTITION_ENOTSUP ? PROS_VFS_ENOTSUP :
                   PROS_VFS_EINVAL;
    }
    return vfs_mount_init(mount, type, &view->device, private_data, read_only);
}

int vfs_probe_filesystem(struct pros_block_device *device,
                         enum pros_filesystem_type *type)
{
    if (!device || !type)
        return PROS_VFS_EINVAL;

    *type = PROS_FS_UNKNOWN;

    /*
     * ProsenjitFS is the only filesystem implemented in the kernel.
     * Compatibility filesystems live outside the kernel and must not add
     * parser-specific logic to the VFS.
     */
    {
        int rc = prosenjitfs_probe(device);
        if (rc == PROSENJITFS_OK) {
            *type = PROS_FS_PROSENJIT;
            return PROS_VFS_OK;
        }
        /*
         * ENOTSUP means this device is simply not ProsenjitFS. Other
         * failures are real probe failures and must not be collapsed into
         * "filesystem unsupported", otherwise media I/O errors disappear.
         */
        if (rc == PROSENJITFS_ENOTSUP)
            return PROS_VFS_ENOSYS;
        if (rc == PROSENJITFS_EOVERFLOW)
            return PROS_VFS_EOVERFLOW;
        if (rc == PROSENJITFS_EINVAL)
            return PROS_VFS_EINVAL;
        if (rc == PROSENJITFS_EIO)
            return PROS_VFS_EIO;
        if (rc == PROSENJITFS_ECORRUPT)
            return PROS_VFS_ECORRUPT;
        return PROS_VFS_EIO;
    }
}

int vfs_mount_set_root(struct pros_vfs_mount *mount,
                       struct pros_vfs_vnode *root)
{
    if (!mount || !root || !root->initialized || root->mount != mount ||
        root->type != PROS_VFS_NODE_DIRECTORY)
        return PROS_VFS_EINVAL;
    /*
     * The mount owns exactly one root reference. Replacing an existing root
     * without first deactivating the mount would leak that ownership and
     * could leave two filesystem roots live under one mount.
     */
    if (mount->root)
        return PROS_VFS_EBUSY;
    if (vfs_vnode_get(root) != PROS_VFS_OK)
        return PROS_VFS_EINVAL;
    mount->root = root;
    return PROS_VFS_OK;
}

int vfs_mount_activate(struct pros_vfs_mount *mount)
{
    uint32_t expected = 0u;
    if (!mount || !mount->device)
        return PROS_VFS_EINVAL;
    if (!__atomic_compare_exchange_n(&mount->lifecycle_state, &expected,
                                     VFS_MOUNT_ACTIVE_BIT, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return PROS_VFS_EBUSY;
    mount->active = 1u;
    ++health_mount_count;
    return PROS_VFS_OK;
}

static int vfs_mount_deactivate_internal(struct pros_vfs_mount *mount)
{
    uint32_t expected = VFS_MOUNT_ACTIVE_BIT;
    if (!mount || !mount->active)
        return PROS_VFS_EINVAL;

    /*
     * Transition ACTIVE -> DRAINING only when no lookup pins exist. This CAS
     * is atomic with lookup admission, so an in-flight path walk cannot race
     * root teardown and a new lookup cannot enter during teardown.
     */
    if (!__atomic_compare_exchange_n(&mount->lifecycle_state, &expected,
                                     VFS_MOUNT_DRAINING_BIT, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if ((expected & (VFS_MOUNT_ACTIVE_BIT | VFS_MOUNT_DRAINING_BIT)) == 0u)
            return PROS_VFS_EINVAL;
        return PROS_VFS_EBUSY;
    }

    /*
     * Open file/directory handles and returned vnode references retain the
     * root through the vnode parent chain. Restore ACTIVE if any remain.
     */
    if (mount->root &&
        __atomic_load_n(&mount->root->refcount, __ATOMIC_ACQUIRE) > 1u) {
        __atomic_store_n(&mount->lifecycle_state, VFS_MOUNT_ACTIVE_BIT,
                         __ATOMIC_RELEASE);
        return PROS_VFS_EBUSY;
    }

    mount->active = 0u;
    if (mount->root) {
        vfs_vnode_put(mount->root);
        mount->root = 0;
    }
    /*
     * Inactive mounts must not retain a usable device/private-data path.
     * This makes stale low-level handles fail closed even if a caller keeps
     * the old mount object around after unmount.
     */
    mount->device = 0;
    if (!mount->provider)
        mount->private_data = 0;
    mount->type = PROS_FS_UNKNOWN;
    mount->read_only = 0u;
    __atomic_store_n(&mount->lifecycle_state, 0u, __ATOMIC_RELEASE);
    ++health_unmount_count;
    return PROS_VFS_OK;
}

/*
 * Provider-owned mounts must use the provider bridge so durable state is
 * flushed and the provider pin is released only after vnode teardown.
 */
int vfs_mount_deactivate(struct pros_vfs_mount *mount)
{
    if (!mount)
        return PROS_VFS_EINVAL;
    if (mount->provider)
        return PROS_VFS_EBUSY;
    return vfs_mount_deactivate_internal(mount);
}

int vfs_mount_deactivate_provider(struct pros_vfs_mount *mount,
                                  struct pros_vfs_provider *provider)
{
    if (!mount || !provider || mount->provider != provider)
        return PROS_VFS_EINVAL;
    return vfs_mount_deactivate_internal(mount);
}

int vfs_mount_abort(struct pros_vfs_mount *mount)
{
    if (!mount || mount->provider || mount->active ||
        mount->namespace_busy ||
        __atomic_load_n(&mount->lifecycle_state, __ATOMIC_ACQUIRE) != 0u)
        return PROS_VFS_EINVAL;

    /* An unactivated mount has no lookup pins; only its own root ref may remain. */
    if (mount->root &&
        __atomic_load_n(&mount->root->refcount, __ATOMIC_ACQUIRE) != 1u)
        return PROS_VFS_EBUSY;

    if (mount->root) {
        vfs_vnode_put(mount->root);
        mount->root = 0;
    }
    mount->device = 0;
    mount->private_data = 0;
    mount->type = PROS_FS_UNKNOWN;
    mount->read_only = 0u;
    __atomic_store_n(&mount->lifecycle_state, 0u, __ATOMIC_RELEASE);
    return PROS_VFS_OK;
}

int vfs_mount_abort_provider(struct pros_vfs_mount *mount,
                             struct pros_vfs_provider *provider)
{
    if (!mount || !provider || mount->provider != provider || mount->active ||
        __atomic_load_n(&mount->lifecycle_state, __ATOMIC_ACQUIRE) != 0u)
        return PROS_VFS_EINVAL;

    /* Rollback is safe only after the provider has dropped its vnode refs. */
    if (mount->root &&
        __atomic_load_n(&mount->root->refcount, __ATOMIC_ACQUIRE) != 1u)
        return PROS_VFS_EBUSY;

    if (mount->root) {
        vfs_vnode_put(mount->root);
        mount->root = 0;
    }
    mount->device = 0;
    if (!mount->provider)
        mount->private_data = 0;
    mount->type = PROS_FS_UNKNOWN;
    mount->read_only = 0u;
    /* Keep the provider association until its cleanup callback succeeds. */
    return PROS_VFS_OK;
}

int vfs_mount_is_read_only(const struct pros_vfs_mount *mount)
{
    if (!mount)
        return PROS_VFS_EINVAL;
    return mount->read_only ? 1 : 0;
}

int vfs_file_init(struct pros_vfs_file *file,
                  struct pros_vfs_mount *mount,
                  const struct pros_vfs_file_ops *ops,
                  void *private_data, uint32_t flags)
{
    int rc;
    if (!file || !mount || !mount->active || !ops ||
        (!ops->read && !ops->read64) || !ops->close)
        return PROS_VFS_EINVAL;
    rc = vfs_active_handle_claim(file, vfs_active_file_handles);
    if (rc != PROS_VFS_OK)
        return rc;
    file->mount = mount;
    file->vnode = 0;
    file->ops = ops;
    file->private_data = private_data;
    file->position = 0;
    file->flags = flags;
    file->open = 1u;
    file->operation_busy = 0u;
    file->auth.uid = UINT32_MAX;
    file->auth.gid = UINT32_MAX;
    file->auth.valid = 0u;
    file->auth.privileged = 0u;
    file->auth.reserved[0] = 0u;
    file->auth.reserved[1] = 0u;
    rc = vfs_active_handle_set_state(
        file, vfs_active_file_handles, PROS_VFS_HANDLE_OPENING,
        PROS_VFS_HANDLE_ACTIVE);
    if (rc != PROS_VFS_OK) {
        vfs_active_handle_release(file, vfs_active_file_handles);
        return rc;
    }
    return PROS_VFS_OK;
}

/*
 * Open-file operations are deliberately fail-fast rather than spinning.
 * A caller can be preempted while a provider performs I/O; waiting here could
 * deadlock if the interrupted context needs the same handle. The byte-sized
 * atomic gate serializes position and close state across CPUs without holding
 * an IRQ-disabled critical section across filesystem callbacks.
 */
static int vfs_file_operation_enter(struct pros_vfs_file *file)
{
    uint8_t expected = 0u;
    int rc = vfs_active_handle_require_active(file, vfs_active_file_handles);
    if (rc != PROS_VFS_OK)
        return rc;
    /*
     * Use an explicit 0 -> 1 compare/exchange for the byte-sized handle gate.
     * Unlike a test-and-set primitive, this documents the only accepted idle
     * state and rejects any non-zero/corrupt gate value without modifying it.
     */
    if (!__atomic_compare_exchange_n(&file->operation_busy, &expected, 1u,
                                     0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return PROS_VFS_EBUSY;
    return PROS_VFS_OK;
}

static void vfs_file_operation_leave(struct pros_vfs_file *file)
{
    if (file)
        __atomic_store_n(&file->operation_busy, 0u, __ATOMIC_RELEASE);
}

/*
 * Validate the identity bound to an open handle without consulting the
 * filesystem provider. This is also used by zero-length I/O, which must be
 * a true no-op and must not trigger metadata or data-device reads.
 */
static int vfs_operation_identity_validate(
    const struct pros_vfs_credentials *open_auth)
{
    struct pros_vfs_credentials current;
    int rc;

    if (vfs_auth_context_validate(open_auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    if (open_auth->privileged)
        return PROS_VFS_OK;

    rc = vfs_get_current_credentials(&current);
    if (rc != PROS_VFS_OK)
        return rc;
    /* A handle cannot be used by a different process identity. */
    if (current.uid != open_auth->uid || current.gid != open_auth->gid)
        return PROS_VFS_EPERM;
    return PROS_VFS_OK;
}

static int vfs_operation_authorize(
    const struct pros_vfs_vnode *vnode,
    const struct pros_vfs_credentials *open_auth,
    uint8_t requested)
{
    int rc = vfs_operation_identity_validate(open_auth);

    if (rc != PROS_VFS_OK)
        return rc;
    return vfs_vnode_authorize(vnode, open_auth, requested);
}

static int vfs_file_read_impl(struct pros_vfs_file *file, void *buffer,
                  uint32_t length, uint32_t *bytes_read)
{
    int rc;
    /*
     * The output count is valid only after a successful read. Clear it before
     * argument and permission checks so every failure leaves a deterministic
     * zero count instead of the caller's stale value.
     */
    if (bytes_read)
        *bytes_read = 0u;
    if (!file || !file->open || !file->mount || !file->mount->active ||
        !file->ops || (!file->ops->read && !file->ops->read64) ||
        !bytes_read || !file->vnode ||
        (file->position > UINT32_MAX && !file->ops->read64))
        return PROS_VFS_EINVAL;
    if ((file->flags & PROS_VFS_O_READ) == 0u)
        return PROS_VFS_EPERM;
    /*
     * Open-time authorization already checked this handle's read permission.
     * For a zero-length no-op, revalidate only the bound actor identity: do
     * not call vnode get_metadata(), which may itself issue block I/O.
     */
    if (length == 0u) {
        rc = vfs_operation_identity_validate(&file->auth);
        if (rc != PROS_VFS_OK)
            vfs_health_error(rc);
        return rc;
    }

    rc = vfs_operation_authorize(file->vnode, &file->auth,
                                 PROS_VFS_ACCESS_READ);
    if (rc != PROS_VFS_OK) {
        vfs_health_error(rc);
        return rc;
    }
    if (!buffer)
        return PROS_VFS_EINVAL;
    /*
     * Do not use the vnode size as an authoritative EOF check. Filesystems
     * may have multiple vnodes for one inode, so a writer can extend the
     * inode after this handle was opened. The filesystem read operation is
     * authoritative and may refresh the vnode size.
     */
    if ((uint64_t)length > UINT64_MAX - file->position)
        return PROS_VFS_EOVERFLOW;

    rc = file->ops->read64 ?
         file->ops->read64(file, file->position, length, buffer) :
         file->ops->read(file, (uint32_t)file->position, length, buffer);
    if (rc < 0) { vfs_health_error(rc); return rc; }

    if ((uint32_t)rc > length)
        return PROS_VFS_EIO;
    file->position += (uint32_t)rc;
    *bytes_read = (uint32_t)rc;
    ++health_read_count;
    return PROS_VFS_OK;
}

static int vfs_file_write_impl(struct pros_vfs_file *file, const void *buffer,
                   uint32_t length, uint32_t *bytes_written)
{
    int rc;
    /*
     * Never leave a caller's stale byte count visible after a rejected or
     * failed write. Filesystem callbacks communicate errors through rc; the
     * output count is meaningful only after a successful callback.
     */
    if (bytes_written)
        *bytes_written = 0u;
    if (!file || !file->open || !file->mount || !file->mount->active ||
        !file->ops || (!file->ops->write && !file->ops->write64) ||
        !bytes_written || !file->vnode)
        return PROS_VFS_EINVAL;
    if ((file->flags & PROS_VFS_O_WRITE) == 0u || file->mount->read_only)
        return PROS_VFS_EPERM;
    rc = vfs_operation_authorize(file->vnode, &file->auth, PROS_VFS_ACCESS_WRITE);
    if (rc != PROS_VFS_OK) {
        vfs_health_error(rc);
        return rc;
    }
    if (length != 0u && !buffer)
        return PROS_VFS_EINVAL;
    /*
     * O_APPEND is an operation-time filesystem contract. Do not derive the
     * write offset from a cached vnode EOF; another handle may have extended
     * the inode since this vnode was created. The Native ProsenjitFS provider
     * selects the authoritative inode EOF under its append serialization.
     */
    if (file->position > UINT32_MAX && !file->ops->write64)
        return PROS_VFS_EOVERFLOW;
    if ((uint64_t)length > UINT64_MAX - file->position)
        return PROS_VFS_EOVERFLOW;
    rc = file->ops->write64 ?
         file->ops->write64(file, file->position, length, buffer) :
         file->ops->write(file, (uint32_t)file->position, length, buffer);
    if (rc < 0) {
        vfs_health_error(rc);
        return rc;
    }
    if ((uint32_t)rc > length)
        return PROS_VFS_EIO;
    if ((file->flags & PROS_VFS_O_APPEND) != 0u)
        file->position = file->vnode->size;
    else
        file->position += (uint32_t)rc;
    *bytes_written = (uint32_t)rc;
    ++health_write_count;
    return PROS_VFS_OK;
}

static int vfs_file_truncate_impl(struct pros_vfs_file *file, uint64_t size)
{
    if (!file || !file->open || !file->mount || !file->mount->active ||
        !file->ops || !file->ops->truncate || !file->vnode)
        return PROS_VFS_EINVAL;
    if ((file->flags & PROS_VFS_O_WRITE) == 0u || file->mount->read_only)
        return PROS_VFS_EPERM;
    {
        int access_rc = vfs_operation_authorize(file->vnode, &file->auth, PROS_VFS_ACCESS_WRITE);
        if (access_rc != PROS_VFS_OK) {
            vfs_health_error(access_rc);
            return access_rc;
        }
    }
    {
        int rc = file->ops->truncate(file, size);
        if (rc < 0) vfs_health_error(rc);
        else ++health_truncate_count;
        return rc;
    }
}

static int vfs_file_close_impl(struct pros_vfs_file *file)
{
    int rc;
    if (!file || !file->open || !file->ops || !file->ops->close)
        return PROS_VFS_EINVAL;
    rc = file->ops->close(file);
    if (rc != PROS_VFS_OK)
        return rc;
    file->open = 0u;
    if (file->vnode)
        vfs_vnode_put(file->vnode);
    file->vnode = 0;
    file->mount = 0;
    file->ops = 0;
    file->private_data = 0;
    file->position = 0;
    file->auth.uid = UINT32_MAX;
    file->auth.gid = UINT32_MAX;
    file->auth.valid = 0u;
    file->auth.privileged = 0u;
    file->auth.reserved[0] = 0u;
    file->auth.reserved[1] = 0u;
    return PROS_VFS_OK;
}

static int vfs_file_seek_impl(struct pros_vfs_file *file, uint64_t position)
{
    if (!file || !file->open || !file->mount || !file->mount->active ||
        !file->vnode || !file->ops)
        return PROS_VFS_EINVAL;
    if (position > UINT32_MAX && !file->ops->read64 &&
        !file->ops->write64)
        return PROS_VFS_EOVERFLOW;
    file->position = position;
    return PROS_VFS_OK;
}

/*
 * Public wrappers own the handle gate. A successful close clears file->open
 * while holding the gate; releasing the gate afterward lets stale callers
 * enter the implementation and receive the normal EINVAL closed-handle result.
 * Failed closes release the gate and leave the handle available for retry.
 */
int vfs_file_read(struct pros_vfs_file *file, void *buffer,
                  uint32_t length, uint32_t *bytes_read)
{
    int rc = vfs_file_operation_enter(file);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_file_read_impl(file, buffer, length, bytes_read);
    vfs_file_operation_leave(file);
    return rc;
}

int vfs_file_write(struct pros_vfs_file *file, const void *buffer,
                   uint32_t length, uint32_t *bytes_written)
{
    int rc = vfs_file_operation_enter(file);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_file_write_impl(file, buffer, length, bytes_written);
    vfs_file_operation_leave(file);
    return rc;
}

int vfs_file_truncate(struct pros_vfs_file *file, uint64_t size)
{
    int rc = vfs_file_operation_enter(file);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_file_truncate_impl(file, size);
    vfs_file_operation_leave(file);
    return rc;
}

int vfs_file_seek(struct pros_vfs_file *file, uint64_t position)
{
    int rc = vfs_file_operation_enter(file);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_file_seek_impl(file, position);
    vfs_file_operation_leave(file);
    return rc;
}

int vfs_file_close(struct pros_vfs_file *file)
{
    int rc = vfs_file_operation_enter(file);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_active_handle_set_state(
        file, vfs_active_file_handles, PROS_VFS_HANDLE_ACTIVE,
        PROS_VFS_HANDLE_CLOSING);
    if (rc != PROS_VFS_OK) {
        vfs_file_operation_leave(file);
        return rc;
    }
    rc = vfs_file_close_impl(file);
    if (rc == PROS_VFS_OK) {
        vfs_file_operation_leave(file);
        vfs_active_handle_release(file, vfs_active_file_handles);
    } else {
        (void)vfs_active_handle_set_state(
            file, vfs_active_file_handles, PROS_VFS_HANDLE_CLOSING,
            PROS_VFS_HANDLE_ACTIVE);
        vfs_file_operation_leave(file);
    }
    return rc;
}


static uint64_t vfs_64bit_test_offset;

static int vfs_64bit_test_read(struct pros_vfs_file *file, uint32_t offset,
                               uint32_t length, void *buffer)
{
    (void)file;
    (void)offset;
    (void)length;
    (void)buffer;
    return PROS_VFS_EINVAL;
}

static int vfs_64bit_test_read64(struct pros_vfs_file *file, uint64_t offset,
                                 uint32_t length, void *buffer)
{
    (void)file;
    if (!buffer || length == 0u)
        return PROS_VFS_EINVAL;
    vfs_64bit_test_offset = offset;
    ((uint8_t *)buffer)[0] = 0x5Au;
    return 1;
}

static int vfs_64bit_test_close(struct pros_vfs_file *file)
{
    return file ? PROS_VFS_OK : PROS_VFS_EINVAL;
}

int vfs_file_64bit_selftest(void)
{
    static const struct pros_vfs_file_ops wide_ops = {
        .close = vfs_64bit_test_close,
        .read64 = vfs_64bit_test_read64
    };
    static const struct pros_vfs_file_ops legacy_ops = {
        .read = vfs_64bit_test_read,
        .close = vfs_64bit_test_close
    };
    static const struct pros_vfs_vnode_ops vnode_ops = {0};
    struct pros_vfs_mount mount = {0};
    struct pros_vfs_vnode vnode;
    struct pros_vfs_file file;
    uint8_t byte = 0u;
    uint32_t count = 0u;
    const uint64_t offset = 0x100000123ULL;

    mount.active = 1u;
    if (vfs_vnode_init(&vnode, &mount, 0, PROS_VFS_NODE_FILE,
                       &vnode_ops, &wide_ops, 0, 1u) != PROS_VFS_OK ||
        vfs_file_init(&file, &mount, &wide_ops, 0, PROS_VFS_O_READ) !=
            PROS_VFS_OK)
        return -1;
    file.vnode = &vnode;
    file.auth.uid = 0u;
    file.auth.gid = 0u;
    file.auth.valid = 1u;
    file.auth.privileged = 1u;
    if (vfs_file_seek(&file, offset) != PROS_VFS_OK ||
        vfs_file_read(&file, &byte, 1u, &count) != PROS_VFS_OK ||
        count != 1u || byte != 0x5Au || vfs_64bit_test_offset != offset ||
        vfs_file_close(&file) != PROS_VFS_OK)
        return -2;

    if (vfs_vnode_init(&vnode, &mount, 0, PROS_VFS_NODE_FILE,
                       &vnode_ops, &legacy_ops, 0, 1u) != PROS_VFS_OK ||
        vfs_file_init(&file, &mount, &legacy_ops, 0, PROS_VFS_O_READ) !=
            PROS_VFS_OK)
        return -3;
    file.vnode = &vnode;
    if (vfs_file_seek(&file, offset) != PROS_VFS_EOVERFLOW ||
        vfs_file_close(&file) != PROS_VFS_OK)
        return -4;
    return PROS_VFS_OK;
}

int vfs_file_admit(const struct pros_vfs_mount *mount,
                   const uint8_t *data,
                   uint64_t length,
                   const struct pros_file_security_metadata *metadata,
                   struct pros_file_admission_record *record)
{
    struct pros_file_admission_input input;

    if (!mount || !mount->device || !mount->active ||
        !data || length == 0 || !metadata || !record)
        return PROS_VFS_EINVAL;

    /*
     * The VFS deliberately does not inspect or reinterpret filesystem
     * formats here. The filesystem driver owns format/provenance/signature
     * evidence; the security admission broker owns hashing and policy.
     */
    input.data = data;
    input.length = length;
    input.metadata = *metadata;

    return pros_file_admission_evaluate(&input, record);
}

static int vfs_security_admission_selftest(void)
{
    struct pros_block_device device;
    struct pros_vfs_mount mount = { 0 };
    struct pros_file_admission_record record;
    struct pros_file_security_metadata metadata = {
        .format_valid = 1,
        .signature_valid = 1,
        .signature_present = 1,
        .local_trusted = 0,
        .local_denied = 0,
        .reputation = PROS_FILE_REPUTATION_UNAVAILABLE,
        .provenance_known = 1
    };
    static const uint8_t data[] = { 'a', 'b', 'c' };

    if (block_device_init(&device, 512, 1,
                          vfs_selftest_read, 0, 0) != PROS_BLOCK_OK)
        return -1;
    if (vfs_mount_init(&mount, PROS_FS_PROSENJIT, &device, 0, 1) !=
        PROS_VFS_OK)
        return -1;

    if (vfs_file_admit(&mount, data, sizeof(data), &metadata, &record) !=
        PROS_VFS_EINVAL)
        return -1;

    if (vfs_mount_activate(&mount) != PROS_VFS_OK ||
        vfs_file_admit(&mount, data, sizeof(data), &metadata, &record) != 0 ||
        record.security.state != PROS_FILE_UNKNOWN ||
        record.security.action != PROS_FILE_QUARANTINE)
        return -1;

    if (vfs_mount_activate(&mount) != PROS_VFS_EBUSY ||
        vfs_mount_deactivate(&mount) != PROS_VFS_OK ||
        vfs_file_admit(&mount, data, sizeof(data), &metadata, &record) !=
            PROS_VFS_EINVAL)
        return -1;

    return 0;
}

int vfs_vnode_init(struct pros_vfs_vnode *vnode,
                   struct pros_vfs_mount *mount,
                   struct pros_vfs_vnode *parent,
                   enum pros_vfs_node_type type,
                   const struct pros_vfs_vnode_ops *ops,
                   const struct pros_vfs_file_ops *file_ops,
                   void *private_data,
                   uint64_t size)
{
    if (!vnode || !mount || !ops ||
        (type != PROS_VFS_NODE_FILE && type != PROS_VFS_NODE_DIRECTORY))
        return PROS_VFS_EINVAL;
    if (type == PROS_VFS_NODE_DIRECTORY && (!ops->lookup || !ops->readdir))
        return PROS_VFS_EINVAL;
    if (type == PROS_VFS_NODE_FILE &&
        (!file_ops || (!file_ops->read && !file_ops->read64) ||
         !file_ops->close))
        return PROS_VFS_EINVAL;
    /*
     * Parent ownership is filesystem/provider-specific: providers such as
     * ProsenjitFS retain and release it in their vnode factory/release hooks.
     */
    vnode->mount = mount;
    vnode->parent = parent;
    vnode->ops = ops;
    vnode->file_ops = file_ops;
    vnode->private_data = private_data;
    vnode->type = type;
    vnode->size = size;
    __atomic_store_n(&vnode->refcount, 1u, __ATOMIC_RELAXED);
    /* Metadata is opt-in: never interpret uninitialized memory as authority. */
    vnode->uid = 0u;
    vnode->gid = 0u;
    vnode->mode = 0u;
    vnode->metadata_valid = 0u;
    vnode->initialized = 1u;
    return PROS_VFS_OK;
}

int vfs_vnode_set_metadata(struct pros_vfs_vnode *vnode, uint16_t mode,
                            uint32_t uid, uint32_t gid)
{
    if (!vnode || !vnode->initialized || (mode & (uint16_t)~0777u) != 0u)
        return PROS_VFS_EINVAL;
    vnode->mode = mode;
    vnode->uid = uid;
    vnode->gid = gid;
    vnode->metadata_valid = 1u;
    return PROS_VFS_OK;
}

/*
 * This primitive deliberately has no implicit "root" or security-level
 * bypass. Callers must supply credentials from a trusted kernel-owned source.
 * Missing inode metadata and malformed requests fail closed.
 */
static int vfs_vnode_check_access(const struct pros_vfs_vnode *vnode, uint32_t uid,
                                  uint32_t gid, uint8_t requested)
{
    uint16_t shift;
    uint16_t bits;
    if (!vnode || !vnode->initialized || !vnode->metadata_valid ||
        requested == 0u || (requested & (uint8_t)~7u) != 0u)
        return PROS_VFS_EPERM;
    if ((vnode->mode & (uint16_t)~0777u) != 0u)
        return PROS_VFS_ECORRUPT;

    if (uid == vnode->uid)
        shift = 6u;
    else if (gid == vnode->gid)
        shift = 3u;
    else
        shift = 0u;
    bits = (uint16_t)((vnode->mode >> shift) & 7u);
    return ((bits & requested) == requested) ? PROS_VFS_OK : PROS_VFS_EPERM;
}

/*
 * Reference acquisition is valid only while the caller already owns a live
 * reference (or another lifetime guard protects the vnode pointer). The CAS
 * prevents resurrection from zero and makes overflow detection race-safe.
 */
int vfs_vnode_get(struct pros_vfs_vnode *vnode)
{
    uint32_t count;

    if (!vnode || !vnode->initialized)
        return PROS_VFS_EINVAL;

    count = __atomic_load_n(&vnode->refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (count == 0u)
            return PROS_VFS_EINVAL;
        if (count == UINT32_MAX)
            return PROS_VFS_EOVERFLOW;
        if (__atomic_compare_exchange_n(&vnode->refcount, &count, count + 1u,
                                        0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return PROS_VFS_OK;
    }
}

/*
 * Exactly one putter can perform the 1 -> 0 transition and run release().
 * Callers must own a reference; this primitive cannot make an unowned raw
 * pointer safe against concurrent provider reclamation.
 */
int vfs_vnode_put(struct pros_vfs_vnode *vnode)
{
    uint32_t count;

    if (!vnode || !vnode->initialized)
        return PROS_VFS_EINVAL;

    count = __atomic_load_n(&vnode->refcount, __ATOMIC_ACQUIRE);
    for (;;) {
        if (count == 0u)
            return PROS_VFS_EINVAL;
        if (__atomic_compare_exchange_n(&vnode->refcount, &count, count - 1u,
                                        0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }

    if (count == 1u) {
        void (*release)(struct pros_vfs_vnode *) = vnode->ops->release;
        if (release)
            release(vnode);
        else
            vnode->initialized = 0u;
    }
    return PROS_VFS_OK;
}

static int vfs_next_component(const char **cursor, char *component)
{
    const char *p = *cursor;
    uint32_t length = 0;
    while (*p == '/')
        ++p;
    if (*p == '\0') {
        *cursor = p;
        return 0;
    }
    while (*p != '\0' && *p != '/') {
        if (length >= PROS_VFS_NAME_MAX)
            return PROS_VFS_EOVERFLOW;
        component[length++] = *p++;
    }
    component[length] = '\0';
    *cursor = p;
    return 1;
}

static int vfs_path_lookup_impl(struct pros_vfs_mount *mount, const char *path,
                               struct pros_vfs_vnode **result,
                               const struct pros_vfs_credentials *auth)
{
    struct pros_vfs_vnode *current;
    const char *cursor;
    char component[PROS_VFS_NAME_MAX + 1u];
    uint32_t components = 0;
    uint32_t path_length = 0u;

    if (!mount || !path || !result || !mount->active || !mount->root ||
        path[0] == '\0')
        return PROS_VFS_EINVAL;
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;

    /*
     * Bound the complete path before taking vnode references or walking its
     * components. Without a total-length limit, a caller can supply a path
     * containing arbitrarily many repeated separators and force unbounded
     * scanning even though each individual component is short.
     */
    while (path_length < PROS_VFS_PATH_MAX && path[path_length] != '\0')
        ++path_length;
    if (path_length >= PROS_VFS_PATH_MAX)
        return PROS_VFS_EOVERFLOW;

    current = mount->root;
    if (vfs_vnode_get(current) != PROS_VFS_OK)
        return PROS_VFS_EINVAL;
    {
        int access_rc = vfs_vnode_authorize(current, auth,
                                            PROS_VFS_ACCESS_EXECUTE);
        if (access_rc != PROS_VFS_OK) {
            vfs_health_error(access_rc);
            vfs_vnode_put(current);
            return access_rc;
        }
    }
    cursor = path;

    while (1) {
        int rc = vfs_next_component(&cursor, component);
        if (rc < 0) {
            vfs_vnode_put(current);
            return rc;
        }
        if (rc == 0)
            break;
        if (++components > PROS_VFS_PATH_MAX / 2u) {
            vfs_vnode_put(current);
            return PROS_VFS_ELOOP;
        }
        if (current->type != PROS_VFS_NODE_DIRECTORY) {
            vfs_vnode_put(current);
            return PROS_VFS_ENOTDIR;
        }
        {
            int access_rc = vfs_vnode_authorize(current, auth, PROS_VFS_ACCESS_EXECUTE);
            if (access_rc != PROS_VFS_OK) {
                vfs_health_error(access_rc);
                vfs_vnode_put(current);
                return access_rc;
            }
        }
        if (component[0] == '.' && component[1] == '\0')
            continue;
        if (component[0] == '.' && component[1] == '.' &&
            component[2] == '\0') {
            struct pros_vfs_vnode *parent =
                current->parent ? current->parent : current;
            if (parent != current && vfs_vnode_get(parent) != PROS_VFS_OK) {
                vfs_vnode_put(current);
                return PROS_VFS_EINVAL;
            }
            if (parent != current)
                vfs_vnode_put(current);
            current = parent;
            continue;
        }
        if (!current->ops->lookup) {
            vfs_vnode_put(current);
            return PROS_VFS_ENOTDIR;
        }
        struct pros_vfs_vnode *next = 0;
        int lookup_rc = current->ops->lookup(current, component, &next);
        if (lookup_rc != PROS_VFS_OK) {
            /*
             * The callback contract transfers no reference on error. Ignore
             * any malformed output pointer: it may be borrowed (even current)
             * and releasing it here could consume the VFS-owned reference.
             */
            vfs_vnode_put(current);
            return lookup_rc;
        }
        if (!next || !next->initialized || next->mount != mount) {
            /*
             * Success transfers exactly one owned reference, even when the
             * returned pointer aliases current. Drop the transferred result
             * and the traversal's current reference independently.
             */
            if (next && next->initialized)
                (void)vfs_vnode_put(next);
            vfs_vnode_put(current);
            return PROS_VFS_EINVAL;
        }
        /* lookup() transfers one owned vnode reference to the caller. */
        vfs_vnode_put(current);
        current = next;
    }
    *result = current;
    return PROS_VFS_OK;
}



/*
 * Pin the mount while walking from its root. Unmount atomically closes this
 * admission gate and can only detach the root after all lookup pins drain.
 */
static int vfs_path_lookup_ctx(struct pros_vfs_mount *mount, const char *path,
                               struct pros_vfs_vnode **result,
                               const struct pros_vfs_credentials *auth)
{
    int rc = vfs_mount_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_path_lookup_impl(mount, path, result, auth);
    vfs_mount_operation_leave(mount);
    return rc;
}


int vfs_path_lookup(struct pros_vfs_mount *mount, const char *path,
                    struct pros_vfs_vnode **result)
{
    struct pros_vfs_credentials auth;
    int rc = vfs_get_current_credentials(&auth);
    return rc == PROS_VFS_OK
         ? vfs_path_lookup_ctx(mount, path, result, &auth) : rc;
}

int vfs_kernel_path_lookup(struct pros_vfs_mount *mount, const char *path,
                           struct pros_vfs_vnode **result)
{
    return vfs_path_lookup_ctx(mount, path, result, &vfs_kernel_credentials);
}

static int vfs_unlink_ctx(struct pros_vfs_mount *mount, const char *path,
                          const struct pros_vfs_credentials *auth)
{
    char parent_path[PROS_VFS_PATH_MAX + 1u];
    uint32_t length = 0u;
    uint32_t last_slash = 0xFFFFFFFFu;
    uint32_t name_start;
    uint32_t i;
    struct pros_vfs_vnode *parent = 0;
    int rc;

    if (!mount || !mount->active || !path || path[0] != '/')
        return PROS_VFS_EINVAL;
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;

    while (1) {
        if (length >= PROS_VFS_PATH_MAX)
            return PROS_VFS_EOVERFLOW;
        if (path[length] == '\0')
            break;
        if (path[length] == '/')
            last_slash = length;
        ++length;
    }

    while (length > 1u && path[length - 1u] == '/')
        --length;
    if (length <= 1u || last_slash == 0xFFFFFFFFu)
        return PROS_VFS_EPERM;
    if (last_slash >= length - 1u)
        return PROS_VFS_EINVAL;

    name_start = last_slash + 1u;
    if (length - name_start == 0u ||
        length - name_start > PROS_VFS_NAME_MAX)
        return PROS_VFS_EINVAL;

    for (i = 0u; i < length - name_start; ++i)
        if (path[name_start + i] == '/')
            return PROS_VFS_EINVAL;
    if ((length - name_start == 1u && path[name_start] == '.') ||
        (length - name_start == 2u && path[name_start] == '.' &&
         path[name_start + 1u] == '.'))
        return PROS_VFS_EINVAL;

    if (last_slash == 0u) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        for (i = 0u; i < last_slash; ++i)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
    }

    rc = vfs_path_lookup_ctx(mount, parent_path, &parent, auth);
    if (rc != PROS_VFS_OK)
        return rc;
    if (!parent || parent->type != PROS_VFS_NODE_DIRECTORY ||
        !parent->ops || !parent->ops->unlink) {
        if (parent)
            vfs_vnode_put(parent);
        return PROS_VFS_ENOTSUP;
    }

    rc = vfs_vnode_authorize(parent, auth, PROS_VFS_ACCESS_WRITE |
                                   PROS_VFS_ACCESS_EXECUTE);
    if (rc == PROS_VFS_OK) {
        char name[PROS_VFS_NAME_MAX + 1u];
        uint32_t name_length = length - name_start;
        for (i = 0u; i < name_length; ++i)
            name[i] = path[name_start + i];
        name[name_length] = '\0';
        rc = parent->ops->unlink(parent, name, auth);
    }
    vfs_vnode_put(parent);
    return rc;
}



int vfs_unlink(struct pros_vfs_mount *mount, const char *path)
{
    struct pros_vfs_credentials auth;
    int rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_get_current_credentials(&auth);
    if (rc == PROS_VFS_OK)
        rc = vfs_unlink_ctx(mount, path, &auth);
    vfs_namespace_operation_leave(mount);
    return rc;
}

int vfs_kernel_unlink(struct pros_vfs_mount *mount, const char *path)
{
    int rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_unlink_ctx(mount, path, &vfs_kernel_credentials);
    vfs_namespace_operation_leave(mount);
    return rc;
}


static int vfs_mkdir_ctx(struct pros_vfs_mount *mount, const char *path,
                         const struct pros_vfs_credentials *auth)
{
    char parent_path[PROS_VFS_PATH_MAX + 1u];
    char name[PROS_VFS_NAME_MAX + 1u];
    uint32_t length = 0u, last_slash = 0xFFFFFFFFu, i;
    struct pros_vfs_vnode *parent = 0;
    struct pros_vfs_vnode *created = 0;
    int rc;

    if (!mount || !mount->active || !path || path[0] != '/')
        return PROS_VFS_EINVAL;
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    while (1) {
        if (length >= PROS_VFS_PATH_MAX)
            return PROS_VFS_EOVERFLOW;
        if (path[length] == '\0')
            break;
        if (path[length] == '/')
            last_slash = length;
        ++length;
    }
    while (length > 1u && path[length - 1u] == '/')
        --length;
    if (length <= 1u || last_slash == 0xFFFFFFFFu ||
        last_slash >= length - 1u)
        return PROS_VFS_EINVAL;

    {
        uint32_t name_length = length - last_slash - 1u;
        if (name_length == 0u || name_length > PROS_VFS_NAME_MAX)
            return PROS_VFS_EINVAL;
        for (i = 0u; i < name_length; ++i) {
            if (path[last_slash + 1u + i] == '/')
                return PROS_VFS_EINVAL;
            name[i] = path[last_slash + 1u + i];
        }
        name[name_length] = '\0';
    }
    if ((length - last_slash - 1u == 1u && name[0] == '.') ||
        (length - last_slash - 1u == 2u && name[0] == '.' && name[1] == '.'))
        return PROS_VFS_EINVAL;

    if (last_slash == 0u) {
        parent_path[0] = '/'; parent_path[1] = '\0';
    } else {
        for (i = 0u; i < last_slash; ++i)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
    }

    rc = vfs_path_lookup_ctx(mount, parent_path, &parent, auth);
    if (rc != PROS_VFS_OK)
        return rc;
    if (!parent || parent->type != PROS_VFS_NODE_DIRECTORY ||
        !parent->ops || !parent->ops->mkdir) {
        if (parent) vfs_vnode_put(parent);
        return PROS_VFS_ENOSYS;
    }
    rc = vfs_vnode_authorize(parent, auth, PROS_VFS_ACCESS_WRITE |
                                   PROS_VFS_ACCESS_EXECUTE);
    if (rc == PROS_VFS_OK)
        rc = parent->ops->mkdir(parent, name, &created, auth);
    if (created) vfs_vnode_put(created);
    vfs_vnode_put(parent);
    if (rc < 0) vfs_health_error(rc); else ++health_mkdir_count;
    return rc;
}

int vfs_mkdir(struct pros_vfs_mount *mount, const char *path)
{
    struct pros_vfs_credentials auth;
    int rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_get_current_credentials(&auth);
    if (rc == PROS_VFS_OK)
        rc = vfs_mkdir_ctx(mount, path, &auth);
    vfs_namespace_operation_leave(mount);
    return rc;
}

int vfs_kernel_mkdir(struct pros_vfs_mount *mount, const char *path)
{
    int rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_mkdir_ctx(mount, path, &vfs_kernel_credentials);
    vfs_namespace_operation_leave(mount);
    return rc;
}


static int vfs_rmdir_ctx(struct pros_vfs_mount *mount, const char *path,
                         const struct pros_vfs_credentials *auth)
{
    char parent_path[PROS_VFS_PATH_MAX + 1u];
    char name[PROS_VFS_NAME_MAX + 1u];
    uint32_t length = 0u, last_slash = 0xFFFFFFFFu, i;
    struct pros_vfs_vnode *parent = 0;
    int rc;

    if (!mount || !mount->active || !path || path[0] != '/')
        return PROS_VFS_EINVAL;
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    while (1) {
        if (length >= PROS_VFS_PATH_MAX)
            return PROS_VFS_EOVERFLOW;
        if (path[length] == '\0')
            break;
        if (path[length] == '/')
            last_slash = length;
        ++length;
    }
    while (length > 1u && path[length - 1u] == '/')
        --length;
    if (length <= 1u || last_slash == 0xFFFFFFFFu ||
        last_slash >= length - 1u)
        return PROS_VFS_EPERM;

    {
        uint32_t name_length = length - last_slash - 1u;
        if (name_length == 0u || name_length > PROS_VFS_NAME_MAX)
            return PROS_VFS_EINVAL;
        for (i = 0u; i < name_length; ++i) {
            if (path[last_slash + 1u + i] == '/')
                return PROS_VFS_EINVAL;
            name[i] = path[last_slash + 1u + i];
        }
        name[name_length] = '\0';
    }
    if ((length - last_slash - 1u == 1u && name[0] == '.') ||
        (length - last_slash - 1u == 2u && name[0] == '.' && name[1] == '.'))
        return PROS_VFS_EINVAL;

    if (last_slash == 0u) {
        parent_path[0] = '/'; parent_path[1] = '\0';
    } else {
        for (i = 0u; i < last_slash; ++i)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
    }
    rc = vfs_path_lookup_ctx(mount, parent_path, &parent, auth);
    if (rc != PROS_VFS_OK)
        return rc;
    if (!parent || parent->type != PROS_VFS_NODE_DIRECTORY ||
        !parent->ops || !parent->ops->rmdir) {
        if (parent) vfs_vnode_put(parent);
        return PROS_VFS_ENOSYS;
    }
    rc = vfs_vnode_authorize(parent, auth, PROS_VFS_ACCESS_WRITE |
                                   PROS_VFS_ACCESS_EXECUTE);
    if (rc == PROS_VFS_OK)
        rc = parent->ops->rmdir(parent, name, auth);
    vfs_vnode_put(parent);
    if (rc < 0) vfs_health_error(rc); else ++health_rmdir_count;
    return rc;
}


int vfs_rmdir(struct pros_vfs_mount *mount, const char *path)
{
    struct pros_vfs_credentials auth;
    int rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_get_current_credentials(&auth);
    if (rc == PROS_VFS_OK)
        rc = vfs_rmdir_ctx(mount, path, &auth);
    vfs_namespace_operation_leave(mount);
    return rc;
}

int vfs_kernel_rmdir(struct pros_vfs_mount *mount, const char *path)
{
    int rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_rmdir_ctx(mount, path, &vfs_kernel_credentials);
    vfs_namespace_operation_leave(mount);
    return rc;
}


static int vfs_rename_ctx(struct pros_vfs_mount *mount, const char *old_path,
                          const char *new_path,
                          const struct pros_vfs_credentials *auth)
{
    char old_parent_path[PROS_VFS_PATH_MAX + 1u];
    char new_parent_path[PROS_VFS_PATH_MAX + 1u];
    char old_name[PROS_VFS_NAME_MAX + 1u];
    char new_name[PROS_VFS_NAME_MAX + 1u];
    uint32_t old_length = 0u, new_length = 0u;
    uint32_t old_slash = 0xFFFFFFFFu, new_slash = 0xFFFFFFFFu;
    uint32_t old_name_length, new_name_length, i;
    struct pros_vfs_vnode *old_parent = 0;
    struct pros_vfs_vnode *new_parent = 0;
    int rc;

    if (!mount || !mount->active || !old_path || !new_path ||
        old_path[0] != '/' || new_path[0] != '/')
        return PROS_VFS_EINVAL;
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;

    while (1) {
        if (old_length >= PROS_VFS_PATH_MAX)
            return PROS_VFS_EOVERFLOW;
        if (old_path[old_length] == '\0')
            break;
        if (old_path[old_length] == '/')
            old_slash = old_length;
        ++old_length;
    }
    while (1) {
        if (new_length >= PROS_VFS_PATH_MAX)
            return PROS_VFS_EOVERFLOW;
        if (new_path[new_length] == '\0')
            break;
        if (new_path[new_length] == '/')
            new_slash = new_length;
        ++new_length;
    }

    while (old_length > 1u && old_path[old_length - 1u] == '/')
        --old_length;
    while (new_length > 1u && new_path[new_length - 1u] == '/')
        --new_length;

    if (old_length <= 1u || new_length <= 1u ||
        old_slash == 0xFFFFFFFFu || new_slash == 0xFFFFFFFFu ||
        old_slash >= old_length - 1u || new_slash >= new_length - 1u)
        return PROS_VFS_EINVAL;

    old_name_length = old_length - old_slash - 1u;
    new_name_length = new_length - new_slash - 1u;
    if (old_name_length == 0u || new_name_length == 0u ||
        old_name_length > PROS_VFS_NAME_MAX ||
        new_name_length > PROS_VFS_NAME_MAX)
        return PROS_VFS_EINVAL;

    for (i = 0u; i < old_name_length; ++i) {
        if (old_path[old_slash + 1u + i] == '/')
            return PROS_VFS_EINVAL;
        old_name[i] = old_path[old_slash + 1u + i];
    }
    for (i = 0u; i < new_name_length; ++i) {
        if (new_path[new_slash + 1u + i] == '/')
            return PROS_VFS_EINVAL;
        new_name[i] = new_path[new_slash + 1u + i];
    }
    old_name[old_name_length] = '\0';
    new_name[new_name_length] = '\0';

    if ((old_name_length == 1u && old_name[0] == '.') ||
        (old_name_length == 2u && old_name[0] == '.' && old_name[1] == '.') ||
        (new_name_length == 1u && new_name[0] == '.') ||
        (new_name_length == 2u && new_name[0] == '.' && new_name[1] == '.'))
        return PROS_VFS_EINVAL;

    if (old_slash == 0u) {
        old_parent_path[0] = '/';
        old_parent_path[1] = '\0';
    } else {
        for (i = 0u; i < old_slash; ++i)
            old_parent_path[i] = old_path[i];
        old_parent_path[old_slash] = '\0';
    }
    if (new_slash == 0u) {
        new_parent_path[0] = '/';
        new_parent_path[1] = '\0';
    } else {
        for (i = 0u; i < new_slash; ++i)
            new_parent_path[i] = new_path[i];
        new_parent_path[new_slash] = '\0';
    }

    rc = vfs_path_lookup_ctx(mount, old_parent_path, &old_parent, auth);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_path_lookup_ctx(mount, new_parent_path, &new_parent, auth);
    if (rc != PROS_VFS_OK) {
        vfs_vnode_put(old_parent);
        return rc;
    }

    if (!old_parent || !new_parent ||
        old_parent->type != PROS_VFS_NODE_DIRECTORY ||
        new_parent->type != PROS_VFS_NODE_DIRECTORY) {
        rc = PROS_VFS_ENOTDIR;
        goto out;
    }
    if (!old_parent->ops || !old_parent->ops->rename) {
        rc = PROS_VFS_ENOSYS;
        goto out;
    }

    rc = vfs_vnode_authorize(old_parent, auth, PROS_VFS_ACCESS_WRITE |
                                         PROS_VFS_ACCESS_EXECUTE);
    if (rc == PROS_VFS_OK)
        rc = vfs_vnode_authorize(new_parent, auth, PROS_VFS_ACCESS_WRITE |
                                          PROS_VFS_ACCESS_EXECUTE);
    if (rc == PROS_VFS_OK)
        rc = old_parent->ops->rename(old_parent, old_name,
                                     new_parent, new_name, auth);
out:
    if (new_parent)
        vfs_vnode_put(new_parent);
    if (old_parent)
        vfs_vnode_put(old_parent);
    return rc;
}

int vfs_rename(struct pros_vfs_mount *mount, const char *old_path,
               const char *new_path)
{
    struct pros_vfs_credentials auth;
    int rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_get_current_credentials(&auth);
    if (rc == PROS_VFS_OK)
        rc = vfs_rename_ctx(mount, old_path, new_path, &auth);
    vfs_namespace_operation_leave(mount);
    return rc;
}

int vfs_kernel_rename(struct pros_vfs_mount *mount, const char *old_path,
                      const char *new_path)
{
    int rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_rename_ctx(mount, old_path, new_path, &vfs_kernel_credentials);
    vfs_namespace_operation_leave(mount);
    return rc;
}


/*
 * Metadata mutation is a VFS policy operation, not a cache setter. The
 * provider must persist the complete updated metadata record before the VFS
 * updates its cache. Only mode bits 0777 are supported in this format/API.
 */
static int vfs_update_metadata(struct pros_vfs_mount *mount, const char *path,
                               uint16_t mode, uint32_t uid, uint32_t gid,
                               uint8_t change_mode, uint8_t change_owner,
                               const struct pros_vfs_credentials *auth)
{
    struct pros_vfs_metadata metadata;
    struct pros_vfs_vnode *vnode = 0;
    int rc;

    if (!mount || !mount->active || !path ||
        (!change_mode && !change_owner) || (mode & (uint16_t)~0777u) != 0u)
        return PROS_VFS_EINVAL;
    if (mount->read_only)
        return PROS_VFS_EPERM;

    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    rc = vfs_path_lookup_ctx(mount, path, &vnode, auth);
    if (rc != PROS_VFS_OK)
        return rc;
    if (!vnode || !vnode->ops || !vnode->ops->get_metadata ||
        !vnode->ops->set_metadata) {
        rc = PROS_VFS_ENOTSUP;
        goto out;
    }

    rc = vnode->ops->get_metadata(vnode, &metadata);
    if (rc != PROS_VFS_OK) {
        rc = rc < 0 ? rc : PROS_VFS_EIO;
        goto out;
    }

    /* chown is administrator-only; chmod is owner-or-administrator. */
    if (change_owner && !auth->privileged) {
        rc = PROS_VFS_EPERM;
        goto out;
    }
    if (change_mode && !auth->privileged &&
        auth->uid != metadata.uid) {
        rc = PROS_VFS_EPERM;
        goto out;
    }

    if (change_mode)
        metadata.mode = mode;
    if (change_owner) {
        if (uid != UINT32_MAX)
            metadata.uid = uid;
        if (gid != UINT32_MAX)
            metadata.gid = gid;
    }
    metadata.reserved[0] = 0u;
    metadata.reserved[1] = 0u;

    rc = vnode->ops->set_metadata(
        vnode, &metadata,
        (uint8_t)((change_mode ? PROS_VFS_METADATA_MODE : 0u) |
                  (change_owner ? PROS_VFS_METADATA_OWNER : 0u)));
    if (rc == PROS_VFS_OK)
        rc = vfs_vnode_set_metadata(vnode, metadata.mode,
                                    metadata.uid, metadata.gid);
    if (rc > 0)
        rc = PROS_VFS_EIO;

out:
    if (vnode)
        (void)vfs_vnode_put(vnode);
    return rc;
}

/*
 * Permission/ownership changes share the namespace admission gate with
 * create, mkdir, unlink, rmdir and rename. Provider namespace callbacks
 * validate parent permissions before taking the provider I/O gate; keeping
 * metadata changes under this outer gate prevents chmod/chown from changing
 * those permissions between authorization and mutation.
 */
int vfs_chmod(struct pros_vfs_mount *mount, const char *path, uint16_t mode)
{
    struct pros_vfs_credentials auth;
    int rc;
    if ((mode & (uint16_t)~0777u) != 0u)
        return PROS_VFS_EINVAL;
    rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_get_current_credentials(&auth);
    if (rc == PROS_VFS_OK)
        rc = vfs_update_metadata(mount, path, mode, 0u, 0u, 1u, 0u, &auth);
    vfs_namespace_operation_leave(mount);
    return rc;
}

int vfs_chown(struct pros_vfs_mount *mount, const char *path,
              uint32_t uid, uint32_t gid)
{
    struct pros_vfs_credentials auth;
    int rc;
    if (uid == UINT32_MAX && gid == UINT32_MAX)
        return PROS_VFS_EINVAL;
    rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_get_current_credentials(&auth);
    if (rc == PROS_VFS_OK)
        rc = vfs_update_metadata(mount, path, 0u, uid, gid, 0u, 1u, &auth);
    vfs_namespace_operation_leave(mount);
    return rc;
}

int vfs_kernel_chmod(struct pros_vfs_mount *mount, const char *path, uint16_t mode)
{
    int rc;
    if ((mode & (uint16_t)~0777u) != 0u)
        return PROS_VFS_EINVAL;
    rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_update_metadata(mount, path, mode, 0u, 0u, 1u, 0u,
                             &vfs_kernel_credentials);
    vfs_namespace_operation_leave(mount);
    return rc;
}

int vfs_kernel_chown(struct pros_vfs_mount *mount, const char *path,
                     uint32_t uid, uint32_t gid)
{
    int rc;
    if (uid == UINT32_MAX && gid == UINT32_MAX)
        return PROS_VFS_EINVAL;
    rc = vfs_namespace_operation_enter(mount);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_update_metadata(mount, path, 0u, uid, gid, 0u, 1u,
                             &vfs_kernel_credentials);
    vfs_namespace_operation_leave(mount);
    return rc;
}

static int vfs_file_open_ctx(struct pros_vfs_mount *mount, const char *path,
                             uint32_t flags, struct pros_vfs_file *file,
                             const struct pros_vfs_credentials *auth)
{
    struct pros_vfs_vnode *vnode = 0;
    int rc;
    if (!file || (flags & ~(PROS_VFS_O_READ | PROS_VFS_O_WRITE |
                            PROS_VFS_O_DIRECTORY | PROS_VFS_O_CREATE |
                            PROS_VFS_O_APPEND)) != 0u ||
        (flags & (PROS_VFS_O_READ | PROS_VFS_O_WRITE)) == 0u ||
        (flags & PROS_VFS_O_DIRECTORY) != 0u)
        return PROS_VFS_EINVAL;
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;

    /* Reject writable opens before lookup/create: O_WRITE|O_CREATE must not
     * allocate an inode or mutate a directory on a read-only mount. */
    if (mount && mount->read_only && (flags & PROS_VFS_O_WRITE) != 0u)
        return PROS_VFS_EPERM;

    rc = vfs_path_lookup_ctx(mount, path, &vnode, auth);
    if (rc == PROS_VFS_ENOENT && (flags & PROS_VFS_O_CREATE) != 0u) {
        char parent_path[PROS_VFS_PATH_MAX + 1u];
        char name[PROS_VFS_NAME_MAX + 1u];
        uint32_t length = 0u, last_slash = 0xFFFFFFFFu;
        while (1) {
            if (length >= PROS_VFS_PATH_MAX)
                return PROS_VFS_EOVERFLOW;
            if (path[length] == '\0')
                break;
            if (path[length] == '/') last_slash = length;
            ++length;
        }
        if (length == 0u ||
            last_slash == 0xFFFFFFFFu || last_slash + 1u >= length)
            return PROS_VFS_EINVAL;
        uint32_t name_length = length - last_slash - 1u;
        if (name_length > PROS_VFS_NAME_MAX)
            return PROS_VFS_EOVERFLOW;
        for (uint32_t i = 0u; i < name_length; ++i)
            name[i] = path[last_slash + 1u + i];
        name[name_length] = '\0';
        if (last_slash == 0u) {
            parent_path[0] = '/'; parent_path[1] = '\0';
        } else {
            for (uint32_t i = 0u; i < last_slash; ++i)
                parent_path[i] = path[i];
            parent_path[last_slash] = '\0';
        }
        rc = vfs_path_lookup_ctx(mount, parent_path, &vnode, auth);
        if (rc != PROS_VFS_OK) return rc;
        if (vnode->type != PROS_VFS_NODE_DIRECTORY || !vnode->ops->create) {
            vfs_vnode_put(vnode); return PROS_VFS_ENOSYS;
        }
        rc = vfs_vnode_authorize(vnode, auth, PROS_VFS_ACCESS_WRITE |
                                      PROS_VFS_ACCESS_EXECUTE);
        if (rc != PROS_VFS_OK) {
            vfs_vnode_put(vnode);
            return rc;
        }
        struct pros_vfs_vnode *created = 0;
        rc = vnode->ops->create(vnode, name, &created, auth);
        vfs_vnode_put(vnode);
        if (rc != PROS_VFS_OK)
            return rc;
        if (!created || !created->initialized ||
            created->mount != mount ||
            created->type != PROS_VFS_NODE_FILE) {
            if (created && created->initialized)
                (void)vfs_vnode_put(created);
            return PROS_VFS_EIO;
        }
        vnode = created;
    } else if (rc != PROS_VFS_OK) {
        return rc;
    }
    if (vnode->type != PROS_VFS_NODE_FILE) {
        vfs_vnode_put(vnode); return PROS_VFS_EISDIR;
    }
    if ((flags & PROS_VFS_O_WRITE) != 0u && mount->read_only) {
        vfs_vnode_put(vnode); return PROS_VFS_EPERM;
    }
    {
        uint32_t access = 0u;
        if ((flags & PROS_VFS_O_READ) != 0u)
            access |= PROS_VFS_ACCESS_READ;
        if ((flags & PROS_VFS_O_WRITE) != 0u)
            access |= PROS_VFS_ACCESS_WRITE;
        rc = vfs_vnode_authorize(vnode, auth, (uint8_t)access);
        if (rc != PROS_VFS_OK) {
            vfs_health_error(rc);
            vfs_vnode_put(vnode);
            return rc;
        }
    }
    rc = vfs_file_init(file, mount, vnode->file_ops, vnode->private_data,
                       flags);
    if (rc != PROS_VFS_OK) {
        vfs_vnode_put(vnode);
        return rc;
    }
    file->vnode = vnode;
    file->auth = *auth;
    if (file->ops->open) {
        rc = file->ops->open(file);
        if (rc != PROS_VFS_OK) {
            /* The provider open callback must roll back its partial state. */
            file->open = 0u;
            file->vnode = 0;
            file->mount = 0;
            file->ops = 0;
            file->private_data = 0;
            (void)vfs_vnode_put(vnode);
            (void)vfs_active_handle_set_state(
                file, vfs_active_file_handles, PROS_VFS_HANDLE_ACTIVE,
                PROS_VFS_HANDLE_CLOSING);
            vfs_active_handle_release(file, vfs_active_file_handles);
            return rc < 0 ? rc : PROS_VFS_EIO;
        }
    }
    return PROS_VFS_OK;
}

int vfs_file_open(struct pros_vfs_mount *mount, const char *path,
                  uint32_t flags, struct pros_vfs_file *file)
{
    struct pros_vfs_credentials auth;
    int rc;
    uint8_t namespace_locked = 0u;
    if ((flags & PROS_VFS_O_CREATE) != 0u) {
        rc = vfs_namespace_operation_enter(mount);
        if (rc != PROS_VFS_OK)
            return rc;
        namespace_locked = 1u;
    }
    rc = vfs_get_current_credentials(&auth);
    if (rc == PROS_VFS_OK)
        rc = vfs_file_open_ctx(mount, path, flags, file, &auth);
    if (namespace_locked)
        vfs_namespace_operation_leave(mount);
    return rc;
}

int vfs_kernel_file_open(struct pros_vfs_mount *mount, const char *path,
                         uint32_t flags, struct pros_vfs_file *file)
{
    int rc;
    if ((flags & PROS_VFS_O_CREATE) != 0u) {
        rc = vfs_namespace_operation_enter(mount);
        if (rc != PROS_VFS_OK)
            return rc;
        rc = vfs_file_open_ctx(mount, path, flags, file,
                               &vfs_kernel_credentials);
        vfs_namespace_operation_leave(mount);
        return rc;
    }
    return vfs_file_open_ctx(mount, path, flags, file, &vfs_kernel_credentials);
}

static int vfs_dir_open_ctx(struct pros_vfs_mount *mount, const char *path,
                            struct pros_vfs_dir *dir,
                            const struct pros_vfs_credentials *auth)
{
    struct pros_vfs_vnode *vnode;
    int rc;
    if (!dir)
        return PROS_VFS_EINVAL;
    if (vfs_auth_context_validate(auth) != PROS_VFS_OK)
        return PROS_VFS_EPERM;
    rc = vfs_path_lookup_ctx(mount, path, &vnode, auth);
    if (rc != PROS_VFS_OK)
        return rc;
    if (vnode->type != PROS_VFS_NODE_DIRECTORY) {
        vfs_vnode_put(vnode);
        return PROS_VFS_ENOTDIR;
    }
    if (!vnode->ops->readdir) {
        vfs_vnode_put(vnode);
        return PROS_VFS_ENOSYS;
    }
    rc = vfs_vnode_authorize(vnode, auth, PROS_VFS_ACCESS_READ);
    if (rc != PROS_VFS_OK) {
        vfs_vnode_put(vnode);
        vfs_health_error(rc);
        return rc;
    }
    rc = vfs_active_handle_claim(dir, vfs_active_dir_handles);
    if (rc != PROS_VFS_OK) {
        vfs_vnode_put(vnode);
        return rc;
    }
    dir->vnode = vnode;
    dir->cookie = 0;
    dir->open = 1u;
    dir->operation_busy = 0u;
    dir->auth = *auth;
    rc = vfs_active_handle_set_state(
        dir, vfs_active_dir_handles, PROS_VFS_HANDLE_OPENING,
        PROS_VFS_HANDLE_ACTIVE);
    if (rc != PROS_VFS_OK) {
        (void)vfs_vnode_put(vnode);
        dir->vnode = 0;
        dir->open = 0u;
        vfs_active_handle_release(dir, vfs_active_dir_handles);
        return rc;
    }
    ++health_dir_open_count;
    return PROS_VFS_OK;
}

static int vfs_dir_read_impl(struct pros_vfs_dir *dir,
                              struct pros_vfs_dirent *entry)
{
    int rc;
    if (!dir || !dir->open || !dir->vnode || !entry ||
        !dir->vnode->ops->readdir)
        return PROS_VFS_EINVAL;
    rc = vfs_operation_authorize(dir->vnode, &dir->auth,
                                 PROS_VFS_ACCESS_READ);
    if (rc != PROS_VFS_OK) {
        vfs_health_error(rc);
        return rc;
    }
    rc = dir->vnode->ops->readdir(dir->vnode, &dir->cookie, entry);
    if (rc < 0) vfs_health_error(rc); else ++health_dir_read_count;
    return rc;
}

static int vfs_dir_operation_enter(struct pros_vfs_dir *dir)
{
    uint8_t expected = 0u;
    int rc = vfs_active_handle_require_active(dir, vfs_active_dir_handles);
    if (rc != PROS_VFS_OK)
        return rc;
    /* Match the file-handle gate: accept only the idle 0 -> 1 transition and
     * leave any unexpected non-zero/corrupt state untouched on rejection. */
    if (!__atomic_compare_exchange_n(&dir->operation_busy, &expected, 1u,
                                     0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return PROS_VFS_EBUSY;
    return PROS_VFS_OK;
}

static void vfs_dir_operation_leave(struct pros_vfs_dir *dir)
{
    __atomic_clear(&dir->operation_busy, __ATOMIC_RELEASE);
}

int vfs_dir_read(struct pros_vfs_dir *dir, struct pros_vfs_dirent *entry)
{
    int rc = vfs_dir_operation_enter(dir);
    if (rc != PROS_VFS_OK)
        return rc;
    rc = vfs_dir_read_impl(dir, entry);
    vfs_dir_operation_leave(dir);
    return rc;
}

int vfs_dir_close(struct pros_vfs_dir *dir)
{
    int rc = vfs_dir_operation_enter(dir);
    if (rc != PROS_VFS_OK)
        return rc;
    if (!dir->open || !dir->vnode) {
        vfs_dir_operation_leave(dir);
        return PROS_VFS_EINVAL;
    }
    rc = vfs_active_handle_set_state(
        dir, vfs_active_dir_handles, PROS_VFS_HANDLE_ACTIVE,
        PROS_VFS_HANDLE_CLOSING);
    if (rc != PROS_VFS_OK) {
        vfs_dir_operation_leave(dir);
        return rc;
    }
    (void)vfs_vnode_put(dir->vnode);
    dir->vnode = 0;
    dir->cookie = 0;
    dir->open = 0;
    dir->auth.uid = UINT32_MAX;
    dir->auth.gid = UINT32_MAX;
    dir->auth.valid = 0u;
    dir->auth.privileged = 0u;
    dir->auth.reserved[0] = 0u;
    dir->auth.reserved[1] = 0u;
    vfs_dir_operation_leave(dir);
    vfs_active_handle_release(dir, vfs_active_dir_handles);
    return PROS_VFS_OK;
}

int vfs_dir_open(struct pros_vfs_mount *mount, const char *path,
                 struct pros_vfs_dir *dir)
{
    struct pros_vfs_credentials auth;
    int rc = vfs_get_current_credentials(&auth);
    return rc == PROS_VFS_OK ? vfs_dir_open_ctx(mount, path, dir, &auth) : rc;
}

int vfs_kernel_dir_open(struct pros_vfs_mount *mount, const char *path,
                        struct pros_vfs_dir *dir)
{
    return vfs_dir_open_ctx(mount, path, dir, &vfs_kernel_credentials);
}


struct vfs_selftest_node {
    const char *name;
    enum pros_vfs_node_type type;
    const uint8_t *data;
    uint32_t size;
    struct pros_vfs_vnode *child0;
    struct pros_vfs_vnode *child1;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
};

static int vfs_selftest_lookup(struct pros_vfs_vnode *parent, const char *name,
                               struct pros_vfs_vnode **result)
{
    struct vfs_selftest_node *dir =
        (struct vfs_selftest_node *)parent->private_data;
    struct pros_vfs_vnode *children[2];
    if (!result)
        return PROS_VFS_EINVAL;
    *result = 0;
    if (!dir || !name)
        return PROS_VFS_EINVAL;
    if (vfs_selftest_mount_probe_enabled &&
        parent->mount == vfs_selftest_mount_probe) {
        vfs_selftest_mount_probe_enabled = 0u;
        vfs_selftest_mount_probe_result =
            vfs_mount_deactivate(vfs_selftest_mount_probe);
    }
    /* Deliberately malformed provider: error outputs are borrowed, not owned. */
    {
        static const char malformed_name[] = "error-parent";
        uint32_t i = 0u;
        while (malformed_name[i] && name[i] &&
               malformed_name[i] == name[i])
            ++i;
        if (!malformed_name[i] && !name[i]) {
            *result = parent;
            return PROS_VFS_EIO;
        }
    }
    children[0] = dir->child0;
    children[1] = dir->child1;
    for (uint32_t n = 0; n < 2u; ++n) {
        if (!children[n])
            continue;
        struct vfs_selftest_node *child =
            (struct vfs_selftest_node *)children[n]->private_data;
        uint32_t i = 0;
        while (child && child->name[i] && name[i] &&
               child->name[i] == name[i])
            ++i;
        if (child && child->name[i] == name[i]) {
            if (vfs_vnode_get(children[n]) != PROS_VFS_OK)
                return PROS_VFS_EINVAL;
            *result = children[n];
            return PROS_VFS_OK;
        }
    }
    return PROS_VFS_ENOENT;
}

static int vfs_selftest_readdir(struct pros_vfs_vnode *vnode,
                                uint64_t *cookie,
                                struct pros_vfs_dirent *entry)
{
    struct vfs_selftest_node *dir =
        (struct vfs_selftest_node *)vnode->private_data;
    struct pros_vfs_vnode *child = 0;
    if (!dir || !cookie || !entry)
        return PROS_VFS_EINVAL;
    if (vfs_selftest_dir_probe_enabled) {
        vfs_selftest_dir_probe_enabled = 0u;
        vfs_selftest_dir_probe_close_result =
            vfs_dir_close(vfs_selftest_dir_probe);
    }
    if (*cookie == 0u)
        child = dir->child0;
    else if (*cookie == 1u)
        child = dir->child1;
    else
        return PROS_VFS_ENOENT;
    if (!child)
        return PROS_VFS_ENOENT;
    struct vfs_selftest_node *node =
        (struct vfs_selftest_node *)child->private_data;
    uint32_t i = 0;
    while (node->name[i] && i < PROS_VFS_NAME_MAX) {
        entry->name[i] = node->name[i];
        ++i;
    }
    entry->name[i] = '\0';
    entry->type = child->type;
    entry->size = child->size;
    ++(*cookie);
    return PROS_VFS_OK;
}

static int vfs_selftest_close_file(struct pros_vfs_file *file)
{
    (void)file;
    return PROS_VFS_OK;
}

static int vfs_selftest_read_file(struct pros_vfs_file *file, uint32_t offset,
                                  uint32_t length, void *buffer)
{
    struct vfs_selftest_node *node =
        (struct vfs_selftest_node *)file->private_data;
    if (!node || !buffer || offset > node->size)
        return PROS_VFS_EINVAL;
    if (vfs_selftest_file_probe_enabled &&
        file == vfs_selftest_file_probe) {
        uint32_t nested_bytes = 0u;
        vfs_selftest_file_probe_enabled = 0u;
        vfs_selftest_file_probe_read_result =
            vfs_file_read(file, buffer, 0u, &nested_bytes);
        vfs_selftest_file_probe_seek_result = vfs_file_seek(file, 0u);
        vfs_selftest_file_probe_write_result =
            vfs_file_write(file, buffer, 0u, &nested_bytes);
        vfs_selftest_file_probe_truncate_result = vfs_file_truncate(file, 0u);
        vfs_selftest_file_probe_close_result = vfs_file_close(file);
    }
    ++vfs_selftest_read_calls;
    uint32_t available = node->size - offset;
    uint32_t take = length < available ? length : available;
    for (uint32_t i = 0; i < take; ++i)
        ((uint8_t *)buffer)[i] = node->data[offset + i];
    return (int)take;
}

static int vfs_selftest_read_error_file(struct pros_vfs_file *file,
                                        uint32_t offset, uint32_t length,
                                        void *buffer)
{
    (void)file;
    (void)offset;
    (void)length;
    (void)buffer;
    return PROS_VFS_EIO;
}

static int vfs_selftest_write_file(struct pros_vfs_file *file,
                                   uint32_t offset, uint32_t length,
                                   const void *buffer)
{
    (void)file;
    (void)offset;
    (void)length;
    (void)buffer;
    ++vfs_selftest_write_calls;
    return PROS_VFS_EIO;
}

static int vfs_selftest_truncate_file(struct pros_vfs_file *file,
                                      uint64_t size)
{
    (void)file;
    (void)size;
    ++vfs_selftest_truncate_calls;
    return PROS_VFS_EIO;
}

static int vfs_selftest_get_metadata(struct pros_vfs_vnode *vnode,
                                      struct pros_vfs_metadata *metadata)
{
    struct vfs_selftest_node *node;
    if (!vnode || !metadata || !vnode->private_data)
        return PROS_VFS_EINVAL;
    node = (struct vfs_selftest_node *)vnode->private_data;
    metadata->uid = node->uid;
    metadata->gid = node->gid;
    metadata->mode = node->mode;
    metadata->reserved[0] = 0u;
    metadata->reserved[1] = 0u;
    return PROS_VFS_OK;
}

static int vfs_selftest_set_metadata(
    struct pros_vfs_vnode *vnode, const struct pros_vfs_metadata *metadata,
    uint8_t fields)
{
    struct vfs_selftest_node *node;
    if (!vnode || !metadata || !vnode->private_data ||
        (fields & (uint8_t)~(PROS_VFS_METADATA_MODE |
                             PROS_VFS_METADATA_OWNER)) != 0u ||
        fields == 0u || (metadata->mode & (uint16_t)~0777u) != 0u)
        return PROS_VFS_EINVAL;
    node = (struct vfs_selftest_node *)vnode->private_data;
    if (fields & PROS_VFS_METADATA_MODE)
        node->mode = metadata->mode;
    if (fields & PROS_VFS_METADATA_OWNER) {
        node->uid = metadata->uid;
        node->gid = metadata->gid;
    }
    return PROS_VFS_OK;
}


static void vfs_selftest_vnode_release(struct pros_vfs_vnode *vnode)
{
    struct pros_vfs_vnode *parent;
    if (!vnode)
        return;
    parent = vnode->parent;
    vnode->parent = 0;
    vnode->private_data = 0;
    vnode->initialized = 0u;
    if (parent && parent != vnode)
        (void)vfs_vnode_put(parent);
}

static int vfs_selftest_vnode_init_child(
    struct pros_vfs_vnode *vnode, struct pros_vfs_mount *mount,
    struct pros_vfs_vnode *parent, enum pros_vfs_node_type type,
    const struct pros_vfs_vnode_ops *ops,
    const struct pros_vfs_file_ops *file_ops, void *private_data,
    uint64_t size)
{
    int rc;
    if (!parent || vfs_vnode_get(parent) != PROS_VFS_OK)
        return PROS_VFS_EINVAL;
    rc = vfs_vnode_init(vnode, mount, parent, type, ops, file_ops,
                        private_data, size);
    if (rc != PROS_VFS_OK)
        (void)vfs_vnode_put(parent);
    return rc;
}

static const struct pros_vfs_vnode_ops vfs_selftest_vnode_ops = {
    .lookup = vfs_selftest_lookup,
    .readdir = vfs_selftest_readdir,
    .release = vfs_selftest_vnode_release,
    .get_metadata = vfs_selftest_get_metadata,
    .set_metadata = vfs_selftest_set_metadata
};

static const struct pros_vfs_file_ops vfs_selftest_file_ops = {
    .read = vfs_selftest_read_file,
    .write = vfs_selftest_write_file,
    .truncate = vfs_selftest_truncate_file,
    .close = vfs_selftest_close_file
};

static const struct pros_vfs_file_ops vfs_selftest_read_error_ops = {
    .read = vfs_selftest_read_error_file,
    .close = vfs_selftest_close_file
};

static int vfs_vnode_api_selftest(void)
{
    static const uint8_t hello[] = "hello-vfs";
    static const uint8_t config[] = "config";
    static struct pros_vfs_vnode root, hello_node, etc_node, config_node;
    static struct vfs_selftest_node root_data, hello_data, etc_data, config_data;
    /* Hardened init APIs require caller-owned objects to start zeroed. */
    struct pros_block_device device = {0};
    struct pros_vfs_mount mount = {0};
    struct pros_vfs_mount partition_mount = {0};
    struct pros_partition_device partition_view = {0};
    struct pros_partition partition = {0};
    struct pros_vfs_file file = {0};
    struct pros_vfs_dir dir = {0};
    struct pros_vfs_dirent entry;
    struct pros_vfs_vnode *found = 0;
    uint8_t output[16];
    uint32_t done;
    uint32_t root_refs_before_lookup;

    root_data = (struct vfs_selftest_node){
        "root", PROS_VFS_NODE_DIRECTORY, 0, 0, &hello_node, &etc_node,
        0u, 0u, 0755u
    };
    hello_data = (struct vfs_selftest_node){
        "hello.txt", PROS_VFS_NODE_FILE, hello, sizeof(hello) - 1u, 0, 0,
        1001u, 1001u, 0640u
    };
    etc_data = (struct vfs_selftest_node){
        "etc", PROS_VFS_NODE_DIRECTORY, 0, 0, &config_node, 0,
        0u, 1001u, 0710u
    };
    config_data = (struct vfs_selftest_node){
        "config", PROS_VFS_NODE_FILE, config, sizeof(config) - 1u, 0, 0,
        1001u, 1001u, 0640u
    };

    if (block_device_init(&device, 512, 1, vfs_selftest_read, 0, 0) !=
            PROS_BLOCK_OK ||
        vfs_mount_init(&mount, PROS_FS_PROSENJIT, &device, 0, 1) !=
            PROS_VFS_OK)
        return -102;

    /*
     * Partition mount must reject a busy mount before mutating the caller's
     * partition view. A late rejection in vfs_mount_init() would leave the
     * view initialized even though the mount operation failed.
     */
    partition_mount.namespace_busy = 1u;
    partition.sector_count = 1u;
    partition_view.first_lba = UINT64_MAX;
    if (vfs_mount_partition(&partition_mount, PROS_FS_PROSENJIT,
                            &partition_view, &device, &partition, 0, 0u) !=
            PROS_VFS_EBUSY ||
        partition_view.first_lba != UINT64_MAX ||
        partition_view.parent != 0)
        return -1021;

    if (vfs_vnode_init(&root, &mount, &root, PROS_VFS_NODE_DIRECTORY,
                       &vfs_selftest_vnode_ops, 0, &root_data, 0) !=
            PROS_VFS_OK ||
        vfs_selftest_vnode_init_child(&hello_node, &mount, &root, PROS_VFS_NODE_FILE,
                       &vfs_selftest_vnode_ops, &vfs_selftest_file_ops,
                       &hello_data, sizeof(hello) - 1u) != PROS_VFS_OK ||
        vfs_selftest_vnode_init_child(&etc_node, &mount, &root, PROS_VFS_NODE_DIRECTORY,
                       &vfs_selftest_vnode_ops, 0, &etc_data, 0) !=
            PROS_VFS_OK ||
        vfs_selftest_vnode_init_child(&config_node, &mount, &etc_node, PROS_VFS_NODE_FILE,
                       &vfs_selftest_vnode_ops, &vfs_selftest_file_ops,
                       &config_data, sizeof(config) - 1u) != PROS_VFS_OK ||
        vfs_mount_set_root(&mount, &root) != PROS_VFS_OK)
        return -103;

    /*
     * Reference ownership arithmetic: verify ordinary retain/release,
     * overflow rejection, and zero-reference resurrection rejection.
     * The test restores the owned reference before any vnode traversal.
     */
    if (vfs_vnode_get(&hello_node) != PROS_VFS_OK ||
        __atomic_load_n(&hello_node.refcount, __ATOMIC_ACQUIRE) != 2u ||
        vfs_vnode_put(&hello_node) != PROS_VFS_OK ||
        __atomic_load_n(&hello_node.refcount, __ATOMIC_ACQUIRE) != 1u)
        return -1011;
    __atomic_store_n(&hello_node.refcount, UINT32_MAX, __ATOMIC_RELEASE);
    if (vfs_vnode_get(&hello_node) != PROS_VFS_EOVERFLOW) {
        __atomic_store_n(&hello_node.refcount, 1u, __ATOMIC_RELEASE);
        return -1012;
    }
    __atomic_store_n(&hello_node.refcount, 0u, __ATOMIC_RELEASE);
    if (vfs_vnode_get(&hello_node) != PROS_VFS_EINVAL ||
        vfs_vnode_put(&hello_node) != PROS_VFS_EINVAL) {
        __atomic_store_n(&hello_node.refcount, 1u, __ATOMIC_RELEASE);
        return -1013;
    }
    __atomic_store_n(&hello_node.refcount, 1u, __ATOMIC_RELEASE);

    /*
     * Permission metadata is inode-derived and unknown metadata denies
     * access. Check owner, group and other classes independently.
     */
    if (vfs_vnode_set_metadata(&hello_node, 0640u, 100u, 200u) !=
            PROS_VFS_OK ||
        vfs_vnode_check_access(&hello_node, 100u, 999u, 6u) !=
            PROS_VFS_OK ||
        vfs_vnode_check_access(&hello_node, 300u, 200u, 4u) !=
            PROS_VFS_OK ||
        vfs_vnode_check_access(&hello_node, 300u, 200u, 2u) !=
            PROS_VFS_EPERM ||
        vfs_vnode_check_access(&hello_node, 300u, 400u, 4u) !=
            PROS_VFS_EPERM ||
        vfs_vnode_check_access(&hello_node, 100u, 200u, 0u) !=
            PROS_VFS_EPERM ||
        vfs_vnode_check_access(&hello_node, 100u, 200u, 8u) !=
            PROS_VFS_EPERM ||
        vfs_vnode_check_access(&config_node, 100u, 200u, 4u) !=
            PROS_VFS_EPERM ||
        vfs_vnode_set_metadata(&config_node, 010000u, 0u, 0u) !=
            PROS_VFS_EINVAL)
        return -1021;

    /* vfs_mount_set_root() acquires the mount's ownership reference. */
    vfs_vnode_put(&root);
    if (vfs_mount_activate(&mount) != PROS_VFS_OK)
        return -104;

    /*
     * A path lookup pins the mount lifecycle word. Unmount must refuse to
     * detach the root while any such pin is held, then succeed after drain.
     */
    if (vfs_mount_operation_enter(&mount) != PROS_VFS_OK ||
        vfs_mount_deactivate(&mount) != PROS_VFS_EBUSY ||
        mount.root != &root || !mount.active) {
        vfs_mount_operation_leave(&mount);
        return -1040;
    }
    vfs_mount_operation_leave(&mount);
    if (__atomic_load_n(&mount.lifecycle_state, __ATOMIC_ACQUIRE) !=
        VFS_MOUNT_ACTIVE_BIT)
        return -10401;

    /*
     * A provider error must not transfer a reference. Even if a broken
     * provider writes a borrowed alias of current into its output, VFS must
     * release only its own traversal reference, not the borrowed pointer.
     */
    found = 0;
    root_refs_before_lookup =
        __atomic_load_n(&root.refcount, __ATOMIC_ACQUIRE);
    {
        int malformed_lookup_rc =
            vfs_kernel_path_lookup(&mount, "/error-parent", &found);
        if (malformed_lookup_rc != PROS_VFS_EIO)
            return -1040111;
        if (found != 0)
            return -1040112;
        if (__atomic_load_n(&root.refcount, __ATOMIC_ACQUIRE) !=
            root_refs_before_lookup)
            return -1040113;
    }

    /*
     * Namespace admission pins the mount through the provider callback.
     * Unmount must refuse teardown until the admitted mutation leaves.
     */
    if (vfs_namespace_operation_enter(&mount) != PROS_VFS_OK)
        return -10402;
    if (vfs_mount_deactivate(&mount) != PROS_VFS_EBUSY ||
        mount.root != &root || !mount.active) {
        vfs_namespace_operation_leave(&mount);
        return -10405;
    }
    vfs_namespace_operation_leave(&mount);
    if (__atomic_load_n(&mount.lifecycle_state, __ATOMIC_ACQUIRE) !=
        VFS_MOUNT_ACTIVE_BIT)
        return -10403;

    /*
     * Namespace mutators share one mount gate. Simulated contention must
     * reject every mutation path before it reaches a provider callback.
     * Failed admission must release its lifecycle pin.
     */
    __atomic_store_n(&mount.namespace_busy, 1u, __ATOMIC_RELEASE);
    if (vfs_kernel_unlink(&mount, "/hello.txt") != PROS_VFS_EBUSY ||
        vfs_kernel_mkdir(&mount, "/new") != PROS_VFS_EBUSY ||
        vfs_kernel_rmdir(&mount, "/etc") != PROS_VFS_EBUSY ||
        vfs_kernel_rename(&mount, "/hello.txt", "/renamed.txt") !=
            PROS_VFS_EBUSY ||
        vfs_kernel_file_open(&mount, "/created.txt",
                             PROS_VFS_O_WRITE | PROS_VFS_O_CREATE, &file) !=
            PROS_VFS_EBUSY ||
        vfs_kernel_chmod(&mount, "/hello.txt", 0600u) != PROS_VFS_EBUSY ||
        vfs_kernel_chown(&mount, "/hello.txt", 200u, UINT32_MAX) !=
            PROS_VFS_EBUSY ||
        vfs_chmod(&mount, "/hello.txt", 0600u) != PROS_VFS_EBUSY ||
        vfs_chown(&mount, "/hello.txt", 200u, UINT32_MAX) !=
            PROS_VFS_EBUSY) {
        __atomic_store_n(&mount.namespace_busy, 0u, __ATOMIC_RELEASE);
        return -10402;
    }
    __atomic_store_n(&mount.namespace_busy, 0u, __ATOMIC_RELEASE);
    if (__atomic_load_n(&mount.lifecycle_state, __ATOMIC_ACQUIRE) !=
        VFS_MOUNT_ACTIVE_BIT)
        return -10404;

    /* A corrupt non-zero namespace gate is rejected without normalization. */
    __atomic_store_n(&mount.namespace_busy, 2u, __ATOMIC_RELEASE);
    if (vfs_kernel_mkdir(&mount, "/corrupt-gate-rejected") != PROS_VFS_EBUSY ||
        __atomic_load_n(&mount.namespace_busy, __ATOMIC_ACQUIRE) != 2u ||
        __atomic_load_n(&mount.lifecycle_state, __ATOMIC_ACQUIRE) !=
            VFS_MOUNT_ACTIVE_BIT) {
        __atomic_store_n(&mount.namespace_busy, 0u, __ATOMIC_RELEASE);
        return -10406;
    }
    __atomic_store_n(&mount.namespace_busy, 0u, __ATOMIC_RELEASE);

    /* Metadata mutation must reject read-only mounts before provider calls. */
    if (vfs_kernel_chmod(&mount, "/hello.txt", 0600u) != PROS_VFS_EPERM ||
        vfs_kernel_chown(&mount, "/hello.txt", 200u, UINT32_MAX) != PROS_VFS_EPERM ||
        vfs_kernel_chmod(&mount, "/hello.txt", 01000u) != PROS_VFS_EINVAL ||
        vfs_kernel_chown(&mount, "/hello.txt", UINT32_MAX, UINT32_MAX) != PROS_VFS_EINVAL)
        return -1041;
    mount.read_only = 0u;
    if (vfs_kernel_chmod(&mount, "/hello.txt", 0600u) != PROS_VFS_OK ||
        hello_data.mode != 0600u ||
        vfs_kernel_chown(&mount, "/hello.txt", 200u, 300u) != PROS_VFS_OK ||
        hello_data.uid != 200u || hello_data.gid != 300u ||
        vfs_kernel_chmod(&mount, "/hello.txt", 0640u) != PROS_VFS_OK ||
        vfs_kernel_chown(&mount, "/hello.txt", 1001u, 1001u) != PROS_VFS_OK ||
        hello_data.mode != 0640u || hello_data.uid != 1001u || hello_data.gid != 1001u)
        return -1042;
    mount.read_only = 1u;

    if (vfs_kernel_path_lookup(&mount, "/etc/../hello.txt", &found) != PROS_VFS_OK ||
        found != &hello_node)
        return -105;
    vfs_vnode_put(found);

    /*
     * Path parsing must bound the entire string, tolerate repeated separators
     * and '.', clamp '..' at root, and preserve the result output on errors.
     */
    found = 0;
    if (vfs_kernel_path_lookup(&mount, "/etc////./../hello.txt", &found) !=
            PROS_VFS_OK ||
        found != &hello_node)
        return -1051;
    vfs_vnode_put(found);

    found = 0;
    if (vfs_kernel_path_lookup(&mount, "/../../hello.txt", &found) != PROS_VFS_OK ||
        found != &hello_node)
        return -1052;
    vfs_vnode_put(found);

    found = &hello_node;
    if (vfs_kernel_path_lookup(&mount, "", &found) != PROS_VFS_EINVAL ||
        found != &hello_node)
        return -1053;
    if (vfs_kernel_path_lookup(&mount, "/missing/child", &found) != PROS_VFS_ENOENT ||
        found != &hello_node)
        return -1054;
    if (vfs_kernel_path_lookup(&mount, "/hello.txt/child", &found) !=
            PROS_VFS_ENOTDIR ||
        found != &hello_node)
        return -1055;

    {
        char overlong_path[PROS_VFS_PATH_MAX + 1u];
        for (uint32_t n = 0u; n < PROS_VFS_PATH_MAX; ++n)
            overlong_path[n] = '/';
        overlong_path[PROS_VFS_PATH_MAX] = '\0';
        if (vfs_kernel_path_lookup(&mount, overlong_path, &found) !=
                PROS_VFS_EOVERFLOW ||
            found != &hello_node)
            return -1056;
    }

    /*
     * Re-enter mount teardown from inside provider lookup while the public
     * path lookup is admitted. The mount pin must make teardown fail fast.
     */
    vfs_selftest_mount_probe = &mount;
    vfs_selftest_mount_probe_result = PROS_VFS_EINVAL;
    vfs_selftest_mount_probe_enabled = 1u;
    if (vfs_kernel_file_open(&mount, "/hello.txt", PROS_VFS_O_READ, &file) !=
            PROS_VFS_OK) {
        vfs_selftest_mount_probe_enabled = 0u;
        vfs_selftest_mount_probe = 0;
        return -106;
    }
    vfs_selftest_mount_probe_enabled = 0u;
    vfs_selftest_mount_probe = 0;
    if (vfs_selftest_mount_probe_result != PROS_VFS_EBUSY) {
        (void)vfs_file_close(&file);
        return -1065;
    }
    /* Release the first probe handle before reusing its storage below. */
    if (vfs_file_close(&file) != PROS_VFS_OK)
        return -1067;

    if (vfs_kernel_file_open(&mount, "/hello.txt", PROS_VFS_O_READ, &file) !=
            PROS_VFS_OK)
        return -106;
    {
        struct pros_vfs_vnode *opened_vnode = file.vnode;
        if (vfs_kernel_file_open(&mount, "/hello.txt", PROS_VFS_O_READ,
                                 &file) != PROS_VFS_EBUSY ||
            !file.open || file.vnode != opened_vnode) {
            (void)vfs_file_close(&file);
            return -1068;
        }
    }
    /*
     * The provider callback runs while the handle gate is held. Attempts to
     * read, seek, write, truncate, or close the same handle must fail with
     * EBUSY without releasing its vnode or changing its file position.
     */
    vfs_selftest_file_probe = &file;
    vfs_selftest_file_probe_enabled = 1u;
    if (vfs_file_read(&file, output, 5u, &done) != PROS_VFS_OK ||
        done != 5u || output[0] != 'h' || output[4] != 'o' ||
        vfs_selftest_file_probe_read_result != PROS_VFS_EBUSY ||
        vfs_selftest_file_probe_seek_result != PROS_VFS_EBUSY ||
        vfs_selftest_file_probe_write_result != PROS_VFS_EBUSY ||
        vfs_selftest_file_probe_truncate_result != PROS_VFS_EBUSY ||
        vfs_selftest_file_probe_close_result != PROS_VFS_EBUSY) {
        vfs_selftest_file_probe_enabled = 0u;
        vfs_selftest_file_probe = 0;
        (void)vfs_file_close(&file);
        return -1066;
    }
    vfs_selftest_file_probe = 0;
    if (vfs_file_seek(&file, 6u) != PROS_VFS_OK ||
        vfs_file_read(&file, output, 16u, &done) != PROS_VFS_OK ||
        done != 3u || output[0] != 'v' ||
        vfs_file_close(&file) != PROS_VFS_OK)
        return -106;

    /*
     * A failed filesystem write must not leave the caller's old byte count
     * looking like a successful partial write.
     */
    mount.read_only = 0u;
    done = 0xA5A5A5A5u;
    if (vfs_kernel_file_open(&mount, "/hello.txt", PROS_VFS_O_WRITE, &file) !=
            PROS_VFS_OK ||
        vfs_file_write(&file, output, 1u, &done) != PROS_VFS_EIO ||
        done != 0u ||
        vfs_file_close(&file) != PROS_VFS_OK)
        return -1061;
    mount.read_only = 1u;

    /*
     * Read failures, including permission rejection and filesystem EIO, must
     * clear a caller-supplied stale byte count before returning.
     */
    mount.read_only = 0u;
    done = 0xA5A5A5A5u;
    if (vfs_kernel_file_open(&mount, "/hello.txt", PROS_VFS_O_WRITE, &file) !=
            PROS_VFS_OK ||
        vfs_file_read(&file, output, 1u, &done) != PROS_VFS_EPERM ||
        done != 0u ||
        vfs_file_close(&file) != PROS_VFS_OK)
        return -1062;
    mount.read_only = 1u;

    done = 0xA5A5A5A5u;
    if (vfs_kernel_file_open(&mount, "/hello.txt", PROS_VFS_O_READ, &file) !=
            PROS_VFS_OK)
        return -1063;
    file.ops = &vfs_selftest_read_error_ops;
    if (vfs_file_read(&file, output, 1u, &done) != PROS_VFS_EIO ||
        done != 0u ||
        vfs_file_close(&file) != PROS_VFS_OK)
        return -1064;

    if (vfs_kernel_dir_open(&mount, "/etc", &dir) != PROS_VFS_OK)
        return -107;
    {
        struct pros_vfs_vnode *opened_vnode = dir.vnode;
        if (vfs_kernel_dir_open(&mount, "/etc", &dir) != PROS_VFS_EBUSY ||
            !dir.open || dir.vnode != opened_vnode) {
            (void)vfs_dir_close(&dir);
            return -1073;
        }
    }
    /* A corrupt non-zero gate must be rejected without silently normalizing
     * its state; only the owner/repair path may clear it. */
    __atomic_store_n(&dir.operation_busy, 2u, __ATOMIC_RELEASE);
    if (vfs_dir_read(&dir, &entry) != PROS_VFS_EBUSY ||
        __atomic_load_n(&dir.operation_busy, __ATOMIC_ACQUIRE) != 2u) {
        __atomic_store_n(&dir.operation_busy, 0u, __ATOMIC_RELEASE);
        (void)vfs_dir_close(&dir);
        return -1070;
    }
    __atomic_store_n(&dir.operation_busy, 0u, __ATOMIC_RELEASE);
    /* Simulate contention: read and close must fail before touching the cursor
     * or dropping the vnode reference while another context owns the gate. */
    __atomic_store_n(&dir.operation_busy, 1u, __ATOMIC_RELEASE);
    if (vfs_dir_read(&dir, &entry) != PROS_VFS_EBUSY ||
        vfs_dir_close(&dir) != PROS_VFS_EBUSY) {
        __atomic_store_n(&dir.operation_busy, 0u, __ATOMIC_RELEASE);
        (void)vfs_dir_close(&dir);
        return -1071;
    }
    __atomic_store_n(&dir.operation_busy, 0u, __ATOMIC_RELEASE);
    /* Re-enter close from the provider's readdir callback. */
    vfs_selftest_dir_probe = &dir;
    vfs_selftest_dir_probe_close_result = PROS_VFS_EINVAL;
    vfs_selftest_dir_probe_enabled = 1u;
    if (vfs_dir_read(&dir, &entry) != PROS_VFS_OK ||
        vfs_selftest_dir_probe_close_result != PROS_VFS_EBUSY ||
        entry.type != PROS_VFS_NODE_FILE ||
        entry.name[0] != 'c') {
        vfs_selftest_dir_probe_enabled = 0u;
        vfs_selftest_dir_probe = 0;
        (void)vfs_dir_close(&dir);
        return -1072;
    }
    vfs_selftest_dir_probe = 0;
    if (vfs_dir_read(&dir, &entry) != PROS_VFS_ENOENT ||
        vfs_dir_read(&dir, &entry) != PROS_VFS_ENOENT ||
        vfs_dir_close(&dir) != PROS_VFS_OK ||
        vfs_dir_read(&dir, &entry) != PROS_VFS_EINVAL ||
        vfs_dir_close(&dir) != PROS_VFS_EINVAL)
        return -107;

    if (vfs_kernel_file_open(&mount, "/etc", PROS_VFS_O_READ, &file) !=
            PROS_VFS_EISDIR ||
        vfs_kernel_dir_open(&mount, "/hello.txt", &dir) != PROS_VFS_ENOTDIR)
        return -108;

    if (vfs_kernel_file_open(&mount, "/hello.txt", PROS_VFS_O_READ, &file) !=
        PROS_VFS_OK)
        return -109;

    /* A mount with an open file must remain active; unmount is refused. */
    if (vfs_mount_deactivate(&mount) != PROS_VFS_EBUSY)
        return -110;

    if (vfs_file_close(&file) != PROS_VFS_OK ||
        vfs_file_seek(&file, 1u) != PROS_VFS_EINVAL)
        return -111;

    /*
     * Exercise actual VFS entry points under a kernel-created limited process.
     * Denials must happen before data callbacks; group execute allows traversal
     * while missing group read denies directory enumeration.
     */
    {
        static struct pros_process actor;
        struct pros_process *previous = process_current();
        struct pros_process *actor_parent = process_lookup(1u);
        struct pros_vfs_file actor_file = {0};
        struct pros_vfs_dir actor_dir = {0};
        struct pros_vfs_vnode *actor_found = 0;
        uint8_t actor_buffer[4] = {0u};
        uint32_t actor_done = 0u;
        uint64_t reads_before, writes_before, truncates_before;
        int actor_rc = 0;
        int actor_created = 0;
        int32_t actor_exit_code = 0;

        /* The surrounding legacy API regressions test read-only mounts. */
        mount.read_only = 0u;
        if (!actor_parent ||
            process_create(actor_parent, &actor, 1002u,
                           PROS_SECURITY_LIMITED) != 0) {
            actor_rc = -120;
            goto permission_actor_out;
        }
        actor_created = 1;
        actor.fs_gid = 1001u; /* synthetic membership for the group permission case */
        process_set_current(&actor);
        if (process_current() != &actor) {
            actor_rc = -121;
            goto permission_actor_restore;
        }

        /*
         * Filesystem identity validity is a canonical Boolean, not a
         * truthy flag. Corrupt nonzero values must fail closed.
         */
        {
            struct pros_vfs_credentials invalid_identity;
            actor.fs_identity_valid = 2u;
            if (vfs_get_current_credentials(&invalid_identity) !=
                    PROS_VFS_EPERM ||
                invalid_identity.valid != 0u) {
                actor.fs_identity_valid = 1u;
                actor_rc = -1211;
                goto permission_actor_restore;
            }
            actor.fs_identity_valid = 1u;
        }

        if (vfs_chmod(&mount, "/hello.txt", 0600u) != PROS_VFS_EPERM ||
            vfs_chown(&mount, "/hello.txt", 200u, UINT32_MAX) != PROS_VFS_EPERM) {
            actor_rc = -140; goto permission_actor_restore;
        }
        actor.fs_uid = 1001u; actor.fs_gid = 1001u;
        if (vfs_chmod(&mount, "/hello.txt", 0600u) != PROS_VFS_OK ||
            hello_data.mode != 0600u ||
            vfs_chmod(&mount, "/hello.txt", 0640u) != PROS_VFS_OK ||
            hello_data.mode != 0640u) {
            actor_rc = -141; goto permission_actor_restore;
        }
        actor.fs_uid = 1002u; actor.fs_gid = 1001u;
        if (vfs_file_open(&mount, "/hello.txt", PROS_VFS_O_READ,
                          &actor_file) != PROS_VFS_OK ||
            vfs_file_read(&actor_file, actor_buffer, 1u, &actor_done) !=
                PROS_VFS_OK || actor_done != 1u) {
            actor_rc = -122;
            goto permission_actor_restore;
        }
        /* Simulate another CPU/context owning the handle gate. Every public
         * operation must fail fast before changing position or calling a provider. */
        {
            uint64_t saved_position = actor_file.position;
            uint32_t saved_reads = vfs_selftest_read_calls;
            uint32_t saved_writes = vfs_selftest_write_calls;
            uint32_t saved_truncates = vfs_selftest_truncate_calls;
            actor_file.operation_busy = 1u;
            if (vfs_file_read(&actor_file, actor_buffer, 1u, &actor_done) != PROS_VFS_EBUSY ||
                vfs_file_write(&actor_file, actor_buffer, 1u, &actor_done) != PROS_VFS_EBUSY ||
                vfs_file_truncate(&actor_file, 0u) != PROS_VFS_EBUSY ||
                vfs_file_seek(&actor_file, 0u) != PROS_VFS_EBUSY ||
                vfs_file_close(&actor_file) != PROS_VFS_EBUSY ||
                actor_file.position != saved_position ||
                vfs_selftest_read_calls != saved_reads ||
                vfs_selftest_write_calls != saved_writes ||
                vfs_selftest_truncate_calls != saved_truncates) {
                actor_file.operation_busy = 0u;
                (void)vfs_file_close(&actor_file);
                actor_rc = -142;
                goto permission_actor_restore;
            }
            actor_file.operation_busy = 0u;
        }
        if (vfs_file_close(&actor_file) != PROS_VFS_OK) {
            actor_rc = -143;
            goto permission_actor_restore;
        }

        writes_before = vfs_selftest_write_calls;
        if (vfs_file_open(&mount, "/hello.txt", PROS_VFS_O_WRITE,
                          &actor_file) != PROS_VFS_EPERM ||
            actor_file.open ||
            vfs_selftest_write_calls != writes_before) {
            actor_rc = -123;
            goto permission_actor_restore;
        }

        if (vfs_path_lookup(&mount, "/etc/config", &actor_found) !=
                PROS_VFS_OK ||
            actor_found != &config_node) {
            actor_rc = -124;
            if (actor_found)
                vfs_vnode_put(actor_found);
            goto permission_actor_restore;
        }
        vfs_vnode_put(actor_found);
        actor_found = 0;
        if (vfs_dir_open(&mount, "/etc", &actor_dir) != PROS_VFS_EPERM ||
            actor_dir.open) {
            actor_rc = -125;
            goto permission_actor_restore;
        }

        actor.fs_uid = 1003u;
        actor.fs_gid = 1003u;
        reads_before = vfs_selftest_read_calls;
        if (vfs_file_open(&mount, "/hello.txt", PROS_VFS_O_READ,
                          &actor_file) != PROS_VFS_EPERM ||
            actor_file.open || vfs_selftest_read_calls != reads_before) {
            actor_rc = -126;
            goto permission_actor_restore;
        }

        actor.fs_uid = 1001u;
        actor.fs_gid = 1001u;

        /*
         * Read permission is also revalidated on an existing handle. Denial
         * must clear the output count and happen before the provider callback.
         */
        if (vfs_file_open(&mount, "/hello.txt", PROS_VFS_O_READ,
                          &actor_file) != PROS_VFS_OK) {
            actor_rc = -137;
            goto permission_actor_restore;
        }
        actor_done = 0xA5A5A5A5u;
        reads_before = vfs_selftest_read_calls;
        actor.fs_uid = 1003u;
        actor.fs_gid = 1003u;
        if (vfs_file_read(&actor_file, actor_buffer, 1u, &actor_done) !=
                PROS_VFS_EPERM ||
            actor_done != 0u ||
            vfs_selftest_read_calls != reads_before) {
            (void)vfs_file_close(&actor_file);
            actor_rc = -138;
            goto permission_actor_restore;
        }
        actor.fs_uid = 1001u;
        actor.fs_gid = 1001u;
        if (vfs_file_close(&actor_file) != PROS_VFS_OK) {
            actor_rc = -139;
            goto permission_actor_restore;
        }

        /*
         * Authorization is rechecked at operation time, not only at open.
         * A handle opened by its owner must not retain write authority after
         * the current process identity changes to an unauthorized user.
         */
        if (vfs_file_open(&mount, "/hello.txt", PROS_VFS_O_WRITE,
                          &actor_file) != PROS_VFS_OK) {
            actor_rc = -131;
            goto permission_actor_restore;
        }
        truncates_before = vfs_selftest_truncate_calls;
        actor.fs_uid = 1003u;
        actor.fs_gid = 1003u;
        if (vfs_file_truncate(&actor_file, 0u) != PROS_VFS_EPERM ||
            vfs_selftest_truncate_calls != truncates_before) {
            (void)vfs_file_close(&actor_file);
            actor_rc = -132;
            goto permission_actor_restore;
        }
        actor.fs_uid = 1001u;
        actor.fs_gid = 1001u;
        if (vfs_file_close(&actor_file) != PROS_VFS_OK) {
            actor_rc = -133;
            goto permission_actor_restore;
        }

        /*
         * A writable handle must also re-check the current actor before write.
         * Denial must clear the byte count and must not enter the backend.
         */
        if (vfs_file_open(&mount, "/hello.txt", PROS_VFS_O_WRITE,
                          &actor_file) != PROS_VFS_OK) {
            actor_rc = -134;
            goto permission_actor_restore;
        }
        actor_done = 0xA5A5A5A5u;
        writes_before = vfs_selftest_write_calls;
        actor.fs_uid = 1003u;
        actor.fs_gid = 1003u;
        if (vfs_file_write(&actor_file, actor_buffer, 1u, &actor_done) !=
                PROS_VFS_EPERM ||
            actor_done != 0u ||
            vfs_selftest_write_calls != writes_before) {
            (void)vfs_file_close(&actor_file);
            actor_rc = -135;
            goto permission_actor_restore;
        }
        actor.fs_uid = 1001u;
        actor.fs_gid = 1001u;
        if (vfs_file_close(&actor_file) != PROS_VFS_OK) {
            actor_rc = -136;
            goto permission_actor_restore;
        }

        actor_done = 0xA5A5A5A5u;
        writes_before = vfs_selftest_write_calls;
        if (vfs_file_open(&mount, "/hello.txt", PROS_VFS_O_WRITE,
                          &actor_file) != PROS_VFS_OK ||
            vfs_file_write(&actor_file, actor_buffer, 1u, &actor_done) !=
                PROS_VFS_EIO ||
            actor_done != 0u ||
            vfs_file_close(&actor_file) != PROS_VFS_OK ||
            vfs_selftest_write_calls != writes_before + 1u) {
            actor_rc = -127;
            goto permission_actor_restore;
        }

permission_actor_restore:
        mount.read_only = 1u;
        process_set_current(previous);
        if (process_current() != previous) {
            process_set_current(0);
            if (process_current() != 0)
                return actor_rc != 0 ? actor_rc : -128;
            if (actor_rc == 0)
                actor_rc = -128;
        }
        if (actor_created) {
            /*
             * process_state()/process_set_current() authenticate registry
             * identity. Reap this synthetic actor through the real process
             * lifecycle rather than destroying an unpublished process object.
             */
            if (process_exit(&actor, 0) != 0) {
                if (actor_rc == 0) actor_rc = -129;
            } else if (process_wait(actor_parent, actor.pid,
                                    &actor_exit_code) != 0 ||
                       actor_exit_code != 0) {
                if (actor_rc == 0) actor_rc = -130;
            }
        }
permission_actor_out:
        if (actor_rc != 0)
            return actor_rc;
    }

    /*
     * Drop the test's vnode references from leaves to root. Each child holds
     * a parent reference, so the mount cannot be deactivated until these are
     * released as well as the open file handle.
     */
    vfs_vnode_put(&config_node);
    vfs_vnode_put(&etc_node);
    vfs_vnode_put(&hello_node);

    if (vfs_mount_deactivate(&mount) != PROS_VFS_OK)
        return -112;

    return 0;
}

uint64_t vfs_health_mount_count(void) { return health_mount_count; }
uint64_t vfs_health_unmount_count(void) { return health_unmount_count; }
uint64_t vfs_health_read_count(void) { return health_read_count; }
uint64_t vfs_health_write_count(void) { return health_write_count; }
uint64_t vfs_health_truncate_count(void) { return health_truncate_count; }
uint64_t vfs_health_lookup_error_count(void) { return health_lookup_error_count; }
uint64_t vfs_health_io_error_count(void) { return health_io_error_count; }
uint64_t vfs_health_permission_error_count(void) { return health_permission_error_count; }
uint64_t vfs_health_corruption_error_count(void) { return health_corruption_error_count; }
uint64_t vfs_health_unlink_count(void) { return health_unlink_count; }
uint64_t vfs_health_mkdir_count(void) { return health_mkdir_count; }
uint64_t vfs_health_rmdir_count(void) { return health_rmdir_count; }
uint64_t vfs_health_rename_count(void) { return health_rename_count; }
uint64_t vfs_health_dir_open_count(void) { return health_dir_open_count; }
uint64_t vfs_health_dir_read_count(void) { return health_dir_read_count; }

int vfs_security_selftest(void)
{
    if (vfs_permission_policy_selftest() != 0)
        return -39;
    if (vfs_probe_selftest() != 0)
        return -40;
    if (vfs_security_admission_selftest() != 0)
        return -41;
    {
        /*
         * Run the broad provider/API regression fixture as trusted kernel
         * self-test code, independent of whichever process initiated the
         * security-manager boot sequence. The actor-specific permission
         * regressions inside vfs_vnode_api_selftest() install their own
         * limited process and explicitly restore this context afterward.
         * Never let the boot process's UID accidentally change legacy fixture
         * expectations or turn an internal self-test into a user operation.
         */
        struct pros_process *previous = process_current();
        int rc;
        process_set_current(0);
        rc = vfs_vnode_api_selftest();
        process_set_current(previous);
        if (rc != 0)
            return -4200 + rc;
    }
    return 0;
}