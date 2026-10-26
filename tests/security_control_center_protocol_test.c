#include "telemetry_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void request_header(uint8_t *b, uint16_t opcode, uint32_t id,
                           uint16_t payload_length)
{
    memset(b, 0, SCC_IPC_MESSAGE_MAX);
    put16(b, SCC_PROTOCOL_VERSION);
    put16(b + 2, opcode);
    put32(b + 4, id);
    put16(b + 8, payload_length);
}

static void response_header(uint8_t *b, uint16_t opcode, uint32_t id,
                            uint32_t status, uint16_t payload_length,
                            uint64_t cursor)
{
    memset(b, 0, SCC_IPC_MESSAGE_MAX);
    put16(b, SCC_PROTOCOL_VERSION);
    put16(b + 2, opcode);
    put32(b + 4, id);
    put32(b + 8, status);
    put16(b + 12, payload_length);
    put64(b + 16, cursor);
}

static void event_record(uint8_t *p, uint64_t sequence)
{
    memset(p, 0, SCC_EVENT_RECORD_SIZE);
    put64(p, sequence);
}

static void test_request_frames(void)
{
    uint8_t b[SCC_IPC_MESSAGE_MAX + 1];
    struct scc_request_view v;
    size_t n;

    assert(scc_parse_request(NULL, 0, &v) == SCC_PARSE_INVALID_ARGUMENT);
    assert(scc_parse_request(b, 16, NULL) == SCC_PARSE_INVALID_ARGUMENT);
    {
        uint8_t unaligned[SCC_IPC_MESSAGE_MAX + 1];
        struct scc_request_view before;
        memset(&before, 0xA5, sizeof(before));
        memcpy(&v, &before, sizeof(v));
        assert(scc_parse_request(b, 15, &v) == SCC_PARSE_SHORT_FRAME);
        assert(memcmp(&v, &before, sizeof(v)) == 0);
        request_header(unaligned + 1, SCC_OP_GET_STATUS, 5, 0);
        assert(scc_parse_request(unaligned + 1, 16, &v) == SCC_PARSE_OK);
        assert(v.request_id == 5);
    }
    for (n = 0; n < SCC_REQUEST_HEADER_SIZE; ++n)
        assert(scc_parse_request(b, n, &v) == SCC_PARSE_SHORT_FRAME);
    assert(scc_parse_request(b, 65, &v) == SCC_PARSE_OVERSIZED_FRAME);

    request_header(b, SCC_OP_GET_STATUS, 0x12345678u, 0);
    assert(scc_parse_request(b, 16, &v) == SCC_PARSE_OK);
    assert(v.opcode == SCC_OP_GET_STATUS && v.request_id == 0x12345678u);
    assert(v.payload_length == 0);

    put16(b, 2);
    assert(scc_parse_request(b, 16, &v) == SCC_PARSE_BAD_VERSION);
    request_header(b, 99, 1, 0);
    assert(scc_parse_request(b, 16, &v) == SCC_PARSE_BAD_OPCODE);
    request_header(b, SCC_OP_GET_STATUS, 1, 1);
    assert(scc_parse_request(b, 17, &v) == SCC_PARSE_BAD_PAYLOAD);
    request_header(b, SCC_OP_GET_STATUS, 1, 0);
    put16(b + 10, 1);
    assert(scc_parse_request(b, 16, &v) == SCC_PARSE_BAD_FLAGS);
    request_header(b, SCC_OP_GET_STATUS, 1, 0);
    put32(b + 12, 1);
    assert(scc_parse_request(b, 16, &v) == SCC_PARSE_BAD_RESERVED);

    request_header(b, SCC_OP_LIST_EVENTS, 7, 16);
    put64(b + 16, 42);
    put32(b + 24, 0xA5u);
    put16(b + 28, 1);
    assert(scc_parse_request(b, 32, &v) == SCC_PARSE_OK);
    assert(v.after_sequence == 42 && v.category_mask == 0xA5u);
    assert(v.max_records == 1);
    put16(b + 28, 0);
    assert(scc_parse_request(b, 32, &v) == SCC_PARSE_BAD_PAYLOAD);
    put16(b + 28, 2);
    assert(scc_parse_request(b, 32, &v) == SCC_PARSE_BAD_PAYLOAD);
    put16(b + 28, 1);
    put16(b + 30, 1);
    assert(scc_parse_request(b, 32, &v) == SCC_PARSE_BAD_RESERVED);

    request_header(b, SCC_OP_GET_EVENT, 8, 8);
    put64(b + 16, 123);
    assert(scc_parse_request(b, 24, &v) == SCC_PARSE_OK);
    assert(v.payload_length == 8);
    put64(b + 16, 0);
    assert(scc_parse_request(b, 24, &v) == SCC_PARSE_BAD_PAYLOAD);
    put64(b + 16, 123);
    assert(scc_parse_request(b, 23, &v) == SCC_PARSE_BAD_LENGTH);
}

static void test_response_frames(void)
{
    uint8_t b[SCC_IPC_MESSAGE_MAX + 1];
    struct scc_response_view v;
    size_t n;

    assert(scc_parse_response(NULL, 0, SCC_OP_GET_STATUS, 1, 0, 0, 0, &v)
           == SCC_PARSE_INVALID_ARGUMENT);
    assert(scc_parse_response(b, 24, SCC_OP_GET_STATUS, 1, 0, 0, 0, NULL)
           == SCC_PARSE_INVALID_ARGUMENT);
    {
        uint8_t unaligned[SCC_IPC_MESSAGE_MAX + 1];
        uint8_t before[sizeof(v)];
        memset(&v, 0x5A, sizeof(v));
        memcpy(before, &v, sizeof(v));
        response_header(b, SCC_OP_GET_STATUS, 1, SCC_STATUS_OK, 24, 0);
        assert(scc_parse_response(b, 23, SCC_OP_GET_STATUS, 1, 0, 0, 0, &v)
               == SCC_PARSE_SHORT_FRAME);
        assert(memcmp(&v, before, sizeof(v)) == 0);
        response_header(unaligned + 1, SCC_OP_GET_STATUS, 6, SCC_STATUS_OK, 24, 0);
        put16(unaligned + 1 + 24, 1);
        put16(unaligned + 1 + 26, 2);
        assert(scc_parse_response(unaligned + 1, 48, SCC_OP_GET_STATUS, 6, 0, 0, 0, &v)
               == SCC_PARSE_OK);
        assert(v.request_id == 6);
    }
    for (n = 0; n < SCC_RESPONSE_HEADER_SIZE; ++n)
        assert(scc_parse_response(b, n, SCC_OP_GET_STATUS, 1, 0, 0, 0, &v)
               == SCC_PARSE_SHORT_FRAME);
    assert(scc_parse_response(b, 65, SCC_OP_GET_STATUS, 1, 0, 0, 0, &v)
           == SCC_PARSE_OVERSIZED_FRAME);

    response_header(b, SCC_OP_GET_STATUS, 9, SCC_STATUS_OK, 24, 0);
    put16(b, 2);
    assert(scc_parse_response(b, 48, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_BAD_VERSION);
    response_header(b, SCC_OP_GET_STATUS, 9, SCC_STATUS_OK, 24, 0);
    put16(b + 14, 1);
    assert(scc_parse_response(b, 48, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_BAD_FLAGS);
    put16(b + 24, 1);
    put16(b + 26, 2);
    assert(scc_parse_response(b, 48, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_OK);
    assert(v.payload_length == 24 && v.event_cursor == 0);
    put16(b + 26, 99);
    assert(scc_parse_response(b, 48, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_BAD_PAYLOAD);
    put16(b + 26, 2);
    put64(b + 16, 5);
    assert(scc_parse_response(b, 48, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_BAD_PAYLOAD);
    put64(b + 16, 0);
    assert(scc_parse_response(b, 48, SCC_OP_GET_STATUS, 10, 0, 0, 0, &v)
           == SCC_PARSE_MISMATCH);
    put32(b + 8, 99);
    assert(scc_parse_response(b, 48, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_BAD_STATUS);

    response_header(b, SCC_OP_GET_STATUS, 9, SCC_STATUS_UNAVAILABLE, 0, 0);
    assert(scc_parse_response(b, 24, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_OK);
    response_header(b, SCC_OP_GET_STATUS, 9, SCC_STATUS_UNAVAILABLE, 1, 0);
    assert(scc_parse_response(b, 25, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_BAD_PAYLOAD);
    response_header(b, SCC_OP_GET_STATUS, 9, SCC_STATUS_UNAVAILABLE, 0, 1);
    assert(scc_parse_response(b, 24, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_BAD_PAYLOAD);

    response_header(b, SCC_OP_LIST_EVENTS, 12, SCC_STATUS_OK, 0, 41);
    assert(scc_parse_response(b, 24, SCC_OP_LIST_EVENTS, 12, 1, 41, 0, &v)
           == SCC_PARSE_OK);
    response_header(b, SCC_OP_LIST_EVENTS, 12, SCC_STATUS_OK, 0, 42);
    assert(scc_parse_response(b, 24, SCC_OP_LIST_EVENTS, 12, 1, 41, 0, &v)
           == SCC_PARSE_BAD_PAYLOAD);

    response_header(b, SCC_OP_LIST_EVENTS, 12, SCC_STATUS_OK, 24, 42);
    event_record(b + 24, 42);
    assert(scc_parse_response(b, 48, SCC_OP_LIST_EVENTS, 12, 1, 41, 0, &v)
           == SCC_PARSE_OK);
    assert(v.event_count == 1 && v.event_cursor == 42);
    assert(scc_parse_response(b, 48, SCC_OP_LIST_EVENTS, 12, 1, 42, 0, &v)
           == SCC_PARSE_BAD_PAYLOAD);
    put64(b + 16, 41);
    assert(scc_parse_response(b, 48, SCC_OP_LIST_EVENTS, 12, 1, 41, 0, &v)
           == SCC_PARSE_BAD_PAYLOAD);

    response_header(b, SCC_OP_LIST_EVENTS, 12, SCC_STATUS_OK, 48, 43);
    assert(scc_parse_response(b, 65, SCC_OP_LIST_EVENTS, 12, 1, 41, 0, &v)
           == SCC_PARSE_OVERSIZED_FRAME);

    response_header(b, SCC_OP_GET_EVENT, 15, SCC_STATUS_OK, 24, 123);
    event_record(b + 24, 123);
    assert(scc_parse_response(b, 48, SCC_OP_GET_EVENT, 15, 0, 0, 123, &v)
           == SCC_PARSE_OK);
    put64(b + 16, 124);
    assert(scc_parse_response(b, 48, SCC_OP_GET_EVENT, 15, 0, 0, 123, &v)
           == SCC_PARSE_BAD_PAYLOAD);
    put64(b + 16, 124);
    put64(b + 24, 124);
    assert(scc_parse_response(b, 48, SCC_OP_GET_EVENT, 15, 0, 0, 123, &v)
           == SCC_PARSE_BAD_PAYLOAD);

    response_header(b, SCC_OP_GET_STATUS, 9, SCC_STATUS_OK, 25, 0);
    assert(scc_parse_response(b, 49, SCC_OP_GET_STATUS, 9, 0, 0, 0, &v)
           == SCC_PARSE_BAD_PAYLOAD);
}

static void test_deterministic_fuzz_smoke(void)
{
    uint8_t b[SCC_IPC_MESSAGE_MAX + 1];
    struct scc_request_view rq;
    struct scc_response_view rs;
    size_t n;
    size_t i;

    for (n = 0; n <= SCC_IPC_MESSAGE_MAX + 1; ++n) {
        for (i = 0; i < n; ++i)
            b[i] = (uint8_t)((i * 37u + n * 13u) & 0xffu);
        (void)scc_parse_request(b, n, &rq);
        (void)scc_parse_response(b, n, SCC_OP_GET_STATUS, 0, 0, 0, 0, &rs);
    }
}

int main(void)
{
    test_request_frames();
    test_response_frames();
    test_deterministic_fuzz_smoke();
    puts("security control center protocol parser tests passed");
    return 0;
}
