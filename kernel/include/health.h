#ifndef PROSENJIT_HEALTH_H
#define PROSENJIT_HEALTH_H

#include <stdint.h>

#define PROS_HEALTH_EVENT_MAX 128u

#define PROS_HEALTH_OK       0
#define PROS_HEALTH_EINVAL  -1
#define PROS_HEALTH_EIO     -2

enum pros_health_severity {
    PROS_HEALTH_DEBUG = 0,
    PROS_HEALTH_INFO = 1,
    PROS_HEALTH_WARN = 2,
    PROS_HEALTH_ERROR = 3,
    PROS_HEALTH_CRITICAL = 4
};

enum pros_health_category {
    PROS_HEALTH_SYSTEM = 0,
    PROS_HEALTH_MEMORY = 1,
    PROS_HEALTH_SCHEDULER = 2,
    PROS_HEALTH_STORAGE = 3,
    PROS_HEALTH_NETWORK = 4,
    PROS_HEALTH_SECURITY = 5,
    PROS_HEALTH_DRIVER = 6,
    PROS_HEALTH_FILESYSTEM = 7,
    PROS_HEALTH_RECOVERY = 8
};

struct pros_health_event {
    uint64_t sequence;
    uint64_t timestamp;
    uint32_t severity;
    uint32_t category;
    uint32_t source;
    uint32_t code;
    uint64_t value;
};

struct pros_health_snapshot {
    uint64_t timestamp;
    uint64_t uptime_ticks;
    uint64_t timer_irq_count;
    uint64_t scheduler_preempt_count;
    uint64_t runnable_threads;
    uint64_t blocked_threads;
    uint64_t dead_threads;
    uint64_t physical_total_pages;
    uint64_t physical_free_pages;
    uint64_t heap_used_bytes;
    uint64_t event_sequence;
    uint32_t event_count;
    uint32_t dropped_events;
    uint32_t last_severity;
    uint32_t last_category;
};

int pros_health_init(void);
int pros_health_record(uint32_t severity, uint32_t category,
                       uint32_t source, uint32_t code, uint64_t value);
int pros_health_snapshot(struct pros_health_snapshot *out);
int pros_health_get_event(uint32_t offset, struct pros_health_event *out);
int pros_health_export(char *buffer, uint32_t capacity, uint32_t *length_out);
int pros_health_sample(void);
int pros_health_selftest(void);

#endif
