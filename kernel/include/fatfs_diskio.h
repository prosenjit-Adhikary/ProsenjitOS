#ifndef PROSENJIT_FATFS_DISKIO_H
#define PROSENJIT_FATFS_DISKIO_H

#include <stdint.h>
#include "block.h"

#define PROS_FATFS_DISK_MAX 8u

/*
 * Bind a FatFs physical-drive number to a partition-local block device.
 * The caller must keep the block device alive until unbind succeeds and must
 * unmount the corresponding FatFs work area before unbinding it.
 */
int pros_fatfs_disk_bind(uint8_t pdrv, struct pros_block_device *device,
                         uint8_t read_only);
int pros_fatfs_disk_unbind(uint8_t pdrv);
int pros_fatfs_diskio_selftest(void);

#endif
