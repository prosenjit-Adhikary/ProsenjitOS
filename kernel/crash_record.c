#include "crash_record.h"

#include <stddef.h>

static struct pros_block_device *crash_device;
static uint64_t crash_first_lba;
static uint64_t crash_sequence;
static uint32_t crash_next_slot;
static uint8_t crash_attached;
static uint8_t crash_capture_active;
static struct pros_crash_record pending_record;
static uint8_t pending_valid;
static uint8_t pending_flush_required;

static void crash_zero(void *buffer, uint32_t size)
{
    uint8_t *p = (uint8_t *)buffer;
    for (uint32_t i = 0u; i < size; ++i)
        p[i] = 0u;
}

static uint32_t crash_crc32c(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < length; ++i) {
        uint8_t value = data[i];
        /* checksum and committed are excluded from the integrity calculation. */
        if (i >= offsetof(struct pros_crash_record, checksum) &&
            i < offsetof(struct pros_crash_record, checksum) + sizeof(uint32_t))
            value = 0u;
        if (i >= offsetof(struct pros_crash_record, committed) &&
            i < offsetof(struct pros_crash_record, committed) + sizeof(uint32_t))
            value = 0u;
        crc ^= value;
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
    return ~crc;
}

static int crash_record_empty(const struct pros_crash_record *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    for (uint32_t i = 0u; i < sizeof(*record); ++i)
        if (bytes[i] != 0u)
            return 0;
    return 1;
}

static int crash_record_valid(const struct pros_crash_record *record)
{
    if (!record ||
        record->magic != PROS_CRASH_RECORD_MAGIC ||
        record->version != PROS_CRASH_RECORD_VERSION ||
        record->size != sizeof(struct pros_crash_record) ||
        record->sequence == 0u ||
        record->committed != 1u)
        return 0;
    return crash_crc32c((const uint8_t *)record,
                        sizeof(struct pros_crash_record)) == record->checksum;
}

static int crash_slot_read(uint32_t slot, struct pros_crash_record *record)
{
    uint8_t sector[512];
    if (!crash_attached || !record || slot >= PROS_CRASH_RECORD_SLOT_COUNT)
        return PROS_CRASH_RECORD_EINVAL;
    crash_zero(sector, sizeof(sector));
    if (block_device_read(crash_device, crash_first_lba + slot, 1u, sector) !=
        PROS_BLOCK_OK)
        return PROS_CRASH_RECORD_EIO;
    crash_zero(record, sizeof(*record));
    for (uint32_t i = 0u; i < sizeof(*record); ++i)
        ((uint8_t *)record)[i] = sector[i];
    return PROS_CRASH_RECORD_OK;
}

static int crash_slot_write(uint32_t slot,
                            const struct pros_crash_record *record)
{
    uint8_t sector[512];
    if (!crash_attached || !record || slot >= PROS_CRASH_RECORD_SLOT_COUNT)
        return PROS_CRASH_RECORD_EINVAL;
    crash_zero(sector, sizeof(sector));
    for (uint32_t i = 0u; i < sizeof(*record); ++i)
        sector[i] = ((const uint8_t *)record)[i];
    if (block_device_write(crash_device, crash_first_lba + slot, 1u, sector) !=
        PROS_BLOCK_OK)
        return PROS_CRASH_RECORD_EIO;
    return block_device_flush(crash_device) == PROS_BLOCK_OK
         ? PROS_CRASH_RECORD_OK : PROS_CRASH_RECORD_EIO;
}

static int crash_find_latest(struct pros_crash_record *latest,
                             uint32_t *slot_out)
{
    struct pros_crash_record a, b;
    int va, vb;
    if (!crash_attached || !latest)
        return PROS_CRASH_RECORD_EINVAL;
    int ra = crash_slot_read(0u, &a);
    int rb = crash_slot_read(1u, &b);
    if (ra != PROS_CRASH_RECORD_OK || rb != PROS_CRASH_RECORD_OK)
        return PROS_CRASH_RECORD_EIO;
    va = crash_record_valid(&a);
    vb = crash_record_valid(&b);
    if (!va && !vb) {
        if (!crash_record_empty(&a) || !crash_record_empty(&b))
            return PROS_CRASH_RECORD_ECORRUPT;
        return PROS_CRASH_RECORD_ENOENT;
    }
    if (va && (!vb || a.sequence >= b.sequence)) {
        *latest = a;
        if (slot_out) *slot_out = 0u;
    } else {
        *latest = b;
        if (slot_out) *slot_out = 1u;
    }
    return PROS_CRASH_RECORD_OK;
}

int pros_crash_record_attach(struct pros_block_device *device,
                             uint64_t first_lba)
{
    struct pros_crash_record latest;
    uint32_t slot = 0u;
    int rc;
    if (crash_attached || pending_valid || crash_capture_active)
        return PROS_CRASH_RECORD_ENOTSUP;
    if (!device || device->sector_size != 512u ||
        first_lba > device->sector_count ||
        device->sector_count - first_lba < PROS_CRASH_RECORD_SLOT_COUNT)
        return PROS_CRASH_RECORD_EINVAL;
    crash_device = device;
    crash_first_lba = first_lba;
    crash_sequence = 0u;
    crash_next_slot = 0u;
    crash_attached = 1u;
    crash_capture_active = 0u;

    rc = crash_find_latest(&latest, &slot);
    if (rc == PROS_CRASH_RECORD_OK) {
        crash_sequence = latest.sequence;
        crash_next_slot = slot ^ 1u;
    } else if (rc != PROS_CRASH_RECORD_ENOENT) {
        crash_attached = 0u;
        crash_device = 0;
        return rc;
    }
    return PROS_CRASH_RECORD_OK;
}

int pros_crash_record_available(void)
{
    return crash_attached ? 1 : 0;
}

void pros_crash_record_detach(void)
{
    crash_device = 0;
    crash_first_lba = 0u;
    crash_sequence = 0u;
    crash_next_slot = 0u;
    crash_attached = 0u;
    crash_capture_active = 0u;
    pending_record = (struct pros_crash_record){0};
    pending_valid = 0u;
    pending_flush_required = 0u;
}

int pros_crash_record_capture(uint64_t vector, uint64_t error_code,
                              uint64_t rip, uint64_t cr2, uint64_t timestamp)
{
    /*
     * NMI can preempt ordinary exception entry even after CLI. Claim before
     * reading pending_valid so a nested capture cannot pass a check/set gap.
     */
    if (__atomic_exchange_n(&crash_capture_active, (uint8_t)1u, __ATOMIC_ACQ_REL))
        return PROS_CRASH_RECORD_ENOTSUP;
    if (pending_valid) {
        __atomic_store_n(&crash_capture_active, (uint8_t)0u, __ATOMIC_RELEASE);
        return PROS_CRASH_RECORD_ENOTSUP;
    }
    if (crash_sequence == UINT64_MAX) {
        __atomic_store_n(&crash_capture_active, (uint8_t)0u, __ATOMIC_RELEASE);
        return PROS_CRASH_RECORD_EOVERFLOW;
    }
    crash_zero(&pending_record, sizeof(pending_record));
    pending_record.magic = PROS_CRASH_RECORD_MAGIC;
    pending_record.version = PROS_CRASH_RECORD_VERSION;
    pending_record.size = sizeof(pending_record);
    pending_record.sequence = crash_sequence + 1u;
    pending_record.timestamp = timestamp;
    pending_record.vector = vector;
    pending_record.error_code = error_code;
    pending_record.rip = rip;
    pending_record.cr2 = cr2;
    pending_record.committed = 1u;
    pending_record.checksum = crash_crc32c((const uint8_t *)&pending_record,
                                           sizeof(pending_record));
    pending_valid = 1u;
    pending_flush_required = 1u;
    __atomic_store_n(&crash_capture_active, 0u, __ATOMIC_RELEASE);
    return PROS_CRASH_RECORD_OK;
}

int pros_crash_record_flush_pending(void)
{
    int rc;
    if (!pending_valid)
        return PROS_CRASH_RECORD_ENOENT;
    if (!crash_record_valid(&pending_record))
        return PROS_CRASH_RECORD_ECORRUPT;
    if (!crash_attached)
        return PROS_CRASH_RECORD_ENOTSUP;
    rc = crash_slot_write(crash_next_slot, &pending_record);
    if (rc != PROS_CRASH_RECORD_OK)
        return rc;
    crash_sequence = pending_record.sequence;
    crash_next_slot ^= 1u;
    pending_valid = 0u;
    pending_flush_required = 0u;
    return PROS_CRASH_RECORD_OK;
}

int pros_crash_record_peek(struct pros_crash_record *record)
{
    if (!record) return PROS_CRASH_RECORD_EINVAL;
    if (pending_valid) {
        if (!crash_record_valid(&pending_record))
            return PROS_CRASH_RECORD_ECORRUPT;
        *record = pending_record;
        return PROS_CRASH_RECORD_OK;
    }
    return crash_find_latest(record, 0);
}

int pros_crash_record_clear(void)
{
    struct pros_crash_record empty;
    crash_zero(&empty, sizeof(empty));
    if (!crash_attached)
        return PROS_CRASH_RECORD_ENOTSUP;
    /* Do not discard the RAM copy until every persistent slot is cleared. */
    for (uint32_t slot = 0u; slot < PROS_CRASH_RECORD_SLOT_COUNT; ++slot)
        if (crash_slot_write(slot, &empty) != PROS_CRASH_RECORD_OK)
            return PROS_CRASH_RECORD_EIO;
    pending_record = (struct pros_crash_record){0};
    pending_valid = 0u;
    pending_flush_required = 0u;
    crash_sequence = 0u;
    crash_next_slot = 0u;
    return PROS_CRASH_RECORD_OK;
}

int pros_crash_record_consume(struct pros_crash_record *record)
{
    int rc;
    if (!record) return PROS_CRASH_RECORD_EINVAL;
    if (pending_valid) {
        if (!crash_record_valid(&pending_record))
            return PROS_CRASH_RECORD_ECORRUPT;
        *record = pending_record;
        pending_valid = 0u;
        pending_flush_required = 0u;
        pending_record = (struct pros_crash_record){0};
        return PROS_CRASH_RECORD_OK;
    }
    rc = crash_find_latest(record, 0);
    if (rc != PROS_CRASH_RECORD_OK) return rc;
    return pros_crash_record_clear();
}

static uint8_t crash_selftest_fail_write;
static uint8_t crash_selftest_fail_flush;

static int crash_selftest_read(struct pros_block_device *device,
                               uint64_t lba, uint32_t count, void *buffer)
{
    uint8_t (*disk)[512] = (uint8_t (*)[512])device->private_data;
    if (!disk || !buffer || lba >= 4u || count == 0u || count > 4u - lba)
        return PROS_BLOCK_EINVAL;
    for (uint32_t s = 0u; s < count; ++s)
        for (uint32_t i = 0u; i < 512u; ++i)
            ((uint8_t *)buffer)[s * 512u + i] = disk[lba + s][i];
    return PROS_BLOCK_OK;
}

static int crash_selftest_write(struct pros_block_device *device,
                                uint64_t lba, uint32_t count,
                                const void *buffer)
{
    uint8_t (*disk)[512] = (uint8_t (*)[512])device->private_data;
    if (!disk || !buffer || lba >= 4u || count == 0u || count > 4u - lba)
        return PROS_BLOCK_EINVAL;
    if (crash_selftest_fail_write)
        return PROS_BLOCK_EIO;
    for (uint32_t s = 0u; s < count; ++s)
        for (uint32_t i = 0u; i < 512u; ++i)
            disk[lba + s][i] = ((const uint8_t *)buffer)[s * 512u + i];
    return PROS_BLOCK_OK;
}

static int crash_selftest_flush(struct pros_block_device *device)
{
    (void)device;
    return crash_selftest_fail_flush ? PROS_BLOCK_EIO : PROS_BLOCK_OK;
}

int pros_crash_record_selftest(void)
{
    static uint8_t disk[4][512];
    struct pros_block_device device;
    struct pros_crash_record record;
    crash_selftest_fail_write = 0u;
    crash_selftest_fail_flush = 0u;
    for (uint32_t s = 0u; s < 4u; ++s)
        for (uint32_t i = 0u; i < 512u; ++i)
            disk[s][i] = 0u;

    if (block_device_init(&device, 512u, 4u, crash_selftest_read,
                          crash_selftest_write, disk) != PROS_BLOCK_OK ||
        block_device_set_flush(&device, crash_selftest_flush) != PROS_BLOCK_OK ||
        pros_crash_record_attach(&device, 1u) != PROS_CRASH_RECORD_OK ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_ENOENT ||
        pros_crash_record_capture(14u, 0x11u, 0x22u, 0x33u, 0x44u) !=
            PROS_CRASH_RECORD_OK ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_OK ||
        record.vector != 14u || record.error_code != 0x11u ||
        record.rip != 0x22u || record.cr2 != 0x33u ||
        pros_crash_record_flush_pending() != PROS_CRASH_RECORD_OK ||
        pros_crash_record_consume(&record) != PROS_CRASH_RECORD_OK ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_ENOENT)
        goto selftest_fail;

    if (pros_crash_record_capture(13u, 1u, 2u, 3u, 4u) != PROS_CRASH_RECORD_OK ||
        pros_crash_record_flush_pending() != PROS_CRASH_RECORD_OK ||
        pros_crash_record_capture(12u, 5u, 6u, 7u, 8u) != PROS_CRASH_RECORD_OK ||
        pros_crash_record_flush_pending() != PROS_CRASH_RECORD_OK ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_OK ||
        record.vector != 12u)
        goto selftest_fail;

    /*
     * Corrupt the in-memory pending copy: readers and persistence must reject
     * it without consuming it. Restore the byte to continue the retry matrix.
     */
    if (pros_crash_record_capture(11u, 9u, 10u, 11u, 12u) !=
        PROS_CRASH_RECORD_OK)
        goto selftest_fail;
    uint32_t saved_checksum = pending_record.checksum;
    pending_record.checksum ^= 0x01010101u;
    if (pros_crash_record_peek(&record) != PROS_CRASH_RECORD_ECORRUPT ||
        pros_crash_record_flush_pending() != PROS_CRASH_RECORD_ECORRUPT ||
        pros_crash_record_consume(&record) != PROS_CRASH_RECORD_ECORRUPT ||
        pending_valid != 1u)
        goto selftest_fail;
    pending_record.checksum = saved_checksum;

    /* Sequence exhaustion must fail closed instead of wrapping to zero. */
    uint64_t saved_sequence = crash_sequence;
    crash_sequence = UINT64_MAX;
    if (pros_crash_record_capture(8u, 1u, 2u, 3u, 4u) !=
            PROS_CRASH_RECORD_EOVERFLOW || pending_valid != 0u)
        goto selftest_fail;
    crash_sequence = saved_sequence;

    /* A simulated nested NMI capture must fail while the owner is active. */
    crash_capture_active = 1u;
    if (pros_crash_record_capture(7u, 1u, 2u, 3u, 4u) !=
            PROS_CRASH_RECORD_ENOTSUP ||
        pending_record.vector != 11u)
        goto selftest_fail;
    crash_capture_active = 0u;

    /* A failed write must retain the pending record for a later retry. */
    crash_selftest_fail_write = 1u;
    if (pros_crash_record_flush_pending() != PROS_CRASH_RECORD_EIO ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_OK ||
        record.vector != 11u)
        goto selftest_fail;
    crash_selftest_fail_write = 0u;
    if (pros_crash_record_flush_pending() != PROS_CRASH_RECORD_OK ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_OK ||
        record.vector != 11u)
        goto selftest_fail;

    /* A failed durability flush must also retain the record for retry. */
    if (pros_crash_record_capture(10u, 13u, 14u, 15u, 16u) !=
        PROS_CRASH_RECORD_OK)
        goto selftest_fail;
    crash_selftest_fail_flush = 1u;
    if (pros_crash_record_flush_pending() != PROS_CRASH_RECORD_EIO ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_OK ||
        record.vector != 10u)
        goto selftest_fail;
    crash_selftest_fail_flush = 0u;
    if (pros_crash_record_flush_pending() != PROS_CRASH_RECORD_OK ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_OK ||
        record.vector != 10u)
        goto selftest_fail;

    /* Corrupt the newest slot: the older valid slot must remain selectable. */
    disk[2][offsetof(struct pros_crash_record, checksum)] ^= 0x5Au;
    if (pros_crash_record_peek(&record) != PROS_CRASH_RECORD_OK ||
        record.vector != 11u)
        goto selftest_fail;

    if (pros_crash_record_clear() != PROS_CRASH_RECORD_OK ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_ENOENT)
        goto selftest_fail;

    /* A failed clear must not discard the only in-memory pending copy. */
    if (pros_crash_record_capture(9u, 17u, 18u, 19u, 20u) !=
        PROS_CRASH_RECORD_OK)
        goto selftest_fail;
    crash_selftest_fail_write = 1u;
    if (pros_crash_record_clear() != PROS_CRASH_RECORD_EIO ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_OK ||
        record.vector != 9u)
        goto selftest_fail;
    crash_selftest_fail_write = 0u;
    if (pros_crash_record_clear() != PROS_CRASH_RECORD_OK ||
        pros_crash_record_peek(&record) != PROS_CRASH_RECORD_ENOENT)
        goto selftest_fail;
    pros_crash_record_detach();
    crash_selftest_fail_write = 0u;
    crash_selftest_fail_flush = 0u;
    return PROS_CRASH_RECORD_OK;

selftest_fail:
    crash_selftest_fail_write = 0u;
    crash_selftest_fail_flush = 0u;
    pros_crash_record_detach();
    return PROS_CRASH_RECORD_EIO;
}
