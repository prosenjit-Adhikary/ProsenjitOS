#include "security_monitor.h"
#include "health.h"

#include <stdint.h>

static struct pros_security_monitor_state states[
    PROS_SECURITY_MONITOR_MAX_PROCESSES];

static uint32_t event_weight(enum pros_security_event_type type) {
    switch (type) {
    case PROS_SECURITY_EVENT_MASS_FILE_MODIFY: return 40u;
    case PROS_SECURITY_EVENT_PERSISTENCE_CHANGE: return 60u;
    case PROS_SECURITY_EVENT_PRIVILEGE_ESCALATION: return 90u;
    case PROS_SECURITY_EVENT_PROCESS_INJECTION: return 140u;
    case PROS_SECURITY_EVENT_SECURITY_TAMPER: return 180u;
    case PROS_SECURITY_EVENT_ABNORMAL_NETWORK: return 50u;
    case PROS_SECURITY_EVENT_CREDENTIAL_ACCESS: return 80u;
    case PROS_SECURITY_EVENT_POLICY_VIOLATION: return 70u;
    default: return 0u;
    }
}

static struct pros_security_monitor_state *find_state(uint64_t pid) {
    if (pid == 0u) return 0;
    for (uint32_t i = 0u; i < PROS_SECURITY_MONITOR_MAX_PROCESSES; ++i)
        if (states[i].active && states[i].pid == pid)
            return &states[i];
    return 0;
}

static struct pros_security_monitor_state *alloc_state(uint64_t pid) {
    for (uint32_t i = 0u; i < PROS_SECURITY_MONITOR_MAX_PROCESSES; ++i) {
        if (!states[i].active) {
            states[i].active = 1u;
            states[i].pid = pid;
            states[i].last_timestamp = 0u;
            states[i].score = 0u;
            states[i].event_count = 0u;
            states[i].action = PROS_SECURITY_MONITOR_ALLOW;
            return &states[i];
        }
    }
    return 0;
}

static enum pros_security_monitor_action decide(
    enum pros_security_event_type type, uint32_t score) {
    if (type == PROS_SECURITY_EVENT_SECURITY_TAMPER ||
        type == PROS_SECURITY_EVENT_PROCESS_INJECTION ||
        score >= PROS_SECURITY_MONITOR_BLOCK_SCORE)
        return PROS_SECURITY_MONITOR_BLOCK;
    if (score >= PROS_SECURITY_MONITOR_QUARANTINE_SCORE)
        return PROS_SECURITY_MONITOR_QUARANTINE;
    return PROS_SECURITY_MONITOR_ALLOW;
}

int pros_security_monitor_record(
    const struct pros_security_event *event,
    struct pros_security_monitor_state *state) {
    struct pros_security_monitor_state *current;
    uint32_t weight;
    uint32_t contribution;

    if (!event || event->pid == 0u ||
        event->type <= PROS_SECURITY_EVENT_NONE ||
        event->type > PROS_SECURITY_EVENT_POLICY_VIOLATION)
        return -1;

    weight = event_weight(event->type);
    if (weight == 0u) return -1;

    current = find_state(event->pid);
    if (!current) current = alloc_state(event->pid);
    if (!current) return -2;

    /*
     * Severity is a bounded multiplier supplied by a trusted kernel
     * telemetry producer. Zero is normalized to one so an event can never
     * disappear merely because a producer omitted severity.
     */
    contribution = weight * (event->severity == 0u ? 1u : event->severity);
    if (contribution > PROS_SECURITY_MONITOR_MAX_SCORE)
        contribution = PROS_SECURITY_MONITOR_MAX_SCORE;

    if (current->score >
        PROS_SECURITY_MONITOR_MAX_SCORE - contribution)
        current->score = PROS_SECURITY_MONITOR_MAX_SCORE;
    else
        current->score += contribution;

    ++current->event_count;
    current->last_timestamp = event->timestamp;
    current->action = decide(event->type, current->score);

    (void)pros_health_record_security(
        0x53444D4Eu, (uint32_t)current->action,
        ((uint64_t)event->type << 32) | current->score);

    if (state) *state = *current;
    return (int)current->action;
}

int pros_security_monitor_get(
    uint64_t pid,
    struct pros_security_monitor_state *state) {
    struct pros_security_monitor_state *current = find_state(pid);
    if (!current || !state) return -1;
    *state = *current;
    return 0;
}

int pros_security_monitor_clear(uint64_t pid) {
    struct pros_security_monitor_state *current = find_state(pid);
    if (!current) return -1;
    current->active = 0u;
    current->action = PROS_SECURITY_MONITOR_ALLOW;
    current->score = 0u;
    current->event_count = 0u;
    return 0;
}

int pros_security_monitor_selftest(void) {
    struct pros_security_event event = {
        .pid = 0x9001u,
        .timestamp = 1u,
        .type = PROS_SECURITY_EVENT_MASS_FILE_MODIFY,
        .severity = 1u
    };
    struct pros_security_monitor_state state;

    (void)pros_security_monitor_clear(event.pid);

    if (pros_security_monitor_record(&event, &state) !=
        PROS_SECURITY_MONITOR_ALLOW)
        return -1;

    event.type = PROS_SECURITY_EVENT_PERSISTENCE_CHANGE;
    event.timestamp = 2u;
    if (pros_security_monitor_record(&event, &state) !=
        PROS_SECURITY_MONITOR_QUARANTINE)
        return -2;

    event.type = PROS_SECURITY_EVENT_PROCESS_INJECTION;
    event.timestamp = 3u;
    if (pros_security_monitor_record(&event, &state) !=
        PROS_SECURITY_MONITOR_BLOCK)
        return -3;

    if (state.score < PROS_SECURITY_MONITOR_BLOCK_SCORE ||
        state.event_count != 3u)
        return -4;

    if (pros_security_monitor_clear(event.pid) != 0 ||
        pros_security_monitor_get(event.pid, &state) == 0)
        return -5;

    return 0;
}
