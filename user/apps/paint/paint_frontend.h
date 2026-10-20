#ifndef PROSENJIT_PAINT_FRONTEND_H
#define PROSENJIT_PAINT_FRONTEND_H

#include "paint_core.h"

enum pros_paint_tool {
    PROS_PAINT_TOOL_PENCIL = 1,
    PROS_PAINT_TOOL_LINE = 2,
    PROS_PAINT_TOOL_RECTANGLE = 3,
    PROS_PAINT_TOOL_ELLIPSE = 4,
    PROS_PAINT_TOOL_FILL = 5
};

struct pros_paint_frontend {
    struct pros_paint_canvas *canvas;
    uint32_t color;
    uint32_t brush_width;
    enum pros_paint_tool tool;
    int filled;
    int dragging;
    int32_t anchor_x;
    int32_t anchor_y;
};

int pros_paint_frontend_init(struct pros_paint_frontend *, struct pros_paint_canvas *);
int pros_paint_frontend_set_tool(struct pros_paint_frontend *, enum pros_paint_tool);
int pros_paint_frontend_set_color(struct pros_paint_frontend *, uint32_t);
int pros_paint_frontend_set_brush_width(struct pros_paint_frontend *, uint32_t);
int pros_paint_frontend_set_filled(struct pros_paint_frontend *, int);
int pros_paint_frontend_pointer_down(struct pros_paint_frontend *, int32_t, int32_t);
int pros_paint_frontend_pointer_up(struct pros_paint_frontend *, int32_t, int32_t);
int pros_paint_frontend_undo(struct pros_paint_frontend *);
int pros_paint_frontend_redo(struct pros_paint_frontend *);

#endif
