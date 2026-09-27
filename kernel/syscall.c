#include "syscall.h"
#include "address_space.h"
#include "paging.h"
#include "prosenjit.h"
#include "process.h"
#include "../include/abi/syscall.h"

extern void user_mode_exit_trampoline(void);

void syscall_dispatch(volatile uint64_t *registers, volatile uint64_t *cpu_frame) {
    if (!registers || !cpu_frame) return;

    switch (registers[0]) {
        case PROSENJIT_SYS_GET_VERSION:
            registers[0] = PROSENJIT_ABI_VERSION;
            return;

        case PROSENJIT_SYS_HANDLE_DUP: {
            struct pros_process *process = process_current();
            uint64_t new_handle = process
                ? process_duplicate_handle(process, registers[5], registers[4])
                : PROSENJIT_HANDLE_INVALID;
            registers[0] = new_handle ? new_handle : PROSENJIT_SYSCALL_EPERM;
            return;
        }

        case PROSENJIT_SYS_HANDLE_CLOSE: {
            struct pros_process *process = process_current();
            if (!process || process_close_handle(process, registers[5]) != 0) {
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }
            registers[0] = PROSENJIT_SYSCALL_OK;
            return;
        }

        case PROSENJIT_SYS_USER_TEST_COMPLETE:
            if ((cpu_frame[1] & 3ULL) != 3ULL) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            if (((volatile uint8_t *)(uintptr_t)PROSENJIT_USER_TEST_VA)[0] != 0x5A) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            if (paging_switch_kernel() != 0) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            /*
             * IRETQ in 64-bit mode consumes RIP, CS, RFLAGS, RSP and SS
             * from the interrupt frame. Switch the return frame to a
             * kernel CPL0 context before returning from the user syscall.
             */
            cpu_frame[0] = (uint64_t)(uintptr_t)user_mode_exit_trampoline;
            cpu_frame[1] = 0x08;
            cpu_frame[3] = (uint64_t)(uintptr_t)(cpu_frame + 5);
            cpu_frame[4] = 0x10;
            registers[0] = PROSENJIT_SYSCALL_OK;
            return;

        default:
            registers[0] = PROSENJIT_SYSCALL_ENOSYS;
            return;
    }
}
