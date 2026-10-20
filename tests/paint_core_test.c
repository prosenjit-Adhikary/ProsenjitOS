#include "../user/apps/paint/paint_core.h"
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); return EXIT_FAILURE; } } while (0)
#include <stdio.h>
#include <stdlib.h>
int main(void){
 struct pros_paint_canvas c;const uint32_t white=0xffffffffu,black=0xff000000u,red=0xffff0000u;
 CHECK(pros_paint_init(&c,16,12,white)==PROS_PAINT_OK);
 CHECK(pros_paint_init(&c,0,12,white)==PROS_PAINT_EINVAL);
 CHECK(pros_paint_undo(&c)==PROS_PAINT_ENOHISTORY);
 CHECK(pros_paint_line(&c,INT32_MIN,0,INT32_MAX,0,red,1)==PROS_PAINT_EINVAL);
 CHECK(pros_paint_line(&c,1,1,8,1,red,1)==PROS_PAINT_OK);
 CHECK(c.pixels[20]==red);CHECK(pros_paint_undo(&c)==PROS_PAINT_OK);CHECK(c.pixels[20]==white);
 CHECK(pros_paint_redo(&c)==PROS_PAINT_OK);CHECK(c.pixels[20]==red);
 CHECK(pros_paint_rectangle(&c,2,2,6,6,black,1,1)==PROS_PAINT_OK);CHECK(c.pixels[3*16+3]==black);
 CHECK(pros_paint_undo(&c)==PROS_PAINT_OK);CHECK(c.pixels[3*16+3]==white);
 CHECK(pros_paint_redo(&c)==PROS_PAINT_OK);CHECK(c.pixels[3*16+3]==black);
 CHECK(pros_paint_undo(&c)==PROS_PAINT_OK);
 CHECK(pros_paint_rectangle(&c,2,2,6,6,black,1,1)==PROS_PAINT_OK);
 CHECK(pros_paint_redo(&c)==PROS_PAINT_ENOHISTORY);
 CHECK(pros_paint_flood_fill(&c,0,0,0xff00ff00u)==PROS_PAINT_OK);CHECK(c.pixels[0]==0xff00ff00u);
 CHECK(pros_paint_ellipse(&c,8,2,13,7,red,1,0)==PROS_PAINT_OK);
 CHECK(pros_paint_save_bmp(&c,"paint-test.bmp")==PROS_PAINT_OK);CHECK(pros_paint_clear(&c,black)==PROS_PAINT_OK);
 CHECK(pros_paint_load_bmp(&c,"paint-test.bmp")==PROS_PAINT_OK);CHECK(c.width==16&&c.height==12);
 CHECK(c.pixels[0]==0xff00ff00u);CHECK(c.pixels[3*16+3]==black);
 CHECK(pros_paint_load_bmp(&c,"missing-paint-file.bmp")==PROS_PAINT_EIO);
 { FILE *bad=fopen("bad-paint.bmp","wb");CHECK(bad);CHECK(fwrite("BM",1,2,bad)==2);CHECK(fclose(bad)==0); }
 CHECK(pros_paint_load_bmp(&c,"bad-paint.bmp")==PROS_PAINT_EFORMAT);
 CHECK(c.width==16&&c.height==12&&c.pixels[0]==0xff00ff00u);
 remove("bad-paint.bmp");
 remove("paint-test.bmp");pros_paint_destroy(&c);puts("ProsenjitOS Paint core tests passed");return 0;
}
