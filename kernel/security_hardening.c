#include <stdint.h>
#include "security_hardening.h"
#include "paging.h"
#include "health.h"
#include "../include/abi/syscall.h"

extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __kernel_data_start[];
extern char __kernel_data_end[];
extern char __kernel_bss_start[];
extern char __kernel_bss_end[];

#define PROS_SECURITY_HARDENING_MAGIC 0x5345434852444E47ULL

static uint64_t hardening_magic;
static uint64_t hardening_generation;
static uint64_t hardening_violations;
static uint8_t hardening_ready;

static int check_range(uint64_t start, uint64_t end,
                       uint64_t required, uint64_t forbidden)
{
    if (start > end || (start & 0xFFFULL) != 0 ||
        (end & 0xFFFULL) != 0)
        return -1;
    if (start == end)
        return 0;

    for (uint64_t va = start; va < end; va += 0x1000ULL) {
        uint64_t flags = 0;
        if (paging_query_page(va, &flags) != 0 ||
            (flags & required) != required ||
            (flags & forbidden) != 0)
            return -1;
    }
    return 0;
}

static int valid_object_type(uint16_t type)
{
    switch (type) {
    case PROS_OBJECT_PROCESS:
    case PROS_OBJECT_THREAD:
    case PROS_OBJECT_ADDRESS_SPACE:
    case PROS_OBJECT_FILE:
    case PROS_OBJECT_DEVICE:
    case PROS_OBJECT_SOCKET:
    case PROS_OBJECT_IPC:
        return 1;
    default:
        return 0;
    }
}

int pros_security_hardening_init(void)
{
    uint64_t text_start = (uint64_t)(uintptr_t)__kernel_text_start;
    uint64_t text_end = (uint64_t)(uintptr_t)__kernel_text_end;
    uint64_t rodata_start = (uint64_t)(uintptr_t)__kernel_rodata_start;
    uint64_t rodata_end = (uint64_t)(uintptr_t)__kernel_rodata_end;
    uint64_t data_start = (uint64_t)(uintptr_t)__kernel_data_start;
    uint64_t data_end = (uint64_t)(uintptr_t)__kernel_data_end;
    uint64_t bss_start = (uint64_t)(uintptr_t)__kernel_bss_start;
    uint64_t bss_end = (uint64_t)(uintptr_t)__kernel_bss_end;

    hardening_magic = PROS_SECURITY_HARDENING_MAGIC;
    if (hardening_generation != UINT64_MAX)
        ++hardening_generation;
    hardening_violations = 0;
    hardening_ready = 0;

    if (check_range(text_start, text_end, PROSENJIT_PAGE_PRESENT,
                    PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_USER |
                    PROSENJIT_PAGE_NX) != 0)
        return -1;
    if (check_range(rodata_start, rodata_end,
                    PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_NX,
                    PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_USER) != 0)
        return -2;
    if (check_range(data_start, data_end,
                    PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_WRITABLE |
                    PROSENJIT_PAGE_NX,
                    PROSENJIT_PAGE_USER) != 0)
        return -3;
    if (check_range(bss_start, bss_end,
                    PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_WRITABLE |
                    PROSENJIT_PAGE_NX,
                    PROSENJIT_PAGE_USER) != 0)
        return -4;

    hardening_ready = 1;
    (void)pros_health_record_security(0x53454348u, 1u,
                                      hardening_generation);
    return 0;
}

int pros_security_hardening_check_process(const struct pros_process *process)
{
    if (!process || process->object.type != PROS_OBJECT_PROCESS ||
        process->object.reserved != 0u ||
        process->object.refcount == 0u || process->pid == 0u)
        return -1;
    if (process->security_level != PROS_SECURITY_LIMITED &&
        process->security_level != PROS_SECURITY_SUPER)
        return -2;
    if ((process->capability_policy & ~PROSENJIT_CAP_RIGHTS_ALL) != 0 ||
        (process->syscall_policy & ~PROSENJIT_SYSCALL_POLICY_ALL) != 0)
        return -3;
    if (process->state < PROS_PROCESS_READY ||
        process->state > PROS_PROCESS_BLOCKED)
        return -4;
    if (process->quarantined > 1u)
        return -5;
    for (uint32_t i = 0u; i < sizeof(process->security_reserved); ++i) {
        if (process->security_reserved[i] != 0u)
            return -6;
    }
    if (process->quarantined) {
        if (process->state != PROS_PROCESS_BLOCKED ||
            (process->quarantine_previous_state != 0u &&
             process->quarantine_previous_state !=
                 (uint8_t)(PROS_PROCESS_READY + 1u) &&
             process->quarantine_previous_state !=
                 (uint8_t)(PROS_PROCESS_RUNNING + 1u)))
            return -7;
    } else if (process->quarantine_previous_state != 0u) {
        return -7;
    }
    if (process->security_level == PROS_SECURITY_LIMITED &&
        (process->capability_policy & PROSENJIT_CAP_ADMIN) != 0)
        return -8;

    /*
     * The process capability table is part of the trusted security state.
     * A corrupted handle entry must never become usable merely because the
     * process-level policy is otherwise valid. Validate every live entry:
     * rights stay within the ABI, generation 0 is never valid, and the
     * referenced object must still be live and typed.
     */
    for (uint32_t i = 1u; i < PROSENJIT_HANDLE_MAX; ++i) {
        const struct pros_handle_entry *entry = &process->handles.entries[i];
        if (!entry->object)
            continue;
        if (entry->generation == 0u ||
            entry->generation == UINT16_MAX || entry->reserved != 0u ||
            entry->object->refcount == 0u ||
            !valid_object_type(entry->object->type) ||
            entry->object->reserved != 0u || entry->rights == 0u ||
            (entry->rights & ~PROSENJIT_CAP_RIGHTS_ALL) != 0u ||
            (entry->rights & ~process->capability_policy) != 0u)
            return -6;
    }
    return 0;
}

void pros_security_hardening_note_violation(uint32_t code, uint64_t value)
{
    if (hardening_violations != UINT64_MAX)
        ++hardening_violations;
    (void)pros_health_record_security(0x53454348u, code, value);
}

uint64_t pros_security_hardening_violations(void)
{
    return hardening_violations;
}

int pros_security_hardening_ready(void)
{
    return hardening_ready &&
           hardening_magic == PROS_SECURITY_HARDENING_MAGIC;
}

int pros_security_hardening_selftest(void)
{
    struct pros_process process = {0};
    struct pros_object referenced_object = {
        .type = PROS_OBJECT_FILE,
        .reserved = 0u,
        .refcount = 1u
    };

    if (!pros_security_hardening_ready())
        return -1;

    process.object.type = PROS_OBJECT_PROCESS;
    process.object.refcount = 1;
    process.pid = 1;
    process.security_level = PROS_SECURITY_LIMITED;
    process.state = PROS_PROCESS_READY;
    process.capability_policy =
        PROSENJIT_CAP_RIGHTS_ALL & ~PROSENJIT_CAP_ADMIN;
    process.syscall_policy = PROSENJIT_SYSCALL_POLICY_ALL;

    if (pros_security_hardening_check_process(&process) != 0)
        return -2;

    process.object.reserved = 1u;
    if (pros_security_hardening_check_process(&process) != -1)
        return -17;
    process.object.reserved = 0u;

    process.capability_policy |= PROSENJIT_CAP_ADMIN;
    if (pros_security_hardening_check_process(&process) == 0)
        return -3;

    process.capability_policy =
        PROSENJIT_CAP_RIGHTS_ALL;
    process.handles.entries[1].generation = 1u;
    process.handles.entries[1].rights = PROSENJIT_CAP_READ;
    process.handles.entries[1].object = &referenced_object;
    if (pros_security_hardening_check_process(&process) != 0 ||
        handle_check(&process.handles, (1ULL << 8) | 1u,
                     PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0)
        return -4;

    process.handles.entries[1].rights = 1ULL << 63;
    if (pros_security_hardening_check_process(&process) != -6 ||
        handle_check(&process.handles, (1ULL << 8) | 1u,
                     PROSENJIT_CAP_READ, PROS_OBJECT_FILE) == 0)
        return -5;

    process.handles.entries[1].rights = 0u;
    if (pros_security_hardening_check_process(&process) != -6 ||
        handle_check(&process.handles, (1ULL << 8) | 1u,
                     PROSENJIT_CAP_READ, PROS_OBJECT_FILE) == 0)
        return -12;

    process.handles.entries[1].rights = PROSENJIT_CAP_READ;
    process.capability_policy = PROSENJIT_CAP_READ;
    process.handles.entries[1].rights |= PROSENJIT_CAP_WRITE;
    if (pros_security_hardening_check_process(&process) != -6)
        return -6;

    process.capability_policy = PROSENJIT_CAP_RIGHTS_ALL;
    process.handles.entries[1].rights = PROSENJIT_CAP_READ;
    process.handles.entries[1].generation = 0u;
    if (pros_security_hardening_check_process(&process) != -6)
        return -7;

    process.handles.entries[1].generation = UINT16_MAX;
    if (pros_security_hardening_check_process(&process) != -6 ||
        handle_check(&process.handles, (UINT64_C(0xFFFF) << 8) | 1u,
                     PROSENJIT_CAP_READ, PROS_OBJECT_FILE) == 0)
        return -18;

    process.handles.entries[1].generation = 1u;
    process.handles.entries[1].reserved = 1u;
    if (pros_security_hardening_check_process(&process) != -6)
        return -8;

    process.handles.entries[1].reserved = 0u;
    referenced_object.reserved = 1u;
    if (pros_security_hardening_check_process(&process) != -6)
        return -9;

    referenced_object.reserved = 0u;
    referenced_object.type = 0xFFFFu;
    if (pros_security_hardening_check_process(&process) != -6)
        return -10;

    referenced_object.type = PROS_OBJECT_FILE;
    referenced_object.refcount = 0u;
    if (pros_security_hardening_check_process(&process) != -6)
        return -13;
    referenced_object.refcount = 1u;

    process.security_reserved[0] = 1u;
    if (pros_security_hardening_check_process(&process) != -6)
        return -14;
    process.security_reserved[0] = 0u;

    process.state = PROS_PROCESS_NEW;
    if (pros_security_hardening_check_process(&process) != -4)
        return -15;
    process.state = PROS_PROCESS_READY;
    process.quarantine_previous_state = 1u;
    if (pros_security_hardening_check_process(&process) != -7)
        return -16;
    process.quarantine_previous_state = 0u;

    process.handles.entries[1].object = 0;
    process.handles.entries[1].rights = 0u;
    process.handles.entries[1].generation = 1u;
    process.handles.entries[1].reserved = 0u;
    if (pros_security_hardening_check_process(&process) != 0)
        return -11;

    return 0;
}
