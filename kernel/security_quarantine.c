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

        /*
         * Quarantine IDs are security-relevant references. Never reuse an
         * ID that is still active, including across uint64 wraparound.
         * The table is bounded, so a finite probe is sufficient and
         * exhaustion fails closed.
         */
        uint64_t id = 0;
        for (uint32_t probe = 0u;
             probe < PROS_SECURITY_QUARANTINE_MAX + 1u; ++probe) {
            id = next_id++;
            if (id == 0u)
                continue;
            if (!find(id))
                break;
            id = 0u;
        }
        if (id == 0u)
            return -2;

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

int pros_security_quarantine_release_owner(uint64_t owner_pid) {
    if (owner_pid == 0u)
        return -1;

    uint32_t released = 0u;
    for (uint32_t i = 0u; i < PROS_SECURITY_QUARANTINE_MAX; ++i) {
        if (!entries[i].active || entries[i].owner_pid != owner_pid)
            continue;
        entries[i].active = 0u;
        if (active_count != 0u)
            --active_count;
        ++released;
    }

    return released == 0u ? -1 : (int)released;
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

    /*
     * The quarantine table is bounded. Fill every slot, verify a new
     * admission fails closed, then release one slot and verify recovery.
     */
    uint64_t ids[PROS_SECURITY_QUARANTINE_MAX];
    for (uint32_t i = 0u; i < PROS_SECURITY_QUARANTINE_MAX; ++i) {
        if (pros_security_quarantine_add(
                0x1000u + i, &record, 2u + i, &ids[i]) != 0 ||
            ids[i] == 0u)
            return -2;
    }
    if (pros_security_quarantine_count() !=
        PROS_SECURITY_QUARANTINE_MAX)
        return -3;
    if (pros_security_quarantine_add(0x2000u, &record, 99u, &id) != -2)
        return -4;
    if (pros_security_quarantine_release(ids[0]) != 0)
        return -5;
    if (pros_security_quarantine_add(0x2000u, &record, 100u, &id) != 0 ||
        id == 0u)
        return -6;

    for (uint32_t i = 0u; i < PROS_SECURITY_QUARANTINE_MAX; ++i)
        (void)pros_security_quarantine_release(ids[i]);
    (void)pros_security_quarantine_release(id);

    if (pros_security_quarantine_count() != 0u)
        return -7;

    /*
     * Owner cleanup must release every active record belonging to the same
     * process while leaving other owners untouched.
     */
    uint64_t owner_ids[2];
    uint64_t other_id = 0u;
    if (pros_security_quarantine_add(0x3000u, &record, 200u, &owner_ids[0]) != 0 ||
        pros_security_quarantine_add(0x3000u, &record, 201u, &owner_ids[1]) != 0 ||
        pros_security_quarantine_add(0x3001u, &record, 202u, &other_id) != 0)
        return -8;
    if (pros_security_quarantine_release_owner(0x3000u) != 2 ||
        pros_security_quarantine_count() != 1u)
        return -9;
    if (pros_security_quarantine_get(owner_ids[0], &copy) == 0 ||
        pros_security_quarantine_get(owner_ids[1], &copy) == 0 ||
        pros_security_quarantine_get(other_id, &copy) != 0)
        return -10;
    if (pros_security_quarantine_release_owner(0x3000u) != -1 ||
        pros_security_quarantine_release_owner(0u) != -1)
        return -11;
    if (pros_security_quarantine_release(other_id) != 0 ||
        pros_security_quarantine_count() != 0u)
        return -12;

    return 0;
}
