#ifndef PROSENJIT_DRIVER_H
#define PROSENJIT_DRIVER_H
#include <stdint.h>
#include "device.h"
#define PROS_DRIVER_NAME_MAX 32u
#define PROS_DRIVER_MAX_IDS 8u
struct pros_driver;
typedef int (*pros_driver_probe_fn)(struct pros_driver *,struct pros_device *);
typedef int (*pros_driver_remove_fn)(struct pros_driver *,struct pros_device *);
struct pros_driver { char name[PROS_DRIVER_NAME_MAX]; enum pros_device_bus bus; const struct pros_device_id *ids; uint32_t id_count; pros_driver_probe_fn probe; pros_driver_remove_fn remove; uint8_t registered; };
int driver_init(struct pros_driver *,const char *,enum pros_device_bus,const struct pros_device_id *,uint32_t,pros_driver_probe_fn,pros_driver_remove_fn);
int driver_supports_device(const struct pros_driver *,const struct pros_device *);
int driver_bind(struct pros_driver *,struct pros_device *);
int driver_unbind(struct pros_driver *,struct pros_device *);
#endif
