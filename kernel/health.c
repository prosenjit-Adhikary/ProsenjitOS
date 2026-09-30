#include "health.h"
#include "heap.h"
#include "memory.h"
#include "timer.h"
#include "thread.h"
#include "block.h"
#include "vfs.h"

extern uint64_t kernel_exception_count(void);
extern uint64_t kernel_page_fault_count(void);
extern uint64_t kernel_last_exception_vector(void);
extern uint64_t kernel_last_exception_rip(void);

static struct pros_health_event events[PROS_HEALTH_EVENT_MAX];
static uint32_t event_head;
static uint32_t event_count;
static uint32_t dropped_events;
static uint64_t next_sequence;
static uint8_t initialized;

static int valid_severity(uint32_t value) {
    return value <= PROS_HEALTH_CRITICAL;
}

static int valid_category(uint32_t value) {
    return value <= PROS_HEALTH_RECOVERY;
}

int pros_health_init(void) {
    for (uint32_t i = 0u; i < PROS_HEALTH_EVENT_MAX; ++i)
        events[i] = (struct pros_health_event){0};
    event_head = 0u;
    event_count = 0u;
    dropped_events = 0u;
    next_sequence = 0u;
    initialized = 1u;
    return PROS_HEALTH_OK;
}

int pros_health_record(uint32_t severity, uint32_t category,
                       uint32_t source, uint32_t code, uint64_t value) {
    if (!initialized || !valid_severity(severity) || !valid_category(category))
        return PROS_HEALTH_EINVAL;

    events[event_head] = (struct pros_health_event){
        .sequence = ++next_sequence,
        .timestamp = timer_ticks(),
        .severity = severity,
        .category = category,
        .source = source,
        .code = code,
        .value = value
    };
    event_head = (event_head + 1u) % PROS_HEALTH_EVENT_MAX;
    if (event_count < PROS_HEALTH_EVENT_MAX)
        ++event_count;
    else
        ++dropped_events;
    return PROS_HEALTH_OK;
}

int pros_health_snapshot(struct pros_health_snapshot *out) {
    if (!initialized || !out)
        return PROS_HEALTH_EINVAL;

    *out = (struct pros_health_snapshot){
        .timestamp = timer_ticks(),
        .uptime_ticks = timer_ticks(),
        .timer_irq_count = timer_irq_count(),
        .scheduler_preempt_count = timer_preempt_count(),
        .runnable_threads = thread_runnable_count(),
        .blocked_threads = thread_blocked_count(),
        .dead_threads = thread_dead_count(),
        .physical_total_pages = physical_memory_total_pages(),
        .physical_free_pages = physical_memory_free_pages(),
        .physical_failed_allocations = physical_memory_failed_allocations(),
        .block_read_count = block_read_count(),
        .block_write_count = block_write_count(),
        .block_error_count = block_error_count(),
        .block_flush_count = block_flush_count(),
        .block_flush_error_count = block_flush_error_count(),
        .exception_count = kernel_exception_count(),
        .page_fault_count = kernel_page_fault_count(),
        .last_exception_vector = kernel_last_exception_vector(),
        .last_exception_rip = kernel_last_exception_rip(),
        .memory_pressure = (physical_memory_total_pages() != 0 &&
                            physical_memory_free_pages() * 100ULL <
                            physical_memory_total_pages() * 10ULL) ? 2u :
                           (physical_memory_total_pages() != 0 &&
                            physical_memory_free_pages() * 100ULL <
                            physical_memory_total_pages() * 20ULL) ? 1u : 0u,
        .heap_used_bytes = heap_used_bytes(),
        .fs_mount_count = vfs_health_mount_count(),
        .fs_unmount_count = vfs_health_unmount_count(),
        .fs_read_count = vfs_health_read_count(),
        .fs_write_count = vfs_health_write_count(),
        .fs_truncate_count = vfs_health_truncate_count(),
        .fs_lookup_error_count = vfs_health_lookup_error_count(),
        .fs_io_error_count = vfs_health_io_error_count(),
        .fs_permission_error_count = vfs_health_permission_error_count(),
        .fs_corruption_error_count = vfs_health_corruption_error_count(),
        .event_sequence = next_sequence,
        .event_count = event_count,
        .dropped_events = dropped_events,
        .last_severity = 0u,
        .last_category = 0u
    };

    if (event_count) {
        uint32_t last = (event_head + PROS_HEALTH_EVENT_MAX - 1u) %
                        PROS_HEALTH_EVENT_MAX;
        out->last_severity = events[last].severity;
        out->last_category = events[last].category;
    }
    return PROS_HEALTH_OK;
}

int pros_health_get_event(uint32_t offset, struct pros_health_event *out) {
    uint32_t oldest;
    uint32_t index;

    if (!initialized || !out || offset >= event_count)
        return PROS_HEALTH_EINVAL;

    oldest = (event_head + PROS_HEALTH_EVENT_MAX - event_count) %
             PROS_HEALTH_EVENT_MAX;
    index = (oldest + offset) % PROS_HEALTH_EVENT_MAX;
    *out = events[index];
    return PROS_HEALTH_OK;
}

static uint32_t append_hex(char *buffer, uint32_t capacity, uint32_t pos, uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    if (pos + 18u > capacity) return UINT32_MAX;
    buffer[pos++] = '0'; buffer[pos++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        buffer[pos++] = hex[(value >> shift) & 0xFu];
    return pos;
}

static uint32_t append_u32(char *buffer, uint32_t capacity, uint32_t pos, uint32_t value) {
    char tmp[10];
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

int pros_health_export(char *buffer, uint32_t capacity, uint32_t *length_out) {
    uint32_t pos = 0u;
    if (!initialized || !buffer || !length_out || capacity == 0u)
        return PROS_HEALTH_EINVAL;
    for (uint32_t i = 0u; i < event_count; ++i) {
        struct pros_health_event event;
        uint32_t next;
        if (pros_health_get_event(i, &event) != PROS_HEALTH_OK)
            return PROS_HEALTH_EIO;
        next = append_u32(buffer, capacity, pos, (uint32_t)event.sequence);
        if (next == UINT32_MAX || next + 1u >= capacity) return PROS_HEALTH_EIO;
        pos = next; buffer[pos++] = ' ';
        next = append_hex(buffer, capacity, pos, event.timestamp);
        if (next == UINT32_MAX || next + 1u >= capacity) return PROS_HEALTH_EIO;
        pos = next; buffer[pos++] = ' ';
        next = append_u32(buffer, capacity, pos, event.severity);
        if (next == UINT32_MAX || next + 1u >= capacity) return PROS_HEALTH_EIO;
        pos = next; buffer[pos++] = ' ';
        next = append_u32(buffer, capacity, pos, event.category);
        if (next == UINT32_MAX || next + 1u >= capacity) return PROS_HEALTH_EIO;
        pos = next; buffer[pos++] = ' ';
        next = append_u32(buffer, capacity, pos, event.source);
        if (next == UINT32_MAX || next + 1u >= capacity) return PROS_HEALTH_EIO;
        pos = next; buffer[pos++] = ' ';
        next = append_u32(buffer, capacity, pos, event.code);
        if (next == UINT32_MAX || next + 1u >= capacity) return PROS_HEALTH_EIO;
        pos = next; buffer[pos++] = ' ';
        next = append_hex(buffer, capacity, pos, event.value);
        if (next == UINT32_MAX || next + 1u >= capacity) return PROS_HEALTH_EIO;
        pos = next; buffer[pos++] = '\n';
    }
    if (pos >= capacity) return PROS_HEALTH_EIO;
    buffer[pos] = '\0';
    *length_out = pos;
    return PROS_HEALTH_OK;
}

int pros_health_sample(void) {
    return pros_health_record(PROS_HEALTH_INFO, PROS_HEALTH_SYSTEM,
                              1u, 1u, physical_memory_free_pages());
}

int pros_health_selftest(void) {
    struct pros_health_snapshot snapshot;
    struct pros_health_event event;
    char export_buffer[1024];
    uint32_t export_length = 0u;

    if (pros_health_init() != PROS_HEALTH_OK ||
        pros_health_record(PROS_HEALTH_INFO, PROS_HEALTH_MEMORY,
                           2u, 1u, heap_used_bytes()) != PROS_HEALTH_OK ||
        pros_health_record(PROS_HEALTH_WARN, PROS_HEALTH_SYSTEM,
                           2u, 2u, 123u) != PROS_HEALTH_OK ||
        pros_health_snapshot(&snapshot) != PROS_HEALTH_OK ||
        snapshot.event_count != 2u ||
        snapshot.last_severity != PROS_HEALTH_WARN ||
        pros_health_get_event(0u, &event) != PROS_HEALTH_OK ||
        event.category != PROS_HEALTH_MEMORY ||
        pros_health_get_event(1u, &event) != PROS_HEALTH_OK ||
        event.value != 123u ||
        pros_health_export(export_buffer, sizeof(export_buffer),
                           &export_length) != PROS_HEALTH_OK ||
        export_length == 0u ||
        export_buffer[export_length] != '\0')
        return PROS_HEALTH_EIO;

    if (pros_health_init() != PROS_HEALTH_OK)
        return PROS_HEALTH_EIO;

    for (uint32_t i = 0u; i < PROS_HEALTH_EVENT_MAX + 1u; ++i) {
        if (pros_health_record(PROS_HEALTH_INFO, PROS_HEALTH_SYSTEM,
                               3u, i, i) != PROS_HEALTH_OK)
            return PROS_HEALTH_EIO;
    }

    if (pros_health_snapshot(&snapshot) != PROS_HEALTH_OK ||
        snapshot.event_count != PROS_HEALTH_EVENT_MAX ||
        snapshot.dropped_events != 1u ||
        pros_health_get_event(0u, &event) != PROS_HEALTH_OK ||
        event.sequence != 2u ||
        event.code != 1u ||
        pros_health_get_event(PROS_HEALTH_EVENT_MAX - 1u, &event) != PROS_HEALTH_OK ||
        event.sequence != PROS_HEALTH_EVENT_MAX + 1u)
        return PROS_HEALTH_EIO;

    return PROS_HEALTH_OK;
}
