#include "telemetry_service.h"

#include <string.h>

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_u64(uint8_t *p, uint64_t v)
{
    write_u32(p, (uint32_t)v);
    write_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t parse_status(enum scc_parse_result result)
{
    if (result == SCC_PARSE_BAD_VERSION)
        return SCC_STATUS_UNSUPPORTED_VERSION;
    if (result == SCC_PARSE_BAD_OPCODE)
        return SCC_STATUS_UNSUPPORTED_OPCODE;
    return SCC_STATUS_INVALID_ARGUMENT;
}

static int emit_response(uint8_t *out, size_t capacity, size_t *length_out,
                         uint16_t opcode, uint32_t request_id,
                         uint32_t status, const uint8_t *payload,
                         uint16_t payload_length, uint64_t cursor)
{
    size_t total = SCC_RESPONSE_HEADER_SIZE + (size_t)payload_length;

    if (out == NULL || length_out == NULL ||
        capacity < SCC_RESPONSE_HEADER_SIZE ||
        total > capacity || total > SCC_IPC_MESSAGE_MAX ||
        (status != SCC_STATUS_OK && (payload_length != 0 || cursor != 0)))
        return -1;

    /* Deterministic initialization prevents stack/buffer residue disclosure. */
    memset(out, 0, total);
    write_u16(out, SCC_PROTOCOL_VERSION);
    write_u16(out + 2, opcode);
    write_u32(out + 4, request_id);
    write_u32(out + 8, status);
    write_u16(out + 12, payload_length);
    write_u16(out + 14, 0);
    write_u64(out + 16, cursor);
    if (payload_length != 0) {
        if (payload == NULL)
            return -1;
        memcpy(out + SCC_RESPONSE_HEADER_SIZE, payload, payload_length);
    }
    *length_out = total;
    return 0;
}

int scc_service_dispatch(const struct scc_service_backend *backend,
                         int peer_authorized,
                         const uint8_t *request, size_t request_length,
                         uint8_t *response, size_t response_capacity,
                         size_t *response_length)
{
    struct scc_request_view view;
    enum scc_parse_result parsed;
    uint8_t payload[SCC_RESPONSE_PAYLOAD_MAX];
    uint16_t payload_length = 0;
    uint64_t cursor = 0;
    uint32_t status = SCC_STATUS_OK;

    if (response_length == NULL)
        return -1;
    *response_length = 0;
    if (response == NULL || response_capacity < SCC_RESPONSE_HEADER_SIZE)
        return -1;
    memset(payload, 0, sizeof(payload));

    parsed = scc_parse_request(request, request_length, &view);
    if (parsed != SCC_PARSE_OK) {
        uint16_t opcode;
        uint32_t request_id;

        /*
         * Only answer when the fixed header can be correlated safely. Never
         * emit an opcode-zero response that a conforming client cannot parse.
         */
        if (request == NULL || request_length < SCC_REQUEST_HEADER_SIZE ||
            request_length > SCC_IPC_MESSAGE_MAX ||
            read_u16(request) != SCC_PROTOCOL_VERSION)
            return -1;
        opcode = read_u16(request + 2);
        if (opcode < SCC_OP_GET_STATUS || opcode > SCC_OP_GET_EVENT)
            return -1;
        request_id = read_u32(request + 4);
        return emit_response(response, response_capacity, response_length,
                             opcode, request_id, parse_status(parsed),
                             NULL, 0, 0);
    }

    /* Only the exact authenticated-allow value may cross this boundary. */
    if (peer_authorized != 1)
        return emit_response(response, response_capacity, response_length,
                             view.opcode, view.request_id, SCC_STATUS_DENIED,
                             NULL, 0, 0);

    if (backend == NULL)
        status = SCC_STATUS_UNAVAILABLE;
    else if (view.opcode == SCC_OP_GET_STATUS) {
        if (backend->get_status == NULL) {
            status = SCC_STATUS_UNAVAILABLE;
        } else {
            int rc = backend->get_status(backend->context, payload);
            if (rc != SCC_STATUS_OK)
                status = rc >= SCC_STATUS_UNSUPPORTED_VERSION &&
                         rc <= SCC_STATUS_INTERNAL_ERROR ?
                         (uint32_t)rc : SCC_STATUS_INTERNAL_ERROR;
            else if (read_u16(payload + 2) > 3)
                status = SCC_STATUS_INTERNAL_ERROR;
            else
                payload_length = 24;
        }
    } else if (view.opcode == SCC_OP_LIST_EVENTS) {
        if (backend->list_events == NULL) {
            status = SCC_STATUS_UNAVAILABLE;
        } else {
            uint16_t max_payload = (uint16_t)(view.max_records * SCC_EVENT_RECORD_SIZE);
            int rc = backend->list_events(backend->context,
                                          view.after_sequence,
                                          view.category_mask,
                                          view.max_records, payload,
                                          &payload_length, &cursor);
            if (rc != SCC_STATUS_OK) {
                status = rc >= SCC_STATUS_UNSUPPORTED_VERSION &&
                         rc <= SCC_STATUS_INTERNAL_ERROR ?
                         (uint32_t)rc : SCC_STATUS_INTERNAL_ERROR;
            } else if (payload_length > max_payload ||
                       payload_length % SCC_EVENT_RECORD_SIZE != 0) {
                status = SCC_STATUS_INTERNAL_ERROR;
            } else {
                uint64_t previous = view.after_sequence;
                uint16_t count = (uint16_t)(payload_length / SCC_EVENT_RECORD_SIZE);
                for (uint16_t i = 0; i < count; ++i) {
                    uint64_t sequence = read_u64(payload + ((size_t)i * SCC_EVENT_RECORD_SIZE));
                    if (sequence <= previous) {
                        status = SCC_STATUS_INTERNAL_ERROR;
                        break;
                    }
                    previous = sequence;
                }
                if (status == SCC_STATUS_OK && cursor != previous)
                    status = SCC_STATUS_INTERNAL_ERROR;
            }
        }
    } else if (view.opcode == SCC_OP_GET_EVENT) {
        uint64_t sequence = read_u64(view.payload);
        if (backend->get_event == NULL) {
            status = SCC_STATUS_UNAVAILABLE;
        } else {
            int rc = backend->get_event(backend->context, sequence, payload);
            if (rc != SCC_STATUS_OK) {
                status = rc >= SCC_STATUS_UNSUPPORTED_VERSION &&
                         rc <= SCC_STATUS_INTERNAL_ERROR ?
                         (uint32_t)rc : SCC_STATUS_INTERNAL_ERROR;
            } else if (read_u64(payload) != sequence) {
                status = SCC_STATUS_INTERNAL_ERROR;
            } else {
                payload_length = SCC_EVENT_RECORD_SIZE;
                cursor = sequence;
            }
        }
    } else {
        status = SCC_STATUS_UNSUPPORTED_OPCODE;
    }

    if (status != SCC_STATUS_OK) {
        payload_length = 0;
        cursor = 0;
    }
    return emit_response(response, response_capacity, response_length,
                         view.opcode, view.request_id, status,
                         payload_length != 0 ? payload : NULL,
                         payload_length, cursor);
}
