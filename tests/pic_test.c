#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "pic.h"

struct port_write { uint16_t port; uint8_t value; };
static struct port_write writes[512];
static size_t write_count;

void pros_pic_test_out8(uint16_t port, uint8_t value)
{
    assert(write_count < sizeof(writes) / sizeof(writes[0]));
    writes[write_count++] = (struct port_write){ port, value };
}

static void clear_writes(void) { write_count = 0u; }

static int last_value(uint16_t port, uint8_t *value)
{
    for (size_t i = write_count; i > 0u; --i) {
        if (writes[i - 1u].port == port) {
            *value = writes[i - 1u].value;
            return 1;
        }
    }
    return 0;
}

static void expect_last(uint16_t port, uint8_t expected)
{
    uint8_t value = 0u;
    assert(last_value(port, &value));
    assert(value == expected);
}

int main(void)
{
    assert(pic_init_for_timer() == 0);
    /* Only IRQ0 is enabled; slave IRQs and cascade are initially masked. */
    expect_last(0x21u, 0xFEu);
    expect_last(0xA1u, 0xFFu);

    clear_writes();
    assert(pic_mask_irq(2u) == -1);
    assert(pic_unmask_irq(2u) == -1);
    assert(pic_mask_irq(16u) == -1);
    assert(pic_unmask_irq(16u) == -1);
    assert(write_count == 0u);

    assert(pic_unmask_irq(1u) == 0);
    expect_last(0x21u, 0xFCu);
    assert(pic_mask_irq(1u) == 0);
    expect_last(0x21u, 0xFEu);

    /* A slave source enables the cascade; masking one of multiple enabled
       slave sources must not disable the remaining source. */
    assert(pic_unmask_irq(10u) == 0);
    expect_last(0xA1u, 0xFBu);
    expect_last(0x21u, 0xFAu);
    assert(pic_unmask_irq(12u) == 0);
    expect_last(0xA1u, 0xEBu);
    expect_last(0x21u, 0xFAu);
    assert(pic_mask_irq(10u) == 0);
    expect_last(0xA1u, 0xEFu);
    expect_last(0x21u, 0xFAu);
    assert(pic_mask_irq(12u) == 0);
    expect_last(0xA1u, 0xFFu);
    expect_last(0x21u, 0xFEu);

    clear_writes();
    pic_eoi_irq(12u);
    assert(write_count == 2u);
    assert(writes[0].port == 0xA0u && writes[0].value == 0x20u);
    assert(writes[1].port == 0x20u && writes[1].value == 0x20u);

    clear_writes();
    pic_eoi_irq(1u);
    assert(write_count == 1u);
    assert(writes[0].port == 0x20u && writes[0].value == 0x20u);

    clear_writes();
    pic_eoi_irq(16u);
    assert(write_count == 0u);

    clear_writes();
    pic_eoi_master();
    assert(write_count == 1u);
    assert(writes[0].port == 0x20u && writes[0].value == 0x20u);

    puts("PIC hosted contract tests passed");
    return 0;
}
