#ifndef PROSENJIT_PAINT_CORE_H
#define PROSENJIT_PAINT_CORE_H
#include <stddef.h>
#include <stdint.h>
#define PROS_PAINT_MAX_DIMENSION 4096u
#define PROS_PAINT_HISTORY_DEPTH 8u
struct pros_paint_canvas { uint32_t width,height; uint32_t *pixels; uint32_t *history[PROS_PAINT_HISTORY_DEPTH]; uint32_t history_count,history_cursor; };
enum pros_paint_status { PROS_PAINT_OK=0, PROS_PAINT_EINVAL=-1, PROS_PAINT_ENOMEM=-2, PROS_PAINT_EFORMAT=-3, PROS_PAINT_EIO=-4, PROS_PAINT_ENOHISTORY=-5 };
int pros_paint_init(struct pros_paint_canvas *,uint32_t,uint32_t,uint32_t);
void pros_paint_destroy(struct pros_paint_canvas *);
int pros_paint_clear(struct pros_paint_canvas *,uint32_t);
int pros_paint_set_pixel(struct pros_paint_canvas *,int32_t,int32_t,uint32_t);
int pros_paint_line(struct pros_paint_canvas *,int32_t,int32_t,int32_t,int32_t,uint32_t,uint32_t);
int pros_paint_rectangle(struct pros_paint_canvas *,int32_t,int32_t,int32_t,int32_t,uint32_t,uint32_t,int);
int pros_paint_ellipse(struct pros_paint_canvas *,int32_t,int32_t,int32_t,int32_t,uint32_t,uint32_t,int);
int pros_paint_flood_fill(struct pros_paint_canvas *,int32_t,int32_t,uint32_t);
int pros_paint_undo(struct pros_paint_canvas *);
int pros_paint_redo(struct pros_paint_canvas *);
int pros_paint_save_bmp(const struct pros_paint_canvas *,const char *);
int pros_paint_load_bmp(struct pros_paint_canvas *,const char *);
#endif
