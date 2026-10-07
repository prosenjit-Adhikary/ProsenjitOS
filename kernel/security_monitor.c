#include "security_monitor.h"
#include "health.h"
#include "intent.h"

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
    uint32_t severity;

    if (!event || event->pid == 0u ||
        event->type <= PROS_SECURITY_EVENT_NONE ||
        event->type > PROS_SECURITY_EVENT_POLICY_VIOLATION)
        return -1;

    weight = event_weight(event->type);
    if (weight == 0u) return -1;

    current = find_state(event->pid);
    if (!current) current = alloc_state(event->pid);
    if (!current) return -2;
    if (current->event_count != 0u &&
        event->timestamp < current->last_timestamp)
        return -3;

    /*
     * Severity is a bounded multiplier supplied by a trusted kernel
     * telemetry producer. Zero is normalized to one so an event can never
     * disappear merely because a producer omitted severity.
     */
    severity = event->severity == 0u ? 1u : event->severity;
    if (severity > PROS_SECURITY_MONITOR_MAX_SCORE / weight)
        contribution = PROS_SECURITY_MONITOR_MAX_SCORE;
    else
        contribution = weight * severity;

    if (current->score >
        PROS_SECURITY_MONITOR_MAX_SCORE - contribution)
        current->score = PROS_SECURITY_MONITOR_MAX_SCORE;
    else
        current->score += contribution;

    /*
     * event_count is telemetry state, not a security score. Saturate it
     * instead of allowing uint32 wrap to make a long-lived process appear
     * to have a small event history again.
     */
    if (current->event_count != UINT32_MAX)
        ++current->event_count;
    current->last_timestamp = event->timestamp;
    current->action = decide(event->type, current->score);

    (void)pros_health_record_security(
        0x53444D4Eu, (uint32_t)current->action,
        ((uint64_t)event->type << 32) | current->score);

    if (state) *state = *current;
    return (int)current->action;
}

int pros_security_monitor_enforce(
    struct pros_process *process,
    const struct pros_security_event *event,
    struct pros_security_monitor_state *state) {
    int action;

    if (!process || !event || process_state(process) == PROS_PROCESS_DEAD ||
        process->pid != event->pid)
        return -1;

    action = pros_security_monitor_record(event, state);
    if (action < 0)
        return action;

    if (action == PROS_SECURITY_MONITOR_QUARANTINE)
        return security_quarantine(process) == 0 ? action : -2;

    if (action == PROS_SECURITY_MONITOR_BLOCK) {
        if (process_exit(process, -0x5344) != 0)
            return -3;
        return action;
    }

    return action;
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
    event.severity = 2u;
    if (pros_security_monitor_record(&event, &state) !=
        PROS_SECURITY_MONITOR_BLOCK)
        return -3;

    if (state.score < PROS_SECURITY_MONITOR_BLOCK_SCORE ||
        state.event_count != 3u)
        return -4;

    if (pros_security_monitor_clear(event.pid) != 0 ||
        pros_security_monitor_get(event.pid, &state) == 0)
        return -5;

    event.pid = 0x9002u;
    event.timestamp = 10u;
    event.type = PROS_SECURITY_EVENT_MASS_FILE_MODIFY;
    event.severity = 1u;
    if (pros_security_monitor_record(&event, &state) !=
        PROS_SECURITY_MONITOR_ALLOW)
        return -6;
    event.timestamp = 9u;
    if (pros_security_monitor_record(&event, &state) != -3)
        return -7;
    event.timestamp = 11u;
    event.severity = UINT32_MAX;
    if (pros_security_monitor_record(&event, &state) !=
        PROS_SECURITY_MONITOR_BLOCK ||
        state.score != PROS_SECURITY_MONITOR_MAX_SCORE)
        return -8;
    (void)pros_security_monitor_clear(event.pid);

    /*
     * The monitor table is bounded. Verify exhaustion fails closed and that
     * releasing one lifetime immediately restores capacity.
     */
    for (uint32_t i = 0u; i < PROS_SECURITY_MONITOR_MAX_PROCESSES; ++i) {
        event.pid = 0xA000u + i;
        event.timestamp = 1u;
        event.type = PROS_SECURITY_EVENT_MASS_FILE_MODIFY;
        event.severity = 1u;
        if (pros_security_monitor_record(&event, &state) !=
            PROS_SECURITY_MONITOR_ALLOW)
            return -9;
    }
    event.pid = 0xB000u;
    if (pros_security_monitor_record(&event, &state) != -2)
        return -10;
    if (pros_security_monitor_clear(0xA000u) != 0)
        return -11;
    if (pros_security_monitor_record(&event, &state) !=
        PROS_SECURITY_MONITOR_ALLOW)
        return -12;

    for (uint32_t i = 0u; i < PROS_SECURITY_MONITOR_MAX_PROCESSES; ++i)
        (void)pros_security_monitor_clear(0xA000u + i);
    (void)pros_security_monitor_clear(0xB000u);

    return 0;
}
