#include "pic.h"

static inline void pic_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t pic_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void pic_io_wait(void) {
    pic_out8(0x80, 0);
}

int pic_init_for_timer(void) {
    /*
     * Remap the 8259 PIC so hardware IRQ vectors do not overlap CPU
     * exceptions. Keep every IRQ masked except IRQ0 until a driver enables it.
     */
    uint8_t master_mask = pic_in8(0x21);
    uint8_t slave_mask = pic_in8(0xA1);

    pic_out8(0x20, 0x11);
    pic_io_wait();
    pic_out8(0xA0, 0x11);
    pic_io_wait();

    pic_out8(0x21, 0x20);
    pic_io_wait();
    pic_out8(0xA1, 0x28);
    pic_io_wait();

    pic_out8(0x21, 0x04);
    pic_io_wait();
    pic_out8(0xA1, 0x02);
    pic_io_wait();

    pic_out8(0x21, 0x01);
    pic_io_wait();
    pic_out8(0xA1, 0x01);
    pic_io_wait();

    /* IRQ0 enabled; all other legacy IRQs remain masked. */
    pic_out8(0x21, (uint8_t)(master_mask & 0xFEu));
    pic_out8(0xA1, 0xFFu);
    return 0;
}

void pic_eoi_master(void) {
    pic_out8(0x20, 0x20);
}
