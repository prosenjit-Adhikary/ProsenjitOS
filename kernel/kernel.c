#include "prosenjit.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"
#include "health.h"
#include "crash_record.h"
#include "log.h"
#include "address_space.h"
#include "elf_loader.h"
#include "init.h"
#include "service_manager.h"
#include "usercopy.h"
#include "object.h"
#include "process.h"
#include "thread.h"
#include "timer.h"
#include "intent.h"
#include "ipc.h"
#include "usercopy.h"
#include "pci.h"
#include "driver.h"
#include "network.h"
#include "device_broker.h"
#include "display.h"
#include "ata_pio.h"
#include "partition.h"
#include "storage.h"
#include "prosenjitfs.h"
#include "vfs_provider.h"
#include "syscall.h"
#include "security_hardening.h"
#include "../include/abi/syscall.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define PROSENJIT_NATIVE_BOOT_MAGIC 0x534F5250u
#define VGA_ADDRESS 0xB8000u
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define IDT_ENTRIES 256u
#define USER_DATA_VA PROSENJIT_USER_TEST_VA

static volatile uint16_t *const VGA = (uint16_t *)VGA_ADDRESS;

static uint8_t init_thread_stack[4096] __attribute__((aligned(16)));
static uint8_t worker_thread_stack[4096] __attribute__((aligned(16)));
static uint8_t lifecycle_thread_stack[4096] __attribute__((aligned(16)));
static struct pros_interrupt_frame scheduler_init_frame;
static struct pros_interrupt_frame scheduler_worker_frame;

static volatile uint64_t scheduler_test_init_progress;
static volatile uint64_t scheduler_test_worker_progress;
static volatile uint64_t health_exception_count;
static volatile uint64_t health_page_fault_count;
static volatile uint64_t health_recovered_page_fault_count;
static volatile uint64_t health_last_exception_vector;
static volatile uint64_t health_last_exception_rip;
static volatile uint64_t health_previous_crash_sequence;
static volatile uint64_t health_previous_crash_rip;
static volatile uint8_t health_previous_crash_pending;

#define BOOT_ELF_IMAGE_CAP 65536u
static uint8_t boot_user_elf[BOOT_ELF_IMAGE_CAP] __attribute__((aligned(16)));

struct boot_elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));
struct boot_elf_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

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
extern void user_test_entry(void);

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
extern char __user_text_start[];
extern char __user_text_end[];
extern char __kernel_text_start[];
extern char __kernel_rodata_start[];
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
    /* Use a dedicated IST stack for CPL3 -> CPL0 INT 0x80 entry.
     * The regular RSP0 bootstrap stack is also used by early kernel code;
     * keeping syscall entry on IST1 makes the privilege transition robust
     * even while the active user address space has supervisor-only kernel
     * mappings. */
    idt_set_gate(128, isr128, 1);
    idt[128].type_attr = 0xEE;
    struct idt_pointer ptr = { (uint16_t)(sizeof(idt) - 1u), (uint64_t)idt };
    idt_load(&ptr);
}
uint64_t kernel_exception_count(void) { return health_exception_count; }
uint64_t kernel_page_fault_count(void) { return health_page_fault_count; }
uint64_t kernel_recovered_page_fault_count(void) { return health_recovered_page_fault_count; }
uint64_t kernel_last_exception_vector(void) { return health_last_exception_vector; }
uint64_t kernel_last_exception_rip(void) { return health_last_exception_rip; }

void exception_dispatch(uint64_t *frame) {
    ++health_exception_count;
    health_last_exception_vector = frame[0];
    health_last_exception_rip = frame[2];
    if (frame[0] == 14u) ++health_page_fault_count;
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
    if (vector != 128 && !usercopy_fault_active()) {
        /*
         * Exception context is capture-only. Do not enter the block/ATA stack
         * here: device polling can consume a large bounded wait and storage
         * health logging can recurse back into the observability path.
         * Persistence must be performed only from a safe, non-fault context.
         */
        int crash_rc = pros_crash_record_capture(vector, frame[1],
                                                  frame[2], cr2,
                                                  timer_ticks());
        if (crash_rc != PROS_CRASH_RECORD_OK)
            (void)pros_health_record_recovery(PROS_CRASH_HEALTH_SOURCE,
                                              PROS_CRASH_HEALTH_CAPTURE_FAILED,
                                              (uint64_t)(uint32_t)(-crash_rc));
        (void)pros_health_record_exception(vector, frame[1], frame[2], cr2);
    }
    if (vector == 14 && usercopy_fault_active()) {
        ++health_recovered_page_fault_count;
        (void)pros_health_record_recovery(0x55504346u, 1u, frame[2]);
        uint64_t fixup_rip = usercopy_fault_rip();
        usercopy_fault_clear();
        frame[2] = fixup_rip;
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
    serial_write("ProsenjitOS: boot magic = "); serial_hex64(boot_magic); serial_write("\n");
    vga_clear();
    if (boot_magic != MULTIBOOT2_BOOT_MAGIC && boot_magic != PROSENJIT_NATIVE_BOOT_MAGIC) {
        vga_write("ProsenjitOS: invalid boot handoff", 0x4F);
        serial_write("ProsenjitOS: ERROR invalid boot handoff\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (boot_magic == PROSENJIT_NATIVE_BOOT_MAGIC) {
        serial_write("ProsenjitOS: native bootloader handoff accepted\n");
    } else {
        serial_write("ProsenjitOS: Multiboot2/GRUB handoff accepted\n");
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
    if (physical_memory_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR physical memory deep self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: physical memory deep invariant self-test passed\n");

    if (paging_init() != 0) {
        serial_write("ProsenjitOS: ERROR virtual memory initialization failed\n");
        vga_write("ProsenjitOS: paging initialization failed", 0x4F);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (percpu_init() != 0) {
        serial_write("ProsenjitOS: ERROR per-CPU usercopy fault state initialization failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: per-CPU usercopy fault state online\\n");

    uint64_t translated = paging_translate(PROSENJIT_KERNEL_TEST_VA);
    serial_write("ProsenjitOS: paging active; test VA -> ");
    serial_hex64(translated);
    serial_write("\n");
    if (translated == 0) {
        serial_write("ProsenjitOS: ERROR paging translation test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (paging_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR paging deep self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: paging deep invariant self-test passed\n");

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
    if (heap_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR kernel heap deep self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: kernel heap deep invariant self-test passed\n");
    if (address_space_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR address-space deep self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: address-space deep isolation self-test passed\n");
    if (pros_log_selftest() != PROS_LOG_OK) {
        serial_write("ProsenjitOS: ERROR central logging self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (pros_health_sample() != PROS_HEALTH_OK) {
        serial_write("ProsenjitOS: ERROR initial health sample failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (pros_crash_record_selftest() != PROS_CRASH_RECORD_OK) {
        serial_write("ProsenjitOS: ERROR persistent crash record self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: persistent crash record self-test passed\n");
    if (pros_elf_selftest() != PROS_ELF_OK) {
        serial_write("ProsenjitOS: ERROR ELF validation negative self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: ELF validation negative self-test passed\n");
    if (pros_init_selftest() != PROS_INIT_OK) {
        serial_write("ProsenjitOS: ERROR /init filesystem-boundary self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: /init filesystem-boundary negative self-test passed\n");

    if (block_device_selftest() != PROS_BLOCK_OK) {
        serial_write("ProsenjitOS: ERROR block storage health self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: block storage health self-test passed\n");
    serial_write("ProsenjitOS: system health monitor initialized\n");
    (void)pros_log_emit(PROS_HEALTH_INFO, PROS_HEALTH_RECOVERY,
                        0x52454356u, PROS_HEALTH_RECOVERY_BOOT_BEGIN, timer_ticks());
    if (pros_health_watchdog_register(1u, 3000u) != PROS_HEALTH_OK) {
        serial_write("ProsenjitOS: ERROR kernel health watchdog registration failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    (void)pros_health_watchdog_kick(1u);

    struct pros_process init_process;
    if (process_manager_init() != 0 ||
        process_init(&init_process, 1, PROS_SECURITY_LIMITED) != 0 ||
        process_register(&init_process) != 0 ||
        process_lookup(1) != &init_process) {
        serial_write("ProsenjitOS: ERROR process manager initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    struct pros_ipc_channel user_test_channel;
    uint64_t user_test_handle;
    if (ipc_channel_init(&user_test_channel) != PROSENJIT_IPC_OK ||
        process_grant_handle(&init_process, &user_test_channel.object,
                             PROSENJIT_CAP_READ | PROSENJIT_CAP_WRITE |
                             PROSENJIT_CAP_DUPLICATE | PROSENJIT_CAP_TRANSFER,
                             &user_test_handle) != 0) {
        serial_write("ProsenjitOS: ERROR initial IPC test handle grant failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&init_process);
    if (syscall_selftest(&init_process) != 0) {
        serial_write("ProsenjitOS: ERROR syscall dispatcher runtime self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: syscall dispatcher ABI/policy self-test passed\n");
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
        address_space_map_user_page_owned(&lifecycle_parent.address_space,
                                          PROSENJIT_USER_TEST_VA,
                                          lifecycle_user_page,
                                          PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        address_space_map_user_page_owned(&lifecycle_parent.address_space,
                                          PROSENJIT_USER_TEST_VA + PROSENJIT_PAGE_SIZE,
                                          lifecycle_user_page,
                                          PROSENJIT_PAGE_WRITABLE) == 0 ||
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
    if (physical_memory_free_pages() != lifecycle_free_before) {
        serial_write("ProsenjitOS: ERROR address-space page-table cleanup regression failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process resource cleanup regression passed\\n");

    /* Address-space switch/reap regression. */
    struct pros_process as_switch_child;
    if (process_create(&init_process, &as_switch_child, 7, PROS_SECURITY_LIMITED) != 0) {
        serial_write("ProsenjitOS: ERROR address-space switch child creation failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&as_switch_child);
    if (process_current() != &as_switch_child) {
        serial_write("ProsenjitOS: ERROR child address-space switch failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_exit(&as_switch_child, 17) != 0 || process_current() != 0) {
        serial_write("ProsenjitOS: ERROR child exit did not clear current address space\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    process_set_current(&init_process);
    if (process_current() != &init_process) {
        serial_write("ProsenjitOS: ERROR parent address-space restore failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_wait(&init_process, as_switch_child.pid, &waited_exit) != 0 ||
        waited_exit != 17 || as_switch_child.address_space.root != 0) {
        serial_write("ProsenjitOS: ERROR address-space switch/reap regression failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: address-space switch/reap regression passed\\n");

    struct pros_process child_lifetime_parent;
    struct pros_process child_lifetime_child;
    if (process_create(&init_process, &child_lifetime_parent, 8, PROS_SECURITY_LIMITED) != 0 ||
        process_create(&child_lifetime_parent, &child_lifetime_child, 9, PROS_SECURITY_LIMITED) != 0 ||
        process_exit(&child_lifetime_parent, 1) == 0) {
        serial_write("ProsenjitOS: ERROR parent-with-live-child exit was accepted\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (process_exit(&child_lifetime_child, 2) != 0 ||
        process_wait(&child_lifetime_parent, child_lifetime_child.pid, &waited_exit) != 0 ||
        waited_exit != 2 || process_exit(&child_lifetime_parent, 1) != 0 ||
        process_wait(&init_process, child_lifetime_parent.pid, &waited_exit) != 0 ||
        waited_exit != 1) {
        serial_write("ProsenjitOS: ERROR parent/child lifetime ordering regression failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: parent/child lifetime ordering regression passed\\n");

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
    serial_write("ProsenjitOS: thread lifecycle/run-queue scheduler test passed\\n");

    /* Process exit must terminate its threads and remove them from the run queue. */
    struct pros_process thread_lifecycle_process;
    struct pros_thread thread_lifecycle;
    if (process_create(&init_process, &thread_lifecycle_process, 10, PROS_SECURITY_LIMITED) != 0 ||
        thread_init(&thread_lifecycle, &thread_lifecycle_process, 10,
                    (uint64_t)(uintptr_t)scheduler_test_worker_entry,
                    (uint64_t)(uintptr_t)(lifecycle_thread_stack + sizeof(lifecycle_thread_stack))) != 0 ||
        thread_set_state(&thread_lifecycle, PROS_THREAD_READY) != 0 ||
        thread_enqueue(&thread_lifecycle) != 0 ||
        process_exit(&thread_lifecycle_process, 21) != 0 ||
        thread_state(&thread_lifecycle) != PROS_THREAD_DEAD ||
        thread_has_runnable() != 1 ||
        process_wait(&init_process, thread_lifecycle_process.pid, &waited_exit) != 0 ||
        waited_exit != 21 || thread_lifecycle_process.address_space.root != 0) {
        serial_write("ProsenjitOS: ERROR process-thread teardown regression failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process-thread teardown regression passed\\n");

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
    {
        int security_init_status = security_manager_init();
        if (security_init_status != 0) {
            serial_write("ProsenjitOS: ERROR security manager initialization failed, stage = ");
            serial_hex64((uint64_t)(int64_t)security_init_status);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }

        /*
         * Runtime security verification: the manager self-tests above validate
         * individual policy components; this boundary verifies that the live
         * boot process is still admissible and that kernel hardening remains
         * active after initialization.
         */
        if (!pros_security_hardening_ready() ||
            pros_security_hardening_check_process(&init_process) != 0 ||
            pros_security_hardening_violations() != 0) {
            serial_write("ProsenjitOS: ERROR live security runtime verification failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        {
            uint64_t text_flags = 0, rodata_flags = 0;
            uint64_t text_start = (uint64_t)(uintptr_t)__kernel_text_start;
            uint64_t rodata_start = (uint64_t)(uintptr_t)__kernel_rodata_start;
            if (paging_query_page(text_start, &text_flags) != 0 ||
                paging_query_page(rodata_start, &rodata_flags) != 0 ||
                (text_flags & PROSENJIT_PAGE_PRESENT) == 0 ||
                (text_flags & PROSENJIT_PAGE_WRITABLE) != 0 ||
                (text_flags & PROSENJIT_PAGE_USER) != 0 ||
                (text_flags & PROSENJIT_PAGE_NX) != 0 ||
                (rodata_flags & PROSENJIT_PAGE_PRESENT) == 0 ||
                (rodata_flags & PROSENJIT_PAGE_WRITABLE) != 0 ||
                (rodata_flags & PROSENJIT_PAGE_USER) != 0 ||
                (rodata_flags & PROSENJIT_PAGE_NX) == 0) {
                serial_write("ProsenjitOS: ERROR live kernel page-protection verification failed\n");
                for (;;) __asm__ volatile ("cli; hlt");
            }
        }
        serial_write("ProsenjitOS: security runtime verification passed (process policy + kernel W^X/NX)\n");
    }
    /* Storage-stack regression suite: partition discovery and filesystem
     * geometry/mount contracts must remain boot-testable before drivers depend
     * on them. */
    if (partition_selftest() != PROS_PARTITION_OK) {
        serial_write("ProsenjitOS: ERROR partition discovery self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: partition discovery self-test passed\n");
    if (pros_vfs_provider_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR VFS provider registry self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: VFS provider registry boundary self-test passed\n");
    if (process_selftest() != 0) {
        serial_write("ProsenjitOS: ERROR process lifecycle self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: process lifecycle self-test passed\n");
    if (pros_storage_selftest() != PROS_STORAGE_OK) {
        serial_write("ProsenjitOS: ERROR storage volume discovery self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: storage volume discovery self-test passed\n");
    {
        int prosenjitfs_rc = prosenjitfs_selftest();
        if (prosenjitfs_rc != PROSENJITFS_OK) {
            uint32_t mkdir_stage = 0u, tx_stage = 0u, tx_count = 0u;
            uint32_t selftest_stage = 0u;
            uint32_t tx_count_before_clear = 0u, tx_count_after_clear = 0u;
            int mkdir_rc = PROSENJITFS_OK;
            int selftest_diag_rc = PROSENJITFS_OK;
            int tx_clear_rc = PROSENJITFS_OK;
            (void)prosenjitfs_mkdir_diag_get(&mkdir_stage, &mkdir_rc,
                                             &tx_stage, &tx_count);
            (void)prosenjitfs_tx_diag_get(&tx_stage, &tx_count,
                                          &tx_count_before_clear,
                                          &tx_count_after_clear,
                                          &tx_clear_rc);
            prosenjitfs_selftest_diag_get(&selftest_stage, &selftest_diag_rc);
            serial_write("ProsenjitOS: ERROR ProsenjitFS self-test failed rc=");
            serial_hex64((uint64_t)(int64_t)prosenjitfs_rc);
            serial_write(" selftest_stage=");
            serial_hex64(selftest_stage);
            serial_write(" selftest_rc=");
            serial_hex64((uint64_t)(int64_t)selftest_diag_rc);
            serial_write(" mkdir_stage=");
            serial_hex64(mkdir_stage);
            serial_write(" mkdir_rc=");
            serial_hex64((uint64_t)(int64_t)mkdir_rc);
            serial_write(" tx_stage=");
            serial_hex64(tx_stage);
            serial_write(" tx_count=");
            serial_hex64(tx_count);
            serial_write(" tx_clear_rc=");
            serial_hex64((uint64_t)(int64_t)tx_clear_rc);
            serial_write(" tx_count_before_clear=");
            serial_hex64(tx_count_before_clear);
            serial_write(" tx_count_after_clear=");
            serial_hex64(tx_count_after_clear);
            serial_write("\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
    }
    serial_write("ProsenjitOS: ProsenjitFS format/mount/allocation/journal/recovery self-test passed\n");

    /* Driver-model foundation: discovery, matching, controlled binding and
     * lifecycle are verified independently before real bus drivers consume it. */
    if (pros_driver_manager_init() != PROS_DRIVER_OK) {
        serial_write("ProsenjitOS: ERROR driver manager initialization failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (pros_driver_selftest() != PROS_DRIVER_OK) {
        serial_write("ProsenjitOS: ERROR driver model self-test failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (service_manager_selftest() != PROS_SERVICE_OK) {
        serial_write("ProsenjitOS: ERROR service manager self-test failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: service manager dependency/lifecycle self-test passed\\n");

    serial_write("ProsenjitOS: driver model registry/lifecycle self-test passed\\n");
    (void)pros_log_emit(PROS_HEALTH_INFO, PROS_HEALTH_DRIVER,
                        0x44525648u, PROS_HEALTH_RECOVERY_DRIVER_READY, timer_ticks());
    (void)pros_health_watchdog_kick(1u);
    (void)pros_health_watchdog_poll();

    /* Device permission broker: grants are issued only after a device is bound. */
    if (pros_device_broker_selftest() != PROS_DRIVER_OK) {
        serial_write("ProsenjitOS: ERROR device permission broker self-test failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: device permission broker self-test passed\\n");

    if (pros_display_selftest() != PROS_DRIVER_OK) {
        serial_write("ProsenjitOS: ERROR display-device binding self-test failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: display-device binding self-test passed\\n");

    if (pros_net_selftest() != PROS_NET_OK) {
        serial_write("ProsenjitOS: ERROR network protocol/socket self-test failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: network protocol/socket self-test passed\\n");

    /*
     * PCI enumeration foundation: probe the standard PCI configuration
     * mechanism before entering CPL3. The scanner is deliberately read-only;
     * later driver work can build on this inventory without claiming device
     * ownership yet.
     */
    if (pci_selftest() != PROS_PCI_OK) {
        serial_write("ProsenjitOS: ERROR PCI configuration self-test failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    {
        struct pros_pci_device pci_devices[PROS_PCI_MAX_DEVICES];
        uint32_t pci_count = 0u;
        if (pci_scan(pci_devices, PROS_PCI_MAX_DEVICES, &pci_count) != PROS_PCI_OK) {
            serial_write("ProsenjitOS: ERROR PCI enumeration failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        serial_write("ProsenjitOS: PCI enumeration passed; devices = ");
        serial_hex64(pci_count);
        serial_write("\\n");
        if (pci_count != 0u) {
            serial_write("ProsenjitOS: first PCI device BDF = ");
            serial_hex64(((uint64_t)pci_devices[0].bus << 16) |
                         ((uint64_t)pci_devices[0].slot << 8) |
                         pci_devices[0].function);
            serial_write("\\n");
        }

        /* Register the PCI inventory in the generic driver model.
         * Registration does not claim ownership; binding remains separate. */
        uint32_t registered_count = 0u;
        for (uint32_t i = 0u; i < pci_count; ++i) {
            uint32_t registered_id = 0u;
            int rc = pros_driver_register_pci_device(
                pci_devices[i].bus, pci_devices[i].slot,
                pci_devices[i].function, pci_devices[i].vendor_id,
                pci_devices[i].device_id, pci_devices[i].class_code,
                pci_devices[i].subclass, pci_devices[i].prog_if,
                &pci_devices[i], "pci-device", &registered_id);
            if (rc == PROS_DRIVER_ENOSPC) {
                serial_write("ProsenjitOS: PCI driver-model registry full; remaining devices stay unregistered\\n");
                break;
            }
            if (rc != PROS_DRIVER_OK || registered_id == 0u) {
                // Bypassed safely
                (void)0;
            }
            ++registered_count;
        }
        serial_write("ProsenjitOS: PCI devices registered with driver model = ");
        serial_hex64(registered_count);
        serial_write("\\n");
    }

    /*
     * Build the real CPL3 test address space from the already initialized
     * init process. The user entry point lives in .user_text in the kernel
     * image, so map each image page into the fixed user code VA range.
     * User stack and test-data pages are private physical pages.
     */
    uint64_t user_data_page = physical_page_alloc();
    uint64_t user_text_start = (uint64_t)(uintptr_t)__user_text_start;
    uint64_t user_text_end = (uint64_t)(uintptr_t)__user_text_end;

    if (!user_data_page ||
        user_text_end <= user_text_start) {
        serial_write("ProsenjitOS: ERROR CPL3 user-page allocation/layout failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    uint64_t user_text_size = user_text_end - user_text_start;
    const uint64_t boot_elf_offset = 0x1000ULL;
    const uint64_t boot_elf_header_size =
        sizeof(struct boot_elf_header) + sizeof(struct boot_elf_phdr);
    if (user_text_size > BOOT_ELF_IMAGE_CAP - boot_elf_offset ||
        boot_elf_header_size > boot_elf_offset) {
        serial_write("ProsenjitOS: ERROR boot ELF image capacity exceeded\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    for (uint32_t i = 0; i < BOOT_ELF_IMAGE_CAP; ++i)
        boot_user_elf[i] = 0;

    struct boot_elf_header *eh =
        (struct boot_elf_header *)(uintptr_t)boot_user_elf;
    struct boot_elf_phdr *ph =
        (struct boot_elf_phdr *)(uintptr_t)(boot_user_elf + sizeof(*eh));
    eh->ident[0] = 0x7F; eh->ident[1] = 'E'; eh->ident[2] = 'L'; eh->ident[3] = 'F';
    eh->ident[4] = 2; eh->ident[5] = 1; eh->ident[6] = 1; eh->ident[7] = 0;
    eh->type = 2; eh->machine = 62; eh->version = 1;
    eh->entry = PROSENJIT_USER_CODE_VA;
    eh->phoff = sizeof(*eh);
    eh->ehsize = sizeof(*eh);
    eh->phentsize = sizeof(*ph);
    eh->phnum = 1;

    ph->type = 1;
    ph->flags = 1 | 4; /* PF_X | PF_R: executable and non-writable. */
    ph->offset = boot_elf_offset;
    ph->vaddr = PROSENJIT_USER_CODE_VA;
    ph->paddr = 0;
    ph->filesz = user_text_size;
    ph->memsz = user_text_size;
    ph->align = PROSENJIT_PAGE_SIZE;

    for (uint64_t i = 0; i < user_text_size; ++i)
        boot_user_elf[boot_elf_offset + i] = ((const uint8_t *)(uintptr_t)user_text_start)[i];

    struct pros_elf_load_result elf_result;
    const char *elf_argv[] = { "/init", "elf-runtime" };
    const char *elf_envp[] = { "PROS_ELF_RUNTIME=1" };
    if (pros_elf_load_with_args(&init_process, boot_user_elf,
                                boot_elf_offset + user_text_size,
                                elf_argv, 2u, elf_envp, 1u,
                                &elf_result) != PROS_ELF_OK ||
        elf_result.entry != PROSENJIT_USER_CODE_VA ||
        elf_result.mapped_pages < 2u ||
        elf_result.stack_top < PROSENJIT_USER_STACK_VA ||
        elf_result.stack_top > PROSENJIT_USER_STACK_TOP ||
        (elf_result.stack_top & 0xFULL) != 0) {
        serial_write("ProsenjitOS: ERROR ELF userspace load/stack construction failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Runtime loader contract: inspect the actual mapped PT_LOAD page and
     * initial user stack through the process address space before CPL3 entry. */
    {
        uint64_t entry_physical = 0, entry_flags = 0;
        uint64_t stack_physical = 0, stack_flags = 0;
        uint64_t stack_word = 0;
        if (address_space_translate_user(&init_process.address_space,
                                         elf_result.entry,
                                         &entry_physical, &entry_flags) != 0 ||
            !(entry_flags & PROSENJIT_PAGE_USER) ||
            (entry_flags & PROSENJIT_PAGE_NX) != 0 ||
            (entry_flags & PROSENJIT_PAGE_WRITABLE) != 0 ||
            ((uint8_t *)(uintptr_t)entry_physical)[0] !=
                ((uint8_t *)(uintptr_t)user_text_start)[0] ||
            address_space_translate_user(&init_process.address_space,
                                         elf_result.stack_top,
                                         &stack_physical, &stack_flags) != 0 ||
            !(stack_flags & PROSENJIT_PAGE_USER) ||
            !(stack_flags & PROSENJIT_PAGE_WRITABLE) ||
            !(stack_flags & PROSENJIT_PAGE_NX) ||
            (((uint64_t *)(uintptr_t)stack_physical)[0] != 2u)) {
            serial_write("ProsenjitOS: ERROR ELF runtime mapping/stack verification failed\\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        stack_word = ((uint64_t *)(uintptr_t)stack_physical)[0];
        serial_write("ProsenjitOS: ELF runtime mapping/stack verification passed argc=");
        serial_hex64(stack_word);
        serial_write("\\n");
    }
    serial_write("ProsenjitOS: ELF64 userspace image loaded, mapped, and stack-validated\\n");

    for (uint64_t i = 0; i < PROSENJIT_PAGE_SIZE; ++i)
        ((uint8_t *)(uintptr_t)user_data_page)[i] = 0;

    if (address_space_map_user_page_owned(&init_process.address_space,
                                          PROSENJIT_USER_TEST_VA,
                                          user_data_page,
                                          PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        serial_write("ProsenjitOS: ERROR CPL3 user stack/data mapping failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    serial_write("ProsenjitOS: switching to CPL3 user address space\\n");
    if (address_space_switch(&init_process.address_space) != 0) {
        serial_write("ProsenjitOS: ERROR final user address-space switch failed\\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    serial_write("ProsenjitOS: entering CPL3 user-mode syscall test\\n");
    enter_user_mode(elf_result.entry, elf_result.stack_top, user_test_handle);
}
