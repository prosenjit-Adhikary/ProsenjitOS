#ifndef PROSENJIT_ATA_PIO_H
#define PROSENJIT_ATA_PIO_H

#include <stdint.h>
#include "block.h"

#define PROS_ATA_OK       0
#define PROS_ATA_EINVAL  -1
#define PROS_ATA_EIO     -2
#define PROS_ATA_ENODEV  -3
#define PROS_ATA_EROFS   -4
#define PROS_ATA_EOVERFLOW -5

struct pros_ata_pio {
    struct pros_block_device device;
    uint16_t io_base;
    uint16_t control_base;
    uint8_t drive;
};

int ata_pio_init(struct pros_ata_pio *, uint8_t channel, uint8_t drive);

#endif
