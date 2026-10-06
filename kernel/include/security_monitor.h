#ifndef PROSENJIT_SECURITY_MONITOR_H
#define PROSENJIT_SECURITY_MONITOR_H

#include <stdint.h>

#define PROS_SECURITY_MONITOR_MAX_PROCESSES 32u
#define PROS_SECURITY_MONITOR_MAX_SCORE 1000u
#define PROS_SECURITY_MONITOR_QUARANTINE_SCORE 100u
#define PROS_SECURITY_MONITOR_BLOCK_SCORE 250u

enum pros_security_event_type {
    PROS_SECURITY_EVENT_NONE = 0,
    PROS_SECURITY_EVENT_MASS_FILE_MODIFY = 1,
    PROS_SECURITY_EVENT_PERSISTENCE_CHANGE = 2,
    PROS_SECURITY_EVENT_PRIVILEGE_ESCALATION = 3,
    PROS_SECURITY_EVENT_PROCESS_INJECTION = 4,
    PROS_SECURITY_EVENT_SECURITY_TAMPER = 5,
    PROS_SECURITY_EVENT_ABNORMAL_NETWORK = 6,
    PROS_SECURITY_EVENT_CREDENTIAL_ACCESS = 7,
    PROS_SECURITY_EVENT_POLICY_VIOLATION = 8
};

enum pros_security_monitor_action {
    PROS_SECURITY_MONITOR_ALLOW = 0,
    PROS_SECURITY_MONITOR_QUARANTINE = 1,
    PROS_SECURITY_MONITOR_BLOCK = 2
};

struct pros_security_event {
    uint64_t pid;
    uint64_t timestamp;
    enum pros_security_event_type type;
    uint32_t severity;
};

struct pros_security_monitor_state {
    uint64_t pid;
    uint64_t last_timestamp;
    uint32_t score;
    uint32_t event_count;
    uint32_t active;
    enum pros_security_monitor_action action;
};

int pros_security_monitor_record(
    const struct pros_security_event *event,
    struct pros_security_monitor_state *state);

int pros_security_monitor_get(
    uint64_t pid,
    struct pros_security_monitor_state *state);

int pros_security_monitor_clear(uint64_t pid);

int pros_security_monitor_selftest(void);

#endif
