#include "prosenjit.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"
#include "address_space.h"
#include "object.h"
#include "process.h"
#include "thread.h"
#include "timer.h"
#include "intent.h"
#include "ipc.h"
#include "../include/abi/syscall.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define VGA_ADDRESS 0xB8000u
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define IDT_ENTRIES 256u
#define USER_DATA_VA PROSENJIT_USER_TEST_VA

static volatile uint16_t *const VGA = (uint16_t *)VGA_ADDRESS;

static uint8_t init_thread_stack[4096] __attribute__((aligned(16)));
static uint8_t worker_thread_stack[4096] __attribute__((aligned(16)));
static struct pros_interrupt_frame scheduler_init_frame;
static struct pros_interrupt_frame scheduler_worker_frame;
static struct pros_ipc_channel user_ipc_channel;

static volatile uint64_t scheduler_test_init_progress;
static volatile uint64_t scheduler_test_worker_progress;

static void scheduler_test_init_entry(void) {
    for (;;) {
        ++scheduler_test_init_progress;
        __asm__ volatile ("pause");
    }
}

static void scheduler_test_worker_entry(void) {
    for (;;) {
        ++scheduler_test_worker_progress;
        __asm__ volatile ("pause");
    }
}
static inline void io_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t io_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
#define COM1_PORT 0x3F8u

static uint16_t vga_entry(char c, uint8_t color) {
    return ((uint16_t)color << 8) | (uint8_t)c;
}
static void vga_clear(void) {
    for (uint32_t i = 0; i < VGA_COLUMNS * VGA_ROWS; ++i) VGA[i] = vga_entry(' ', 0x07);
}
static void vga_write(const char *text, uint8_t color) {
    uint32_t i = 0;
    while (text[i] != '\0' && i < VGA_COLUMNS * VGA_ROWS) {
        VGA[i] = vga_entry(text[i], color);
        ++i;
    }
}
static void serial_init(void) {
    io_out8(COM1_PORT + 1u, 0x00); io_out8(COM1_PORT + 3u, 0x80);
    io_out8(COM1_PORT + 0u, 0x03); io_out8(COM1_PORT + 1u, 0x00);
    io_out8(COM1_PORT + 3u, 0x03); io_out8(COM1_PORT + 2u, 0xC7);
    io_out8(COM1_PORT + 4u, 0x03);
}
static int serial_ready(void) { return (io_in8(COM1_PORT + 5u) & 0x20u) != 0; }
static void serial_putc(char c) {
    while (!serial_ready()) __asm__ volatile ("pause");
    io_out8(COM1_PORT, (uint8_t)c);
}
static void serial_write(const char *text) {
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n') serial_putc('\r');
        serial_putc(text[i]);
    }
}
static void serial_hex64(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    serial_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4) serial_putc(hex[(value >> shift) & 0xFu]);
}

struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

static struct idt_gate idt[IDT_ENTRIES];
extern void idt_load(const struct idt_pointer *ptr);
extern void isr128(void);
extern void isr32(void);
extern void enter_user_mode(uint64_t entry, uint64_t stack_top, uint64_t initial_handle);
extern void cpu_load_tss(void);
extern void thread_start_trampoline(void);

struct gdtr_snapshot {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void debug_tss_descriptor(void) {
    struct gdtr_snapshot gdtr;
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    serial_write("ProsenjitOS: GDTR base = ");
    serial_hex64(gdtr.base);
    serial_write("\n");
    serial_write("ProsenjitOS: GDTR limit = ");
    serial_hex64(gdtr.limit);
    serial_write("\n");
    volatile uint64_t *gdt = (volatile uint64_t *)(uintptr_t)gdtr.base;
    serial_write("ProsenjitOS: TSS descriptor low = ");
    serial_hex64(gdt[5]);
    serial_write("\n");
    serial_write("ProsenjitOS: TSS descriptor high = ");
    serial_hex64(gdt[6]);
    serial_write("\n");
}
extern void user_mode_exit_trampoline(void);
extern volatile uint64_t usercopy_fault_active;
extern volatile uint64_t usercopy_fault_rip;
extern char __user_text_start[];
extern char __user_text_end[];
#define ISR(n) extern void isr##n(void);
ISR(0) ISR(1) ISR(2) ISR(3) ISR(4) ISR(5) ISR(6) ISR(7)
ISR(8) ISR(9) ISR(10) ISR(11) ISR(12) ISR(13) ISR(14) ISR(15)
ISR(16) ISR(17) ISR(18) ISR(19) ISR(20) ISR(21) ISR(22) ISR(23)
ISR(24) ISR(25) ISR(26) ISR(27) ISR(28) ISR(29) ISR(30) ISR(31)

static void idt_set_gate(uint32_t n, void (*handler)(void), uint8_t ist) {
    uint64_t addr = (uint64_t)handler;
    idt[n].offset_low = (uint16_t)addr;
    idt[n].selector = 0x08;
    idt[n].ist = ist & 0x07u;
    idt[n].type_attr = 0x8E;
    idt[n].offset_mid = (uint16_t)(addr >> 16);
    idt[n].offset_high = (uint32_t)(addr >> 32);
    idt[n].zero = 0;
}
void idt_init(void) {
    static void (*const handlers[IDT_ENTRIES])(void) = {
        isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,isr12,isr13,isr14,isr15,
        isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31
    };
    for (uint32_t i = 0; i < IDT_ENTRIES; ++i) idt[i].type_attr = 0;
    for (uint32_t i = 0; i < 32; ++i) idt_set_gate(i, handlers[i], 0);
    idt_set_gate(8, handlers[8], 2); /* dedicated double-fault stack */
    /*
     * Keep #GP/#PF on the normal CPL0 stack while debugging the user entry
     * path. If delivery itself fails, #DF will still have its dedicated IST.
     */
    idt_set_gate(13, handlers[13], 0);
    idt_set_gate(14, handlers[14], 0);
    idt_set_gate(32, isr32, 0);
    /* Use a dedicated IST stack for CPL3 -> CPL0 INT 0x80 entry.\n     * The regular RSP0 bootstrap stack is also used by early kernel code;\n     * keeping syscall entry on IST1 makes the privilege transition robust\n     * even while the active user address space has supervisor-only kernel\n     * mappings. */\n    idt_set_gate(128, isr128, 1);
    idt[128].type_attr = 0xEE;
    struct idt_pointer ptr = { (uint16_t)(sizeof(idt) - 1u), (uint64_t)idt };
    idt_load(&ptr);
}
void exception_dispatch(uint64_t *frame) {
    uint64_t vector = frame[0];
    serial_write("ProsenjitOS: CPU exception vector ");
    serial_hex64(vector);
    serial_write("\n");
    serial_write("ProsenjitOS: exception error code = ");
    serial_hex64(frame[1]);
    serial_write("\n");
    serial_write("ProsenjitOS: exception RIP = ");
    serial_hex64(frame[2]);
    serial_write("\n");
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    if (vector == 14 && usercopy_fault_active) {
        usercopy_fault_active = 0;
        frame[2] = usercopy_fault_rip;
        return;
    }
    serial_write("ProsenjitOS: CR2 = ");
    serial_hex64(cr2);
    serial_write("\n");
    if (vector == 128) {
        if (((volatile uint8_t *)(uintptr_t)USER_DATA_VA)[0] != 0x5A) {
            serial_write("ProsenjitOS: ERROR user-mode write verification failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        serial_write("ProsenjitOS: CPL3 syscall returned to kernel successfully\n");
        if (paging_switch_kernel() != 0) {
            serial_write("ProsenjitOS: ERROR kernel address-space restore failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        frame[2] = (uint64_t)(uintptr_t)user_mode_exit_trampoline;
        frame[3] = 0x08;
        /* exception_dispatch's frame is: vector,error,RIP,CS,RFLAGS,RSP,SS */
        frame[5] = (uint64_t)(uintptr_t)(frame + 7);
        frame[6] = 0x10;
        return;
    }
    vga_write("ProsenjitOS: CPU exception - system halted", 0x4F);
    for (;;) __asm__ volatile ("cli; hlt");
}

void user_mode_test_complete(void) {
    serial_write("ProsenjitOS: CPL3 user-mode and IPC syscall test passed\n");
    vga_write("ProsenjitOS: CPL3 user mode online", 0x0F);
    for (;;) __asm__ volatile ("cli; hlt");
}

void kernel_main(uint32_t boot_magic, uint32_t boot_info) {
    serial_init();
    serial_write("ProsenjitOS: early kernel entry\n");
    serial_write("ProsenjitOS: Multiboot2 magic = "); serial_hex64(boot_magic); serial_write("\n");
    vga_clear();
    if (boot_magic != MULTIBOOT2_BOOT_MAGIC) {
        vga_write("ProsenjitOS: invalid boot handoff", 0x4F);
        serial_write("ProsenjitOS: ERROR invalid Multiboot2 handoff\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    idt_init();
    __asm__ volatile ("cli");
    serial_write("ProsenjitOS: IDT loaded; attempting LTR\n");
    debug_tss_descriptor();
    cpu_load_tss();
    serial_write("ProsenjitOS: TSS loaded successfully\n");
    if (timer_init(100) != 0) {
        serial_write("ProsenjitOS: ERROR PIT timer initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: PIT timer initialized at 100 Hz\n");
    serial_write("ProsenjitOS: IDT loaded; exceptions armed; hardware IRQs masked\n");
    serial_write("ProsenjitOS: Multiboot2 info = "); serial_hex64(boot_info); serial_write("\n");
    serial_write("ProsenjitOS: x86-64 long mode active\n");
    if (physical_memory_init((uint64_t)boot_info) != 0) {
        serial_write("ProsenjitOS: ERROR physical memory initialization failed\n");
        vga_write("ProsenjitOS: memory initialization failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    serial_write("ProsenjitOS: physical memory manager online\n");
    serial_write("ProsenjitOS: free pages = ");
    serial_hex64(physical_memory_free_pages());
    serial_write("\n");

    uint64_t test_page = physical_page_alloc();
    if (test_page == 0) {
        serial_write("ProsenjitOS: ERROR page allocation test failed\n");
        vga_write("ProsenjitOS: page allocation failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: allocated test page = ");
    serial_hex64(test_page);
    serial_write("\n");
    physical_page_free(test_page);
    serial_write("ProsenjitOS: page allocation/free test passed\n");

    if (paging_init() != 0) {
        serial_write("ProsenjitOS: ERROR virtual memory initialization failed\n");
        vga_write("ProsenjitOS: paging initialization failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    uint64_t translated = paging_translate(PROSENJIT_KERNEL_TEST_VA);
    serial_write("ProsenjitOS: paging active; test VA -> ");
    serial_hex64(translated);
    serial_write("\n");
    if (translated == 0) {
        serial_write("ProsenjitOS: ERROR paging translation test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    if (heap_init() != 0) {
        serial_write("ProsenjitOS: ERROR kernel heap initialization failed\n");
        vga_write("ProsenjitOS: heap initialization failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    void *heap_test = kmalloc(128);
    if (!heap_test) {
        serial_write("ProsenjitOS: ERROR kernel heap allocation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    ((volatile uint8_t *)heap_test)[0] = 0xA5;
    if (((volatile uint8_t *)heap_test)[0] != 0xA5) {
        serial_write("ProsenjitOS: ERROR kernel heap memory test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    kfree(heap_test);
    serial_write("ProsenjitOS: kernel heap allocation/free test passed\n");

    struct pros_process init_process;
    if (process_manager_init() != 0 ||
        process_init(&init_process, 1, PROS_SECURITY_LIMITED) != 0 ||
        process_register(&init_process) != 0 ||
        process_lookup(1) != &init_process) {
        serial_write("ProsenjitOS: ERROR process manager initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    struct pros_object user_test_object;
    uint64_t user_test_handle;
    object_init(&user_test_object, PROS_OBJECT_FILE);
    if (process_grant_handle(&init_process, &user_test_object,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_DUPLICATE | PROSENJIT_CAP_TRANSFER, &user_test_handle) != 0) {
        serial_write("ProsenjitOS: ERROR initial process handle grant failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&init_process);
    if (process_state(&init_process) != PROS_PROCESS_NEW ||
        process_set_state(&init_process, PROS_PROCESS_RUNNING) != 0 ||
        process_state(&init_process) != PROS_PROCESS_RUNNING) {
        serial_write("ProsenjitOS: ERROR initial process lifecycle test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    struct pros_process child_process;
    if (process_create(&init_process, &child_process, 2,
                       PROS_SECURITY_LIMITED) != 0 ||
        child_process.parent_pid != init_process.pid ||
        process_state(&child_process) != PROS_PROCESS_READY ||
        process_set_state(&child_process, PROS_PROCESS_RUNNING) != 0 ||
        process_state(&child_process) != PROS_PROCESS_RUNNING) {
        serial_write("ProsenjitOS: ERROR child process lifecycle test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_exit(&child_process, 42) != 0 ||
        process_state(&child_process) != PROS_PROCESS_ZOMBIE) {
        serial_write("ProsenjitOS: ERROR process exit test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    struct pros_process duplicate_pid_process;
    if (process_create(&init_process, &duplicate_pid_process, child_process.pid,
                       PROS_SECURITY_LIMITED) == 0) {
        serial_write("ProsenjitOS: ERROR duplicate live/zombie PID was accepted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    int32_t waited_exit = 0;
    if (process_wait(&init_process, child_process.pid, &waited_exit) != 0 ||
        waited_exit != 42 ||
        process_lookup(child_process.pid) != 0) {
        serial_write("ProsenjitOS: ERROR process wait/reap test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process lifecycle/parent/exit-wait test passed\n");
    /* Lifecycle regressions: zombie parent and current-process clearing. */
    struct pros_process lifecycle_parent;
    if (process_create(&init_process, &lifecycle_parent, 3,
                       PROS_SECURITY_LIMITED) != 0 ||
        process_unregister(&lifecycle_parent) != -1 ||
        process_exit(&lifecycle_parent, 7) != 0 ||
        process_wait(&lifecycle_parent, child_process.pid, &waited_exit) != -1) {
        serial_write("ProsenjitOS: ERROR process state validation regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&lifecycle_parent);
    if (process_current() != 0) {
        serial_write("ProsenjitOS: ERROR zombie process became current\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_wait(&init_process, lifecycle_parent.pid, &waited_exit) != 0 ||
        waited_exit != 7 ||
        lifecycle_parent.address_space.root != 0 ||
        process_lookup(lifecycle_parent.pid) != 0) {
        serial_write("ProsenjitOS: ERROR zombie process reap validation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_unregister(&lifecycle_parent) != -1) {
        serial_write("ProsenjitOS: ERROR unreaped process unregister was accepted\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&init_process);
    serial_write("ProsenjitOS: process state validation regression passed\n");

    struct pros_object lifecycle_object;
    uint64_t lifecycle_handle = PROSENJIT_HANDLE_INVALID;
    uint64_t lifecycle_free_before = physical_memory_free_pages();
    uint64_t lifecycle_user_page = physical_page_alloc();
    object_init(&lifecycle_object, PROS_OBJECT_FILE);
    if (!lifecycle_user_page ||
        process_create(&init_process, &lifecycle_parent, 6,
                       PROS_SECURITY_LIMITED) != 0 ||
        address_space_map_user_page(&lifecycle_parent.address_space,
                                    PROSENJIT_USER_TEST_VA,
                                    lifecycle_user_page,
                                    PROSENJIT_PAGE_WRITABLE) != 0 ||
        process_grant_handle(&lifecycle_parent, &lifecycle_object,
                             PROSENJIT_CAP_READ, &lifecycle_handle) != 0 ||
        lifecycle_object.refcount != 2 ||
        process_exit(&lifecycle_parent, 13) != 0 ||
        process_wait(&init_process, lifecycle_parent.pid, &waited_exit) != 0 ||
        waited_exit != 13 ||
        lifecycle_object.refcount != 1 ||
        lifecycle_parent.address_space.root != 0) {
        serial_write("ProsenjitOS: ERROR process resource cleanup regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    physical_page_free(lifecycle_user_page);
    if (physical_memory_free_pages() != lifecycle_free_before) {
        serial_write("ProsenjitOS: ERROR address-space page-table cleanup regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process resource cleanup regression passed\n");



    struct pros_thread init_thread;
    struct pros_thread worker_thread;
    if (thread_manager_init() != 0 ||
        thread_init(&init_thread, &init_process, 1,
                    (uint64_t)(uintptr_t)scheduler_test_init_entry,
                    (uint64_t)(uintptr_t)(init_thread_stack + sizeof(init_thread_stack))) != 0 ||
        thread_init(&worker_thread, &init_process, 2,
                    (uint64_t)(uintptr_t)scheduler_test_worker_entry,
                    (uint64_t)(uintptr_t)(worker_thread_stack + sizeof(worker_thread_stack))) != 0 ||
        thread_set_state(&init_thread, PROS_THREAD_READY) != 0 ||
        thread_set_state(&worker_thread, PROS_THREAD_READY) != 0 ||
        thread_enqueue(&init_thread) != 0 ||
        thread_enqueue(&worker_thread) != 0 ||
        thread_schedule() != 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING ||
        thread_schedule() != 0 ||
        thread_current() != &worker_thread ||
        thread_state(&worker_thread) != PROS_THREAD_RUNNING ||
        thread_schedule() != 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING) {
        serial_write("ProsenjitOS: ERROR thread scheduler foundation test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: thread lifecycle/run-queue scheduler test passed\n");

    /* IPC wait-queue regression: block a receiver, schedule another thread,
     * then wake the receiver when a message arrives. */
    struct pros_ipc_channel ipc_wait_test;
    scheduler_init_frame.rax = PROSENJIT_SYSCALL_INTERNAL_BLOCK;
    scheduler_init_frame.rip = 0x1002;
    init_thread.interrupt_frame = &scheduler_init_frame;
    init_thread.blocked_syscall = PROSENJIT_SYS_IPC_RECEIVE;
    if (ipc_channel_init(&ipc_wait_test) != PROSENJIT_IPC_OK ||
        thread_current() != &init_thread ||
        ipc_channel_wait(&ipc_wait_test, &init_thread) != PROSENJIT_IPC_OK ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED ||
        init_thread.wait_channel != &ipc_wait_test ||
        thread_schedule() != 0 ||
        thread_current() != &worker_thread ||
        thread_state(&init_thread) != PROS_THREAD_BLOCKED) {
        serial_write("ProsenjitOS: ERROR IPC wait-block regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    const uint8_t wait_message[] = { 'W', 'A', 'K', 'E' };
    if (ipc_channel_send(&ipc_wait_test, wait_message, sizeof(wait_message)) != PROSENJIT_IPC_OK ||
        thread_state(&init_thread) != PROS_THREAD_READY ||
        init_thread.wait_channel != 0 ||
        init_thread.blocked_syscall != 0 ||
        scheduler_init_frame.rax != PROSENJIT_SYS_IPC_RECEIVE ||
        scheduler_init_frame.rip != 0x1000 ||
        ipc_wait_test.waiter_count != 0 ||
        thread_schedule() != 0 ||
        thread_current() != &init_thread ||
        thread_state(&init_thread) != PROS_THREAD_RUNNING) {
        serial_write("ProsenjitOS: ERROR IPC wakeup regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: IPC wait-queue block/wakeup test passed\n");

    /* Regression guard for interrupt-driven scheduler frame save/restore. */
    scheduler_init_frame.rax = 0x1111111111111111ULL;
    scheduler_init_frame.rip = 0xAAAAAAAAAAAAAAAAULL;
    scheduler_init_frame.cs = 0x08;
    scheduler_init_frame.rflags = 0x202;
    struct pros_interrupt_frame *first_switch =
        thread_schedule_interrupt(&scheduler_init_frame);
    if (thread_current() != &worker_thread ||
        first_switch != worker_thread.interrupt_frame ||
        first_switch->rip != (uint64_t)(uintptr_t)thread_start_trampoline ||
        worker_thread.interrupt_frame == &scheduler_init_frame) {
        serial_write("ProsenjitOS: ERROR interrupt-frame first-switch regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    scheduler_worker_frame.rax = 0x2222222222222222ULL;
    scheduler_worker_frame.rip = 0xBBBBBBBBBBBBBBBBULL;
    scheduler_worker_frame.cs = 0x08;
    scheduler_worker_frame.rflags = 0x202;
    struct pros_interrupt_frame *second_switch =
        thread_schedule_interrupt(&scheduler_worker_frame);
    if (thread_current() != &init_thread ||
        second_switch != &scheduler_init_frame ||
        init_thread.interrupt_frame != &scheduler_init_frame ||
        scheduler_init_frame.rax != 0x1111111111111111ULL ||
        scheduler_init_frame.rip != 0xAAAAAAAAAAAAAAAAULL ||
        worker_thread.interrupt_frame != &scheduler_worker_frame) {
        serial_write("ProsenjitOS: ERROR interrupt-frame save/restore regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: interrupt-frame scheduler save/restore test passed\n");

    serial_write("ProsenjitOS: initial limited process and capability context online\n");
    if (security_manager_init() != 0) {
        serial_write("ProsenjitOS: ERROR security manager initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /*
     * Regression guard for CPL3 HANDLE_DUP authorization: DUPLICATE is the
     * operation authority, while requested_rights describes the attenuated
     * rights of the new handle. A READ-only duplication must therefore be
     * allowed from a READ|DUPLICATE source, while requesting WRITE must fail.
     */
    struct pros_intent duplicate_intent;
    uint64_t duplicate_reason;
    if (security_make_intent(&duplicate_intent, PROS_INTENT_DUPLICATE,
                             PROS_OBJECT_FILE, user_test_handle,
                             PROSENJIT_CAP_READ) != 0 ||
        security_authorize(&init_process, &duplicate_intent, 1,
                           &duplicate_reason) != PROS_DECISION_ALLOW) {
        serial_write("ProsenjitOS: ERROR duplicate READ authorization regression\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (security_make_intent(&duplicate_intent, PROS_INTENT_DUPLICATE,
                             PROS_OBJECT_FILE, user_test_handle,
                             PROSENJIT_CAP_WRITE) != 0 ||
        security_authorize(&init_process, &duplicate_intent, 1,
                           &duplicate_reason) != PROS_DECISION_DENY) {
        serial_write("ProsenjitOS: ERROR duplicate WRITE attenuation regression\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: duplicate authorization attenuation test passed\n");

    struct pros_intent security_intent;
    uint64_t security_reason;
    if (security_make_intent(&security_intent, PROS_INTENT_READ,
                             PROS_OBJECT_FILE, user_test_handle,
                             PROSENJIT_CAP_READ) != 0 ||
        security_authorize(&init_process, &security_intent, 1,
                           &security_reason) != PROS_DECISION_ALLOW) {
        serial_write("ProsenjitOS: ERROR intent/capability/policy allow test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (security_make_intent(&security_intent, PROS_INTENT_WRITE,
                             PROS_OBJECT_FILE, user_test_handle,
                             PROSENJIT_CAP_WRITE) != 0 ||
        security_authorize(&init_process, &security_intent, 1,
                           &security_reason) != PROS_DECISION_DENY) {
        serial_write("ProsenjitOS: ERROR capability-deny policy test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (security_make_intent(&security_intent, PROS_INTENT_ADMIN,
                             PROS_OBJECT_FILE, user_test_handle,
                             PROSENJIT_CAP_ADMIN) != 0 ||
        security_authorize(&init_process, &security_intent, 1,
                           &security_reason) != PROS_DECISION_QUARANTINE ||
        !security_is_quarantined(&init_process)) {
        serial_write("ProsenjitOS: ERROR quarantine decision test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (security_clear_quarantine(&init_process) != 0 ||
        security_is_quarantined(&init_process) ||
        security_ledger_count() < 3) {
        serial_write("ProsenjitOS: ERROR quarantine/ledger recovery test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    struct pros_reason_record latest_reason;
    if (security_ledger_latest(&latest_reason) != 0 ||
        latest_reason.decision != PROS_DECISION_QUARANTINE ||
        latest_reason.reason != PROS_REASON_QUARANTINE_REQUEST) {
        serial_write("ProsenjitOS: ERROR reason ledger verification failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: intent -> capability -> policy -> decision -> reason-ledger -> quarantine test passed\n");

    if ((process_allowed_capabilities(&init_process) & PROSENJIT_CAP_ADMIN) != 0 ||
        (process_allowed_capabilities(&init_process) & PROSENJIT_CAP_READ) == 0) {
        serial_write("ProsenjitOS: ERROR limited-process capability policy test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    struct pros_object admin_test_object;
    uint64_t admin_test_handle;
    object_init(&admin_test_object, PROS_OBJECT_DEVICE);
    if (process_grant_handle(&init_process, &admin_test_object,
                             PROSENJIT_CAP_ADMIN, &admin_test_handle) == 0) {
        serial_write("ProsenjitOS: ERROR limited process received ADMIN capability\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: limited-process capability policy test passed\n");

    /*
     * Capability foundation test:
     * - handles carry explicit rights
     * - type/rights checks are enforced by the kernel
     * - closing a handle invalidates its old generation
     */
    struct pros_handle_table capability_table;
    struct pros_object capability_object;
    handle_table_init(&capability_table);
    object_init(&capability_object, PROS_OBJECT_FILE);

    uint64_t read_handle = handle_alloc(&capability_table, &capability_object,
                                        PROSENJIT_CAP_READ);
    if (read_handle == PROSENJIT_HANDLE_INVALID ||
        handle_check(&capability_table, read_handle,
                     PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
        handle_check(&capability_table, read_handle,
                     PROSENJIT_CAP_WRITE, PROS_OBJECT_FILE) == 0 ||
        handle_check(&capability_table, read_handle,
                     PROSENJIT_CAP_READ, PROS_OBJECT_DEVICE) == 0) {
        serial_write("ProsenjitOS: ERROR capability enforcement test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    if (handle_close(&capability_table, read_handle) != 0 ||
        handle_check(&capability_table, read_handle,
                     PROSENJIT_CAP_READ, PROS_OBJECT_FILE) == 0) {
        serial_write("ProsenjitOS: ERROR stale capability handle test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: capability handle/rights test passed\n");
    if (process_check_handle(&init_process, user_test_handle,
                             PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
        process_check_handle(&init_process, user_test_handle,
                             PROSENJIT_CAP_WRITE, PROS_OBJECT_FILE) == 0) {
        serial_write("ProsenjitOS: ERROR process capability authorization test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process capability authorization test passed\n");

    /* Cross-process copy-transfer: the source remains valid and the target
     * receives only the explicitly requested subset of rights. */
    struct pros_process transfer_target;
    uint64_t transferred_handle;
    if (process_init(&transfer_target, 2, PROS_SECURITY_LIMITED) != 0) {
        serial_write("ProsenjitOS: ERROR transfer target process creation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    transferred_handle = process_transfer_handle_copy(&init_process, &transfer_target,
                                                       user_test_handle,
                                                       PROSENJIT_CAP_READ);
    if (transferred_handle == PROSENJIT_HANDLE_INVALID ||
        process_check_handle(&transfer_target, transferred_handle,
                             PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
        process_check_handle(&transfer_target, transferred_handle,
                             PROSENJIT_CAP_WRITE, PROS_OBJECT_FILE) == 0 ||
        process_check_handle(&init_process, user_test_handle,
                             PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
        process_transfer_handle_copy(&init_process, &transfer_target,
                                     user_test_handle,
                                     PROSENJIT_CAP_WRITE) != PROSENJIT_HANDLE_INVALID) {
        serial_write("ProsenjitOS: ERROR capability copy-transfer test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_close_handle(&transfer_target, transferred_handle) != 0 ||
        process_check_handle(&transfer_target, transferred_handle,
                             PROSENJIT_CAP_READ, PROS_OBJECT_FILE) == 0) {
        serial_write("ProsenjitOS: ERROR transferred handle lifecycle test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: capability copy-transfer test passed\n");

    /*
     * Cross-process move-transfer:
     * - requires TRANSFER authority
     * - attenuates rights
     * - consumes the source only after target allocation succeeds
     */
    struct pros_object move_test_object;
    uint64_t move_source_handle;
    uint64_t moved_handle;
    object_init(&move_test_object, PROS_OBJECT_FILE);
    if (process_grant_handle(&init_process, &move_test_object,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_TRANSFER,
                             &move_source_handle) != 0) {
        serial_write("ProsenjitOS: ERROR move-transfer source grant failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    moved_handle = process_transfer_handle_move(&init_process, &transfer_target,
                                                move_source_handle,
                                                PROSENJIT_CAP_READ);
    if (moved_handle == PROSENJIT_HANDLE_INVALID ||
        process_check_handle(&transfer_target, moved_handle,
                             PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0 ||
        process_check_handle(&transfer_target, moved_handle,
                             PROSENJIT_CAP_WRITE, PROS_OBJECT_FILE) == 0 ||
        process_check_handle(&init_process, move_source_handle,
                             PROSENJIT_CAP_READ, PROS_OBJECT_FILE) == 0 ||
        process_transfer_handle_move(&init_process, &transfer_target,
                                     move_source_handle,
                                     PROSENJIT_CAP_READ) != PROSENJIT_HANDLE_INVALID) {
        serial_write("ProsenjitOS: ERROR capability move-transfer test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    if (process_close_handle(&transfer_target, moved_handle) != 0) {
        serial_write("ProsenjitOS: ERROR moved handle close failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: capability move-transfer test passed\n");

    /* Inactive-process regression: use a fresh valid source capability so
     * rejection is proven by the zombie target state, not by a stale source. */
    struct pros_process inactive_transfer_target;
    struct pros_object inactive_move_object;
    uint64_t inactive_move_source;
    object_init(&inactive_move_object, PROS_OBJECT_FILE);
    if (process_grant_handle(&init_process, &inactive_move_object,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_TRANSFER,
                             &inactive_move_source) != 0 ||
        process_create(&init_process, &inactive_transfer_target, 7,
                       PROS_SECURITY_LIMITED) != 0 ||
        process_exit(&inactive_transfer_target, 99) != 0 ||
        process_transfer_handle_move(&init_process, &inactive_transfer_target,
                                     inactive_move_source,
                                     PROSENJIT_CAP_READ) != PROSENJIT_HANDLE_INVALID ||
        process_check_handle(&init_process, inactive_move_source,
                             PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0) {
        serial_write("ProsenjitOS: ERROR inactive capability move regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_wait(&init_process, inactive_transfer_target.pid, &waited_exit) != 0 ||
        waited_exit != 99) {
        serial_write("ProsenjitOS: ERROR inactive transfer target reap failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_check_handle(&init_process, inactive_move_source,
                             PROSENJIT_CAP_READ, PROS_OBJECT_FILE) != 0) {
        serial_write("ProsenjitOS: ERROR inactive move unexpectedly consumed source\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_close_handle(&init_process, inactive_move_source) != 0) {
        serial_write("ProsenjitOS: ERROR inactive move source cleanup failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: inactive capability move regression passed\n");


    /*
     * IPC foundation regression:
     * - channel is a first-class capability object
     * - FIFO ordering is preserved
     * - bounded capacity is enforced
     * - receive with a short buffer leaves the message queued
     */
    struct pros_ipc_channel ipc_test_channel;
    uint64_t ipc_handle;
    const uint8_t ipc_message[] = { 'P', 'r', 'o', 's', 'e', 'n', 'j', 'i', 't' };
    uint8_t ipc_buffer[PROSENJIT_IPC_MESSAGE_MAX];
    uint32_t ipc_length = 0;

    if (ipc_channel_init(&ipc_test_channel) != PROSENJIT_IPC_OK ||
        ipc_channel_capacity(&ipc_test_channel) != PROSENJIT_IPC_CAPACITY ||
        ipc_channel_count(&ipc_test_channel) != 0 ||
        process_grant_handle(&init_process, &ipc_test_channel.object,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE |
                             PROSENJIT_CAP_DUPLICATE | PROSENJIT_CAP_TRANSFER,
                             &ipc_handle) != 0 ||
        process_check_handle(&init_process, ipc_handle,
                             PROSENJIT_CAP_WRITE, PROS_OBJECT_IPC) != 0 ||
        process_check_handle(&init_process, ipc_handle,
                             PROSENJIT_CAP_READ, PROS_OBJECT_IPC) != 0) {
        serial_write("ProsenjitOS: ERROR IPC capability setup test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    if (ipc_channel_send(&ipc_test_channel, ipc_message, sizeof(ipc_message)) != 0 ||
        ipc_channel_count(&ipc_test_channel) != 1 ||
        ipc_channel_receive(&ipc_test_channel, ipc_buffer, 4, &ipc_length) !=
            PROSENJIT_IPC_EMSGSIZE ||
        ipc_channel_count(&ipc_test_channel) != 1 ||
        ipc_channel_receive(&ipc_test_channel, ipc_buffer, sizeof(ipc_buffer),
                            &ipc_length) != 0 ||
        ipc_length != sizeof(ipc_message)) {
        serial_write("ProsenjitOS: ERROR IPC send/receive regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    for (uint32_t i = 0; i < sizeof(ipc_message); ++i) {
        if (ipc_buffer[i] != ipc_message[i]) {
            serial_write("ProsenjitOS: ERROR IPC FIFO payload mismatch\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    if (ipc_channel_receive(&ipc_test_channel, ipc_buffer, sizeof(ipc_buffer),
                            &ipc_length) != PROSENJIT_IPC_EEMPTY) {
        serial_write("ProsenjitOS: ERROR IPC empty-queue regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    for (uint32_t i = 0; i < PROSENJIT_IPC_CAPACITY; ++i) {
        if (ipc_channel_send(&ipc_test_channel, ipc_message,
                             sizeof(ipc_message)) != PROSENJIT_IPC_OK) {
            serial_write("ProsenjitOS: ERROR IPC capacity fill failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    if (ipc_channel_count(&ipc_test_channel) != PROSENJIT_IPC_CAPACITY ||
        ipc_channel_send(&ipc_test_channel, ipc_message,
                         sizeof(ipc_message)) != PROSENJIT_IPC_EFULL ||
        ipc_channel_send(&ipc_test_channel, ipc_message,
                         PROSENJIT_IPC_MESSAGE_MAX + 1u) !=
            PROSENJIT_IPC_EMSGSIZE) {
        serial_write("ProsenjitOS: ERROR IPC bounds regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    if (process_close_handle(&init_process, ipc_handle) != 0 ||
        process_check_handle(&init_process, ipc_handle,
                             PROSENJIT_CAP_READ, PROS_OBJECT_IPC) == 0) {
        serial_write("ProsenjitOS: ERROR IPC capability lifecycle failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: IPC channel/capability foundation test passed\n");

    struct pros_address_space *user_space = &init_process.address_space;
    uint64_t user_page = physical_page_alloc();
    if (!user_page ||
        address_space_map_user_page(user_space, PROSENJIT_USER_TEST_VA,
                                    user_page, PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        serial_write("ProsenjitOS: ERROR user page mapping failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (address_space_switch(user_space) != 0) {
        serial_write("ProsenjitOS: ERROR user address-space switch failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    ((volatile uint8_t *)(uintptr_t)PROSENJIT_USER_TEST_VA)[0] = 0x5A;
    if (((volatile uint8_t *)(uintptr_t)PROSENJIT_USER_TEST_VA)[0] != 0x5A) {
        serial_write("ProsenjitOS: ERROR user mapping memory test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (paging_switch_kernel() != 0) {
        serial_write("ProsenjitOS: ERROR kernel address-space restore failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: user address-space isolation foundation test passed\n");

    /* Direct fault-fixup regression: the assembly backend must recover from
     * an unexpected supervisor-side page fault and return an error. */
    {
        uint8_t fault_copy_probe = 0;
        extern int copy_from_user_asm(void *destination, const void *source, uint64_t size);
        if (copy_from_user_asm(&fault_copy_probe,
                               (const void *)(uintptr_t)0x0000700000000000ULL, 1) == 0) {
            serial_write("ProsenjitOS: ERROR usercopy page-fault recovery test failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        serial_write("ProsenjitOS: usercopy page-fault recovery test passed\n");
    }

    uint64_t user_text_size = (uint64_t)(uintptr_t)__user_text_end -
                              (uint64_t)(uintptr_t)__user_text_start;
    if (user_text_size == 0 || user_text_size > PROSENJIT_PAGE_SIZE) {
        serial_write("ProsenjitOS: ERROR user test code does not fit one page\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    uint64_t user_code_page = physical_page_alloc();
    uint64_t user_data_page = physical_page_alloc();
    uint64_t user_stack_page = physical_page_alloc();
    if (!user_code_page || !user_data_page || !user_stack_page) {
        serial_write("ProsenjitOS: ERROR user-mode page allocation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    for (uint64_t i = 0; i < user_text_size; ++i)
        ((uint8_t *)(uintptr_t)user_code_page)[i] =
            ((const uint8_t *)__user_text_start)[i];

    if (address_space_map_user_page(user_space, PROSENJIT_USER_CODE_VA,
                                    user_code_page, 0) != 0 ||
        address_space_map_user_page(user_space, PROSENJIT_USER_TEST_VA,
                                    user_data_page,
                                    PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        address_space_map_user_page(user_space, PROSENJIT_USER_STACK_VA,
                                    user_stack_page,
                                    PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        serial_write("ProsenjitOS: ERROR user-mode mappings failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /*
     * Validate the complete user-code page walk before IRET. This catches
     * permission/table-construction errors before they become a nested
     * page-fault/double-fault during CPL3 entry.
     */
    {
        const uint64_t mask = 0x000FFFFFFFFFF000ULL;
        const uint64_t va = PROSENJIT_USER_CODE_VA;
        const uint32_t pml4_i = (uint32_t)((va >> 39) & 0x1FFu);
        const uint32_t pdpt_i = (uint32_t)((va >> 30) & 0x1FFu);
        const uint32_t pd_i = (uint32_t)((va >> 21) & 0x1FFu);
        const uint32_t pt_i = (uint32_t)((va >> 12) & 0x1FFu);
        uint64_t *root = user_space->root;
        uint64_t pml4e = root[pml4_i];
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & mask);
        uint64_t pdpte = pdpt[pdpt_i];
        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & mask);
        uint64_t pde = pd[pd_i];
        uint64_t *pt = (uint64_t *)(uintptr_t)(pde & mask);
        uint64_t pte = pt[pt_i];
        serial_write("ProsenjitOS: user code PML4E = "); serial_hex64(pml4e); serial_write("\n");
        serial_write("ProsenjitOS: user code PDPTE = "); serial_hex64(pdpte); serial_write("\n");
        serial_write("ProsenjitOS: user code PDE = "); serial_hex64(pde); serial_write("\n");
        serial_write("ProsenjitOS: user code PTE = "); serial_hex64(pte); serial_write("\n");
        if ((pml4e & 0x7ULL) != 0x7ULL ||
            (pdpte & 0x7ULL) != 0x7ULL ||
            (pde & 0x7ULL) != 0x7ULL ||
            (pte & 0x7ULL) != 0x5ULL ||
            (pte & (1ULL << 63)) != 0) {
            serial_write("ProsenjitOS: ERROR user code page walk validation failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }

    /*
     * Give the CPL3 test a fresh capability after all kernel-side handle
     * lifecycle tests. This keeps the user-mode ABI test independent from
     * earlier slot/generation churn.
     */
    uint64_t user_mode_handle;
    uint64_t user_ipc_handle;
    if (ipc_channel_init(&user_ipc_channel) != PROSENJIT_IPC_OK ||
        process_grant_handle(&init_process, &user_ipc_channel.object,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE |
                             PROSENJIT_CAP_DUPLICATE | PROSENJIT_CAP_TRANSFER,
                             &user_ipc_handle) != 0) {
        serial_write("ProsenjitOS: ERROR user IPC capability preparation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_check_handle(&init_process, user_ipc_handle,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE |
                             PROSENJIT_CAP_DUPLICATE,
                             PROS_OBJECT_IPC) != 0 ||
        (user_mode_handle = process_duplicate_handle(
             &init_process, user_ipc_handle,
             PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE |
             PROSENJIT_CAP_DUPLICATE)) == PROSENJIT_HANDLE_INVALID ||
        process_check_handle(&init_process, user_mode_handle,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE |
                             PROSENJIT_CAP_DUPLICATE,
                             PROS_OBJECT_IPC) != 0) {
        serial_write("ProsenjitOS: ERROR user-mode capability preparation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: user-mode capability prepared\n");

    serial_write("ProsenjitOS: entering CPL3 user-mode test\n");
    if (address_space_switch(user_space) != 0) {
        serial_write("ProsenjitOS: ERROR user address-space switch failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    enter_user_mode(PROSENJIT_USER_CODE_VA, PROSENJIT_USER_STACK_TOP, user_mode_handle);
    for (;;) __asm__ volatile ("cli; hlt");

    vga_write("ProsenjitOS 0.1.0 - kernel online", 0x0F);
    serial_write("ProsenjitOS: kernel online\n");
    for (;;) __asm__ volatile ("hlt");
}
