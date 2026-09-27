#include "prosenjit.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define VGA_ADDRESS 0xB8000u
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define IDT_ENTRIES 32u

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
    for (uint32_t i = 0; i < IDT_ENTRIES; ++i) idt_set_gate(i, handlers[i]);
    struct idt_pointer ptr = { (uint16_t)(sizeof(idt) - 1u), (uint64_t)idt };
    idt_load(&ptr);
}
void exception_dispatch(uint64_t *frame) {
    uint64_t vector = frame[0];
    serial_write("ProsenjitOS: CPU exception vector ");
    serial_hex64(vector);
    serial_write("\n");
    vga_write("ProsenjitOS: CPU exception - system halted", 0x4F);
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
    __asm__ volatile ("sti");
    serial_write("ProsenjitOS: IDT loaded; exceptions armed\n");
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

    vga_write("ProsenjitOS 0.1.0 - kernel online", 0x0F);
    serial_write("ProsenjitOS: kernel online\n");
    for (;;) __asm__ volatile ("hlt");
}
