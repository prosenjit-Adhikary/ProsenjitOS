#ifndef PROSENJIT_PIC_H
#define PROSENJIT_PIC_H

#include <stdint.h>

/* Remap legacy 8259 PICs and leave only the PIT timer IRQ enabled. */
int pic_init_for_timer(void);

/* Mask/unmask a device IRQ (0..15). IRQ2 is reserved for the PIC cascade. */
int pic_mask_irq(uint8_t irq);
int pic_unmask_irq(uint8_t irq);

/* Acknowledge IRQ0-IRQ7 on the master PIC. */
void pic_eoi_master(void);

/* Acknowledge a specific legacy IRQ; slave IRQs receive slave then master EOI. */
void pic_eoi_irq(uint8_t irq);

#endif
