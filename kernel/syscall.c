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

        case PROSENJIT_SYS_USER_TEST_COMPLETE: {
            struct pros_process *process = process_current();
            if ((cpu_frame[1] & 3ULL) != 3ULL) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            if (!process ||
                address_space_validate_user_range(&process->address_space,
                                                  PROSENJIT_USER_TEST_VA, 1, 0) != 0) {
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }

            if (((volatile uint8_t *)(uintptr_t)PROSENJIT_USER_TEST_VA)[0] != 0x5A) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            /*
             * INT 0x80 entered from CPL3. Do not rewrite its IRET
             * frame to a CPL0 CS: IRET is a return mechanism, not a
             * privilege-escalation mechanism. The assembly syscall exit
             * path converts this internal sentinel into a direct jump
             * to the kernel trampoline.
             */
            registers[0] = PROSENJIT_SYSCALL_INTERNAL_EXIT;
            return;
        }

        default:
            registers[0] = PROSENJIT_SYSCALL_ENOSYS;
            return;
    }
}
