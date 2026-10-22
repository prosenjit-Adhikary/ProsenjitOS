#ifndef PROSENJIT_ABI_DESKTOP_H
#define PROSENJIT_ABI_DESKTOP_H

/*
 * Shared Desktop Integration Layer ABI.
 *
 * This is a userspace/service protocol contract, not a window system or a
 * kernel syscall. Device ownership and user-memory validation remain with
 * the kernel/device broker. Services must validate the full payload again.
 */
#include <stdint.h>

#define PROSENJIT_DESKTOP_ABI_VERSION 1u
#define PROS_DESKTOP_MAX_DIMENSION 16384u
#define PROS_DESKTOP_MAX_CLIPBOARD_BYTES (1024u * 1024u)
#define PROS_DESKTOP_MAX_AUDIO_BYTES (256u * 1024u)
#define PROS_DESKTOP_MAX_MESSAGE_BYTES (1024u * 1024u)

enum pros_desktop_message_type {
    PROS_DESKTOP_MSG_WINDOW_CREATE = 1,
    PROS_DESKTOP_MSG_WINDOW_DESTROY = 2,
    PROS_DESKTOP_MSG_WINDOW_MOVE = 3,
    PROS_DESKTOP_MSG_WINDOW_RESIZE = 4,
    PROS_DESKTOP_MSG_WINDOW_FOCUS = 5,
    PROS_DESKTOP_MSG_FRAME_PRESENT = 6,
    PROS_DESKTOP_MSG_INPUT_KEY = 7,
    PROS_DESKTOP_MSG_INPUT_POINTER = 8,
    PROS_DESKTOP_MSG_CLIPBOARD_READ = 9,
    PROS_DESKTOP_MSG_CLIPBOARD_WRITE = 10,
    PROS_DESKTOP_MSG_AUDIO_SUBMIT = 11,
    PROS_DESKTOP_MSG_AUDIO_CONTROL = 12
};

enum pros_desktop_input_flags {
    PROS_DESKTOP_INPUT_PRESS = 0x0001u,
    PROS_DESKTOP_INPUT_RELEASE = 0x0002u,
    PROS_DESKTOP_INPUT_RELATIVE = 0x0004u,
    PROS_DESKTOP_INPUT_REPEAT = 0x0008u
};

enum pros_desktop_audio_format {
    PROS_DESKTOP_AUDIO_S16_LE = 1u,
    PROS_DESKTOP_AUDIO_F32_LE = 2u
};

/* All messages are size-tagged; unknown flags/reserved values fail closed. */
struct pros_desktop_message_header {
    uint32_t version;
    uint32_t struct_size;
    uint32_t type;
    uint32_t flags;
    uint32_t payload_size;
    uint32_t reserved;
};

struct pros_desktop_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

struct pros_desktop_window_request {
    struct pros_desktop_message_header header;
    uint64_t window_id;
    struct pros_desktop_rect bounds;
    uint32_t window_flags;
    uint32_t reserved;
};

struct pros_desktop_input_event {
    struct pros_desktop_message_header header;
    uint64_t timestamp;
    uint64_t target_window;
    int32_t x;
    int32_t y;
    uint32_t code;
    uint32_t input_flags;
};

struct pros_desktop_clipboard_request {
    struct pros_desktop_message_header header;
    uint64_t transfer_id;
    uint32_t mime_type;
    uint32_t reserved;
};

struct pros_desktop_audio_request {
    struct pros_desktop_message_header header;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t format;
    uint32_t frame_count;
    uint32_t reserved;
};

static inline int pros_desktop_message_header_valid(
    const struct pros_desktop_message_header *header)
{
    if (header == 0)
        return 0;
    if (header->version != PROSENJIT_DESKTOP_ABI_VERSION ||
        header->struct_size < sizeof(*header) ||
        header->struct_size > PROS_DESKTOP_MAX_MESSAGE_BYTES ||
        (uint64_t)header->struct_size + (uint64_t)header->payload_size >
            PROS_DESKTOP_MAX_MESSAGE_BYTES ||
        header->reserved != 0u || header->flags != 0u)
        return 0;
    if (header->type < PROS_DESKTOP_MSG_WINDOW_CREATE ||
        header->type > PROS_DESKTOP_MSG_AUDIO_CONTROL)
        return 0;
    return 1;
}

/*
 * Validate the received transport buffer as well as the declared lengths.
 * Call this before reading any type-specific fields or payload bytes.
 */
static inline int pros_desktop_message_fits(
    const struct pros_desktop_message_header *header,
    uint64_t received_bytes)
{
    uint64_t declared_bytes;

    if (!pros_desktop_message_header_valid(header))
        return 0;
    declared_bytes = (uint64_t)header->struct_size +
                     (uint64_t)header->payload_size;
    return declared_bytes <= received_bytes;
}

/* Reject zero-sized, oversized, and signed-coordinate overflow rectangles. */
static inline int pros_desktop_rect_valid(const struct pros_desktop_rect *rect)
{
    int64_t right;
    int64_t bottom;

    if (rect == 0 || rect->width == 0u || rect->height == 0u ||
        rect->width > PROS_DESKTOP_MAX_DIMENSION ||
        rect->height > PROS_DESKTOP_MAX_DIMENSION)
        return 0;
    right = (int64_t)rect->x + (int64_t)rect->width;
    bottom = (int64_t)rect->y + (int64_t)rect->height;
    return right <= INT32_MAX && right >= INT32_MIN &&
           bottom <= INT32_MAX && bottom >= INT32_MIN;
}

/* Clipboard and audio requests use separate, stricter per-operation limits. */
static inline int pros_desktop_clipboard_size_valid(uint32_t bytes)
{
    return bytes <= PROS_DESKTOP_MAX_CLIPBOARD_BYTES;
}

static inline int pros_desktop_audio_request_valid(
    const struct pros_desktop_audio_request *request)
{
    uint64_t bytes_per_sample;
    uint64_t total_bytes;

    if (request == 0 ||
        !pros_desktop_message_header_valid(&request->header) ||
        request->header.type != PROS_DESKTOP_MSG_AUDIO_SUBMIT ||
        request->header.struct_size < sizeof(*request) ||
        request->reserved != 0u ||
        request->sample_rate < 8000u || request->sample_rate > 192000u ||
        request->channels == 0u || request->channels > 8u ||
        request->frame_count == 0u)
        return 0;
    if (request->format == PROS_DESKTOP_AUDIO_S16_LE)
        bytes_per_sample = 2u;
    else if (request->format == PROS_DESKTOP_AUDIO_F32_LE)
        bytes_per_sample = 4u;
    else
        return 0;
    total_bytes = (uint64_t)request->frame_count *
                  (uint64_t)request->channels * bytes_per_sample;
    return total_bytes <= PROS_DESKTOP_MAX_AUDIO_BYTES &&
           request->header.payload_size == total_bytes;
}

#endif
