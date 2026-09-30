#ifndef PROSENJIT_NETWORK_H
#define PROSENJIT_NETWORK_H
#include <stdint.h>
#define PROS_NET_OK 0
#define PROS_NET_EINVAL -1
#define PROS_NET_EIO -2
#define PROS_NET_MAX_FRAME 1536u
struct pros_net_device;
typedef int (*pros_net_rx_fn)(struct pros_net_device *, const uint8_t *, uint32_t);
typedef int (*pros_net_tx_fn)(struct pros_net_device *, const uint8_t *, uint32_t);
struct pros_net_device {
    const char *name;
    uint8_t mac[6];
    uint32_t mtu;
    uint8_t up;
    pros_net_rx_fn receive;
    pros_net_tx_fn transmit;
    void *private_data;
};
int pros_net_device_init(struct pros_net_device *, const char *, const uint8_t[6], uint32_t, pros_net_rx_fn, pros_net_tx_fn, void *);
int pros_net_set_up(struct pros_net_device *, uint8_t);
int pros_net_ethernet_validate(const uint8_t *, uint32_t);
uint16_t pros_net_ipv4_checksum(const void *, uint32_t);
int pros_net_selftest(void);
#endif
