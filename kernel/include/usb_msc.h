#ifndef PROSENJIT_USB_MSC_H
#define PROSENJIT_USB_MSC_H
#include <stdint.h>
#include "block.h"
#define PROS_USB_MSC_OK 0
#define PROS_USB_MSC_EINVAL -1
#define PROS_USB_MSC_EUNSUPPORTED -2
#define PROS_USB_MSC_EIO -3
#define PROS_USB_MSC_EBUSY -4
#define PROS_USB_MSC_EOVERFLOW -5
#define PROS_USB_MSC_ECOMMAND -6
#define PROS_USB_MSC_XFER_STALL 1
/* Bulk callbacks report the exact transferred byte count on success or STALL.
 * A zero return requires a complete transfer; STALL may report a partial data
 * phase so BOT can validate CSW residue. EBUSY is reserved for contention
 * before a transfer is posted. clear_halt clears that endpoint.
 * reset_recovery performs BOT class reset and clears both bulk endpoint halts. */
struct pros_usb_msc_transport {
    void *context;
    int (*bulk_out)(void *, uint8_t, const void *, uint32_t, uint32_t *);
    int (*bulk_in)(void *, uint8_t, void *, uint32_t, uint32_t *);
    int (*clear_halt)(void *, uint8_t);
    int (*reset_recovery)(void *, uint8_t, uint8_t, uint8_t);
};
struct pros_usb_msc {
    struct pros_block_device block;
    struct pros_usb_msc_transport transport;
    uint64_t sector_count;
    uint32_t sector_size, next_tag;
    uint8_t interface_number, endpoint_in, endpoint_out, lun;
    uint8_t initialized, read_only, use_rw16, busy, faulted;
    uint32_t io_state; /* high bit closes admission; low bits count active callbacks */
    volatile uint32_t command_lock; /* Fail-fast lock for BOT + REQUEST SENSE. */
    uint8_t last_sense_key, last_asc, last_ascq;
};
int pros_usb_msc_init(struct pros_usb_msc *,
                      const struct pros_usb_msc_transport *,
                      uint8_t, uint8_t, uint8_t, uint8_t);
int pros_usb_msc_detach(struct pros_usb_msc *);
struct pros_block_device *pros_usb_msc_block_device(struct pros_usb_msc *);
int pros_usb_msc_selftest(void);
#endif
