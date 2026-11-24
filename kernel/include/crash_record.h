#ifndef PROSENJIT_CRASH_RECORD_H
#define PROSENJIT_CRASH_RECORD_H

#include <stdint.h>
#include "block.h"

#define PROS_CRASH_RECORD_OK        0
#define PROS_CRASH_RECORD_EINVAL   -1
#define PROS_CRASH_RECORD_EIO      -2
#define PROS_CRASH_RECORD_ENOTSUP  -3
#define PROS_CRASH_RECORD_ENOENT   -4
#define PROS_CRASH_RECORD_ECORRUPT -5
#define PROS_CRASH_RECORD_EOVERFLOW -6

#define PROS_CRASH_RECORD_MAGIC 0x50524352u /* "PRCR" */
#define PROS_CRASH_RECORD_VERSION 1u
#define PROS_CRASH_RECORD_SLOT_COUNT 2u
#define PROS_CRASH_RECORD_DEFAULT_LBA 2046u

#define PROS_CRASH_HEALTH_SOURCE 0x43525348u /* "CRSH" */
#define PROS_CRASH_HEALTH_RECOVERED 1u
#define PROS_CRASH_HEALTH_VECTOR 2u
#define PROS_CRASH_HEALTH_INVALID 3u
#define PROS_CRASH_HEALTH_CAPTURE_FAILED 4u
#define PROS_CRASH_HEALTH_UNAVAILABLE 5u

struct pros_crash_record {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint64_t sequence;
    uint64_t timestamp;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cr2;
    uint32_t checksum;
    uint32_t committed;
};

int pros_crash_record_attach(struct pros_block_device *device,
                             uint64_t first_lba);
int pros_crash_record_available(void);
void pros_crash_record_detach(void);
int pros_crash_record_capture(uint64_t vector, uint64_t error_code,
                              uint64_t rip, uint64_t cr2, uint64_t timestamp);
/* Persist a previously captured in-memory crash record from safe context only. */
int pros_crash_record_flush_pending(void);
int pros_crash_record_peek(struct pros_crash_record *record);
int pros_crash_record_consume(struct pros_crash_record *record);
int pros_crash_record_clear(void);
int pros_crash_record_selftest(void);

#endif
