#include "security_quarantine.h"

#include <stdint.h>

static struct pros_quarantine_entry entries[PROS_SECURITY_QUARANTINE_MAX];
static uint64_t next_id = 1;
static uint32_t active_count;

static void copy_digest(uint8_t dst[PROS_FILE_SHA256_BYTES],
                         const uint8_t src[PROS_FILE_SHA256_BYTES]) {
    for (uint32_t i = 0; i < PROS_FILE_SHA256_BYTES; ++i)
        dst[i] = src[i];
}

static struct pros_quarantine_entry *find(uint64_t id) {
    if (id == 0) return 0;
    for (uint32_t i = 0; i < PROS_SECURITY_QUARANTINE_MAX; ++i)
        if (entries[i].active && entries[i].id == id)
            return &entries[i];
    return 0;
}

int pros_security_quarantine_add(
    uint64_t owner_pid,
    const struct pros_file_admission_record *record,
    uint64_t timestamp,
    uint64_t *entry_id) {
    if (!record || record->security.action != PROS_FILE_QUARANTINE ||
        owner_pid == 0)
        return -1;

    for (uint32_t i = 0; i < PROS_SECURITY_QUARANTINE_MAX; ++i) {
        if (entries[i].active) continue;

        uint64_t id = next_id++;
        if (id == 0) id = next_id++;

        entries[i].id = id;
        entries[i].timestamp = timestamp;
        entries[i].owner_pid = owner_pid;
        copy_digest(entries[i].sha256, record->sha256);
        entries[i].state = record->security.state;
        entries[i].action = record->security.action;
        entries[i].evidence = record->security.evidence;
        entries[i].active = 1;
        ++active_count;

        if (entry_id) *entry_id = id;
        return 0;
    }
    return -2;
}

int pros_security_quarantine_get(
    uint64_t entry_id,
    struct pros_quarantine_entry *entry) {
    struct pros_quarantine_entry *source;

    if (!entry) return -1;
    source = find(entry_id);
    if (!source) return -1;
    *entry = *source;
    return 0;
}

int pros_security_quarantine_release(uint64_t entry_id) {
    struct pros_quarantine_entry *entry = find(entry_id);
    if (!entry) return -1;

    entry->active = 0;
    --active_count;
    return 0;
}

uint32_t pros_security_quarantine_count(void) {
    return active_count;
}

int pros_security_quarantine_selftest(void) {
    static const uint8_t digest[PROS_FILE_SHA256_BYTES] = { 0 };
    struct pros_file_admission_record record = {
        .sha256 = { 0 },
        .security = {
            .state = PROS_FILE_SUSPICIOUS,
            .action = PROS_FILE_QUARANTINE,
            .evidence = PROS_FILE_EVIDENCE_REPUTATION_SUSPICIOUS
        }
    };
    struct pros_quarantine_entry copy;
    uint64_t id = 0;

    (void)digest;
    if (pros_security_quarantine_add(1, &record, 1, &id) != 0 || id == 0)
        return -1;
    if (pros_security_quarantine_get(id, &copy) != 0 ||
        copy.owner_pid != 1 ||
        copy.state != PROS_FILE_SUSPICIOUS ||
        copy.action != PROS_FILE_QUARANTINE)
        return -1;
    if (pros_security_quarantine_release(id) != 0 ||
        pros_security_quarantine_count() != 0)
        return -1;
    return 0;
}
