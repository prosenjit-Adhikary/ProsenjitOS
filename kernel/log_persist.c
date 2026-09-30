#include "log_persist.h"

static struct pros_vfs_mount *persist_mount;
static struct pros_vfs_file persist_file;
static struct pros_log_persist_policy persist_policy;
static uint8_t initialized;
static uint8_t attached;
static uint32_t bytes_written;

static uint32_t bounded_strlen(const char *s, uint32_t max) {
    uint32_t n = 0u;
    if (!s) return 0u;
    while (n < max && s[n]) ++n;
    return n;
}

int pros_log_persist_init(void) {
    persist_mount = 0;
    persist_file = (struct pros_vfs_file){0};
    persist_policy = (struct pros_log_persist_policy){
        .max_bytes = PROS_LOG_PERSIST_MAX_BYTES,
        .rotate_bytes = PROS_LOG_PERSIST_ROTATE_BYTES,
        .max_files = 3u
    };
    initialized = 1u;
    attached = 0u;
    bytes_written = 0u;
    return PROS_LOG_PERSIST_OK;
}

int pros_log_persist_attach(struct pros_vfs_mount *mount,
                            const char *path,
                            const struct pros_log_persist_policy *policy) {
    uint32_t length;
    if (!initialized || !mount || !mount->active || !mount->root ||
        !path || !policy)
        return PROS_LOG_PERSIST_EINVAL;
    if (mount->read_only || policy->max_bytes == 0u ||
        policy->max_bytes > PROS_LOG_PERSIST_MAX_BYTES ||
        policy->rotate_bytes == 0u ||
        policy->rotate_bytes > policy->max_bytes ||
        policy->max_files == 0u || policy->max_files > 8u)
        return PROS_LOG_PERSIST_EINVAL;

    length = bounded_strlen(path, PROS_LOG_PERSIST_PATH_MAX);
    if (length == 0u || length >= PROS_LOG_PERSIST_PATH_MAX)
        return PROS_LOG_PERSIST_EINVAL;

    if (attached)
        (void)vfs_file_close(&persist_file);

    if (vfs_file_open(mount, path, PROS_VFS_O_WRITE | PROS_VFS_O_CREATE,
                      &persist_file) != PROS_VFS_OK)
        return PROS_LOG_PERSIST_EIO;

    persist_mount = mount;
    persist_policy = *policy;
    attached = 1u;
    bytes_written = 0u;
    return PROS_LOG_PERSIST_OK;
}

int pros_log_persist_emit(const struct pros_health_event *event) {
    char line[96];
    uint32_t written = 0u;
    int n;

    if (!initialized || !event)
        return PROS_LOG_PERSIST_EINVAL;
    if (!attached)
        return PROS_LOG_PERSIST_ENOTSUP;
    if (bytes_written >= persist_policy.rotate_bytes)
        return PROS_LOG_PERSIST_EIO;

    n = 0;
    n += __builtin_snprintf(line + n, sizeof(line) - (uint32_t)n,
                            "%llu %llu %u %u %u %u %llu\n",
                            (unsigned long long)event->sequence,
                            (unsigned long long)event->timestamp,
                            event->severity, event->category,
                            event->source, event->code,
                            (unsigned long long)event->value);
    if (n <= 0 || (uint32_t)n >= sizeof(line) ||
        bytes_written + (uint32_t)n > persist_policy.max_bytes)
        return PROS_LOG_PERSIST_EIO;

    if (vfs_file_seek(&persist_file, bytes_written) != PROS_VFS_OK ||
        vfs_file_write(&persist_file, line, (uint32_t)n, &written) != PROS_VFS_OK ||
        written != (uint32_t)n)
        return PROS_LOG_PERSIST_EIO;

    bytes_written += written;
    return PROS_LOG_PERSIST_OK;
}

int pros_log_persist_flush(void) {
    if (!initialized)
        return PROS_LOG_PERSIST_EINVAL;
    if (!attached)
        return PROS_LOG_PERSIST_ENOTSUP;
    return PROS_LOG_PERSIST_OK;
}

int pros_log_persist_available(void) {
    return initialized && attached;
}

int pros_log_persist_selftest(void) {
    if (pros_log_persist_init() != PROS_LOG_PERSIST_OK)
        return PROS_LOG_PERSIST_EIO;
    if (pros_log_persist_available())
        return PROS_LOG_PERSIST_EIO;
    return PROS_LOG_PERSIST_OK;
}
