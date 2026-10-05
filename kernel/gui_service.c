#include "gui_service.h"
#include "../include/abi/gui.h"
#include "display.h"
#include "graphics.h"

static int rect_valid(const struct pros_gui_rect *r)
{
    if (!r || r->width == 0u || r->height == 0u) return 0;
    if (r->x < 0 || r->y < 0) return 0;
    if ((uint64_t)(uint32_t)r->x + r->width > PROS_GUI_MAX_WIDTH) return 0;
    if ((uint64_t)(uint32_t)r->y + r->height > PROS_GUI_MAX_HEIGHT) return 0;
    return 1;
}

int pros_gui_validate_surface(const struct pros_gui_surface_request *req)
{
    if (!req || req->format != PROS_GFX_FORMAT_ARGB8888 || req->reserved != 0u)
        return -1;
    if (req->width == 0u || req->height == 0u ||
        req->width > PROS_GUI_MAX_WIDTH || req->height > PROS_GUI_MAX_HEIGHT)
        return -1;
    if ((uint64_t)req->width * req->height > UINT64_MAX / sizeof(uint32_t))
        return -1;
    return 0;
}

int pros_gui_validate_window(const struct pros_gui_window_request *req)
{
    if (!req || req->reserved != 0u || req->visible > 1u) return -1;
    if (req->opacity > 255u) return -1;
    if (req->x < -(int32_t)PROS_GUI_MAX_WIDTH || req->x > (int32_t)PROS_GUI_MAX_WIDTH ||
        req->y < -(int32_t)PROS_GUI_MAX_HEIGHT || req->y > (int32_t)PROS_GUI_MAX_HEIGHT)
        return -1;
    return req->surface_handle != 0u ? 0 : -1;
}

int pros_gui_validate_damage(const struct pros_gui_damage_request *req)
{
    if (!req || req->count > PROS_GUI_MAX_DAMAGE || req->surface_handle == 0u)
        return -1;
    for (uint32_t i = 0; i < req->count; ++i)
        if (!rect_valid(&req->rects[i])) return -1;
    return 0;
}

int pros_gui_validate_mode(const struct pros_gui_mode_request *req)
{
    if (!req || req->format != PROS_GFX_FORMAT_ARGB8888 ||
        req->width == 0u || req->height == 0u ||
        req->width > PROS_GUI_MAX_WIDTH || req->height > PROS_GUI_MAX_HEIGHT ||
        req->stride < req->width)
        return -1;
    if ((uint64_t)req->stride * req->height > UINT64_MAX / sizeof(uint32_t))
        return -1;
    return 0;
}

int pros_gui_validate_input(const struct pros_gui_input_event *event)
{
    if (!event || event->sequence == 0u || event->device_id == 0u) return -1;
    if (event->type != PROS_GUI_INPUT_KEYBOARD && event->type != PROS_GUI_INPUT_POINTER)
        return -1;
    return 0;
}

int pros_gui_selftest(void)
{
    struct pros_gui_surface_request surface = {640u, 480u, PROS_GFX_FORMAT_ARGB8888, 0u};
    struct pros_gui_window_request window = {1u, 0, 0, 1u, 255u, 0u};
    struct pros_gui_damage_request damage = {1u, 1u, {{0, 0, 16u, 16u}}};
    struct pros_gui_mode_request mode = {640u, 480u, 640u, PROS_GFX_FORMAT_ARGB8888};
    struct pros_gui_input_event input = {1u, 1u, PROS_GUI_INPUT_KEYBOARD, 0u, 0, 0, 30u, 1};

    if (pros_gui_validate_surface(&surface) != 0 ||
        pros_gui_validate_window(&window) != 0 ||
        pros_gui_validate_damage(&damage) != 0 ||
        pros_gui_validate_mode(&mode) != 0 ||
        pros_gui_validate_input(&input) != 0)
        return -1;
    surface.width = PROS_GUI_MAX_WIDTH + 1u;
    if (pros_gui_validate_surface(&surface) == 0) return -2;
    surface.width = 640u;
    damage.count = PROS_GUI_MAX_DAMAGE + 1u;
    if (pros_gui_validate_damage(&damage) == 0) return -3;
    damage.count = 1u;
    damage.rects[0].x = (int32_t)PROS_GUI_MAX_WIDTH;
    if (pros_gui_validate_damage(&damage) == 0) return -4;
    mode.stride = UINT32_MAX;
    mode.height = UINT32_MAX;
    if (pros_gui_validate_mode(&mode) == 0) return -5;
    input.type = 99u;
    if (pros_gui_validate_input(&input) == 0) return -6;
    return 0;
}
