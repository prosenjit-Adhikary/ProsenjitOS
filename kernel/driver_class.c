#include "driver_class.h"

int pros_driver_class_validate(enum pros_device_class class_id)
{
    return class_id <= PROS_DEVICE_CLASS_AUDIO ? PROS_DRIVER_OK
                                               : PROS_DRIVER_EINVAL;
}

int pros_driver_class_has_contract(enum pros_device_class class_id)
{
    switch (class_id) {
    case PROS_DEVICE_CLASS_INPUT:
    case PROS_DEVICE_CLASS_DISPLAY:
    case PROS_DEVICE_CLASS_NETWORK:
        return PROS_DRIVER_OK;
    case PROS_DEVICE_CLASS_STORAGE:
    case PROS_DEVICE_CLASS_SERIAL:
    case PROS_DEVICE_CLASS_USB:
    case PROS_DEVICE_CLASS_AUDIO:
        return PROS_DRIVER_ENOTSUP;
    case PROS_DEVICE_CLASS_OTHER:
    default:
        return PROS_DRIVER_EINVAL;
    }
}
