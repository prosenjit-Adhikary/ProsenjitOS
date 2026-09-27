#include "prosenjit.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define VGA_ADDRESS 0xB8000u
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u

static volatile uint16_t *const VGA = (uint16_t *)VGA_ADDRESS;
static volatile uint8_t *const COM1 = (uint8_t *)0x3F8u;

static uint16_t vga_entry(char c, uint8_t color) {
    return ((uint16_t)color << 8) | (uint8_t)c;
}

static void vga_clear(void) {
    for (uint32_t i = 0; i < VGA_COLUMNS * VGA_ROWS; ++i) {
        VGA[i] = vga_entry(' ', 0x07);
    }
}

static void vga_write(const char *text, uint8_t color) {
    uint32_t i = 0;
    while (text[i] != '\0' && i < VGA_COLUMNS * VGA_ROWS) {
        VGA[i] = vga_entry(text[i], color);
        ++i;
    }
}

static void serial_init(void) {
    COM1[1] = 0x00;
    COM1[3] = 0x80;
    COM1[0] = 0x03;
    COM1[1] = 0x00;
    COM1[3] = 0x03;
    COM1[2] = 0xC7;
    COM1[4] = 0x03;
}

static int serial_ready(void) {
    return (COM1[5] & 0x20u) != 0;
}

static void serial_putc(char c) {
    while (!serial_ready()) {
        __asm__ volatile ("pause");
    }
    COM1[0] = (uint8_t)c;
}

static void serial_write(const char *text) {
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n') {
            serial_putc('\r');
        }
        serial_putc(text[i]);
    }
}

static void serial_hex32(uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    serial_write("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        serial_putc(hex[(value >> shift) & 0xFu]);
    }
}

void kernel_main(uint32_t boot_magic, uint32_t boot_info) {
    serial_init();
    serial_write("ProsenjitOS: early kernel entry\n");
    serial_write("ProsenjitOS: Multiboot2 magic = ");
    serial_hex32(boot_magic);
    serial_write("\n");

    vga_clear();

    if (boot_magic != MULTIBOOT2_BOOT_MAGIC) {
        vga_write("ProsenjitOS: invalid boot handoff", 0x4F);
        serial_write("ProsenjitOS: ERROR invalid Multiboot2 handoff\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    serial_write("ProsenjitOS: Multiboot2 info = ");
    serial_hex32(boot_info);
    serial_write("\n");
    serial_write("ProsenjitOS: x86-64 long mode active\n");

    vga_write("ProsenjitOS 0.1.0 - kernel online", 0x0F);
    serial_write("ProsenjitOS: kernel online\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
