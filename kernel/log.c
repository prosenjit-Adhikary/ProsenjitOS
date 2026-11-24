#include "log.h"
#include "log_persist.h"

static uint8_t initialized;
static uint32_t persistence_failures;

int pros_log_init(void) {
    if (pros_health_init() != PROS_HEALTH_OK ||
        pros_log_persist_init() != PROS_LOG_OK)
        return PROS_LOG_EIO;
    initialized = 1u;
    persistence_failures = 0u;
    return PROS_LOG_OK;
}

int pros_log_emit(uint32_t severity, uint32_t category,
                  uint32_t source, uint32_t code, uint64_t value) {
    if (!initialized)
        return PROS_LOG_EINVAL;
    int rc = pros_health_record(severity, category, source, code, value);
    struct pros_health_snapshot snapshot;
    struct pros_health_event event;
    if (rc != PROS_LOG_OK)
        return rc;
    if (pros_health_snapshot(&snapshot) == PROS_HEALTH_OK &&
        snapshot.event_count != 0u &&
        pros_health_get_event(snapshot.event_count - 1u, &event) == PROS_HEALTH_OK)
        {
            int persist_rc = pros_log_persist_emit(&event);
            if (persist_rc != PROS_LOG_PERSIST_OK) {
                ++persistence_failures;
                /* Record degradation in the authoritative in-memory ring. Do
                 * not recurse through pros_log_emit(), because the persistence
                 * sink is precisely the backend that just failed. */
                (void)pros_health_record(PROS_HEALTH_WARN,
                                         PROS_HEALTH_RECOVERY,
                                         0x4C4F4750u, 2u,
                                         (uint64_t)(uint32_t)(-persist_rc));
            }
        }
    return PROS_LOG_OK;
}

int pros_log_attach_persistent(struct pros_vfs_mount *mount, const char *path) {
    struct pros_log_persist_policy policy = {
        .max_bytes = PROS_LOG_PERSIST_MAX_BYTES,
        .rotate_bytes = PROS_LOG_PERSIST_ROTATE_BYTES,
        .max_files = 3u
    };
    if (!initialized || !mount || !path)
        return PROS_LOG_EINVAL;
    return pros_log_persist_attach(mount, path, &policy);
}

uint32_t pros_log_persistence_failures(void) {
    return persistence_failures;
}

int pros_log_detach_persistent(void) {
    if (!initialized)
        return PROS_LOG_EINVAL;
    return pros_log_persist_detach();
}

int pros_log_sink_available(uint32_t sink) {
    if (!initialized)
        return PROS_LOG_EINVAL;
    if (sink != PROS_LOG_SINK_HEALTH)
        return PROS_LOG_EINVAL;
    return PROS_LOG_OK;
}

int pros_log_selftest(void) {
    struct pros_health_snapshot snapshot;
    if (pros_log_init() != PROS_LOG_OK)
        return -101;
    if (pros_log_persist_selftest() != PROS_LOG_PERSIST_OK)
        return -102;
    if (pros_log_emit(PROS_HEALTH_INFO, PROS_HEALTH_SYSTEM,
                      0x4C4F4701u, 1u, 1u) != PROS_LOG_OK)
        return -103;
    if (pros_health_snapshot(&snapshot) != PROS_LOG_OK)
        return -104;
    if (snapshot.event_count == 0u)
        return PROS_LOG_EIO;
    return pros_log_sink_available(PROS_LOG_SINK_HEALTH);
}
