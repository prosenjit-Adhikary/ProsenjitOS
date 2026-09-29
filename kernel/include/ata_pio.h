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

struct pros_ata_pio {
    struct pros_block_device block;
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t drive;
    uint8_t lba48;
};

int pros_ata_pio_probe(struct pros_ata_pio *ata);
int pros_ata_pio_selftest(void);

#endif
