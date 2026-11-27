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

struct pros_ata_io_ops {
    void (*out8)(void *context, uint16_t port, uint8_t value);
    uint8_t (*in8)(void *context, uint16_t port);
    uint16_t (*in16)(void *context, uint16_t port);
    void (*out16)(void *context, uint16_t port, uint16_t value);
};

struct pros_ata_pio {
    struct pros_block_device block;
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t drive;
    uint8_t lba48;
    uint8_t flush_command;
    uint8_t identity_valid; /* IDENTIFY serial/model/firmware passed validation. */
    char identity_serial[21];
    char identity_firmware[9];
    char identity_model[41];
    volatile uint32_t io_lock; /* Fail-fast serialization for ATA task-file I/O. */
    volatile uint32_t io_operation_active; /* Includes callback epilogue; panic path must fail closed. */
    struct pros_ata_trace_event trace[PROS_ATA_TRACE_DEPTH];
    uint32_t trace_count;
    uint32_t trace_head;
    /* NULL selects native x86 port I/O; tests may supply per-device ops. */
    const struct pros_ata_io_ops *io_ops;
    void *io_context;
};

void pros_ata_pio_trace_reset(struct pros_ata_pio *ata);
int pros_ata_pio_trace_find(const struct pros_ata_pio *ata, uint64_t lba,
                            uint8_t write, uint64_t *trace_lba,
                            uint32_t *trace_count);

int pros_ata_pio_probe_at(struct pros_ata_pio *ata, uint16_t io_base,
                          uint16_t ctrl_base, uint8_t drive);
int pros_ata_pio_probe(struct pros_ata_pio *ata);
/* One-sector panic-context write+flush; bounded, fail-fast, no block/health wrapper. */
int pros_ata_pio_panic_write(struct pros_block_device *device, uint64_t lba,
                             const void *sector);
/* One-sector panic-only write+flush; bypasses block/health wrappers. */
int pros_ata_pio_panic_write_flush(struct pros_ata_pio *ata, uint64_t lba,
                                   const void *sector);
int pros_ata_pio_selftest_at(uint16_t io_base, uint16_t ctrl_base,
                             uint8_t drive);
int pros_ata_pio_selftest(void);

#endif
