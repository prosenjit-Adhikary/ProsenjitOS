#include "telemetry_protocol.h"

static uint16_t scc_read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t scc_read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t scc_read_u64(const uint8_t *p)
{
    return (uint64_t)scc_read_u32(p) |
           ((uint64_t)scc_read_u32(p + 4) << 32);
}

static int scc_known_opcode(uint16_t opcode)
{
    return opcode == SCC_OP_GET_STATUS ||
           opcode == SCC_OP_LIST_EVENTS ||
           opcode == SCC_OP_GET_EVENT;
}

enum scc_parse_result scc_parse_request(
    const uint8_t *bytes, size_t length, struct scc_request_view *out)
{
    struct scc_request_view parsed;
    uint16_t payload_length;
    uint16_t opcode;

    if (bytes == NULL || out == NULL)
        return SCC_PARSE_INVALID_ARGUMENT;
    if (length < SCC_REQUEST_HEADER_SIZE)
        return SCC_PARSE_SHORT_FRAME;
    if (length > SCC_IPC_MESSAGE_MAX)
        return SCC_PARSE_OVERSIZED_FRAME;

    if (scc_read_u16(bytes) != SCC_PROTOCOL_VERSION)
        return SCC_PARSE_BAD_VERSION;

    opcode = scc_read_u16(bytes + 2);
    if (!scc_known_opcode(opcode))
        return SCC_PARSE_BAD_OPCODE;
    if (scc_read_u16(bytes + 10) != 0)
        return SCC_PARSE_BAD_FLAGS;
    if (scc_read_u32(bytes + 12) != 0)
        return SCC_PARSE_BAD_RESERVED;

    payload_length = scc_read_u16(bytes + 8);
    if ((size_t)payload_length != length - SCC_REQUEST_HEADER_SIZE)
        return SCC_PARSE_BAD_LENGTH;

    if ((opcode == SCC_OP_GET_STATUS && payload_length != 0) ||
        (opcode == SCC_OP_LIST_EVENTS && payload_length != 16) ||
        (opcode == SCC_OP_GET_EVENT && payload_length != 8))
        return SCC_PARSE_BAD_PAYLOAD;

    parsed.version = SCC_PROTOCOL_VERSION;
    parsed.opcode = opcode;
    parsed.request_id = scc_read_u32(bytes + 4);
    parsed.payload_length = payload_length;
    parsed.payload = bytes + SCC_REQUEST_HEADER_SIZE;
    parsed.after_sequence = 0;
    parsed.category_mask = 0;
    parsed.max_records = 0;

    if (opcode == SCC_OP_LIST_EVENTS) {
        const uint8_t *p = parsed.payload;
        uint16_t max_records = scc_read_u16(p + 12);

        if (scc_read_u16(p + 14) != 0)
            return SCC_PARSE_BAD_RESERVED;
        if (max_records < 1 || max_records > (uint16_t)SCC_EVENT_RECORDS_MAX)
            return SCC_PARSE_BAD_PAYLOAD;

        parsed.after_sequence = scc_read_u64(p);
        parsed.category_mask = scc_read_u32(p + 8);
        parsed.max_records = max_records;
    } else if (opcode == SCC_OP_GET_EVENT &&
               scc_read_u64(parsed.payload) == 0) {
        return SCC_PARSE_BAD_PAYLOAD;
    }

    *out = parsed;
    return SCC_PARSE_OK;
}

enum scc_parse_result scc_parse_response(
    const uint8_t *bytes, size_t length, uint16_t expected_opcode,
    uint32_t expected_request_id, uint16_t requested_max_records,
    uint64_t prior_cursor, uint64_t expected_event_sequence,
    struct scc_response_view *out)
{
    struct scc_response_view parsed;
    uint16_t payload_length;
    uint16_t opcode;
    uint32_t status;
    const uint8_t *payload;
    uint16_t count;
    uint16_t i;
    uint64_t previous_sequence;

    if (bytes == NULL || out == NULL)
        return SCC_PARSE_INVALID_ARGUMENT;
    if (length < SCC_RESPONSE_HEADER_SIZE)
        return SCC_PARSE_SHORT_FRAME;
    if (length > SCC_IPC_MESSAGE_MAX)
        return SCC_PARSE_OVERSIZED_FRAME;

    if (scc_read_u16(bytes) != SCC_PROTOCOL_VERSION)
        return SCC_PARSE_BAD_VERSION;

    opcode = scc_read_u16(bytes + 2);
    if (!scc_known_opcode(opcode))
        return SCC_PARSE_BAD_OPCODE;
    if (scc_read_u16(bytes + 14) != 0)
        return SCC_PARSE_BAD_FLAGS;

    payload_length = scc_read_u16(bytes + 12);
    if ((size_t)payload_length != length - SCC_RESPONSE_HEADER_SIZE)
        return SCC_PARSE_BAD_LENGTH;
    if ((size_t)payload_length > SCC_RESPONSE_PAYLOAD_MAX)
        return SCC_PARSE_OVERSIZED_FRAME;

    status = scc_read_u32(bytes + 8);
    if (status > (uint32_t)SCC_STATUS_INTERNAL_ERROR)
        return SCC_PARSE_BAD_STATUS;

    if (opcode != expected_opcode ||
        scc_read_u32(bytes + 4) != expected_request_id)
        return SCC_PARSE_MISMATCH;

    payload = bytes + SCC_RESPONSE_HEADER_SIZE;
    if (status != SCC_STATUS_OK) {
        if (payload_length != 0 || scc_read_u64(bytes + 16) != 0)
            return SCC_PARSE_BAD_PAYLOAD;
        parsed.version = SCC_PROTOCOL_VERSION;
        parsed.opcode = opcode;
        parsed.request_id = expected_request_id;
        parsed.status = status;
        parsed.payload_length = 0;
        parsed.event_cursor = 0;
        parsed.payload = payload;
        parsed.event_count = 0;
        *out = parsed;
        return SCC_PARSE_OK;
    }

    if (opcode == SCC_OP_GET_STATUS) {
        if (payload_length != 24 || scc_read_u64(bytes + 16) != 0)
            return SCC_PARSE_BAD_PAYLOAD;
        if (scc_read_u16(payload + 2) > 3)
            return SCC_PARSE_BAD_PAYLOAD;
        count = 0;
    } else if (opcode == SCC_OP_LIST_EVENTS) {
        uint64_t cursor = scc_read_u64(bytes + 16);

        if (requested_max_records < 1 ||
            requested_max_records > (uint16_t)SCC_EVENT_RECORDS_MAX)
            return SCC_PARSE_INVALID_ARGUMENT;
        if (payload_length % SCC_EVENT_RECORD_SIZE != 0)
            return SCC_PARSE_BAD_PAYLOAD;

        count = (uint16_t)(payload_length / SCC_EVENT_RECORD_SIZE);
        if (count > requested_max_records ||
            count > (uint16_t)SCC_EVENT_RECORDS_MAX)
            return SCC_PARSE_BAD_PAYLOAD;

        if (count == 0) {
            if (cursor != prior_cursor)
                return SCC_PARSE_BAD_PAYLOAD;
        } else {
            previous_sequence = prior_cursor;
            for (i = 0; i < count; ++i) {
                uint64_t sequence =
                    scc_read_u64(payload + ((size_t)i * SCC_EVENT_RECORD_SIZE));
                if (sequence <= previous_sequence)
                    return SCC_PARSE_BAD_PAYLOAD;
                previous_sequence = sequence;
            }
            if (cursor != previous_sequence)
                return SCC_PARSE_BAD_PAYLOAD;
        }
    } else {
        if (payload_length != SCC_EVENT_RECORD_SIZE)
            return SCC_PARSE_BAD_PAYLOAD;
        if (expected_event_sequence == 0 ||
            scc_read_u64(payload) != expected_event_sequence ||
            scc_read_u64(bytes + 16) != expected_event_sequence)
            return SCC_PARSE_BAD_PAYLOAD;
        count = 1;
    }

    parsed.version = SCC_PROTOCOL_VERSION;
    parsed.opcode = opcode;
    parsed.request_id = expected_request_id;
    parsed.status = status;
    parsed.payload_length = payload_length;
    parsed.event_cursor = scc_read_u64(bytes + 16);
    parsed.payload = payload;
    parsed.event_count = count;
    *out = parsed;
    return SCC_PARSE_OK;
}
