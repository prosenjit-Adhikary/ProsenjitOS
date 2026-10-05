#ifndef PROSENJIT_NETDEV_H
#define PROSENJIT_NETDEV_H

#include <stdint.h>

#define PROS_NETDEV_OK       0
#define PROS_NETDEV_EINVAL  -1
#define PROS_NETDEV_ENOSPC  -2
#define PROS_NETDEV_ENODEV  -3
#define PROS_NETDEV_EPERM   -4

#define PROS_NETDEV_MAX 8u
#define PROS_NETDEV_NAME_MAX 31u

struct pros_netdev_stats {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
};

struct pros_netdev_ops {
    int (*start)(void *ctx);
    int (*stop)(void *ctx);
    int (*transmit)(void *ctx, const void *frame, uint32_t length);
};

struct pros_netdev {
    uint32_t id;
    const char *name;
    void *ctx;
    const struct pros_netdev_ops *ops;
    struct pros_netdev_stats stats;
    uint8_t registered;
    uint8_t running;
};

int pros_netdev_init(void);
int pros_netdev_register(struct pros_netdev *);
int pros_netdev_unregister(uint32_t id);
const struct pros_netdev *pros_netdev_get(uint32_t id);
int pros_netdev_start(uint32_t id);
int pros_netdev_stop(uint32_t id);
int pros_netdev_transmit(uint32_t id, const void *frame, uint32_t length);
int pros_netdev_get_stats(uint32_t id, struct pros_netdev_stats *);
int pros_netdev_selftest(void);

#endif
