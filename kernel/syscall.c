#include "syscall.h"
#include "address_space.h"
#include "paging.h"
#include "prosenjit.h"
#include "process.h"
#include "../include/abi/syscall.h"

extern void user_mode_exit_trampoline(void);

void syscall_dispatch(uint64_t *registers, uint64_t *cpu_frame) {
    if (!registers || !cpu_frame) return;

    switch (registers[0]) {
        case PROSENJIT_SYS_GET_VERSION:
            registers[0] = PROSENJIT_ABI_VERSION;
            return;

        case PROSENJIT_SYS_HANDLE_CLOSE: {
            struct pros_process *process = process_current();
            if (!process || process_close_handle(process, registers[9]) != 0) {
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }
            registers[0] = PROSENJIT_SYSCALL_OK;
            return;
        }

        case PROSENJIT_SYS_USER_TEST_COMPLETE:
            /*
             * The test syscall is deliberately restricted to a CPL3 caller.
             * A future production syscall layer will perform per-process
             * identity and capability checks before dispatching operations.
             */
            if ((cpu_frame[1] & 3ULL) != 3ULL) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            if (((volatile uint8_t *)(uintptr_t)PROSENJIT_USER_TEST_VA)[0] != 0x5A) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            /*
             * Leave user mode through a controlled kernel trampoline. The
             * current user stack is intentionally discarded by the trampoline.
             */
            if (paging_switch_kernel() != 0) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            cpu_frame[0] = (uint64_t)(uintptr_t)user_mode_exit_trampoline;
            cpu_frame[1] = 0x08; /* kernel code selector */
            registers[0] = PROSENJIT_SYSCALL_OK;
            return;

        default:
            registers[0] = PROSENJIT_SYSCALL_ENOSYS;
            return;
    }
}
