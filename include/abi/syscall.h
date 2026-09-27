#ifndef PROSENJIT_ABI_SYSCALL_H
#define PROSENJIT_ABI_SYSCALL_H

#include <stdint.h>

/*
 * ProsenjitOS user/kernel ABI.
 *
 * This header is intentionally shared by kernel code and future user-space
 * programs. Keep syscall numbers and ABI-visible values stable once published.
 */
#define PROSENJIT_ABI_VERSION 0x00010000ULL

enum pros_syscall_number {
    PROSENJIT_SYS_GET_VERSION = 1,
    PROSENJIT_SYS_USER_TEST_COMPLETE = 2,
};

#define PROSENJIT_SYSCALL_OK        0ULL
#define PROSENJIT_SYSCALL_ENOSYS    0xFFFFFFFFFFFFFFDAULL
#define PROSENJIT_SYSCALL_EPERM     0xFFFFFFFFFFFFFFF3ULL

#endif
