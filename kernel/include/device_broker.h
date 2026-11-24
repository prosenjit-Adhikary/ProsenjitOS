#ifndef PROSENJIT_DEVICE_BROKER_H
#define PROSENJIT_DEVICE_BROKER_H

#include <stdint.h>
#include "driver.h"

#define PROS_DEVICE_PERMISSION_READ    0x0001u
#define PROS_DEVICE_PERMISSION_WRITE   0x0002u
#define PROS_DEVICE_PERMISSION_CONTROL 0x0004u
#define PROS_DEVICE_PERMISSION_MAP    0x0008u

#define PROS_DEVICE_BROKER_MAX_GRANTS 64u

struct pros_device_grant {
    uint32_t device_id;
    uint32_t process_id;
    uint32_t permissions;
    uint8_t active;
};

int pros_device_broker_init(void);
/* Drop every grant associated with a device during lifecycle teardown. */
void pros_device_broker_drop_device(uint32_t device_id);
int pros_device_grant(uint32_t device_id, uint32_t process_id,
                      uint32_t permissions);
int pros_device_revoke(uint32_t device_id, uint32_t process_id);
int pros_device_check(uint32_t device_id, uint32_t process_id,
                      uint32_t permission);
int pros_device_broker_selftest(void);

#endif
