#include <assert.h>
#include <stdint.h>
#include "usb_descriptor.h"
#include "driver.h"

static const uint8_t keyboard_config[] = {
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 2, 0, 1, 3, 1, 1, 0,
    7, 5, 0x81, 3, 8, 0, 10
};

int main(void)
{
    struct pros_usb_hid_endpoint out = {0};
    assert(pros_usb_find_boot_hid_endpoint(keyboard_config,
           sizeof(keyboard_config), &out) == PROS_DRIVER_OK);
    assert(out.interface_number == 2u && out.alternate_setting == 0u);
    assert(out.endpoint_address == 0x81u && out.max_packet_size == 8u);
    assert(out.interval == 10u && out.kind == PROS_USB_HID_KEYBOARD);

    uint8_t mouse[sizeof(keyboard_config)];
    for (uint32_t i = 0; i < sizeof(mouse); ++i) mouse[i] = keyboard_config[i];
    mouse[16] = 2; /* boot mouse protocol */
    mouse[22] = 4; /* endpoint max packet size */
    assert(pros_usb_find_boot_hid_endpoint(mouse, sizeof(mouse), &out) == PROS_DRIVER_OK);
    assert(out.kind == PROS_USB_HID_MOUSE && out.max_packet_size == 4u);

    assert(pros_usb_find_boot_hid_endpoint(0, sizeof(keyboard_config), &out) == PROS_DRIVER_EINVAL);
    assert(pros_usb_find_boot_hid_endpoint(keyboard_config, 8u, &out) == PROS_DRIVER_EINVAL);

    uint8_t bad[sizeof(keyboard_config)];
    for (uint32_t i = 0; i < sizeof(bad); ++i) bad[i] = keyboard_config[i];
    bad[2] = 0xFFu; bad[3] = 0xFFu;
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);
    for (uint32_t i = 0; i < sizeof(bad); ++i) bad[i] = keyboard_config[i];
    bad[18] = 1u;
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);
    for (uint32_t i = 0; i < sizeof(bad); ++i) bad[i] = keyboard_config[i];
    bad[20] = 0x01u; /* endpoint OUT, not interrupt-IN */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_ENODEV);
    for (uint32_t i = 0; i < sizeof(bad); ++i) bad[i] = keyboard_config[i];
    bad[9] = 1u; /* descriptor shorter than interface minimum */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);
    return 0;
}
