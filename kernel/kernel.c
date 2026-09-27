#include "prosenjit.h"

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;

static uint16_t vga_entry(char c, uint8_t color) {
    return ((uint16_t)color << 8) | (uint8_t)c;
}

void kernel_main(void) {
    const char *message = "ProsenjitOS Kernel v" PROSENJIT_OS_VERSION " - booted";
    for (uint32_t i = 0; message[i] != '\0'; ++i) {
        VGA[i] = vga_entry(message[i], 0x07);
    }
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
