#ifndef PROSENJIT_ATA_PIO_H
#define PROSENJIT_ATA_PIO_H

#include <stdint.h>
#include "block.h"

#define PROS_ATA_OK        0
#define PROS_ATA_ENODEV   -1
#define PROS_ATA_EINVAL   -2
#define PROS_ATA_EIO      -3
#define PROS_ATA_ETIMEOUT -4
#define PROS_ATA_ENOTSUP  -5

#define PROS_ATA_TRACE_DEPTH 64u

struct pros_ata_trace_event {
    uint64_t lba;
    uint32_t count;
    uint8_t write;
};

struct pros_ata_pio {
    struct pros_block_device block;
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t drive;
    uint8_t lba48;
    struct pros_ata_trace_event trace[PROS_ATA_TRACE_DEPTH];
    uint32_t trace_count;
    uint32_t trace_head;
};

void pros_ata_pio_trace_reset(struct pros_ata_pio *ata);
int pros_ata_pio_trace_find(const struct pros_ata_pio *ata, uint64_t lba,
                            uint8_t write, uint64_t *trace_lba,
                            uint32_t *trace_count);

int pros_ata_pio_probe(struct pros_ata_pio *ata);
int pros_ata_pio_selftest(void);

#endif
