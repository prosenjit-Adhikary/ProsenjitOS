#include "syscall.h"
#include "address_space.h"
#include "paging.h"
#include "prosenjit.h"
#include "process.h"
#include "thread.h"
#include "intent.h"
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
            uint64_t source_handle = registers[5];
            uint64_t requested_rights = registers[4];
            uint16_t object_type = PROS_OBJECT_NONE;
            struct pros_intent intent;
            uint64_t reason = PROS_REASON_NONE;
            uint64_t tid = 0;
            struct pros_thread *current = thread_current();

            if (current) tid = current->tid;
            if (!process ||
                process_handle_type(process, source_handle, &object_type) != 0 ||
                security_make_intent(&intent, PROS_INTENT_DUPLICATE,
                                     object_type, source_handle,
                                     requested_rights) != 0 ||
                security_authorize(process, &intent, tid, &reason) != PROS_DECISION_ALLOW) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            uint64_t new_handle = process_duplicate_handle(process,
                                                            source_handle,
                                                            requested_rights);
            if (new_handle == PROSENJIT_HANDLE_INVALID) {
                serial_write("ProsenjitOS: syscall DUP denied by handle operation\\n");
                registers[0] = PROSENJIT_SYSCALL_EPERM;
            } else {
                registers[0] = new_handle;
            }
            return;
        }

        case PROSENJIT_SYS_HANDLE_CLOSE: {
            struct pros_process *process = process_current();
            uint64_t handle = registers[5];
            if (!process) {
                serial_write("ProsenjitOS: syscall CLOSE failed: no current process\\n");
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }
            if (process_close_handle(process, handle) != 0) {
                serial_write("ProsenjitOS: syscall CLOSE failed: invalid/stale handle\\n");
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
