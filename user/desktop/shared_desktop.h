#ifndef PROSENJIT_SHARED_DESKTOP_H
#define PROSENJIT_SHARED_DESKTOP_H
#include <stdint.h>
#include "abi/desktop.h"
#define PROS_SD_ABI_VERSION 1u
#define PROS_SD_MAX_PROCESSES 16u
#define PROS_SD_MAX_WINDOWS 32u
#define PROS_SD_CLIPBOARD_MAX 256u
#define PROS_SD_AUDIO_MAX_SESSIONS 16u
enum pros_sd_status { PROS_SD_OK=0, PROS_SD_EINVAL=-1, PROS_SD_EFULL=-2, PROS_SD_ENOENT=-3, PROS_SD_EPERM=-4, PROS_SD_ESTATE=-5, PROS_SD_ESTALE=-6, PROS_SD_EDISCONNECTED=-7, PROS_SD_EBOUNDS=-8, PROS_SD_ENOTSUP=-9 };
enum pros_sd_subsystem { PROS_SD_NATIVE=1, PROS_SD_WINDOWS=2 };
enum pros_sd_process_state { PROS_SD_PROC_UNUSED=0, PROS_SD_PROC_CONNECTED, PROS_SD_PROC_DISCONNECTED, PROS_SD_PROC_EXITED };
enum pros_sd_window_state { PROS_SD_WIN_UNUSED=0, PROS_SD_WIN_LIVE };
enum pros_sd_event_type { PROS_SD_EVENT_NONE=0, PROS_SD_EVENT_FOCUS, PROS_SD_EVENT_KEY, PROS_SD_EVENT_POINTER, PROS_SD_EVENT_CLIPBOARD_CHANGED, PROS_SD_EVENT_PROCESS_EXIT, PROS_SD_EVENT_SUBSYSTEM_LOST };
enum pros_sd_clipboard_rights { PROS_SD_CLIPBOARD_READ=1u, PROS_SD_CLIPBOARD_WRITE=2u };
struct pros_sd_event { uint32_t abi_version, struct_size, type, flags; uint64_t target_handle, owner_process; int32_t value0, value1; };
struct pros_sd_process { uint64_t id; uint32_t generation, subsystem, state, clipboard_rights; };
struct pros_sd_window { uint64_t handle, owner_process; uint32_t generation, state; };
struct pros_sd_audio_session { uint64_t handle, owner_process; uint32_t generation, active; };
struct pros_sd_context {
 struct pros_sd_process processes[PROS_SD_MAX_PROCESSES];
 struct pros_sd_window windows[PROS_SD_MAX_WINDOWS];
 struct pros_sd_audio_session audio[PROS_SD_AUDIO_MAX_SESSIONS];
 uint64_t focused_window, next_handle;
 uint32_t event_count, disconnected_mask, clipboard_size;
 unsigned char clipboard[PROS_SD_CLIPBOARD_MAX];
};
int pros_sd_init(struct pros_sd_context *);
int pros_sd_connect(struct pros_sd_context *, uint64_t, uint32_t, uint32_t);
int pros_sd_disconnect(struct pros_sd_context *, uint64_t);
int pros_sd_process_exit(struct pros_sd_context *, uint64_t);
int pros_sd_window_create(struct pros_sd_context *, uint64_t, uint64_t *);
int pros_sd_window_focus(struct pros_sd_context *, uint64_t, uint64_t);
int pros_sd_window_destroy(struct pros_sd_context *, uint64_t, uint64_t);
int pros_sd_input_route(struct pros_sd_context *, uint32_t, int32_t, int32_t, struct pros_sd_event *);
int pros_sd_clipboard_write(struct pros_sd_context *, uint64_t, const void *, uint32_t);
int pros_sd_clipboard_read(struct pros_sd_context *, uint64_t, void *, uint32_t, uint32_t *);
int pros_sd_audio_open(struct pros_sd_context *, uint64_t, uint64_t *);
int pros_sd_audio_close(struct pros_sd_context *, uint64_t, uint64_t);
int pros_sd_validate_event(const struct pros_sd_event *);
/* Validates the existing Part 1 wire ABI before state/ownership checks. */
int pros_sd_validate_request(const struct pros_desktop_message_header *, uint64_t);
#endif
