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
        process->object.refcount == 0 || process->pid == 0)
        return -1;
    if (process->security_level != PROS_SECURITY_LIMITED &&
        process->security_level != PROS_SECURITY_SUPER)
        return -2;
    if ((process->capability_policy & ~PROSENJIT_CAP_RIGHTS_ALL) != 0 ||
        (process->syscall_policy & ~PROSENJIT_SYSCALL_POLICY_ALL) != 0)
        return -3;
    if (process->quarantined > 1u)
        return -4;
    if (process->security_level == PROS_SECURITY_LIMITED &&
        (process->capability_policy & PROSENJIT_CAP_ADMIN) != 0)
        return -5;

    if (handle_table_validate(&process->handles) != 0)
        return -6;
    for (uint32_t i = 1u; i < PROSENJIT_HANDLE_MAX; ++i) {
        const struct pros_handle_entry *entry = &process->handles.entries[i];
        if (entry->object &&
            (entry->rights & ~process->capability_policy) != 0)
            return -6;
    }
    return 0;
}

void pros_security_hardening_note_violation(uint32_t code, uint64_t value)
{
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

    if (!pros_security_hardening_ready())
        return -1;

    process.object.type = PROS_OBJECT_PROCESS;
    process.object.refcount = 1;
    process.pid = 1;
    process.security_level = PROS_SECURITY_LIMITED;
    process.capability_policy =
        PROSENJIT_CAP_RIGHTS_ALL & ~PROSENJIT_CAP_ADMIN;
    process.syscall_policy = PROSENJIT_SYSCALL_POLICY_ALL;
    handle_table_init(&process.handles);

    if (pros_security_hardening_check_process(&process) != 0)
        return -2;

    process.capability_policy |= PROSENJIT_CAP_ADMIN;
    if (pros_security_hardening_check_process(&process) == 0)
        return -3;

    /* Keep the fixture a LIMITED process while validating a legal handle. */
    process.capability_policy =
        PROSENJIT_CAP_RIGHTS_ALL & ~PROSENJIT_CAP_ADMIN;
    process.handles.entries[1].generation = 1u;
    process.handles.entries[1].rights = PROSENJIT_CAP_READ;
    process.handles.entries[1].object = &process.object;
    if (pros_security_hardening_check_process(&process) != 0)
        return -4;

    process.handles.entries[1].rights = 1ULL << 63;
    if (pros_security_hardening_check_process(&process) != -6)
        return -5;

    if (handle_table_selftest() != 0)
        return -6;

    return 0;
}
