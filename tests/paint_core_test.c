#include "../user/apps/paint/paint_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
 struct pros_paint_canvas c;const uint32_t white=0xffffffffu,black=0xff000000u,red=0xffff0000u;
 assert(pros_paint_init(&c,16,12,white)==PROS_PAINT_OK);
 assert(pros_paint_init(&c,0,12,white)==PROS_PAINT_EINVAL);
 assert(pros_paint_line(&c,1,1,8,1,red,1)==PROS_PAINT_OK);
 assert(c.pixels[20]==red);assert(pros_paint_undo(&c)==PROS_PAINT_OK);assert(c.pixels[20]==white);
 assert(pros_paint_redo(&c)==PROS_PAINT_OK);assert(c.pixels[20]==red);
 assert(pros_paint_rectangle(&c,2,2,6,6,black,1,1)==PROS_PAINT_OK);assert(c.pixels[3*16+3]==black);
 assert(pros_paint_flood_fill(&c,0,0,0xff00ff00u)==PROS_PAINT_OK);assert(c.pixels[0]==0xff00ff00u);
 assert(pros_paint_ellipse(&c,8,2,13,7,red,1,0)==PROS_PAINT_OK);
 assert(pros_paint_save_bmp(&c,"paint-test.bmp")==PROS_PAINT_OK);assert(pros_paint_clear(&c,black)==PROS_PAINT_OK);
 assert(pros_paint_load_bmp(&c,"paint-test.bmp")==PROS_PAINT_OK);assert(c.width==16&&c.height==12);
 assert(c.pixels[0]==0xff00ff00u);assert(c.pixels[3*16+3]==black);
 assert(pros_paint_load_bmp(&c,"missing-paint-file.bmp")==PROS_PAINT_EIO);
 remove("paint-test.bmp");pros_paint_destroy(&c);puts("ProsenjitOS Paint core tests passed");return 0;
}
