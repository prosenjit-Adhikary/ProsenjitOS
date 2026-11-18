#ifndef PROSENJIT_THREAD_H
#define PROSENJIT_THREAD_H

#include <stdint.h>
#include "process.h"

enum pros_thread_owner_kind {
    PROS_THREAD_OWNER_PROCESS = 0,
    PROS_THREAD_OWNER_KERNEL = 1,
};

enum pros_thread_state {
    PROS_THREAD_NEW = 0,
    PROS_THREAD_READY = 1,
    PROS_THREAD_RUNNING = 2,
    PROS_THREAD_BLOCKED = 3,
    PROS_THREAD_DEAD = 4,
};

enum pros_interrupt_frame_kind {
    PROS_INTERRUPT_FRAME_NONE = 0,
    PROS_INTERRUPT_FRAME_KERNEL = 1,
    PROS_INTERRUPT_FRAME_USER = 2,
};

struct pros_cpu_context {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rsp, rip;
    uint64_t rflags;
};

/* Shared assembly prefix: 15 GPRs followed by the CPU IRET tail.
 * CPL0 tail: RIP, CS, RFLAGS (18 qwords total).
 * CPL3 tail: RIP, CS, RFLAGS, user RSP, user SS (20 qwords total).
 * Timer IRQ synthetic vector/error words are before this pointer, not in it.
 */
struct pros_interrupt_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags;
    uint64_t user_rsp, user_ss; /* valid only for PROS_INTERRUPT_FRAME_USER */
};

_Static_assert(__builtin_offsetof(struct pros_interrupt_frame, rip) == 120,
               "interrupt GPR prefix ABI changed");
_Static_assert(__builtin_offsetof(struct pros_interrupt_frame, user_rsp) == 144,
               "CPL3 user RSP offset changed");
_Static_assert(sizeof(struct pros_interrupt_frame) == 160,
               "maximum CPL3 interrupt frame must be 20 qwords");

struct pros_thread {
    uint64_t tid;
    struct pros_process *process;
    uint32_t owner_kind;
    uint32_t state;
    uint32_t priority;
    uint64_t entry;
    struct pros_cpu_context context;
    struct pros_interrupt_frame *interrupt_frame;
    /* Stable per-thread backing for a saved CPL3 IRET frame. */
    struct pros_interrupt_frame user_interrupt_frame_storage;
    uint32_t interrupt_frame_kind;
    void *wait_channel;
    uint64_t blocked_syscall;
    struct pros_thread *next;
    struct pros_thread *prev;
    struct pros_thread *registry_next;
    /* Owned supervisor-only kernel stack; released by process wait/reap. */
    void *kernel_stack_base;
    uint64_t kernel_stack_size;
    uint64_t kernel_stack_pages[4];
    uint32_t kernel_stack_slot;
};

#define PROS_THREAD_KERNEL_STACK_SIZE 16384u

int thread_manager_init(void);
int thread_init(struct pros_thread *thread, struct pros_process *process,
                uint64_t tid, uint64_t entry);
int thread_init_user(struct pros_thread *thread, struct pros_process *process,
                     uint64_t tid, uint64_t entry, uint64_t user_rsp);
int thread_abort_unpublished(struct pros_thread *thread);
/* Read-only aggregate preflight for unpublished or queued spawn rollback. */
int thread_spawn_rollback_preflight(struct pros_thread *thread, int queued);
/* Reject address-space owned pages that alias any registered kernel stack. */
int thread_address_space_disjoint_preflight(
    const struct pros_address_space *space);
/* Create a supervisor-only thread with no process/address-space owner. */
int thread_init_kernel(struct pros_thread *thread, uint64_t tid, uint64_t entry);
/* Validate kernel-thread ownership, queue admission, and stack cleanup. */
int thread_kernel_selftest(void);
/* Snapshot stack allocator ownership for corruption-injection regressions. */
int thread_stack_allocator_snapshot(uint64_t *allocated_slots,
                                    uint64_t *quarantined_slots);
int thread_set_state(struct pros_thread *thread, uint32_t state);
int thread_process_is_current(const struct pros_process *process);
int thread_process_has_live(const struct pros_process *process);
int thread_process_reapable(const struct pros_process *process);
/* Synthetic RSP/RSP0 rejection regression for process reap preflight. */
int thread_process_reap_context_selftest(const struct pros_process *process);
int thread_registry_integrity_selftest(void);
/* Exercise process_wait rejection with corrupted run-queue endpoints. */
int thread_runq_corruption_wait_selftest(struct pros_process *parent,
                                         struct pros_process *child);
int thread_terminate_process(struct pros_process *process);
int thread_reap_process(struct pros_process *process);
int thread_reap_kernel(struct pros_thread *thread);
uint32_t thread_state(const struct pros_thread *thread);
int thread_enqueue(struct pros_thread *thread);
int thread_remove_queued(struct pros_thread *thread);
struct pros_thread *thread_dequeue(void);
struct pros_thread *thread_current(void);
int thread_schedule(void);
int thread_schedule_preempt(void);
int thread_block(void *wait_channel);
int thread_wake(struct pros_thread *thread);
int thread_has_runnable(void);

/* Read-only scheduler population counters for observability. */
uint64_t thread_runnable_count(void);
uint64_t thread_blocked_count(void);
uint64_t thread_dead_count(void);

/* Save the current IRQ frame and select the next runnable thread frame. */
struct pros_interrupt_frame *thread_schedule_interrupt(
    struct pros_interrupt_frame *current_frame,
    enum pros_interrupt_frame_kind frame_kind);

/* Switches CPU execution from prev to next using their saved kernel stacks. */
void thread_context_switch(struct pros_thread *prev,
                           struct pros_thread *next);

#endif
