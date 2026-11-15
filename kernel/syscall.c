#include "syscall.h"
#include "address_space.h"
#include "paging.h"
#include "prosenjit.h"
#include "process.h"
#include "thread.h"
#include "intent.h"
#include "ipc.h"
#include "usercopy.h"
#include "../include/abi/syscall.h"

#define COM1_PORT 0x3F8u

static inline void syscall_io_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t syscall_io_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int syscall_serial_ready(void) {
    return (syscall_io_in8(COM1_PORT + 5u) & 0x20u) != 0;
}

static void syscall_serial_putc(char c) {
    while (!syscall_serial_ready()) __asm__ volatile ("pause");
    syscall_io_out8(COM1_PORT, (uint8_t)c);
}

static void syscall_serial_write(const char *text) {
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n') syscall_serial_putc('\r');
        syscall_serial_putc(text[i]);
    }
}

static void syscall_serial_hex64(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    syscall_serial_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        syscall_serial_putc(hex[(value >> shift) & 0xFu]);
}

static int syscall_rights_valid(uint64_t rights) {
    return rights != 0 &&
           (rights & ~PROSENJIT_CAP_RIGHTS_ALL) == 0;
}

extern void user_mode_exit_trampoline(void);
extern uint64_t cpu_get_kernel_stack(void);

static int syscall_entry_stack_verified;

static int syscall_validate_entry_stack(const volatile uint64_t *cpu_frame) {
    /*
     * syscall_selftest() and other in-kernel callers exercise the dispatcher
     * with synthetic frames. Only a frame whose saved CS has CPL3 came from
     * the privilege transition and is subject to the RSP0/IRET-tail checks.
     */
    if ((cpu_frame[1] & 3u) != 3u)
        return 0;

    struct pros_thread *thread = thread_current();
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    if (!thread || !thread->kernel_stack_base ||
        thread->kernel_stack_size < PROS_THREAD_KERNEL_STACK_SIZE)
        return -1;

    uint64_t base = (uint64_t)(uintptr_t)thread->kernel_stack_base;
    if (base > UINT64_MAX - thread->kernel_stack_size)
        return -1;
    uint64_t top = base + thread->kernel_stack_size;
    if (rsp < base || rsp >= top || cpu_get_kernel_stack() != top)
        return -1;

    /* CPL3 -> CPL0 entry must provide the complete hardware IRET tail. */
    if ((cpu_frame[4] & 3u) != 3u ||
        cpu_frame[3] == 0 || cpu_frame[3] >= 0x0000800000000000ULL)
        return -1;
    return 0;
}

void syscall_dispatch(volatile uint64_t *registers, volatile uint64_t *cpu_frame) {
    if (!registers || !cpu_frame) return;

    if (syscall_validate_entry_stack(cpu_frame) != 0) {
        syscall_serial_write("ProsenjitOS: ERROR CPL3 syscall did not enter on the owning RSP0 stack/frame\n");
        registers[0] = PROSENJIT_SYSCALL_EPERM;
        return;
    }
    if (!syscall_entry_stack_verified) {
        syscall_entry_stack_verified = 1;
        syscall_serial_write("ProsenjitOS: CPL3 syscall per-thread RSP0 and five-word frame validation passed\n");
    }

    struct pros_process *policy_process = process_current();
    uint64_t syscall_number = registers[0];
    if (policy_process && syscall_number >= 1 && syscall_number <= 64 &&
        (process_allowed_syscalls(policy_process) & (1ULL << (syscall_number - 1U))) == 0) {
        registers[0] = PROSENJIT_SYSCALL_EPERM;
        return;
    }

    switch (registers[0]) {
        case PROSENJIT_SYS_GET_VERSION:
            registers[0] = PROSENJIT_ABI_VERSION;
            return;

        case PROSENJIT_SYS_HANDLE_DUP: {
            struct pros_process *process = process_current();
            uint64_t source_handle = registers[5];
            uint64_t requested_rights = registers[4];
            uint16_t object_type = PROS_OBJECT_NONE;

            if (!syscall_rights_valid(requested_rights)) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }
            struct pros_intent intent;
            uint64_t reason = PROS_REASON_NONE;
            uint64_t tid = 0;
            struct pros_thread *current = thread_current();

            if (current) tid = current->tid;
            /*
             * A zero source handle is an explicitly tested invalid-capability
             * case. Reject it before object-type/intent construction so the
             * diagnostic identifies the actual failure instead of reporting
             * the cascading type_rc/intent_rc sentinel values as a policy
             * authorization failure. The syscall still fails closed with EPERM.
             */
            if (source_handle == PROSENJIT_HANDLE_INVALID) {
                syscall_serial_write("ProsenjitOS: DUP rejected: invalid source handle=0 rights=");
                syscall_serial_hex64(requested_rights);
                syscall_serial_write(" tid=");
                syscall_serial_hex64(tid);
                syscall_serial_write("\n");
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }
            int type_rc = process ? process_handle_type(process, source_handle, &object_type) : -1;
            int intent_rc = -1;
            enum pros_security_decision decision = PROS_DECISION_DENY;
            if (type_rc == 0)
                intent_rc = security_make_intent(&intent, PROS_INTENT_DUPLICATE,
                                                 object_type, source_handle,
                                                 requested_rights);
            if (intent_rc == 0)
                decision = security_authorize(process, &intent, tid, &reason);

            if (!process || type_rc != 0 || intent_rc != 0 ||
                decision != PROS_DECISION_ALLOW) {
                syscall_serial_write("ProsenjitOS: DUP denied: process=");
                syscall_serial_hex64((uint64_t)(uintptr_t)process);
                syscall_serial_write(" handle=");
                syscall_serial_hex64(source_handle);
                syscall_serial_write(" rights=");
                syscall_serial_hex64(requested_rights);
                syscall_serial_write(" type=");
                syscall_serial_hex64(object_type);
                syscall_serial_write(" type_rc=");
                syscall_serial_hex64((uint64_t)(int64_t)type_rc);
                syscall_serial_write(" intent_rc=");
                syscall_serial_hex64((uint64_t)(int64_t)intent_rc);
                syscall_serial_write(" decision=");
                syscall_serial_hex64((uint64_t)decision);
                syscall_serial_write(" reason=");
                syscall_serial_hex64(reason);
                syscall_serial_write(" tid=");
                syscall_serial_hex64(tid);
                syscall_serial_write("\n");
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            uint64_t new_handle = process_duplicate_handle(process,
                                                            source_handle,
                                                            requested_rights);
            if (new_handle == PROSENJIT_HANDLE_INVALID) {
                syscall_serial_write("ProsenjitOS: DUP operation failed, source=");
                syscall_serial_hex64(source_handle);
                syscall_serial_write(" rights=");
                syscall_serial_hex64(requested_rights);
                syscall_serial_write("\n");
                registers[0] = PROSENJIT_SYSCALL_EPERM;
            } else {
                registers[0] = new_handle;
            }
            return;
        }

        case PROSENJIT_SYS_HANDLE_TRANSFER: {
            struct pros_process *source = process_current();
            uint64_t source_handle = registers[5];
            uint64_t target_pid = registers[4];
            uint64_t requested_rights = registers[3];
            uint64_t mode = registers[2];
            struct pros_process *target = process_lookup(target_pid);
            struct pros_thread *current = thread_current();
            uint64_t tid = current ? current->tid : 0;
            uint16_t object_type = PROS_OBJECT_NONE;
            struct pros_intent intent;
            uint64_t reason = PROS_REASON_NONE;

            if (!source || !target || source == target ||
                mode > 1 || !syscall_rights_valid(requested_rights) ||
                process_handle_type(source, source_handle, &object_type) != 0 ||
                security_make_intent(&intent, PROS_INTENT_TRANSFER,
                                     object_type, source_handle,
                                     requested_rights) != 0 ||
                security_authorize(source, &intent, tid, &reason) != PROS_DECISION_ALLOW) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            uint64_t new_handle = mode == 0
                ? process_transfer_handle_copy(source, target, source_handle, requested_rights)
                : process_transfer_handle_move(source, target, source_handle, requested_rights);
            registers[0] = (new_handle == PROSENJIT_HANDLE_INVALID)
                ? PROSENJIT_SYSCALL_EPERM : new_handle;
            return;
        }

        case PROSENJIT_SYS_HANDLE_CLOSE: {
            struct pros_process *process = process_current();
            uint64_t handle = registers[5];
            if (!process) {
                syscall_serial_write("ProsenjitOS: CLOSE failed: no current process, handle=");
                syscall_serial_hex64(handle);
                syscall_serial_write("\n");
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }
            if (process_close_handle(process, handle) != 0) {
                syscall_serial_write("ProsenjitOS: CLOSE failed: invalid/stale handle=");
                syscall_serial_hex64(handle);
                syscall_serial_write("\n");
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }
            registers[0] = PROSENJIT_SYSCALL_OK;
            return;
        }

        case PROSENJIT_SYS_IPC_SEND: {
            struct pros_process *process = process_current();
            uint64_t handle = registers[5];
            uint64_t buffer = registers[4];
            uint64_t length = registers[3];
            struct pros_thread *current = thread_current();
            uint64_t tid = current ? current->tid : 0;
            uint16_t object_type = PROS_OBJECT_NONE;
            struct pros_intent intent;
            uint64_t reason = PROS_REASON_NONE;

            int validation_stage = 0;
            if (!process) validation_stage = 1;
            else if (length > PROSENJIT_IPC_MESSAGE_MAX) validation_stage = 2;
            else if (address_space_validate_user_range(&process->address_space,
                                                        buffer, length ? length : 1, 0) != 0)
                validation_stage = 3;
            else if (process_handle_type(process, handle, &object_type) != 0)
                validation_stage = 4;
            else if (object_type != PROS_OBJECT_IPC) validation_stage = 5;
            else if (security_make_intent(&intent, PROS_INTENT_WRITE,
                                          PROS_OBJECT_IPC, handle,
                                          PROSENJIT_CAP_WRITE) != 0)
                validation_stage = 6;
            else if (security_authorize(process, &intent, tid, &reason) != PROS_DECISION_ALLOW)
                validation_stage = 7;
            if (validation_stage != 0) {
                syscall_serial_write("ProsenjitOS: IPC SEND rejected stage=");
                syscall_serial_hex64(validation_stage);
                syscall_serial_write(" handle=");
                syscall_serial_hex64(handle);
                syscall_serial_write(" buffer=");
                syscall_serial_hex64(buffer);
                syscall_serial_write(" length=");
                syscall_serial_hex64(length);
                syscall_serial_write(" type=");
                syscall_serial_hex64(object_type);
                syscall_serial_write(" reason=");
                syscall_serial_hex64(reason);
                syscall_serial_write("\n");
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            uint32_t slot = (uint32_t)(handle & 0xFFULL);
            uint16_t generation = (uint16_t)(handle >> 8);
            struct pros_handle_entry *entry = &process->handles.entries[slot];
            if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX ||
                generation == 0 || !entry->object ||
                entry->generation != generation) {
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }

            uint8_t message[PROSENJIT_IPC_MESSAGE_MAX];
            if (length != 0 && copy_from_user(&process->address_space, message,
                                              buffer, length) != 0) {
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }
            int rc = ipc_channel_send((struct pros_ipc_channel *)entry->object,
                                      message, (uint32_t)length);
            registers[0] = (rc == PROSENJIT_IPC_OK) ? PROSENJIT_SYSCALL_OK :
                           (rc == PROSENJIT_IPC_EFULL ? PROSENJIT_SYSCALL_EPERM :
                            PROSENJIT_SYSCALL_EBADF);
            return;
        }

        case PROSENJIT_SYS_IPC_RECEIVE: {
            struct pros_process *process = process_current();
            uint64_t handle = registers[5];
            uint64_t buffer = registers[4];
            uint64_t capacity = registers[3];
            uint64_t length_out = registers[2];
            struct pros_thread *current = thread_current();
            uint64_t tid = current ? current->tid : 0;
            uint16_t object_type = PROS_OBJECT_NONE;
            struct pros_intent intent;
            uint64_t reason = PROS_REASON_NONE;

            if (!process || capacity > PROSENJIT_IPC_MESSAGE_MAX ||
                capacity == 0 ||
                address_space_validate_user_range(&process->address_space,
                                                  buffer, capacity, 1) != 0 ||
                address_space_validate_user_range(&process->address_space,
                                                  length_out, sizeof(uint32_t), 1) != 0 ||
                process_handle_type(process, handle, &object_type) != 0 ||
                object_type != PROS_OBJECT_IPC ||
                security_make_intent(&intent, PROS_INTENT_READ,
                                     PROS_OBJECT_IPC, handle,
                                     PROSENJIT_CAP_READ) != 0 ||
                security_authorize(process, &intent, tid, &reason) != PROS_DECISION_ALLOW) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }

            uint32_t slot = (uint32_t)(handle & 0xFFULL);
            uint16_t generation = (uint16_t)(handle >> 8);
            struct pros_handle_entry *entry = &process->handles.entries[slot];
            if (slot == 0 || slot >= PROSENJIT_HANDLE_MAX ||
                generation == 0 || !entry->object ||
                entry->generation != generation) {
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }

            uint8_t message[PROSENJIT_IPC_MESSAGE_MAX];
            uint32_t received = 0;
ipc_receive_retry: ;
            int rc = ipc_channel_receive((struct pros_ipc_channel *)entry->object,
                                          message, (uint32_t)capacity, &received);
            if (rc == PROSENJIT_IPC_EEMPTY) {
                /*
                 * Do not block if there is no runnable successor. Until the
                 * kernel has a dedicated idle thread, returning an error is
                 * safer than suspending the only runnable execution context.
                 */
                if (!current || !thread_has_runnable()) {
                    registers[0] = PROSENJIT_SYSCALL_EPERM;
                    return;
                }
                /*
                 * Publish the resume identity before registering the waiter.
                 * ipc_channel_wait() releases the channel lock before it
                 * returns; a sender on another CPU may wake us immediately.
                 * The wake path must therefore never observe a blocked
                 * receiver with blocked_syscall still unset.
                 */
                /*
                 * A thread may retain the frame from an earlier interrupt.
                 * Invalidate it before publishing this wait so an early sender
                 * cannot rewind RIP in a stale frame. The scheduler publishes
                 * the fresh syscall frame before retrying any pending wakeup.
                 */
                current->interrupt_frame = 0;
                current->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;
                current->blocked_syscall = PROSENJIT_SYS_IPC_RECEIVE;
                int wait_rc = ipc_channel_wait((struct pros_ipc_channel *)entry->object,
                                               current);
                if (wait_rc == PROSENJIT_IPC_EALREADY) {
                    current->blocked_syscall = 0;
                    /*
                     * A sender published a message after the empty receive
                     * but before waiter registration. Retry receive instead
                     * of returning an error and leaving the queued message
                     * stranded for this syscall.
                     */
                    goto ipc_receive_retry;
                }
                if (wait_rc != PROSENJIT_IPC_OK) {
                    current->blocked_syscall = 0;
                    registers[0] = PROSENJIT_SYSCALL_EPERM;
                    return;
                }
                registers[0] = PROSENJIT_SYSCALL_INTERNAL_BLOCK;
                return;
            }
            if (rc != PROSENJIT_IPC_OK) {
                registers[0] = (rc == PROSENJIT_IPC_EMSGSIZE)
                               ? PROSENJIT_SYSCALL_EPERM
                               : PROSENJIT_SYSCALL_EBADF;
                return;
            }
            if (received != 0 && copy_to_user(&process->address_space,
                                               buffer, message, received) != 0) {
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }
            if (copy_to_user(&process->address_space, length_out,
                             &received, sizeof(received)) != 0) {
                registers[0] = PROSENJIT_SYSCALL_EBADF;
                return;
            }
            registers[0] = received;
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

            /* This syscall only validates the user test mapping. Process
             * termination is a separate, explicit SYS_EXIT operation. */
            registers[0] = PROSENJIT_SYSCALL_OK;
            return;
        }

        case PROSENJIT_SYS_EXIT: {
            struct pros_process *process = process_current();
            struct pros_thread *thread = thread_current();
            if ((cpu_frame[1] & 3ULL) != 3ULL || !process || !thread ||
                thread->process != process) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }
            if (process_exit(process, (int32_t)registers[5]) != 0) {
                registers[0] = PROSENJIT_SYSCALL_EPERM;
                return;
            }
            /* isr128 consumes this sentinel and jumps to the kernel exit path. */
            registers[0] = PROSENJIT_SYSCALL_INTERNAL_EXIT;
            return;
        }

        default:
            registers[0] = PROSENJIT_SYSCALL_ENOSYS;
            return;
    }
}


int syscall_selftest(struct pros_process *process) {
    volatile uint64_t regs[6] = { 0, 0, 0, 0, 0, 0 };
    volatile uint64_t frame[7] = { 0, 0, 0, 0, 0, 0, 0 };
    uint64_t saved_policy = process ? process->syscall_policy : 0;

    syscall_dispatch(regs, frame);
    if (regs[0] != PROSENJIT_SYSCALL_ENOSYS)
        return -1;

    regs[0] = PROSENJIT_SYS_GET_VERSION;
    syscall_dispatch(regs, frame);
    if (regs[0] != PROSENJIT_ABI_VERSION)
        return -2;

    regs[0] = PROSENJIT_SYS_EXIT;
    regs[5] = 0;
    syscall_dispatch(regs, frame);
    if (regs[0] != PROSENJIT_SYSCALL_EPERM)
        return -20;

    if (process) {
        process->syscall_policy = 0;
        regs[0] = PROSENJIT_SYS_GET_VERSION;
        syscall_dispatch(regs, frame);
        if (regs[0] != PROSENJIT_SYSCALL_EPERM) {
            process->syscall_policy = saved_policy;
            return -3;
        }

        process->syscall_policy = saved_policy;
        regs[0] = 0xFFFFFFFFULL;
        syscall_dispatch(regs, frame);
        if (regs[0] != PROSENJIT_SYSCALL_ENOSYS)
            return -4;
    }

    syscall_dispatch(0, frame);
    syscall_dispatch(regs, 0);
    return 0;
}
