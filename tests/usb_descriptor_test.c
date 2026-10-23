#include <assert.h>
#include <stdint.h>
#include "usb_descriptor.h"
#include "driver.h"

static const uint8_t keyboard_config[] = {
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 2, 0, 1, 3, 1, 1, 0,
    7, 5, 0x81, 3, 8, 0, 10
};

static void copy_config(uint8_t *dst)
{
    for (uint32_t i = 0; i < sizeof(keyboard_config); ++i)
        dst[i] = keyboard_config[i];
}

int main(void)
{
    struct pros_usb_hid_endpoint out = {0};
    assert(pros_usb_find_boot_hid_endpoint(keyboard_config,
           sizeof(keyboard_config), &out) == PROS_DRIVER_OK);
    assert(out.interface_number == 2u && out.alternate_setting == 0u);
    assert(out.endpoint_address == 0x81u && out.max_packet_size == 8u);
    assert(out.interval == 10u && out.kind == PROS_USB_HID_KEYBOARD);

    uint8_t mouse[sizeof(keyboard_config)];
    copy_config(mouse);
    mouse[16] = 2; /* boot mouse protocol */
    mouse[22] = 4; /* endpoint max packet size */
    assert(pros_usb_find_boot_hid_endpoint(mouse, sizeof(mouse), &out) == PROS_DRIVER_OK);
    assert(out.kind == PROS_USB_HID_MOUSE && out.max_packet_size == 4u);

    assert(pros_usb_find_boot_hid_endpoint(0, sizeof(keyboard_config), &out) == PROS_DRIVER_EINVAL);
    assert(pros_usb_find_boot_hid_endpoint(keyboard_config, 8u, &out) == PROS_DRIVER_EINVAL);

    uint8_t bad[sizeof(keyboard_config)];
    copy_config(bad);
    bad[2] = 0xFFu; bad[3] = 0xFFu;
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[0] = 10u; /* standard configuration header length is exactly nine */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[5] = 0u; /* bConfigurationValue must be nonzero */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[7] = 0x60u; /* reserved bmAttributes bit 7 must be set; low bits reserved */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[7] = 0x81u; /* reserved bmAttributes low bit */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[18] = 1u;
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[4] = 2u; /* configuration declares two interfaces but contains one */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[20] = 0x01u; /* endpoint OUT, not interrupt-IN */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_ENODEV);

    copy_config(bad);
    bad[9] = 10u; /* standard interface descriptor length must be exactly nine */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[18] = 8u; /* standard endpoint descriptor length must be exactly seven */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[9] = 1u; /* descriptor shorter than interface minimum */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[13] = 0u; /* interface declares zero endpoints but contains one */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[13] = 2u; /* interface declares two endpoints but contains one */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    uint8_t malformed_out_endpoint[32];
    copy_config(malformed_out_endpoint);
    malformed_out_endpoint[2] = 32u;
    malformed_out_endpoint[13] = 2u;
    malformed_out_endpoint[25] = 7u;
    malformed_out_endpoint[26] = 5u;
    malformed_out_endpoint[27] = 0x02u; /* OUT endpoint, not selected */
    malformed_out_endpoint[28] = 0x43u; /* reserved attribute bits */
    malformed_out_endpoint[29] = 8u;
    malformed_out_endpoint[30] = 0u;
    malformed_out_endpoint[31] = 10u;
    assert(pros_usb_find_boot_hid_endpoint(malformed_out_endpoint,
           sizeof(malformed_out_endpoint), &out) == PROS_DRIVER_EIO);

    uint8_t duplicate_endpoint[32];
    copy_config(duplicate_endpoint);
    duplicate_endpoint[2] = 32u;
    duplicate_endpoint[13] = 2u;
    for (uint32_t i = 0; i < 7u; ++i)
        duplicate_endpoint[25u + i] = duplicate_endpoint[18u + i];
    assert(pros_usb_find_boot_hid_endpoint(duplicate_endpoint,
           sizeof(duplicate_endpoint), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[20] = 0x91u; /* reserved endpoint-address bits */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[20] = 0x80u; /* endpoint zero is not a valid interrupt endpoint */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[21] = 0x43u; /* interrupt endpoint has reserved attribute bits */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[24] = 0u; /* bInterval must be nonzero */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    copy_config(bad);
    bad[23] = 0x08u; /* unsupported high-bandwidth transaction encoding */
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    /* Failure must not partially overwrite the caller's endpoint structure. */
    out.interface_number = 99u;
    out.alternate_setting = 98u;
    out.endpoint_address = 97u;
    out.interval = 96u;
    out.max_packet_size = 95u;
    out.kind = PROS_USB_HID_MOUSE;
    copy_config(bad);
    bad[24] = 0u;
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);
    assert(out.interface_number == 99u && out.alternate_setting == 98u);
    assert(out.endpoint_address == 97u && out.interval == 96u);
    assert(out.max_packet_size == 95u && out.kind == PROS_USB_HID_MOUSE);

    /* A boot keyboard needs its complete eight-byte report in one packet. */
    copy_config(bad);
    bad[22] = 7u;
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);

    /* A boot mouse report needs at least three bytes. */
    copy_config(bad);
    bad[16] = PROS_USB_PROTOCOL_MOUSE;
    bad[22] = 2u;
    assert(pros_usb_find_boot_hid_endpoint(bad, sizeof(bad), &out) == PROS_DRIVER_EIO);
    return 0;
}
