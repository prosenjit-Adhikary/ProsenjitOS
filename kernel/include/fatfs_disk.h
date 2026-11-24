#ifndef PROSENJIT_FATFS_DISK_H
#define PROSENJIT_FATFS_DISK_H

#include <stdint.h>
#include "block.h"

#define PROS_FATFS_DISK_OK       0
#define PROS_FATFS_DISK_EINVAL -1
#define PROS_FATFS_DISK_EBUSY  -2
#define PROS_FATFS_DISK_ENODEV -3

/*
 * Bind one FatFs physical drive to a device-local block address space.
 * For partitioned media, pass &volume.device.device, never the parent disk.
 */
int pros_fatfs_disk_bind(uint8_t pdrv, struct pros_block_device *device,
                         uint8_t read_only);
int pros_fatfs_disk_unbind(uint8_t pdrv);
/* Atomically quarantine or restore writes on an already-bound drive. */
int pros_fatfs_disk_set_read_only(uint8_t pdrv, uint8_t read_only);
int pros_fatfs_disk_is_bound(uint8_t pdrv);
/* Internal fault-injection diagnostic: op/stage/driver result, packed. */
uint32_t pros_fatfs_disk_test_last_status(void);
void pros_fatfs_disk_test_reset_status(void);
int pros_fatfs_disk_selftest(void);

#endif
