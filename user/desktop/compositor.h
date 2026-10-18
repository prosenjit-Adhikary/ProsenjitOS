#ifndef PROSENJIT_USER_DESKTOP_COMPOSITOR_H
#define PROSENJIT_USER_DESKTOP_COMPOSITOR_H
#include <stddef.h>
#include <stdint.h>
#define PROS_UI_MAX_WINDOWS 8u
#define PROS_UI_TITLE_MAX 32u
#define PROS_UI_MAGIC 0x50554931u
#define PROS_UI_OK 0
#define PROS_UI_EINVAL -1
#define PROS_UI_ENOSPC -2
#define PROS_UI_ENOENT -3
/* Caller-owned packed 0xAARRGGBB surface; no hardware access occurs here. */
struct pros_ui_surface { uint32_t *pixels, width, height, pitch_pixels; size_t capacity_pixels; };
struct pros_ui_window { uint32_t id; int32_t x, y; uint32_t width, height; char title[PROS_UI_TITLE_MAX]; uint8_t used; };
struct pros_ui_compositor {
 uint32_t magic; struct pros_ui_surface surface;
 struct pros_ui_window windows[PROS_UI_MAX_WINDOWS];
 uint8_t z_order[PROS_UI_MAX_WINDOWS]; uint32_t window_count, next_id, focused_id;
};
int pros_ui_init(struct pros_ui_compositor *, const struct pros_ui_surface *);
int pros_ui_window_create(struct pros_ui_compositor *, const char *, int32_t, int32_t, uint32_t, uint32_t, uint32_t *);
int pros_ui_window_close(struct pros_ui_compositor *, uint32_t);
int pros_ui_window_focus(struct pros_ui_compositor *, uint32_t);
int pros_ui_pointer_press(struct pros_ui_compositor *, int32_t, int32_t, uint32_t *);
int pros_ui_render(struct pros_ui_compositor *);
#endif
