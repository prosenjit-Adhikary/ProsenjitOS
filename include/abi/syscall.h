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
    PROSENJIT_SYS_HANDLE_CLOSE = 3,
    PROSENJIT_SYS_HANDLE_DUP = 4,
    PROSENJIT_SYS_IPC_SEND = 5,
    PROSENJIT_SYS_IPC_RECEIVE = 6,
    PROSENJIT_SYS_HANDLE_TRANSFER = 7,
};

#define PROSENJIT_SYSCALL_OK        0ULL
#define PROSENJIT_SYSCALL_ENOSYS    0xFFFFFFFFFFFFFFDAULL
#define PROSENJIT_SYSCALL_EPERM     0xFFFFFFFFFFFFFFF3ULL
#define PROSENJIT_SYSCALL_EBADF     0xFFFFFFFFFFFFFFF6ULL

/*
 * Internal kernel-only dispatcher result. It is deliberately outside the
 * public errno-style ABI range and is consumed by the assembly syscall exit
 * path after INT 0x80 has already switched CPL3 -> CPL0.
 */
#define PROSENJIT_SYSCALL_INTERNAL_EXIT 0xFFFFFFFFFFFFFF00ULL
#define PROSENJIT_SYSCALL_INTERNAL_BLOCK 0xFFFFFFFFFFFFFF01ULL

/* Per-process sandbox policy bits. A zero bit denies the corresponding syscall. */
#define PROSENJIT_SYSCALL_POLICY(n) (1ULL << ((n) - 1U))
#define PROSENJIT_SYSCALL_POLICY_ALL \
    (PROSENJIT_SYSCALL_POLICY(PROSENJIT_SYS_GET_VERSION) | \
     PROSENJIT_SYSCALL_POLICY(PROSENJIT_SYS_USER_TEST_COMPLETE) | \
     PROSENJIT_SYSCALL_POLICY(PROSENJIT_SYS_HANDLE_CLOSE) | \
     PROSENJIT_SYSCALL_POLICY(PROSENJIT_SYS_HANDLE_DUP) | \
     PROSENJIT_SYSCALL_POLICY(PROSENJIT_SYS_IPC_SEND) | \
     PROSENJIT_SYSCALL_POLICY(PROSENJIT_SYS_IPC_RECEIVE) | \
     PROSENJIT_SYSCALL_POLICY(PROSENJIT_SYS_HANDLE_TRANSFER))

#endif
