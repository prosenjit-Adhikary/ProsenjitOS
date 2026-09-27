#include "prosenjit.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"
#include "address_space.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define VGA_ADDRESS 0xB8000u
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define IDT_ENTRIES 256u
#define USER_DATA_VA PROSENJIT_USER_TEST_VA

static volatile uint16_t *const VGA = (uint16_t *)VGA_ADDRESS;
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
extern void enter_user_mode(uint64_t entry, uint64_t stack_top);
extern void cpu_load_tss(void);
extern void user_mode_exit_trampoline(void);
extern char __user_text_start[];
extern char __user_text_end[];
#define ISR(n) extern void isr##n(void);
ISR(0) ISR(1) ISR(2) ISR(3) ISR(4) ISR(5) ISR(6) ISR(7)
ISR(8) ISR(9) ISR(10) ISR(11) ISR(12) ISR(13) ISR(14) ISR(15)
ISR(16) ISR(17) ISR(18) ISR(19) ISR(20) ISR(21) ISR(22) ISR(23)
ISR(24) ISR(25) ISR(26) ISR(27) ISR(28) ISR(29) ISR(30) ISR(31)

static void idt_set_gate(uint32_t n, void (*handler)(void)) {
    uint64_t addr = (uint64_t)handler;
    idt[n].offset_low = (uint16_t)addr;
    idt[n].selector = 0x08;
    idt[n].ist = 0;
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
    for (uint32_t i = 0; i < 32; ++i) idt_set_gate(i, handlers[i]);
    idt_set_gate(128, isr128);
    idt[128].type_attr = 0xEE;
    struct idt_pointer ptr = { (uint16_t)(sizeof(idt) - 1u), (uint64_t)idt };
    idt_load(&ptr);
}
void exception_dispatch(uint64_t *frame) {
    uint64_t vector = frame[0];
    serial_write("ProsenjitOS: CPU exception vector ");
    serial_hex64(vector);
    serial_write("\n");
    if (vector == 128) {
        if (((volatile uint8_t *)(uintptr_t)USER_DATA_VA)[0] != 0x5A) {
            serial_write("ProsenjitOS: ERROR user-mode write verification failed\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        serial_write("ProsenjitOS: CPL3 syscall returned to kernel successfully\n");
        if (paging_switch_kernel() != 0) {
            serial_write("ProsenjitOS: ERROR kernel address-space restore failed\\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        frame[2] = (uint64_t)(uintptr_t)user_mode_exit_trampoline;
        frame[3] = 0x08;
        return;
    }
    vga_write("ProsenjitOS: CPU exception - system halted", 0x4F);
    for (;;) __asm__ volatile ("cli; hlt");
}

void user_mode_test_complete(void) {
    serial_write("ProsenjitOS: CPL3 user-mode execution test passed\n");
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
    cpu_load_tss();
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

    struct pros_address_space user_space;
    if (address_space_create(&user_space) != 0) {
        serial_write("ProsenjitOS: ERROR user address-space creation failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    uint64_t user_page = physical_page_alloc();
    if (!user_page ||
        address_space_map_user_page(&user_space, PROSENJIT_USER_TEST_VA,
                                    user_page, PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        serial_write("ProsenjitOS: ERROR user page mapping failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (address_space_switch(&user_space) != 0) {
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

    if (address_space_map_user_page(&user_space, PROSENJIT_USER_CODE_VA,
                                    user_code_page, 0) != 0 ||
        address_space_map_user_page(&user_space, PROSENJIT_USER_TEST_VA,
                                    user_data_page,
                                    PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0 ||
        address_space_map_user_page(&user_space, PROSENJIT_USER_STACK_VA,
                                    user_stack_page,
                                    PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        serial_write("ProsenjitOS: ERROR user-mode mappings failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    serial_write("ProsenjitOS: entering CPL3 user-mode test\n");
    if (address_space_switch(&user_space) != 0) {
        serial_write("ProsenjitOS: ERROR user address-space switch failed\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    enter_user_mode(PROSENJIT_USER_CODE_VA, PROSENJIT_USER_STACK_TOP);
    for (;;) __asm__ volatile ("cli; hlt");

    vga_write("ProsenjitOS 0.1.0 - kernel online", 0x0F);
    serial_write("ProsenjitOS: kernel online\n");
    for (;;) __asm__ volatile ("hlt");
}
