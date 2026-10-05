#ifndef PROSENJIT_ABI_GUI_H
#define PROSENJIT_ABI_GUI_H

#include <stdint.h>

/*
 * Stable kernel/user graphics-session message contract.
 *
 * These messages are transported over a capability-authorized IPC endpoint;
 * they are not direct hardware commands. Kernel display/input providers remain
 * privileged and the future graphics/session service owns desktop policy.
 */

#define PROS_GUI_ABI_VERSION 0x00010000ULL
#define PROS_GUI_MAX_WIDTH 4096u
#define PROS_GUI_MAX_HEIGHT 4096u
#define PROS_GUI_MAX_DAMAGE 8u /* fits inside the current bounded IPC message */
#define PROS_GUI_MAX_WINDOWS 256u
#define PROS_GUI_MAX_EVENT_QUEUE 256u

enum pros_gui_message_type {
    PROS_GUI_MSG_CREATE_SURFACE = 1,
    PROS_GUI_MSG_DESTROY_SURFACE = 2,
    PROS_GUI_MSG_DAMAGE = 3,
    PROS_GUI_MSG_MOVE = 4,
    PROS_GUI_MSG_VISIBILITY = 5,
    PROS_GUI_MSG_OPACITY = 6,
    PROS_GUI_MSG_PRESENT = 7,
    PROS_GUI_MSG_DISPLAY_MODE = 8,
    PROS_GUI_MSG_INPUT_EVENT = 9,
    PROS_GUI_MSG_SESSION_READY = 10,
};

enum pros_gui_input_type {
    PROS_GUI_INPUT_KEYBOARD = 1,
    PROS_GUI_INPUT_POINTER = 2,
};

struct pros_gui_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct pros_gui_surface_request {
    uint32_t width;
    uint32_t height;
    uint32_t format; /* ARGB8888 = 1 */
    uint32_t reserved;
} __attribute__((packed));

struct pros_gui_window_request {
    uint64_t surface_handle;
    int32_t x;
    int32_t y;
    uint8_t visible;
    uint8_t opacity;
    uint16_t reserved;
} __attribute__((packed));

struct pros_gui_damage_request {
    uint64_t surface_handle;
    uint32_t count;
    struct pros_gui_rect rects[PROS_GUI_MAX_DAMAGE];
} __attribute__((packed));

struct pros_gui_mode_request {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
} __attribute__((packed));

struct pros_gui_input_event {
    uint64_t sequence;
    uint32_t device_id;
    uint16_t type;
    uint16_t flags;
    int32_t x;
    int32_t y;
    uint32_t code;
    int32_t value;
} __attribute__((packed));

#endif
