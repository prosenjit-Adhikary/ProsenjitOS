#include "abi/desktop.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, code) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return (code); \
    } \
} while (0)

int main(void)
{
    struct pros_desktop_message_header header = {
        PROSENJIT_DESKTOP_ABI_VERSION,
        sizeof(struct pros_desktop_message_header),
        PROS_DESKTOP_MSG_WINDOW_CREATE, 0u, 0u, 0u
    };
    struct pros_desktop_rect rect = {0, 0, 1920u, 1080u};
    struct pros_desktop_window_request window;
    struct pros_desktop_input_event input;
    struct pros_desktop_clipboard_request clipboard;
    struct pros_desktop_audio_request audio;

    CHECK(pros_desktop_message_header_valid(&header), 1);
    CHECK(!pros_desktop_message_fits(&header, 0u), 25);
    CHECK(pros_desktop_message_fits(&header, sizeof(header)), 21);
    CHECK(!pros_desktop_message_fits(&header, sizeof(header) - 1u), 22);
    header.payload_size = 16u;
    CHECK(!pros_desktop_message_fits(&header, sizeof(header)), 23);
    CHECK(pros_desktop_message_fits(&header, sizeof(header) + 16u), 24);
    header.payload_size = 0u;
    CHECK(!pros_desktop_message_header_valid(0), 2);
    header.version++;
    CHECK(!pros_desktop_message_header_valid(&header), 3);
    header.version = PROSENJIT_DESKTOP_ABI_VERSION;
    header.flags = 1u;
    CHECK(!pros_desktop_message_header_valid(&header), 4);
    header.flags = 0u;
    header.type = UINT32_MAX;
    CHECK(!pros_desktop_message_header_valid(&header), 5);
    header.type = PROS_DESKTOP_MSG_WINDOW_CREATE;
    header.payload_size = PROS_DESKTOP_MAX_MESSAGE_BYTES + 1u;
    CHECK(!pros_desktop_message_header_valid(&header), 6);
    header.payload_size = 0u;
    header.struct_size = PROS_DESKTOP_MAX_MESSAGE_BYTES + 1u;
    CHECK(!pros_desktop_message_header_valid(&header), 19);
    header.struct_size = sizeof(header);
    header.payload_size = PROS_DESKTOP_MAX_MESSAGE_BYTES;
    CHECK(!pros_desktop_message_header_valid(&header), 20);
    header.payload_size = 0u;

    CHECK(pros_desktop_rect_valid(&rect), 7);
    rect.width = 0u;
    CHECK(!pros_desktop_rect_valid(&rect), 8);
    rect.width = PROS_DESKTOP_MAX_DIMENSION + 1u;
    CHECK(!pros_desktop_rect_valid(&rect), 9);
    rect.width = 2u;
    rect.x = INT32_MAX;
    CHECK(!pros_desktop_rect_valid(&rect), 10);
    rect.x = 0;
    rect.height = 1u;
    CHECK(pros_desktop_rect_valid(&rect), 11);

    CHECK(pros_desktop_clipboard_size_valid(0u), 12);
    CHECK(pros_desktop_clipboard_size_valid(
              PROS_DESKTOP_MAX_CLIPBOARD_BYTES), 13);
    CHECK(!pros_desktop_clipboard_size_valid(
              PROS_DESKTOP_MAX_CLIPBOARD_BYTES + 1u), 14);


    (void)memset(&window, 0, sizeof(window));
    window.header.version = PROSENJIT_DESKTOP_ABI_VERSION;
    window.header.struct_size = sizeof(window);
    window.header.type = PROS_DESKTOP_MSG_WINDOW_CREATE;
    window.bounds = rect;
    CHECK(pros_desktop_window_request_valid(&window, sizeof(window)), 28);
    CHECK(!pros_desktop_window_request_valid(&window, sizeof(window) - 1u), 29);
    window.header.type = PROS_DESKTOP_MSG_WINDOW_FOCUS;
    window.bounds.width = 0u;
    CHECK(pros_desktop_window_request_valid(&window, sizeof(window)), 30);
    window.header.type = PROS_DESKTOP_MSG_WINDOW_CREATE;
    CHECK(!pros_desktop_window_request_valid(&window, sizeof(window)), 31);
    window.bounds = rect;
    window.reserved = 1u;
    CHECK(!pros_desktop_window_request_valid(&window, sizeof(window)), 32);
    window.reserved = 0u;
    window.window_flags = 1u; /* No window flags are defined in ABI v1. */
    CHECK(!pros_desktop_window_request_valid(&window, sizeof(window)), 46);

    (void)memset(&input, 0, sizeof(input));
    input.header.version = PROSENJIT_DESKTOP_ABI_VERSION;
    input.header.struct_size = sizeof(input);
    input.header.type = PROS_DESKTOP_MSG_INPUT_KEY;
    input.input_flags = PROS_DESKTOP_INPUT_PRESS;
    CHECK(pros_desktop_input_event_valid(&input, sizeof(input)), 33);
    input.input_flags = 0u;
    CHECK(!pros_desktop_input_event_valid(&input, sizeof(input)), 42);
    input.input_flags = PROS_DESKTOP_INPUT_REPEAT;
    CHECK(!pros_desktop_input_event_valid(&input, sizeof(input)), 43);
    input.input_flags = PROS_DESKTOP_INPUT_PRESS | PROS_DESKTOP_INPUT_REPEAT;
    CHECK(pros_desktop_input_event_valid(&input, sizeof(input)), 44);
    input.input_flags = PROS_DESKTOP_INPUT_RELATIVE;
    CHECK(!pros_desktop_input_event_valid(&input, sizeof(input)), 34);
    input.input_flags = PROS_DESKTOP_INPUT_PRESS | PROS_DESKTOP_INPUT_RELEASE;
    CHECK(!pros_desktop_input_event_valid(&input, sizeof(input)), 35);
    input.header.type = PROS_DESKTOP_MSG_INPUT_POINTER;
    input.input_flags = PROS_DESKTOP_INPUT_RELATIVE;
    CHECK(pros_desktop_input_event_valid(&input, sizeof(input)), 36);
    input.input_flags = PROS_DESKTOP_INPUT_REPEAT;
    CHECK(!pros_desktop_input_event_valid(&input, sizeof(input)), 45);
    input.input_flags = 0x80000000u;
    CHECK(!pros_desktop_input_event_valid(&input, sizeof(input)), 37);

    (void)memset(&clipboard, 0, sizeof(clipboard));
    clipboard.header.version = PROSENJIT_DESKTOP_ABI_VERSION;
    clipboard.header.struct_size = sizeof(clipboard);
    clipboard.header.type = PROS_DESKTOP_MSG_CLIPBOARD_READ;
    CHECK(pros_desktop_clipboard_request_valid(&clipboard,
                                                sizeof(clipboard)), 38);
    clipboard.header.payload_size = 1u;
    CHECK(!pros_desktop_clipboard_request_valid(
              &clipboard, sizeof(clipboard) + 1u), 39);
    clipboard.header.type = PROS_DESKTOP_MSG_CLIPBOARD_WRITE;
    clipboard.header.payload_size = 4u;
    CHECK(pros_desktop_clipboard_request_valid(
              &clipboard, sizeof(clipboard) + 4u), 40);
    clipboard.reserved = 1u;
    CHECK(!pros_desktop_clipboard_request_valid(
              &clipboard, sizeof(clipboard) + 4u), 41);

    (void)memset(&audio, 0, sizeof(audio));
    audio.header.version = PROSENJIT_DESKTOP_ABI_VERSION;
    audio.header.struct_size = sizeof(audio);
    audio.header.type = PROS_DESKTOP_MSG_AUDIO_SUBMIT;
    audio.header.payload_size = 480u * 2u * 2u;
    audio.sample_rate = 48000u;
    audio.channels = 2u;
    audio.format = PROS_DESKTOP_AUDIO_S16_LE;
    audio.frame_count = 480u;
    CHECK(pros_desktop_audio_request_valid(
              &audio, sizeof(audio) + audio.header.payload_size), 15);
    CHECK(!pros_desktop_audio_request_valid(&audio, sizeof(audio) - 1u), 26);
    CHECK(!pros_desktop_audio_request_valid(
              &audio, sizeof(audio) + audio.header.payload_size - 1u), 27);
    audio.channels = 9u;
    CHECK(!pros_desktop_audio_request_valid(
              &audio, sizeof(audio) + audio.header.payload_size), 16);
    audio.channels = 2u;
    audio.header.payload_size = 1u;
    CHECK(!pros_desktop_audio_request_valid(
              &audio, sizeof(audio) + audio.header.payload_size), 17);
    audio.header.payload_size = PROS_DESKTOP_MAX_AUDIO_BYTES + 1u;
    audio.frame_count = 100000u;
    CHECK(!pros_desktop_audio_request_valid(
              &audio, sizeof(audio) + audio.header.payload_size), 18);

    (void)puts("shared desktop ABI tests passed");
    return 0;
}
