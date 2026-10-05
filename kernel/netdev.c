#include "netdev.h"

static struct pros_netdev devices[PROS_NETDEV_MAX];
static uint32_t next_id = 1u;
static uint8_t ready;

int pros_netdev_init(void)
{
    for (uint32_t i = 0; i < PROS_NETDEV_MAX; ++i)
        devices[i] = (struct pros_netdev){0};
    next_id = 1u;
    ready = 1u;
    return PROS_NETDEV_OK;
}

int pros_netdev_register(struct pros_netdev *device)
{
    if (!ready || !device || !device->name || !device->ops ||
        !device->ops->start || !device->ops->stop ||
        !device->ops->transmit)
        return PROS_NETDEV_EINVAL;
    for (uint32_t i = 0; i < PROS_NETDEV_MAX; ++i) {
        if (!devices[i].registered) {
            uint32_t id = next_id++;
            if (id == 0u) id = next_id++;
            devices[i] = *device;
            devices[i].id = id;
            devices[i].registered = 1u;
            devices[i].running = 0u;
            devices[i].stats = (struct pros_netdev_stats){0};
            device->id = id;
            device->registered = 1u;
            device->running = 0u;
            return PROS_NETDEV_OK;
        }
    }
    return PROS_NETDEV_ENOSPC;
}

int pros_netdev_unregister(uint32_t id)
{
    if (!ready || id == 0u)
        return PROS_NETDEV_EINVAL;
    for (uint32_t i = 0; i < PROS_NETDEV_MAX; ++i)
        if (devices[i].registered && devices[i].id == id) {
            if (devices[i].running) {
                int rc = devices[i].ops->stop(devices[i].ctx);
                if (rc != PROS_NETDEV_OK)
                    return rc;
                devices[i].running = 0u;
            }
            devices[i] = (struct pros_netdev){0};
            return PROS_NETDEV_OK;
        }
    return PROS_NETDEV_ENODEV;
}

const struct pros_netdev *pros_netdev_get(uint32_t id)
{
    if (!ready || id == 0u)
        return 0;
    for (uint32_t i = 0; i < PROS_NETDEV_MAX; ++i)
        if (devices[i].registered && devices[i].id == id)
            return &devices[i];
    return 0;
}

int pros_netdev_start(uint32_t id)
{
    for (uint32_t i = 0; i < PROS_NETDEV_MAX; ++i)
        if (devices[i].registered && devices[i].id == id) {
            if (devices[i].running)
                return PROS_NETDEV_OK;
            int rc = devices[i].ops->start(devices[i].ctx);
            if (rc == PROS_NETDEV_OK)
                devices[i].running = 1u;
            return rc;
        }
    return PROS_NETDEV_ENODEV;
}

int pros_netdev_stop(uint32_t id)
{
    for (uint32_t i = 0; i < PROS_NETDEV_MAX; ++i)
        if (devices[i].registered && devices[i].id == id) {
            if (!devices[i].running)
                return PROS_NETDEV_OK;
            int rc = devices[i].ops->stop(devices[i].ctx);
            if (rc == PROS_NETDEV_OK)
                devices[i].running = 0u;
            return rc;
        }
    return PROS_NETDEV_ENODEV;
}

int pros_netdev_transmit(uint32_t id, const void *frame, uint32_t length)
{
    if (!frame || length == 0u)
        return PROS_NETDEV_EINVAL;
    for (uint32_t i = 0; i < PROS_NETDEV_MAX; ++i)
        if (devices[i].registered && devices[i].id == id) {
            if (!devices[i].running)
                return PROS_NETDEV_EPERM;
            int rc = devices[i].ops->transmit(devices[i].ctx, frame, length);
            if (rc == PROS_NETDEV_OK) {
                ++devices[i].stats.tx_packets;
                devices[i].stats.tx_bytes += length;
            } else {
                ++devices[i].stats.tx_errors;
            }
            return rc;
        }
    return PROS_NETDEV_ENODEV;
}

int pros_netdev_get_stats(uint32_t id, struct pros_netdev_stats *stats)
{
    if (!stats)
        return PROS_NETDEV_EINVAL;
    for (uint32_t i = 0; i < PROS_NETDEV_MAX; ++i)
        if (devices[i].registered && devices[i].id == id) {
            *stats = devices[i].stats;
            return PROS_NETDEV_OK;
        }
    return PROS_NETDEV_ENODEV;
}

static uint32_t test_start_count;
static uint32_t test_stop_count;
static uint32_t test_tx_count;

static int test_start(void *ctx) { (void)ctx; ++test_start_count; return PROS_NETDEV_OK; }
static int test_stop(void *ctx) { (void)ctx; ++test_stop_count; return PROS_NETDEV_OK; }
static int test_transmit(void *ctx, const void *frame, uint32_t length)
{
    (void)ctx; (void)frame;
    if (!length) return PROS_NETDEV_EINVAL;
    ++test_tx_count;
    return PROS_NETDEV_OK;
}

int pros_netdev_selftest(void)
{
    static const struct pros_netdev_ops ops = { test_start, test_stop, test_transmit };
    static const uint8_t frame[64] = {0};
    struct pros_netdev device = { .name = "selftest-net", .ctx = 0, .ops = &ops };
    struct pros_netdev_stats stats;

    test_start_count = test_stop_count = test_tx_count = 0u;
    if (pros_netdev_init() != PROS_NETDEV_OK) return -1;
    if (pros_netdev_register(&device) != PROS_NETDEV_OK || device.id == 0u) return -2;
    if (pros_netdev_transmit(device.id, frame, sizeof(frame)) != PROS_NETDEV_EPERM) return -3;
    if (pros_netdev_start(device.id) != PROS_NETDEV_OK) return -4;
    if (pros_netdev_transmit(device.id, frame, sizeof(frame)) != PROS_NETDEV_OK) return -5;
    if (pros_netdev_get_stats(device.id, &stats) != PROS_NETDEV_OK ||
        stats.tx_packets != 1u || stats.tx_bytes != sizeof(frame)) return -6;
    if (pros_netdev_stop(device.id) != PROS_NETDEV_OK) return -7;
    if (test_start_count != 1u || test_stop_count != 1u || test_tx_count != 1u) return -8;
    if (pros_netdev_unregister(device.id) != PROS_NETDEV_OK) return -9;
    return 0;
}
