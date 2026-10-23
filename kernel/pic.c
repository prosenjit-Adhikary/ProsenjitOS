#include "pic.h"

#define PIC_MASTER_COMMAND 0x20u
#define PIC_MASTER_DATA    0x21u
#define PIC_SLAVE_COMMAND  0xA0u
#define PIC_SLAVE_DATA     0xA1u
#define PIC_EOI            0x20u
#define PIC_CASCADE_IRQ    2u

static uint8_t master_mask = 0xFFu;
static uint8_t slave_mask = 0xFFu;

static inline void pic_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void pic_io_wait(void) {
    pic_out8(0x80u, 0u);
}

static void pic_write_masks(void) {
    pic_out8(PIC_MASTER_DATA, master_mask);
    pic_out8(PIC_SLAVE_DATA, slave_mask);
}

int pic_init_for_timer(void) {
    /*
     * Remap the 8259 PIC so hardware IRQ vectors do not overlap CPU
     * exceptions. Keep every IRQ masked except IRQ0 until a driver enables it.
     */
    pic_out8(PIC_MASTER_COMMAND, 0x11u);
    pic_io_wait();
    pic_out8(PIC_SLAVE_COMMAND, 0x11u);
    pic_io_wait();

    pic_out8(PIC_MASTER_DATA, 0x20u);
    pic_io_wait();
    pic_out8(PIC_SLAVE_DATA, 0x28u);
    pic_io_wait();

    pic_out8(PIC_MASTER_DATA, 0x04u);
    pic_io_wait();
    pic_out8(PIC_SLAVE_DATA, 0x02u);
    pic_io_wait();

    pic_out8(PIC_MASTER_DATA, 0x01u);
    pic_io_wait();
    pic_out8(PIC_SLAVE_DATA, 0x01u);
    pic_io_wait();

    master_mask = 0xFEu; /* IRQ0 only; cascade IRQ2 stays masked until a slave IRQ is enabled. */
    slave_mask = 0xFFu;
    pic_write_masks();
    return 0;
}

int pic_mask_irq(uint8_t irq) {
    if (irq > 15u || irq == PIC_CASCADE_IRQ)
        return -1;

    if (irq < 8u) {
        master_mask = (uint8_t)(master_mask | (uint8_t)(1u << irq));
    } else {
        slave_mask = (uint8_t)(slave_mask | (uint8_t)(1u << (irq - 8u)));
        if (slave_mask == 0xFFu)
            master_mask = (uint8_t)(master_mask | (uint8_t)(1u << PIC_CASCADE_IRQ));
    }
    pic_write_masks();
    return 0;
}

int pic_unmask_irq(uint8_t irq) {
    if (irq > 15u || irq == PIC_CASCADE_IRQ)
        return -1;

    if (irq < 8u) {
        master_mask = (uint8_t)(master_mask & (uint8_t)~(1u << irq));
    } else {
        slave_mask = (uint8_t)(slave_mask & (uint8_t)~(1u << (irq - 8u)));
        /* Any enabled slave source requires the master cascade line. */
        master_mask = (uint8_t)(master_mask & (uint8_t)~(1u << PIC_CASCADE_IRQ));
    }
    pic_write_masks();
    return 0;
}

void pic_eoi_master(void) {
    pic_out8(PIC_MASTER_COMMAND, PIC_EOI);
}

void pic_eoi_irq(uint8_t irq) {
    if (irq > 15u)
        return;
    /* Slave PIC must be acknowledged before the master cascade IRQ. */
    if (irq >= 8u)
        pic_out8(PIC_SLAVE_COMMAND, PIC_EOI);
    pic_out8(PIC_MASTER_COMMAND, PIC_EOI);
}
