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

static uint32_t append_u64(char *buffer, uint32_t capacity, uint32_t pos, uint64_t value) {
    char tmp[20];
    uint32_t n = 0u;
    if (value == 0u) {
        if (pos >= capacity) return UINT32_MAX;
        buffer[pos++] = '0';
        return pos;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    if (pos + n > capacity) return UINT32_MAX;
    while (n) buffer[pos++] = tmp[--n];
    return pos;
}

static uint32_t append_u32(char *buffer, uint32_t capacity, uint32_t pos, uint32_t value) {
    return append_u64(buffer, capacity, pos, value);
}

int pros_log_persist_emit(const struct pros_health_event *event) {
    char line[96];
    uint32_t pos = 0u, next, written = 0u;

    if (!initialized || !event)
        return PROS_LOG_PERSIST_EINVAL;
    if (!attached)
        return PROS_LOG_PERSIST_ENOTSUP;
    if (bytes_written >= persist_policy.rotate_bytes)
        return PROS_LOG_PERSIST_ENOTSUP;

    next = append_u64(line, sizeof(line), pos, event->sequence);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u64(line, sizeof(line), pos, event->timestamp);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u32(line, sizeof(line), pos, event->severity);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u32(line, sizeof(line), pos, event->category);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u32(line, sizeof(line), pos, event->source);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u32(line, sizeof(line), pos, event->code);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = ' ';
    next = append_u64(line, sizeof(line), pos, event->value);
    if (next == UINT32_MAX || next + 1u >= sizeof(line)) return PROS_LOG_PERSIST_EIO;
    pos = next; line[pos++] = '\n';

    if (bytes_written + pos > persist_policy.max_bytes)
        return PROS_LOG_PERSIST_ENOTSUP;
    if (vfs_file_seek(&persist_file, bytes_written) != PROS_VFS_OK ||
        vfs_file_write(&persist_file, line, pos, &written) != PROS_VFS_OK ||
        written != pos)
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
