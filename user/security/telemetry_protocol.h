#ifndef PROSENJIT_SECURITY_TELEMETRY_PROTOCOL_H
#define PROSENJIT_SECURITY_TELEMETRY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define SCC_PROTOCOL_VERSION 1u
#define SCC_REQUEST_HEADER_SIZE 16u
#define SCC_RESPONSE_HEADER_SIZE 24u
#define SCC_EVENT_RECORD_SIZE 24u
#define SCC_IPC_MESSAGE_MAX 64u
#define SCC_RESPONSE_PAYLOAD_MAX (SCC_IPC_MESSAGE_MAX - SCC_RESPONSE_HEADER_SIZE)
#define SCC_EVENT_RECORDS_MAX (SCC_RESPONSE_PAYLOAD_MAX / SCC_EVENT_RECORD_SIZE)

enum scc_opcode {
    SCC_OP_GET_STATUS = 1,
    SCC_OP_LIST_EVENTS = 2,
    SCC_OP_GET_EVENT = 3
};

enum scc_status {
    SCC_STATUS_OK = 0,
    SCC_STATUS_UNSUPPORTED_VERSION = 1,
    SCC_STATUS_UNSUPPORTED_OPCODE = 2,
    SCC_STATUS_INVALID_ARGUMENT = 3,
    SCC_STATUS_DENIED = 4,
    SCC_STATUS_UNAVAILABLE = 5,
    SCC_STATUS_STALE = 6,
    SCC_STATUS_BUFFER_TOO_SMALL = 7,
    SCC_STATUS_INTERNAL_ERROR = 8
};

enum scc_parse_result {
    SCC_PARSE_OK = 0,
    SCC_PARSE_INVALID_ARGUMENT,
    SCC_PARSE_SHORT_FRAME,
    SCC_PARSE_OVERSIZED_FRAME,
    SCC_PARSE_BAD_VERSION,
    SCC_PARSE_BAD_OPCODE,
    SCC_PARSE_BAD_STATUS,
    SCC_PARSE_BAD_FLAGS,
    SCC_PARSE_BAD_RESERVED,
    SCC_PARSE_BAD_LENGTH,
    SCC_PARSE_BAD_PAYLOAD,
    SCC_PARSE_MISMATCH
};

struct scc_request_view {
    uint16_t version;
    uint16_t opcode;
    uint32_t request_id;
    uint16_t payload_length;
    const uint8_t *payload;
    uint64_t after_sequence;
    uint32_t category_mask;
    uint16_t max_records;
};

struct scc_response_view {
    uint16_t version;
    uint16_t opcode;
    uint32_t request_id;
    uint32_t status;
    uint16_t payload_length;
    uint64_t event_cursor;
    const uint8_t *payload;
    uint16_t event_count;
};

/*
 * Pure framing/shape validation only. These functions do not authenticate the
 * peer, validate handle rights, or establish that telemetry is trustworthy.
 * Returned payload pointers refer to the caller-owned input buffer. Output
 * structures are assigned only on SCC_PARSE_OK; ignore them on every error.
 */
enum scc_parse_result scc_parse_request(
    const uint8_t *bytes, size_t length, struct scc_request_view *out);

enum scc_parse_result scc_parse_response(
    const uint8_t *bytes, size_t length, uint16_t expected_opcode,
    uint32_t expected_request_id, uint16_t requested_max_records,
    uint64_t prior_cursor, uint64_t expected_event_sequence,
    struct scc_response_view *out);

#endif
