#include "compositor.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c,m) do{if(!(c)){fprintf(stderr,"FAIL: %s\n",m);return 1;}}while(0)
int main(void){
 uint32_t px[160u*120u],a=0,b=0,hit=0;struct pros_ui_surface s={px,160,120,160,160u*120u};struct pros_ui_compositor u;
 memset(px,0,sizeof(px));CHECK(pros_ui_init(&u,&s)==0,"init");CHECK(pros_ui_render(&u)==0,"empty render");
 CHECK(px[0]==0xFF182536u,"desktop background");CHECK(px[119u*160u]==0xFF101923u,"panel");
 CHECK(pros_ui_window_create(&u,"Terminal",12,10,90,70,&a)==0,"create first");
 CHECK(pros_ui_window_create(&u,"Settings",30,20,80,65,&b)==0,"create second");
 CHECK(pros_ui_render(&u)==0,"render windows");CHECK(px[22u*160u+40u]==0xFF167D9Au,"top focused title");
 CHECK(pros_ui_pointer_press(&u,40,30,&hit)==0&&hit==b,"topmost hit");
 CHECK(pros_ui_window_focus(&u,a)==0,"focus old");CHECK(pros_ui_render(&u)==0,"rerender");
 CHECK(px[12u*160u+20u]==0xFF167D9Au,"focus style");
 CHECK(pros_ui_window_close(&u,b)==0,"close");CHECK(pros_ui_window_close(&u,b)==PROS_UI_ENOENT,"stale id");
 CHECK(pros_ui_render(&u)==0,"render after close");
 CHECK(pros_ui_window_create(&u,"../bad",0,0,80,60,&hit)==PROS_UI_EINVAL,"bad title");
 CHECK(pros_ui_window_create(&u,"small",0,0,20,20,&hit)==PROS_UI_EINVAL,"bad geometry");
 s.pitch_pixels=159;CHECK(pros_ui_init(&u,&s)==PROS_UI_EINVAL,"short pitch");
 s.pitch_pixels=160;s.capacity_pixels=1;CHECK(pros_ui_init(&u,&s)==PROS_UI_EINVAL,"short backing");
 puts("desktop compositor/window primitives passed");return 0;
}