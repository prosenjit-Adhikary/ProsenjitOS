#include "fs_provider_transport.h"

#define TRANSPORT_REF_SHIFT 16u
#define TRANSPORT_SLOT_MASK 0xFFFFu
#define TRANSPORT_REF_TAG 0x4653ULL
static uint64_t next_device_ref = 1u;

static uint64_t make_ref(uint32_t slot, uint16_t generation)
{
    return (TRANSPORT_REF_TAG << 48) |
           ((uint64_t)generation << TRANSPORT_REF_SHIFT) |
           (uint64_t)slot;
}

static uint32_t ref_slot(uint64_t ref)
{
    return (uint32_t)(ref & TRANSPORT_SLOT_MASK);
}

static uint16_t ref_generation(uint64_t ref)
{
    return (uint16_t)((ref >> TRANSPORT_REF_SHIFT) & 0xFFFFu);
}

static int ref_valid(uint64_t ref)
{
    return (ref >> 48) == TRANSPORT_REF_TAG &&
           ref_slot(ref) < PROS_FS_TRANSPORT_MAX_BUFFERS &&
           ref_generation(ref) != 0u;
}

void pros_fs_transport_init(struct pros_fs_transport *transport,
                            struct pros_block_device *device)
{
    if (!transport) return;
    object_init(&transport->object, PROS_OBJECT_DEVICE);
    transport->device = device;
    /* Device references must never wrap into an earlier live reference. */
    if (next_device_ref == 0u || next_device_ref == UINT64_MAX) {
        transport->device_ref = 0u;
        return;
    }
    transport->device_ref = next_device_ref++;
    for (uint32_t i = 0u; i < PROS_FS_TRANSPORT_MAX_BUFFERS; ++i) {
        transport->buffers[i].length = 0u;
        transport->buffers[i].flags = 0u;
        transport->buffers[i].generation = 1u;
        transport->buffers[i].in_use = 0u;
        transport->buffers[i].reserved = 0u;
    }
}

int pros_fs_transport_valid(const struct pros_fs_transport *transport)
{
    return transport && transport->device &&
           transport->device->sector_size != 0u &&
           transport->device->sector_count != 0u &&
           transport->device->read != 0 &&
           transport->object.type == PROS_OBJECT_DEVICE &&
           transport->object.refcount != 0u &&
           transport->device_ref != 0u;
}

uint64_t pros_fs_transport_device_ref(const struct pros_fs_transport *transport)
{
    if (!pros_fs_transport_valid(transport)) return 0u;
    return transport->device_ref;
}

int pros_fs_transport_check_device_ref(const struct pros_fs_transport *transport,
                                       uint64_t ref)
{
    if (!pros_fs_transport_valid(transport) || ref == 0u) return PROS_FS_TRANSPORT_EINVAL;
    return ref == pros_fs_transport_device_ref(transport) ?
           PROS_FS_TRANSPORT_OK : PROS_FS_TRANSPORT_EPERM;
}

uint64_t pros_fs_transport_buffer_alloc(struct pros_fs_transport *transport,
                                        uint32_t flags)
{
    if (!pros_fs_transport_valid(transport) ||
        flags == 0u ||
        (flags & ~(PROS_FS_TRANSPORT_BUFFER_READ |
                   PROS_FS_TRANSPORT_BUFFER_WRITE)) != 0u)
        return 0u;

    for (uint32_t i = 0u; i < PROS_FS_TRANSPORT_MAX_BUFFERS; ++i) {
        struct pros_fs_transport_buffer *buffer = &transport->buffers[i];
        if (!buffer->in_use) {
            buffer->in_use = 1u;
            buffer->flags = flags;
            buffer->length = 0u;
            return make_ref(i, buffer->generation);
        }
    }
    return 0u;
}

int pros_fs_transport_buffer_release(struct pros_fs_transport *transport,
                                      uint64_t ref)
{
    if (!pros_fs_transport_valid(transport) || !ref_valid(ref))
        return PROS_FS_TRANSPORT_EINVAL;
    uint32_t slot = ref_slot(ref);
    struct pros_fs_transport_buffer *buffer = &transport->buffers[slot];
    if (!buffer->in_use || buffer->generation != ref_generation(ref))
        return PROS_FS_TRANSPORT_EINVAL;
    buffer->in_use = 0u;
    buffer->length = 0u;
    buffer->flags = 0u;
    ++buffer->generation;
    if (buffer->generation == 0u) buffer->generation = 1u;
    return PROS_FS_TRANSPORT_OK;
}

int pros_fs_transport_buffer_bounds(const struct pros_fs_transport *transport,
                                    uint64_t ref, uint32_t offset,
                                    uint32_t length)
{
    if (!pros_fs_transport_valid(transport) || !ref_valid(ref))
        return PROS_FS_TRANSPORT_EINVAL;
    uint32_t slot = ref_slot(ref);
    const struct pros_fs_transport_buffer *buffer = &transport->buffers[slot];
    if (!buffer->in_use || buffer->generation != ref_generation(ref))
        return PROS_FS_TRANSPORT_EINVAL;
    if (offset > PROS_FS_TRANSPORT_BUFFER_SIZE ||
        length > PROS_FS_TRANSPORT_BUFFER_SIZE - offset)
        return PROS_FS_TRANSPORT_EOVERFLOW;
    return PROS_FS_TRANSPORT_OK;
}

int pros_fs_transport_buffer_write(struct pros_fs_transport *transport,
                                   uint64_t ref, uint32_t offset,
                                   const void *src, uint32_t length)
{
    if (!src)
        return PROS_FS_TRANSPORT_EINVAL;
    int bounds_rc = pros_fs_transport_buffer_bounds(transport, ref, offset, length);
    if (bounds_rc != PROS_FS_TRANSPORT_OK)
        return bounds_rc;
    uint32_t slot = ref_slot(ref);
    struct pros_fs_transport_buffer *buffer = &transport->buffers[slot];
    if ((buffer->flags & PROS_FS_TRANSPORT_BUFFER_WRITE) == 0u)
        return PROS_FS_TRANSPORT_EPERM;
    for (uint32_t i = 0u; i < length; ++i)
        buffer->bytes[offset + i] = ((const uint8_t *)src)[i];
    if (offset + length > buffer->length) buffer->length = offset + length;
    return PROS_FS_TRANSPORT_OK;
}

int pros_fs_transport_buffer_read(const struct pros_fs_transport *transport,
                                  uint64_t ref, uint32_t offset,
                                  void *dst, uint32_t length)
{
    if (!dst)
        return PROS_FS_TRANSPORT_EINVAL;
    int bounds_rc = pros_fs_transport_buffer_bounds(transport, ref, offset, length);
    if (bounds_rc != PROS_FS_TRANSPORT_OK)
        return bounds_rc;
    uint32_t slot = ref_slot(ref);
    const struct pros_fs_transport_buffer *buffer = &transport->buffers[slot];
    if ((buffer->flags & PROS_FS_TRANSPORT_BUFFER_READ) == 0u)
        return PROS_FS_TRANSPORT_EPERM;
    if (offset > buffer->length || length > buffer->length - offset)
        return PROS_FS_TRANSPORT_EOVERFLOW;
    for (uint32_t i = 0u; i < length; ++i)
        ((uint8_t *)dst)[i] = buffer->bytes[offset + i];
    return PROS_FS_TRANSPORT_OK;
}

int pros_fs_transport_read(struct pros_fs_transport *transport,
                           uint64_t buffer_ref, uint64_t lba,
                           uint32_t sectors)
{
    if (!pros_fs_transport_valid(transport) || !ref_valid(buffer_ref) ||
        sectors == 0u || sectors > PROS_FS_TRANSPORT_BUFFER_SIZE /
                              transport->device->sector_size)
        return PROS_FS_TRANSPORT_EINVAL;
    if (pros_fs_transport_buffer_bounds(transport, buffer_ref, 0u,
                                        sectors * transport->device->sector_size) !=
            PROS_FS_TRANSPORT_OK)
        return PROS_FS_TRANSPORT_EOVERFLOW;
    uint32_t slot = ref_slot(buffer_ref);
    struct pros_fs_transport_buffer *buffer = &transport->buffers[slot];
    if (!buffer->in_use || buffer->generation != ref_generation(buffer_ref) ||
        (buffer->flags & PROS_FS_TRANSPORT_BUFFER_READ) == 0u)
        return PROS_FS_TRANSPORT_EPERM;
    if (block_device_read(transport->device, lba, sectors, buffer->bytes) !=
            PROS_BLOCK_OK)
        return PROS_FS_TRANSPORT_EIO;
    buffer->length = sectors * transport->device->sector_size;
    return PROS_FS_TRANSPORT_OK;
}

int pros_fs_transport_write(struct pros_fs_transport *transport,
                            uint64_t buffer_ref, uint64_t lba,
                            uint32_t sectors)
{
    if (!pros_fs_transport_valid(transport) || !ref_valid(buffer_ref) ||
        sectors == 0u || sectors > PROS_FS_TRANSPORT_BUFFER_SIZE /
                              transport->device->sector_size)
        return PROS_FS_TRANSPORT_EINVAL;
    uint32_t bytes = sectors * transport->device->sector_size;
    if (pros_fs_transport_buffer_bounds(transport, buffer_ref, 0u, bytes) !=
            PROS_FS_TRANSPORT_OK)
        return PROS_FS_TRANSPORT_EOVERFLOW;
    uint32_t slot = ref_slot(buffer_ref);
    struct pros_fs_transport_buffer *buffer = &transport->buffers[slot];
    if (!buffer->in_use || buffer->generation != ref_generation(buffer_ref) ||
        (buffer->flags & PROS_FS_TRANSPORT_BUFFER_WRITE) == 0u ||
        buffer->length < bytes)
        return PROS_FS_TRANSPORT_EPERM;
    return block_device_write(transport->device, lba, sectors, buffer->bytes) ==
           PROS_BLOCK_OK ? PROS_FS_TRANSPORT_OK : PROS_FS_TRANSPORT_EIO;
}

struct transport_test_disk {
    uint8_t sectors[2u * 512u];
};

static int transport_test_read(struct pros_block_device *device,
                               uint64_t lba, uint32_t count, void *buffer)
{
    struct transport_test_disk *disk = device ?
        (struct transport_test_disk *)device->private_data : 0;
    if (!disk || !buffer || lba >= 2u || count == 0u || count > 2u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t i = 0u; i < count * 512u; ++i)
        ((uint8_t *)buffer)[i] = disk->sectors[lba * 512u + i];
    return PROS_BLOCK_OK;
}

static int transport_test_write(struct pros_block_device *device,
                                uint64_t lba, uint32_t count,
                                const void *buffer)
{
    struct transport_test_disk *disk = device ?
        (struct transport_test_disk *)device->private_data : 0;
    if (!disk || !buffer || lba >= 2u || count == 0u || count > 2u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t i = 0u; i < count * 512u; ++i)
        disk->sectors[lba * 512u + i] = ((const uint8_t *)buffer)[i];
    return PROS_BLOCK_OK;
}

int pros_fs_transport_selftest(void)
{
    struct transport_test_disk disk = {0};
    struct pros_block_device device;
    struct pros_fs_transport transport;
    uint8_t src[16];
    uint8_t dst[16] = {0};
    uint64_t ref;

    for (uint32_t i = 0u; i < sizeof(src); ++i)
        src[i] = (uint8_t)(0xA0u + i);

    if (block_device_init(&device, 512u, 2u, transport_test_read,
                          transport_test_write, &disk) != PROS_BLOCK_OK)
        return -1;

    pros_fs_transport_init(&transport, &device);
    if (!pros_fs_transport_valid(&transport) ||
        pros_fs_transport_device_ref(&transport) == 0u ||
        pros_fs_transport_check_device_ref(
            &transport, pros_fs_transport_device_ref(&transport)) !=
            PROS_FS_TRANSPORT_OK)
        return -2;

    ref = pros_fs_transport_buffer_alloc(
        &transport, PROS_FS_TRANSPORT_BUFFER_READ |
                    PROS_FS_TRANSPORT_BUFFER_WRITE);
    if (ref == 0u ||
        pros_fs_transport_buffer_write(&transport, ref, 0u, src,
                                       sizeof(src)) != PROS_FS_TRANSPORT_OK)
        return -3;

    if (pros_fs_transport_write(&transport, ref, 0u, 1u) !=
            PROS_FS_TRANSPORT_OK)
        return -4;

    disk.sectors[0] = 0u;
    if (pros_fs_transport_read(&transport, ref, 0u, 1u) !=
            PROS_FS_TRANSPORT_OK ||
        pros_fs_transport_buffer_read(&transport, ref, 0u, dst,
                                      sizeof(dst)) != PROS_FS_TRANSPORT_OK)
        return -5;

    for (uint32_t i = 0u; i < sizeof(dst); ++i)
        if (dst[i] != src[i]) return -6;

    if (pros_fs_transport_buffer_bounds(&transport, ref,
                                        PROS_FS_TRANSPORT_BUFFER_SIZE - 1u,
                                        2u) != PROS_FS_TRANSPORT_EOVERFLOW ||
        pros_fs_transport_buffer_write(&transport, ref,
                                       PROS_FS_TRANSPORT_BUFFER_SIZE - 1u,
                                       src, 2u) != PROS_FS_TRANSPORT_EOVERFLOW ||
        pros_fs_transport_buffer_read(&transport, ref,
                                      PROS_FS_TRANSPORT_BUFFER_SIZE - 1u,
                                      dst, 2u) != PROS_FS_TRANSPORT_EOVERFLOW ||
        pros_fs_transport_check_device_ref(&transport,
                                           pros_fs_transport_device_ref(&transport) + 1u) !=
            PROS_FS_TRANSPORT_EPERM)
        return -7;

    if (pros_fs_transport_buffer_release(&transport, ref) !=
            PROS_FS_TRANSPORT_OK ||
        pros_fs_transport_buffer_read(&transport, ref, 0u, dst,
                                      sizeof(dst)) != PROS_FS_TRANSPORT_EINVAL)
        return -8;

    return PROS_FS_TRANSPORT_OK;
}
