#include "paint_frontend.h"

static int frontend_valid(const struct pros_paint_frontend *f)
{
    return f && f->canvas && f->canvas->pixels && f->canvas->width &&
           f->canvas->height;
}

int pros_paint_frontend_init(struct pros_paint_frontend *f,
                             struct pros_paint_canvas *canvas)
{
    if (!f || !canvas || !canvas->pixels || !canvas->width || !canvas->height)
        return PROS_PAINT_EINVAL;
    f->canvas = canvas;
    f->color = 0xff000000u;
    f->brush_width = 1u;
    f->tool = PROS_PAINT_TOOL_PENCIL;
    f->filled = 0;
    f->dragging = 0;
    f->anchor_x = 0;
    f->anchor_y = 0;
    return PROS_PAINT_OK;
}

int pros_paint_frontend_set_tool(struct pros_paint_frontend *f,
                                 enum pros_paint_tool tool)
{
    if (!frontend_valid(f) || tool < PROS_PAINT_TOOL_PENCIL ||
        tool > PROS_PAINT_TOOL_FILL)
        return PROS_PAINT_EINVAL;
    f->tool = tool;
    f->dragging = 0;
    return PROS_PAINT_OK;
}

int pros_paint_frontend_set_color(struct pros_paint_frontend *f, uint32_t color)
{
    if (!frontend_valid(f)) return PROS_PAINT_EINVAL;
    f->color = color;
    return PROS_PAINT_OK;
}

int pros_paint_frontend_set_brush_width(struct pros_paint_frontend *f,
                                        uint32_t width)
{
    if (!frontend_valid(f) || !width || width > 128u)
        return PROS_PAINT_EINVAL;
    f->brush_width = width;
    return PROS_PAINT_OK;
}

int pros_paint_frontend_set_filled(struct pros_paint_frontend *f, int filled)
{
    if (!frontend_valid(f) || (filled != 0 && filled != 1))
        return PROS_PAINT_EINVAL;
    f->filled = filled;
    return PROS_PAINT_OK;
}

int pros_paint_frontend_pointer_down(struct pros_paint_frontend *f,
                                     int32_t x, int32_t y)
{
    if (!frontend_valid(f) || x < 0 || y < 0 ||
        (uint32_t)x >= f->canvas->width || (uint32_t)y >= f->canvas->height)
        return PROS_PAINT_EINVAL;
    if (f->tool == PROS_PAINT_TOOL_FILL)
        return pros_paint_flood_fill(f->canvas, x, y, f->color);
    f->anchor_x = x;
    f->anchor_y = y;
    f->dragging = 1;
    return PROS_PAINT_OK;
}

int pros_paint_frontend_pointer_up(struct pros_paint_frontend *f,
                                   int32_t x, int32_t y)
{
    int rc;
    if (!frontend_valid(f) || !f->dragging || x < 0 || y < 0 ||
        (uint32_t)x >= f->canvas->width || (uint32_t)y >= f->canvas->height)
        return PROS_PAINT_EINVAL;
    f->dragging = 0;
    switch (f->tool) {
    case PROS_PAINT_TOOL_PENCIL:
    case PROS_PAINT_TOOL_LINE:
        rc = pros_paint_line(f->canvas, f->anchor_x, f->anchor_y, x, y,
                             f->color, f->brush_width);
        break;
    case PROS_PAINT_TOOL_RECTANGLE:
        rc = pros_paint_rectangle(f->canvas, f->anchor_x, f->anchor_y, x, y,
                                  f->color, f->brush_width, f->filled);
        break;
    case PROS_PAINT_TOOL_ELLIPSE:
        rc = pros_paint_ellipse(f->canvas, f->anchor_x, f->anchor_y, x, y,
                                f->color, f->brush_width, f->filled);
        break;
    default:
        return PROS_PAINT_EINVAL;
    }
    return rc;
}

int pros_paint_frontend_undo(struct pros_paint_frontend *f)
{
    if (!frontend_valid(f)) return PROS_PAINT_EINVAL;
    f->dragging = 0;
    return pros_paint_undo(f->canvas);
}

int pros_paint_frontend_redo(struct pros_paint_frontend *f)
{
    if (!frontend_valid(f)) return PROS_PAINT_EINVAL;
    f->dragging = 0;
    return pros_paint_redo(f->canvas);
}
