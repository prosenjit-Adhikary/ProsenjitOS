#include "ata_pio.h"

#define ATA_PRIMARY_BASE 0x1F0u
#define ATA_PRIMARY_CTRL 0x3F6u

#define ATA_REG_DATA      0u
#define ATA_REG_ERROR     1u
#define ATA_REG_FEATURES  1u
#define ATA_REG_SECCOUNT0 2u
#define ATA_REG_LBA0      3u
#define ATA_REG_LBA1      4u
#define ATA_REG_LBA2      5u
#define ATA_REG_HDDEVSEL  6u
#define ATA_REG_STATUS    7u
#define ATA_REG_COMMAND   7u

#define ATA_CMD_IDENTIFY       0xECu
#define ATA_CMD_READ_PIO       0x20u
#define ATA_CMD_READ_PIO_EXT   0x24u
#define ATA_CMD_WRITE_PIO      0x30u
#define ATA_CMD_WRITE_PIO_EXT  0x34u
#define ATA_CMD_CACHE_FLUSH    0xE7u
#define ATA_CMD_CACHE_FLUSH_EXT 0xEAu

#define ATA_SR_ERR  0x01u
#define ATA_SR_DRQ  0x08u
#define ATA_SR_SRV  0x10u
#define ATA_SR_DF   0x20u
#define ATA_SR_RDY  0x40u
#define ATA_SR_BSY  0x80u

#define ATA_TIMEOUT 1000000u
#define ATA_PANIC_TIMEOUT 8192u
static int ata_io_try_lock(struct pros_ata_pio *ata)
{
    return __atomic_exchange_n(&ata->io_lock, 1u, __ATOMIC_ACQUIRE) == 0u;
}
static void ata_io_unlock(struct pros_ata_pio *ata)
{
    __atomic_store_n(&ata->io_lock, 0u, __ATOMIC_RELEASE);
}
static int ata_lba48_identify_valid(const uint16_t identify[256])
{
    return identify && (identify[83] & 0xC000u) == 0x4000u &&
           (identify[83] & (1u << 10)) != 0u;
}
static int ata_status_absent_or_floating(uint8_t status)
{
    /* ATA specifies 0x00 for no device; 0xFF is a floating bus. */
    return status == 0u || status == 0xFFu;
}

static int ata_identify_logical_sector_bytes(
    const uint16_t identify[256], uint32_t *bytes)
{
    uint16_t info;
    uint64_t words;
    if (!identify || !bytes)
        return 0;
    info = identify[106];
    /* An invalid word 106 means the ATA-defined legacy 512-byte default. */
    if ((info & 0xC000u) != 0x4000u || (info & (1u << 12)) == 0u) {
        *bytes = 512u;
        return 1;
    }
    /* Bit 12 makes words 117-118 authoritative for a long logical sector. */
    words = (uint64_t)identify[117] |
            ((uint64_t)identify[118] << 16);
    if (words <= 256u || words > UINT32_MAX / 2u)
        return 0;
    *bytes = (uint32_t)(words * 2u);
    return 1;
}


/* Classify terminal status before testing DRQ. */
static int ata_drq_terminal_status(uint8_t status)
{
    if (ata_status_absent_or_floating(status))
        return PROS_ATA_ENODEV;
    if (status & (ATA_SR_ERR | ATA_SR_DF))
        return PROS_ATA_EIO;
    return PROS_ATA_OK;
}

static uint8_t ata_select_flush_command(const uint16_t identify[256],
                                        uint8_t lba48)
{
    uint16_t command_sets;
    if (!identify)
        return 0u;

    command_sets = identify[83];
    /* Word 83 is valid only when bits 15:14 are exactly 01b. */
    if ((command_sets & 0xC000u) != 0x4000u ||
        (command_sets & (1u << 12)) == 0u)
        return 0u;

    if (lba48 && (command_sets & (1u << 13)) != 0u)
        return ATA_CMD_CACHE_FLUSH_EXT;
    return ATA_CMD_CACHE_FLUSH;
}

static int ata_logical_sector_size_selftest(void)
{
    uint16_t identify[256] = {0};
    uint32_t bytes = 0u;

    if (!ata_identify_logical_sector_bytes(identify, &bytes) ||
        bytes != 512u)
        return PROS_ATA_EIO;

    identify[106] = 0x4000u; /* Valid word 106, legacy 256-word sector. */
    if (!ata_identify_logical_sector_bytes(identify, &bytes) ||
        bytes != 512u)
        return PROS_ATA_EIO;

    identify[106] = 0x5000u; /* Valid + long logical sector. */
    identify[117] = 2048u;   /* 4096-byte logical sectors are unsupported. */
    if (!ata_identify_logical_sector_bytes(identify, &bytes) ||
        bytes != 4096u)
        return PROS_ATA_EIO;

    identify[117] = 0u; /* Long-sector flag with malformed size must reject. */
    if (ata_identify_logical_sector_bytes(identify, &bytes))
        return PROS_ATA_EIO;

    identify[106] = 0x8000u; /* Invalid word-106 validity bits: default 512. */
    if (!ata_identify_logical_sector_bytes(identify, &bytes) ||
        bytes != 512u)
        return PROS_ATA_EIO;
    return PROS_ATA_OK;
}

static int ata_flush_policy_selftest(void)
{
    uint16_t identify[256] = {0};
    if (!ata_status_absent_or_floating(0u) ||
        !ata_status_absent_or_floating(0xFFu) ||
        ata_status_absent_or_floating(0x40u))
        return PROS_ATA_EIO;

    identify[83] = 0x5000u; /* Valid word 83 + mandatory FLUSH CACHE. */
    if (ata_select_flush_command(identify, 0u) != ATA_CMD_CACHE_FLUSH ||
        ata_select_flush_command(identify, 1u) != ATA_CMD_CACHE_FLUSH)
        return PROS_ATA_EIO;

    identify[83] = 0x7000u; /* FLUSH CACHE EXT is independently advertised. */
    if (ata_select_flush_command(identify, 1u) != ATA_CMD_CACHE_FLUSH_EXT ||
        ata_select_flush_command(identify, 0u) != ATA_CMD_CACHE_FLUSH)
        return PROS_ATA_EIO;

    identify[83] = 0x4000u; /* Valid word, but no flush capability. */
    if (ata_select_flush_command(identify, 1u) != 0u)
        return PROS_ATA_EIO;

    identify[83] = 0x3000u; /* Invalid word-83 validity bits. */
    if (ata_select_flush_command(identify, 1u) != 0u)
        return PROS_ATA_EIO;
    identify[83] = 0x0400u; /* Validity 00b with capability bit set. */
    if (ata_lba48_identify_valid(identify))
        return PROS_ATA_EIO;
    identify[83] = 0x8400u; /* Validity 10b. */
    if (ata_lba48_identify_valid(identify))
        return PROS_ATA_EIO;
    identify[83] = 0xC400u; /* Validity 11b. */
    if (ata_lba48_identify_valid(identify))
        return PROS_ATA_EIO;
    identify[83] = 0x4000u; /* Validity 01b, but capability absent. */
    if (ata_lba48_identify_valid(identify))
        return PROS_ATA_EIO;
    identify[83] = 0x4400u; /* Validity 01b plus LBA48 support. */
    if (!ata_lba48_identify_valid(identify))
        return PROS_ATA_EIO;

    return PROS_ATA_OK;
}

static inline void ata_native_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t ata_native_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t ata_native_in16(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void ata_native_out16(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void ata_out8(const struct pros_ata_pio *ata, uint16_t port,
                            uint8_t value)
{
    if (ata && ata->io_ops && ata->io_ops->out8)
        ata->io_ops->out8(ata->io_context, port, value);
    else
        ata_native_out8(port, value);
}

static inline uint8_t ata_in8(const struct pros_ata_pio *ata, uint16_t port)
{
    if (ata && ata->io_ops && ata->io_ops->in8)
        return ata->io_ops->in8(ata->io_context, port);
    return ata_native_in8(port);
}

static inline uint16_t ata_in16(const struct pros_ata_pio *ata, uint16_t port)
{
    if (ata && ata->io_ops && ata->io_ops->in16)
        return ata->io_ops->in16(ata->io_context, port);
    return ata_native_in16(port);
}

static inline void ata_out16(struct pros_ata_pio *ata, uint16_t port,
                             uint16_t value)
{
    if (ata && ata->io_ops && ata->io_ops->out16)
        ata->io_ops->out16(ata->io_context, port, value);
    else
        ata_native_out16(port, value);
}

static void ata_delay400ns(const struct pros_ata_pio *ata)
{
    (void)ata_in8(ata, ata->ctrl_base);
    (void)ata_in8(ata, ata->ctrl_base);
    (void)ata_in8(ata, ata->ctrl_base);
    (void)ata_in8(ata, ata->ctrl_base);
}

static int ata_wait_not_busy_limit(const struct pros_ata_pio *ata,
                                      uint32_t limit)
{
    if (!ata || limit == 0u)
        return PROS_ATA_EINVAL;
    for (uint32_t i = 0u; i < limit; ++i) {
        uint8_t status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
        /* A floating/absent device must not look like an idle successful one. */
        if (status == 0u || status == 0xFFu)
            return PROS_ATA_ENODEV;
        if ((status & ATA_SR_BSY) == 0u)
            return PROS_ATA_OK;
    }
    return PROS_ATA_ETIMEOUT;
}

static int ata_wait_drq_limit(const struct pros_ata_pio *ata,
                              uint32_t limit)
{
    if (!ata || limit == 0u)
        return PROS_ATA_EINVAL;
    for (uint32_t i = 0u; i < limit; ++i) {
        uint8_t status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
        int terminal_rc = ata_drq_terminal_status(status);
        if (terminal_rc != PROS_ATA_OK)
            return terminal_rc;
        if (status & ATA_SR_DRQ)
            return PROS_ATA_OK;
        if (status & ATA_SR_BSY)
            continue;
        if ((status & ATA_SR_RDY) == 0u)
            continue;
    }
    return PROS_ATA_ETIMEOUT;
}

static int ata_wait_not_busy(const struct pros_ata_pio *ata)
{
    return ata_wait_not_busy_limit(ata, ATA_TIMEOUT);
}

static int ata_wait_drq(const struct pros_ata_pio *ata)
{
    return ata_wait_drq_limit(ata, ATA_TIMEOUT);
}

static void ata_select(const struct pros_ata_pio *ata, uint32_t lba)
{
    ata_out8(ata, ata->io_base + ATA_REG_HDDEVSEL,
             (uint8_t)(0xE0u | (ata->drive ? 0x10u : 0u) |
                       ((lba >> 24) & 0x0Fu)));
    ata_delay400ns(ata);
}

/* ATA IDENTIFY strings store the first character in each word's high byte. */
static int ata_identify_decode_string(const uint16_t *identify,
                                     uint32_t first_word,
                                     uint32_t word_count,
                                     char *output, uint32_t output_size)
{
    uint32_t length = word_count * 2u;
    uint32_t first = 0u;
    uint32_t end = length;

    if (!identify || !output || output_size == 0u ||
        length + 1u > output_size)
        return 0;
    for (uint32_t i = 0u; i < length; ++i) {
        uint16_t word = identify[first_word + i / 2u];
        uint8_t ch = (i & 1u) ? (uint8_t)word : (uint8_t)(word >> 8);
        if (ch < 0x20u || ch > 0x7Eu)
            return 0;
    }
    while (first < end) {
        uint16_t word = identify[first_word + first / 2u];
        uint8_t ch = (first & 1u) ? (uint8_t)word : (uint8_t)(word >> 8);
        if (ch != ' ')
            break;
        ++first;
    }
    while (end > first) {
        uint32_t index = end - 1u;
        uint16_t word = identify[first_word + index / 2u];
        uint8_t ch = (index & 1u) ? (uint8_t)word : (uint8_t)(word >> 8);
        if (ch != ' ')
            break;
        --end;
    }
    if (first == end)
        return 0;
    uint32_t out = 0u;
    for (uint32_t i = first; i < end; ++i) {
        uint16_t word = identify[first_word + i / 2u];
        output[out++] = (char)((i & 1u) ? (uint8_t)word :
                                          (uint8_t)(word >> 8));
    }
    output[out] = '\0';
    return 1;
}

static void ata_identify_store_identity(struct pros_ata_pio *ata,
                                        const uint16_t identify[256])
{
    ata->identity_valid = 0u;
    ata->identity_serial[0] = '\0';
    ata->identity_firmware[0] = '\0';
    ata->identity_model[0] = '\0';

    /* Serial (10-19), firmware (23-26), and model (27-46). */
    if (ata_identify_decode_string(identify, 10u, 10u,
                                   ata->identity_serial,
                                   sizeof(ata->identity_serial)) &&
        ata_identify_decode_string(identify, 23u, 4u,
                                   ata->identity_firmware,
                                   sizeof(ata->identity_firmware)) &&
        ata_identify_decode_string(identify, 27u, 20u,
                                   ata->identity_model,
                                   sizeof(ata->identity_model)))
        ata->identity_valid = 1u;
}

static int ata_identity_text_equal(const char *left, const char *right)
{
    if (!left || !right)
        return 0;
    while (*left && *right) {
        if (*left++ != *right++)
            return 0;
    }
    return *left == '\0' && *right == '\0';
}

static int ata_identify_identity_selftest(void)
{
    uint16_t identify[256] = {0};
    struct pros_ata_pio ata = {0};
    char serial[21], firmware[9], model[41];

    /* Construct IDENTIFY words using ATA's high-byte/low-byte character order. */
    const char serial_text[] = "SN1234";
    const char firmware_text[] = "1.0";
    const char model_text[] = "QEMU HARDDISK";
    for (uint32_t i = 0u; i < 10u; ++i) identify[10u + i] = 0x2020u;
    for (uint32_t i = 0u; i < 4u; ++i) identify[23u + i] = 0x2020u;
    for (uint32_t i = 0u; i < 20u; ++i) identify[27u + i] = 0x2020u;
    for (uint32_t i = 0u; i < sizeof(serial_text) - 1u; ++i) {
        uint32_t w = 10u + i / 2u;
        identify[w] = (uint16_t)((identify[w] & ((i & 1u) ? 0xFF00u : 0x00FFu)) |
                         ((uint16_t)(uint8_t)serial_text[i] << ((i & 1u) ? 0u : 8u)));
    }
    for (uint32_t i = 0u; i < sizeof(firmware_text) - 1u; ++i) {
        uint32_t w = 23u + i / 2u;
        identify[w] = (uint16_t)((identify[w] & ((i & 1u) ? 0xFF00u : 0x00FFu)) |
                         ((uint16_t)(uint8_t)firmware_text[i] << ((i & 1u) ? 0u : 8u)));
    }
    for (uint32_t i = 0u; i < sizeof(model_text) - 1u; ++i) {
        uint32_t w = 27u + i / 2u;
        identify[w] = (uint16_t)((identify[w] & ((i & 1u) ? 0xFF00u : 0x00FFu)) |
                         ((uint16_t)(uint8_t)model_text[i] << ((i & 1u) ? 0u : 8u)));
    }
    if (!ata_identify_decode_string(identify, 10u, 10u, serial, sizeof(serial)) ||
        !ata_identify_decode_string(identify, 23u, 4u, firmware, sizeof(firmware)) ||
        !ata_identify_decode_string(identify, 27u, 20u, model, sizeof(model)) ||
        serial[0] != 'S' || serial[5] != '4' || serial[6] != '\0' ||
        firmware[0] != '1' || firmware[2] != '0' || firmware[3] != '\0' ||
        model[0] != 'Q' || model[12] != 'K' || model[13] != '\0')
        return PROS_ATA_EIO;
    ata_identify_store_identity(&ata, identify);
    if (!ata.identity_valid || !ata_identity_text_equal(ata.identity_serial, "SN1234") ||
        !ata_identity_text_equal(ata.identity_firmware, "1.0") ||
        !ata_identity_text_equal(ata.identity_model, "QEMU HARDDISK"))
        return PROS_ATA_EIO;

    /* Blank identity and non-printable bytes must never be persistence keys. */
    for (uint32_t i = 0u; i < 10u; ++i) identify[10u + i] = 0x2020u;
    if (ata_identify_decode_string(identify, 10u, 10u, serial, sizeof(serial)))
        return PROS_ATA_EIO;
    ata_identify_store_identity(&ata, identify);
    if (ata.identity_valid || ata.identity_serial[0] != '\0')
        return PROS_ATA_EIO;
    identify[10] = 0x001Fu;
    if (ata_identify_decode_string(identify, 10u, 10u, serial, sizeof(serial)))
        return PROS_ATA_EIO;
    return PROS_ATA_OK;
}

static int ata_identify(struct pros_ata_pio *ata)
{
    uint16_t identify[256];
    uint8_t status;
    uint32_t lba28_sectors;
    uint64_t lba48_sectors;
    uint32_t logical_sector_bytes = 0u;

    ata_out8(ata, ata->io_base + ATA_REG_HDDEVSEL,
             (uint8_t)(0xA0u | (ata->drive ? 0x10u : 0u)));
    ata_delay400ns(ata);
    ata_out8(ata, ata->io_base + ATA_REG_SECCOUNT0, 0u);
    ata_out8(ata, ata->io_base + ATA_REG_LBA0, 0u);
    ata_out8(ata, ata->io_base + ATA_REG_LBA1, 0u);
    ata_out8(ata, ata->io_base + ATA_REG_LBA2, 0u);
    ata_out8(ata, ata->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
    if (ata_status_absent_or_floating(status))
        return PROS_ATA_ENODEV;
    if (status & ATA_SR_ERR)
        return PROS_ATA_ENOTSUP;
    {
        int rc = ata_wait_drq(ata);
        if (rc != PROS_ATA_OK)
            return rc;
    }

    for (uint32_t i = 0u; i < 256u; ++i)
        identify[i] = ata_in16(ata, ata->io_base + ATA_REG_DATA);

    /* Word 0 bit 15 identifies non-ATA packet devices. */
    if (identify[0] & 0x8000u)
        return PROS_ATA_ENOTSUP;

    /* This PIO/block path is 512-byte logical-sector only. Do not silently
     * reinterpret 4Kn media using the hard-coded 512-byte transfer geometry. */
    if (!ata_identify_logical_sector_bytes(identify, &logical_sector_bytes) ||
        logical_sector_bytes != 512u)
        return PROS_ATA_ENOTSUP;

    lba28_sectors = (uint32_t)identify[60] |
                    ((uint32_t)identify[61] << 16);
    lba48_sectors = (uint64_t)identify[100] |
                    ((uint64_t)identify[101] << 16) |
                    ((uint64_t)identify[102] << 32) |
                    ((uint64_t)identify[103] << 48);

    if (lba48_sectors != 0u && ata_lba48_identify_valid(identify)) {
        ata->lba48 = 1u;
        ata->block.sector_count = lba48_sectors;
    } else if (lba28_sectors != 0u) {
        ata->lba48 = 0u;
        ata->block.sector_count = lba28_sectors;
    } else {
        return PROS_ATA_ENOTSUP;
    }

    ata->flush_command = ata_select_flush_command(identify, ata->lba48);
    ata_identify_store_identity(ata, identify);
    ata->block.sector_size = 512u;
    return PROS_ATA_OK;
}

static void ata_trace_record(struct pros_ata_pio *ata, uint64_t lba,
                              uint32_t count, uint8_t write)
{
    struct pros_ata_trace_event *event;
    if (!ata)
        return;
    event = &ata->trace[ata->trace_head];
    event->lba = lba;
    event->count = count;
    event->write = write;
    ata->trace_head = (ata->trace_head + 1u) % PROS_ATA_TRACE_DEPTH;
    if (ata->trace_count < PROS_ATA_TRACE_DEPTH)
        ++ata->trace_count;
}

void pros_ata_pio_trace_reset(struct pros_ata_pio *ata)
{
    if (!ata || !ata_io_try_lock(ata))
        return;
    ata->trace_count = 0u;
    ata->trace_head = 0u;
    ata_io_unlock(ata);
}

int pros_ata_pio_trace_find(const struct pros_ata_pio *ata, uint64_t lba,
                            uint8_t write, uint64_t *trace_lba,
                            uint32_t *trace_count)
{
    struct pros_ata_pio *mutable_ata = (struct pros_ata_pio *)(uintptr_t)ata;
    int found = PROS_ATA_ENOTSUP;
    if (!ata || !trace_lba || !trace_count)
        return PROS_ATA_EINVAL;
    /* Trace writers execute under io_lock; readers must share that lock. */
    if (!ata_io_try_lock(mutable_ata))
        return PROS_ATA_EIO;
    for (uint32_t n = 0u; n < ata->trace_count; ++n) {
        uint32_t index = (ata->trace_head + PROS_ATA_TRACE_DEPTH - 1u - n) %
                         PROS_ATA_TRACE_DEPTH;
        const struct pros_ata_trace_event *event = &ata->trace[index];
        if (event->write == write && lba >= event->lba &&
            lba - event->lba < event->count) {
            *trace_lba = event->lba;
            *trace_count = event->count;
            found = PROS_ATA_OK;
            break;
        }
    }
    ata_io_unlock(mutable_ata);
    return found;
}

static int ata_prepare(struct pros_ata_pio *ata, uint64_t lba,
                       uint32_t count, uint8_t command)
{
    if (!ata || count == 0u || count > 256u ||
        !block_device_lba_range_valid(&ata->block, lba, count))
        return PROS_ATA_EINVAL;

    if (!ata->lba48 && lba > 0x0FFFFFFFu)
        return PROS_ATA_ENOTSUP;

    /* A prior command may still own the task file. Preserve the
     * distinction between an absent device and a device that never became
     * ready; the block API will map either failure to its I/O error. */
    {
        int rc = ata_wait_not_busy(ata);
        if (rc != PROS_ATA_OK)
            return rc;
    }
    {
        uint8_t status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
        int status_rc = ata_drq_terminal_status(status);
        if (status_rc != PROS_ATA_OK || (status & ATA_SR_DRQ))
            return status_rc == PROS_ATA_ENODEV ? status_rc : PROS_ATA_EIO;
    }

    if (ata->lba48) {
        ata_out8(ata, ata->io_base + ATA_REG_HDDEVSEL,
                 (uint8_t)(0x40u | (ata->drive ? 0x10u : 0u)));
        ata_delay400ns(ata);

        ata_out8(ata, ata->io_base + ATA_REG_SECCOUNT0, (uint8_t)(count >> 8));
        ata_out8(ata, ata->io_base + ATA_REG_LBA0, (uint8_t)(lba >> 24));
        ata_out8(ata, ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 32));
        ata_out8(ata, ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 40));

        ata_out8(ata, ata->io_base + ATA_REG_SECCOUNT0, (uint8_t)(count & 0xFFu));
        ata_out8(ata, ata->io_base + ATA_REG_LBA0, (uint8_t)lba);
        ata_out8(ata, ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        ata_out8(ata, ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    } else {
        ata_select(ata, (uint32_t)lba);
        ata_out8(ata, ata->io_base + ATA_REG_SECCOUNT0,
                 (uint8_t)(count == 256u ? 0u : count));
        ata_out8(ata, ata->io_base + ATA_REG_LBA0, (uint8_t)lba);
        ata_out8(ata, ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        ata_out8(ata, ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    }

    ata_out8(ata, ata->io_base + ATA_REG_COMMAND, command);
    return PROS_ATA_OK;
}

static int ata_read_unlocked(struct pros_block_device *device, uint64_t lba,
                     uint32_t count, void *buffer)
{
    struct pros_ata_pio *ata = (struct pros_ata_pio *)device->private_data;
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t remaining = count;

    if (!ata || !buffer || device->sector_size != 512u)
        return PROS_BLOCK_EINVAL;

    while (remaining != 0u) {
        uint32_t chunk = remaining > 256u ? 256u : remaining;
        ata_trace_record(ata, lba, chunk, 0u);
        int rc = ata_prepare(ata, lba, chunk,
                             ata->lba48 ? ATA_CMD_READ_PIO_EXT :
                                          ATA_CMD_READ_PIO);
        if (rc != PROS_ATA_OK)
            return PROS_BLOCK_EIO;

        for (uint32_t s = 0u; s < chunk; ++s) {
            rc = ata_wait_drq(ata);
            if (rc != PROS_ATA_OK)
                return PROS_BLOCK_EIO;
            for (uint32_t w = 0u; w < 256u; ++w)
                ((uint16_t *)(void *)dst)[w] =
                    ata_in16(ata, ata->io_base + ATA_REG_DATA);
            dst += 512u;
        }
        /* Each PIO command must finish before the next task-file command. */
        if (ata_wait_not_busy(ata) != PROS_ATA_OK)
            return PROS_BLOCK_EIO;
        {
            uint8_t status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
            if (ata_drq_terminal_status(status) != PROS_ATA_OK ||
                (status & ATA_SR_DRQ))
                return PROS_BLOCK_EIO;
        }
        /* Only advance when another chunk remains. A request ending at
         * sector_count is valid, but sector_count itself is not a valid LBA. */
        if (remaining > chunk) {
            uint64_t next_lba;
            if (block_device_lba_add(device, lba, chunk, &next_lba) !=
                    PROS_BLOCK_OK)
                return PROS_BLOCK_EIO;
            lba = next_lba;
        }
        remaining -= chunk;
    }
    return PROS_BLOCK_OK;
}

static int ata_read(struct pros_block_device *device, uint64_t lba,
                    uint32_t count, void *buffer)
{
    struct pros_ata_pio *ata;
    int rc;
    if (!device || !device->private_data)
        return PROS_BLOCK_EINVAL;
    ata = (struct pros_ata_pio *)device->private_data;
    if (!ata_io_try_lock(ata))
        return PROS_BLOCK_EIO;
    rc = ata_read_unlocked(device, lba, count, buffer);
    ata_io_unlock(ata);
    return rc;
}

static int ata_write_unlocked(struct pros_block_device *device, uint64_t lba,
                      uint32_t count, const void *buffer)
{
    struct pros_ata_pio *ata = (struct pros_ata_pio *)device->private_data;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t remaining = count;

    if (!ata || !buffer || device->sector_size != 512u)
        return PROS_BLOCK_EINVAL;

    while (remaining != 0u) {
        uint32_t chunk = remaining > 256u ? 256u : remaining;
        ata_trace_record(ata, lba, chunk, 1u);
        int rc = ata_prepare(ata, lba, chunk,
                             ata->lba48 ? ATA_CMD_WRITE_PIO_EXT :
                                          ATA_CMD_WRITE_PIO);
        if (rc != PROS_ATA_OK)
            return PROS_BLOCK_EIO;

        for (uint32_t s = 0u; s < chunk; ++s) {
            rc = ata_wait_drq(ata);
            if (rc != PROS_ATA_OK)
                return PROS_BLOCK_EIO;
            for (uint32_t w = 0u; w < 256u; ++w)
                ata_out16(ata, ata->io_base + ATA_REG_DATA,
                          ((const uint16_t *)(const void *)src)[w]);
            src += 512u;
        }

        /* Wait for the device to finish the PIO write before issuing the
         * next command. The final data word only completes the host transfer;
         * the device may still have BSY asserted while committing the sector. */
        rc = ata_wait_not_busy(ata);
        if (rc != PROS_ATA_OK)
            return PROS_BLOCK_EIO;
        {
            uint8_t status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
            if (ata_drq_terminal_status(status) != PROS_ATA_OK ||
                (status & ATA_SR_DRQ))
                return PROS_BLOCK_EIO;
        }

        /* Do not calculate a one-past-end LBA after the final chunk:
         * sector_count is a valid exclusive end, not a valid sector index. */
        if (remaining > chunk) {
            uint64_t next_lba;
            if (block_device_lba_add(device, lba, chunk, &next_lba) !=
                    PROS_BLOCK_OK)
                return PROS_BLOCK_EIO;
            lba = next_lba;
        }
        remaining -= chunk;
    }

    return PROS_BLOCK_OK;
}

static int ata_write(struct pros_block_device *device, uint64_t lba,
                     uint32_t count, const void *buffer)
{
    struct pros_ata_pio *ata;
    int rc;
    if (!device || !device->private_data)
        return PROS_BLOCK_EINVAL;
    ata = (struct pros_ata_pio *)device->private_data;
    if (!ata_io_try_lock(ata))
        return PROS_BLOCK_EIO;
    rc = ata_write_unlocked(device, lba, count, buffer);
    ata_io_unlock(ata);
    return rc;
}

static int ata_flush_unlocked(struct pros_block_device *device)
{
    struct pros_ata_pio *ata = (struct pros_ata_pio *)device->private_data;
    if (!ata)
        return PROS_BLOCK_EINVAL;
    if (ata->flush_command == 0u)
        return PROS_BLOCK_ENOTSUP;

    /* Do not issue a new command while the device is still processing one. */
    if (ata_wait_not_busy(ata) != PROS_ATA_OK)
        return PROS_BLOCK_EIO;
    {
        uint8_t status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
        if (ata_drq_terminal_status(status) != PROS_ATA_OK ||
            (status & ATA_SR_DRQ))
            return PROS_BLOCK_EIO;
    }

    ata_out8(ata, ata->io_base + ATA_REG_COMMAND, ata->flush_command);
    ata_delay400ns(ata);
    if (ata_wait_not_busy(ata) != PROS_ATA_OK)
        return PROS_BLOCK_EIO;
    /* A flush can complete with BSY clear but still report device failure. */
    {
        uint8_t status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
        if (ata_drq_terminal_status(status) != PROS_ATA_OK ||
            (status & ATA_SR_DRQ))
            return PROS_BLOCK_EIO;
    }
    return PROS_BLOCK_OK;
}

static int ata_flush(struct pros_block_device *device)
{
    struct pros_ata_pio *ata;
    int rc;
    if (!device || !device->private_data)
        return PROS_BLOCK_EINVAL;
    ata = (struct pros_ata_pio *)device->private_data;
    if (!ata_io_try_lock(ata))
        return PROS_BLOCK_EIO;
    rc = ata_flush_unlocked(device);
    ata_io_unlock(ata);
    return rc;
}

int pros_ata_pio_panic_write_flush(struct pros_ata_pio *ata,
                                      uint64_t lba, const void *sector)
{
    const uint16_t *words = (const uint16_t *)sector;
    uint8_t status;
    int rc = PROS_ATA_EIO;

    if (!ata || !sector || ((uintptr_t)sector & 1u) != 0u ||
        ata->drive > 1u ||
        !((ata->io_base == ATA_PRIMARY_BASE &&
           ata->ctrl_base == ATA_PRIMARY_CTRL) ||
          (ata->io_base == 0x170u && ata->ctrl_base == 0x376u)) ||
        ata->block.sector_size != 512u ||
        !block_device_lba_range_valid(&ata->block, lba, 1u))
        return PROS_ATA_EINVAL;
    if (!ata->identity_valid)
        return PROS_ATA_ENOTSUP;
    if (ata->flush_command != ATA_CMD_CACHE_FLUSH &&
        ata->flush_command != ATA_CMD_CACHE_FLUSH_EXT)
        return PROS_ATA_ENOTSUP;
    if ((!ata->lba48 && lba > 0x0FFFFFFFu) ||
        (ata->lba48 && lba > 0x0000FFFFFFFFFFFFull))
        return PROS_ATA_ENOTSUP;

    /* Never re-enter storage after a fatal exception interrupted block I/O
     * or its health callback. This path deliberately bypasses block wrappers. */
    if (block_device_io_active() != 0u)
        return PROS_ATA_EIO;
    if (!ata_io_try_lock(ata))
        return PROS_ATA_EIO;
    if (block_device_io_active() != 0u) {
        ata_io_unlock(ata);
        return PROS_ATA_EIO;
    }

    rc = ata_wait_not_busy_limit(ata, ATA_PANIC_TIMEOUT);
    if (rc != PROS_ATA_OK)
        goto out;
    status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
    rc = ata_drq_terminal_status(status);
    if (rc != PROS_ATA_OK || (status & ATA_SR_DRQ)) {
        if (rc == PROS_ATA_OK) rc = PROS_ATA_EIO;
        goto out;
    }

    if (ata->lba48) {
        ata_out8(ata, ata->io_base + ATA_REG_HDDEVSEL,
                 (uint8_t)(0x40u | (ata->drive ? 0x10u : 0u)));
        ata_delay400ns(ata);
        ata_out8(ata, ata->io_base + ATA_REG_SECCOUNT0, 0u);
        ata_out8(ata, ata->io_base + ATA_REG_LBA0, (uint8_t)(lba >> 24));
        ata_out8(ata, ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 32));
        ata_out8(ata, ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 40));
        ata_out8(ata, ata->io_base + ATA_REG_SECCOUNT0, 1u);
        ata_out8(ata, ata->io_base + ATA_REG_LBA0, (uint8_t)lba);
        ata_out8(ata, ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        ata_out8(ata, ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
        ata_out8(ata, ata->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO_EXT);
    } else {
        ata_select(ata, (uint32_t)lba);
        ata_out8(ata, ata->io_base + ATA_REG_SECCOUNT0, 1u);
        ata_out8(ata, ata->io_base + ATA_REG_LBA0, (uint8_t)lba);
        ata_out8(ata, ata->io_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        ata_out8(ata, ata->io_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
        ata_out8(ata, ata->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    }

    rc = ata_wait_drq_limit(ata, ATA_PANIC_TIMEOUT);
    if (rc != PROS_ATA_OK)
        goto out;
    for (uint32_t i = 0u; i < 256u; ++i)
        ata_out16(ata, ata->io_base + ATA_REG_DATA, words[i]);

    rc = ata_wait_not_busy_limit(ata, ATA_PANIC_TIMEOUT);
    if (rc != PROS_ATA_OK)
        goto out;
    status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
    rc = ata_drq_terminal_status(status);
    if (rc != PROS_ATA_OK || (status & ATA_SR_DRQ)) {
        if (rc == PROS_ATA_OK) rc = PROS_ATA_EIO;
        goto out;
    }

    ata_out8(ata, ata->io_base + ATA_REG_COMMAND, ata->flush_command);
    ata_delay400ns(ata);
    rc = ata_wait_not_busy_limit(ata, ATA_PANIC_TIMEOUT);
    if (rc != PROS_ATA_OK)
        goto out;
    status = ata_in8(ata, ata->io_base + ATA_REG_STATUS);
    rc = ata_drq_terminal_status(status);
    if (rc != PROS_ATA_OK || (status & ATA_SR_DRQ)) {
        if (rc == PROS_ATA_OK) rc = PROS_ATA_EIO;
        goto out;
    }
    rc = PROS_ATA_OK;

out:
    ata_io_unlock(ata);
    return rc;
}

struct ata_fake_io {
    uint8_t status[8];
    uint32_t status_count;
    uint32_t status_index;
    uint32_t status_reads;
    uint32_t fail_status_at;
    uint32_t data_in_words;
    uint32_t data_out_words;
    uint32_t sector_count;
    uint32_t sectors_remaining;
    uint32_t command_count;
    uint8_t dynamic_status;
    uint8_t injected_error;
};

static void ata_fake_out8(void *context, uint16_t port, uint8_t value)
{
    struct ata_fake_io *fake = (struct ata_fake_io *)context;
    if (!fake || !fake->dynamic_status)
        return;
    if (port == ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0) {
        fake->sector_count = value;
    } else if (port == ATA_PRIMARY_BASE + ATA_REG_COMMAND &&
               (value == ATA_CMD_READ_PIO || value == ATA_CMD_WRITE_PIO)) {
        fake->sectors_remaining = fake->sector_count == 0u
            ? 256u : fake->sector_count;
        ++fake->command_count;
    }
}

static uint8_t ata_fake_in8(void *context, uint16_t port)
{
    struct ata_fake_io *fake = (struct ata_fake_io *)context;
    if (port != ATA_PRIMARY_BASE + ATA_REG_STATUS)
        return ATA_SR_RDY;
    ++fake->status_reads;
    if (fake->fail_status_at != 0u &&
        fake->status_reads >= fake->fail_status_at)
        fake->injected_error = 1u;
    /* ATA ERR/DF remains latched until a new command/reset; keep it sticky. */
    if (fake->injected_error)
        return ATA_SR_ERR;
    if (fake->dynamic_status) {
        if (fake->sectors_remaining != 0u) {
            --fake->sectors_remaining;
            return ATA_SR_DRQ | ATA_SR_RDY;
        }
        return ATA_SR_RDY;
    }
    if (fake->status_index < fake->status_count)
        return fake->status[fake->status_index++];
    return ATA_SR_RDY;
}

static uint16_t ata_fake_in16(void *context, uint16_t port)
{
    struct ata_fake_io *fake = (struct ata_fake_io *)context;
    (void)port;
    ++fake->data_in_words;
    return 0xA55Au;
}

static void ata_fake_out16(void *context, uint16_t port, uint16_t value)
{
    struct ata_fake_io *fake = (struct ata_fake_io *)context;
    (void)port;
    (void)value;
    ++fake->data_out_words;
}

static const struct pros_ata_io_ops ata_fake_ops = {
    .out8 = ata_fake_out8,
    .in8 = ata_fake_in8,
    .in16 = ata_fake_in16,
    .out16 = ata_fake_out16
};

/* The data phase can finish while the device still reports a terminal error.
 * Both read and write callbacks must reject that final completion status. */
static int ata_final_completion_fault_selftest(void)
{
    struct pros_ata_pio ata = {0};
    struct ata_fake_io fake = {
        .status = { ATA_SR_RDY, ATA_SR_RDY, ATA_SR_DRQ | ATA_SR_RDY,
                    ATA_SR_RDY, ATA_SR_ERR },
        .status_count = 5u
    };
    uint8_t buffer[512] __attribute__((aligned(2)));

    ata.io_base = ATA_PRIMARY_BASE;
    ata.ctrl_base = ATA_PRIMARY_CTRL;
    ata.io_ops = &ata_fake_ops;
    ata.io_context = &fake;
    if (block_device_init(&ata.block, 512u, 32u,
                          ata_read, ata_write, &ata) != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    if (block_device_read(&ata.block, 4u, 1u, buffer) != PROS_BLOCK_EIO ||
        fake.data_in_words != 256u || fake.status_index != 5u)
        return PROS_ATA_EIO;

    fake.status_index = 0u;
    fake.data_out_words = 0u;
    if (block_device_write(&ata.block, 4u, 1u, buffer) != PROS_BLOCK_EIO ||
        fake.data_out_words != 256u || fake.status_index != 5u)
        return PROS_ATA_EIO;
    return PROS_ATA_OK;
}

/*
 * Sweep a sticky ATA ERR injection over every status poll reached by a
 * successful two-sector PIO command. The same matrix covers read and write.
 */
static int ata_status_poll_fault_sweep_selftest(void)
{
    struct pros_ata_pio ata = {0};
    struct ata_fake_io fake = { .dynamic_status = 1u };
    uint8_t buffer[1024] __attribute__((aligned(2)));
    uint32_t successful_status_reads;

    ata.io_base = ATA_PRIMARY_BASE;
    ata.ctrl_base = ATA_PRIMARY_CTRL;
    ata.io_ops = &ata_fake_ops;
    ata.io_context = &fake;
    if (block_device_init(&ata.block, 512u, 32u,
                          ata_read, ata_write, &ata) != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    if (block_device_read(&ata.block, 4u, 2u, buffer) != PROS_BLOCK_OK ||
        fake.status_reads == 0u || fake.data_in_words != 512u)
        return PROS_ATA_EIO;
    successful_status_reads = fake.status_reads;

    for (uint32_t fail_at = 1u; fail_at <= successful_status_reads; ++fail_at) {
        fake = (struct ata_fake_io){
            .dynamic_status = 1u,
            .fail_status_at = fail_at
        };
        if (block_device_read(&ata.block, 4u, 2u, buffer) != PROS_BLOCK_EIO ||
            fake.status_reads < fail_at ||
            fake.data_in_words > 512u)
            return PROS_ATA_EIO;
    }

    for (uint32_t fail_at = 1u; fail_at <= successful_status_reads; ++fail_at) {
        fake = (struct ata_fake_io){
            .dynamic_status = 1u,
            .fail_status_at = fail_at
        };
        if (block_device_write(&ata.block, 4u, 2u, buffer) != PROS_BLOCK_EIO ||
            fake.status_reads < fail_at ||
            fake.data_out_words > 512u)
            return PROS_ATA_EIO;
    }

    return PROS_ATA_OK;
}

/* Exercise the real chunking loop at 257 sectors: one 256-sector command
 * followed by a one-sector command, for both read and write. */
static int ata_chunk_boundary_selftest(void)
{
    struct pros_ata_pio ata = {0};
    struct ata_fake_io fake = { .dynamic_status = 1u };
    static uint8_t buffer[257u * 512u] __attribute__((aligned(2)));
    const uint32_t expected_words = 257u * 256u;

    ata.io_base = ATA_PRIMARY_BASE;
    ata.ctrl_base = ATA_PRIMARY_CTRL;
    ata.io_ops = &ata_fake_ops;
    ata.io_context = &fake;
    if (block_device_init(&ata.block, 512u, 512u,
                          ata_read, ata_write, &ata) != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    if (block_device_read(&ata.block, 10u, 257u, buffer) != PROS_BLOCK_OK ||
        fake.command_count != 2u || fake.data_in_words != expected_words ||
        fake.sectors_remaining != 0u)
        return PROS_ATA_EIO;

    fake.sector_count = 0u;
    fake.sectors_remaining = 0u;
    fake.command_count = 0u;
    fake.data_out_words = 0u;
    if (block_device_write(&ata.block, 10u, 257u, buffer) != PROS_BLOCK_OK ||
        fake.command_count != 2u || fake.data_out_words != expected_words ||
        fake.sectors_remaining != 0u)
        return PROS_ATA_EIO;
    return PROS_ATA_OK;
}

/* Exercise the real block read/write callbacks with an injected failure on
 * sector two. Sector one transfers fully; sector two must fail before data
 * transfer and the public block API must not report a partial success. */
static int ata_multi_sector_fault_selftest(void)
{
    struct pros_ata_pio ata = {0};
    struct ata_fake_io fake = {
        .status = { ATA_SR_RDY, ATA_SR_RDY,
                    ATA_SR_DRQ | ATA_SR_RDY, ATA_SR_ERR },
        .status_count = 4u
    };
    uint8_t buffer[1024] __attribute__((aligned(2)));

    ata.io_base = ATA_PRIMARY_BASE;
    ata.ctrl_base = ATA_PRIMARY_CTRL;
    ata.io_ops = &ata_fake_ops;
    ata.io_context = &fake;
    if (block_device_init(&ata.block, 512u, 32u,
                          ata_read, ata_write, &ata) != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    if (block_device_read(&ata.block, 4u, 2u, buffer) != PROS_BLOCK_EIO ||
        fake.data_in_words != 256u || fake.status_index != 4u)
        return PROS_ATA_EIO;

    fake.status_index = 0u;
    fake.data_out_words = 0u;
    if (block_device_write(&ata.block, 4u, 2u, buffer) != PROS_BLOCK_EIO ||
        fake.data_out_words != 256u || fake.status_index != 4u)
        return PROS_ATA_EIO;

    return PROS_ATA_OK;
}

int pros_ata_pio_probe_at(struct pros_ata_pio *ata, uint16_t io_base,
                          uint16_t ctrl_base, uint8_t drive)
{
    int rc;
    if (!ata || drive > 1u)
        return PROS_ATA_EINVAL;
    if (!((io_base == ATA_PRIMARY_BASE && ctrl_base == ATA_PRIMARY_CTRL) ||
          (io_base == 0x170u && ctrl_base == 0x376u)))
        return PROS_ATA_EINVAL;

    /* Accept only the two standard legacy compatibility-mode ATA channels. */
    ata->io_ops = 0;
    ata->io_context = 0;
    ata->io_base = io_base;
    ata->ctrl_base = ctrl_base;
    ata->drive = drive;
    ata->lba48 = 0u;
    ata->flush_command = 0u;
    ata->identity_valid = 0u;
    ata->identity_serial[0] = '\0';
    ata->identity_firmware[0] = '\0';
    ata->identity_model[0] = '\0';
    __atomic_store_n(&ata->io_lock, 0u, __ATOMIC_RELAXED);
    pros_ata_pio_trace_reset(ata);

    rc = block_device_init(&ata->block, 512u, 1u,
                           ata_read, ata_write, ata);
    if (rc != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    rc = ata_identify(ata);
    if (rc != PROS_ATA_OK)
        return rc;

    return block_device_set_flush(&ata->block, ata_flush) == PROS_BLOCK_OK
        ? PROS_ATA_OK : PROS_ATA_EIO;
}

int pros_ata_pio_probe(struct pros_ata_pio *ata)
{
    return pros_ata_pio_probe_at(ata, ATA_PRIMARY_BASE,
                                 ATA_PRIMARY_CTRL, 0u);
}

int pros_ata_pio_selftest_at(uint16_t io_base, uint16_t ctrl_base,
                             uint8_t drive)
{
    struct pros_ata_pio ata;
    uint8_t sector[512];
    struct pros_ata_pio lock_probe = {0};
    uint64_t trace_lba = 0u;
    uint32_t trace_count = 0u;

    if (pros_ata_pio_probe_at(&lock_probe, ATA_PRIMARY_BASE,
                              0x376u, 0u) != PROS_ATA_EINVAL ||
        pros_ata_pio_probe_at(&lock_probe, ATA_PRIMARY_BASE,
                              ATA_PRIMARY_CTRL, 2u) != PROS_ATA_EINVAL)
        return PROS_ATA_EIO;

    if (!ata_io_try_lock(&lock_probe) || ata_io_try_lock(&lock_probe))
        return PROS_ATA_EIO;
    if (pros_ata_pio_trace_find(&lock_probe, 0u, 0u,
                                &trace_lba, &trace_count) != PROS_ATA_EIO) {
        ata_io_unlock(&lock_probe);
        return PROS_ATA_EIO;
    }
    ata_io_unlock(&lock_probe);
    if (!ata_io_try_lock(&lock_probe))
        return PROS_ATA_EIO;
    ata_io_unlock(&lock_probe);
    if (pros_ata_pio_trace_find(&lock_probe, 0u, 0u,
                                &trace_lba, &trace_count) != PROS_ATA_ENOTSUP)
        return PROS_ATA_EIO;

    if (ata_identify_identity_selftest() != PROS_ATA_OK ||
        ata_logical_sector_size_selftest() != PROS_ATA_OK ||
        ata_flush_policy_selftest() != PROS_ATA_OK ||
        ata_multi_sector_fault_selftest() != PROS_ATA_OK ||
        ata_final_completion_fault_selftest() != PROS_ATA_OK ||
        ata_chunk_boundary_selftest() != PROS_ATA_OK ||
        ata_status_poll_fault_sweep_selftest() != PROS_ATA_OK)
        return PROS_ATA_EIO;

    /* FS-11: terminal per-sector status classification. */
    if (ata_drq_terminal_status(0u) != PROS_ATA_ENODEV ||
        ata_drq_terminal_status(0xFFu) != PROS_ATA_ENODEV ||
        ata_drq_terminal_status(ATA_SR_ERR) != PROS_ATA_EIO ||
        ata_drq_terminal_status(ATA_SR_DF) != PROS_ATA_EIO ||
        ata_drq_terminal_status(ATA_SR_BSY) != PROS_ATA_OK ||
        ata_drq_terminal_status(ATA_SR_RDY) != PROS_ATA_OK ||
        ata_drq_terminal_status(ATA_SR_DRQ | ATA_SR_RDY) != PROS_ATA_OK)
        return PROS_ATA_EIO;

    int rc = pros_ata_pio_probe_at(&ata, io_base, ctrl_base, drive);
    if (rc != PROS_ATA_OK)
        return rc;

    if (block_device_read(&ata.block, 0u, 1u, sector) != PROS_BLOCK_OK)
        return PROS_ATA_EIO;

    /* A real disk read must provide the conventional MBR signature or a
     * non-partitioned volume may be present. Do not guess a filesystem. */
    if ((sector[510] != 0x55u || sector[511] != 0xAAu) &&
        !(sector[0] == 0u && sector[1] == 0u))
        return PROS_ATA_EIO;

    return PROS_ATA_OK;
}


int pros_ata_pio_selftest(void)
{
    return pros_ata_pio_selftest_at(ATA_PRIMARY_BASE,
                                    ATA_PRIMARY_CTRL, 0u);
}
