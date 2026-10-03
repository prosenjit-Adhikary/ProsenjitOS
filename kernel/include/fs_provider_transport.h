#ifndef PROSENJIT_FS_PROVIDER_TRANSPORT_H
#define PROSENJIT_FS_PROVIDER_TRANSPORT_H

#include <stdint.h>
#include "block.h"
#include "object.h"

#define PROS_FS_TRANSPORT_MAX_BUFFERS 8u
#define PROS_FS_TRANSPORT_BUFFER_SIZE 4096u

#define PROS_FS_TRANSPORT_OK         0
#define PROS_FS_TRANSPORT_EINVAL    -1
#define PROS_FS_TRANSPORT_EBUSY     -2
#define PROS_FS_TRANSPORT_EOVERFLOW -3
#define PROS_FS_TRANSPORT_EPERM     -4

#define PROS_FS_TRANSPORT_BUFFER_READ  (1u << 0)
#define PROS_FS_TRANSPORT_BUFFER_WRITE (1u << 1)

struct pros_fs_transport_buffer {
    uint8_t bytes[PROS_FS_TRANSPORT_BUFFER_SIZE];
    uint32_t length;
    uint32_t flags;
    uint16_t generation;
    uint8_t in_use;
    uint8_t reserved;
};

struct pros_fs_transport {
    struct pros_object object;
    struct pros_block_device *device;
    uint64_t device_ref;
    struct pros_fs_transport_buffer buffers[PROS_FS_TRANSPORT_MAX_BUFFERS];
};

void pros_fs_transport_init(struct pros_fs_transport *,
                            struct pros_block_device *);
int pros_fs_transport_valid(const struct pros_fs_transport *);
uint64_t pros_fs_transport_device_ref(const struct pros_fs_transport *);
int pros_fs_transport_check_device_ref(const struct pros_fs_transport *,
                                       uint64_t);
uint64_t pros_fs_transport_buffer_alloc(struct pros_fs_transport *,
                                        uint32_t flags);
int pros_fs_transport_buffer_release(struct pros_fs_transport *, uint64_t);
int pros_fs_transport_buffer_write(struct pros_fs_transport *, uint64_t,
                                   uint32_t offset, const void *, uint32_t);
int pros_fs_transport_buffer_read(const struct pros_fs_transport *, uint64_t,
                                  uint32_t offset, void *, uint32_t);
int pros_fs_transport_buffer_bounds(const struct pros_fs_transport *, uint64_t,
                                    uint32_t offset, uint32_t length);
int pros_fs_transport_read(struct pros_fs_transport *, uint64_t buffer_ref,
                           uint64_t lba, uint32_t sectors);
int pros_fs_transport_write(struct pros_fs_transport *, uint64_t buffer_ref,
                            uint64_t lba, uint32_t sectors);
int pros_fs_transport_selftest(void);

#endif
