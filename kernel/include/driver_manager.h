#ifndef PROSENJIT_DRIVER_MANAGER_H
#define PROSENJIT_DRIVER_MANAGER_H
#include <stdint.h>
#include "driver.h"
#define PROS_DRIVER_REGISTRY_MAX 32u
#define PROS_DEVICE_REGISTRY_MAX 64u
struct pros_driver_manager { struct pros_driver *drivers[PROS_DRIVER_REGISTRY_MAX]; uint32_t driver_count; struct pros_device *devices[PROS_DEVICE_REGISTRY_MAX]; uint32_t device_count; };
void driver_manager_init(struct pros_driver_manager *);
int driver_manager_register_driver(struct pros_driver_manager *,struct pros_driver *);
int driver_manager_register_device(struct pros_driver_manager *,struct pros_device *);
int driver_manager_probe_device(struct pros_driver_manager *,struct pros_device *);
int driver_manager_probe_all(struct pros_driver_manager *);
int driver_manager_remove_device(struct pros_driver_manager *,struct pros_device *);
int driver_manager_selftest(void);
#endif
