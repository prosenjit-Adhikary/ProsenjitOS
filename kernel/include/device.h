#ifndef PROSENJIT_DEVICE_H
#define PROSENJIT_DEVICE_H
#include <stdint.h>
#define PROS_DEVICE_NAME_MAX 32u
enum pros_device_bus { PROS_DEVICE_BUS_UNKNOWN=0, PROS_DEVICE_BUS_PCI, PROS_DEVICE_BUS_USB, PROS_DEVICE_BUS_PLATFORM, PROS_DEVICE_BUS_VIRTUAL };
enum pros_device_class { PROS_DEVICE_CLASS_UNKNOWN=0, PROS_DEVICE_CLASS_STORAGE, PROS_DEVICE_CLASS_NETWORK, PROS_DEVICE_CLASS_DISPLAY, PROS_DEVICE_CLASS_INPUT, PROS_DEVICE_CLASS_OTHER };
struct pros_device_id { enum pros_device_bus bus; uint16_t vendor_id, device_id, subsystem_vendor_id, subsystem_device_id; uint8_t class_code, subclass, prog_if; };
struct pros_device { uint32_t handle; char name[PROS_DEVICE_NAME_MAX]; struct pros_device_id id; enum pros_device_class class; void *driver_data; uint8_t claimed; };
void device_init(struct pros_device *, uint32_t, const char *, const struct pros_device_id *, enum pros_device_class);
int device_matches_id(const struct pros_device *, const struct pros_device_id *);
#endif
