#include "display_input.h"
int pros_display_init(struct pros_display_device*d,uint32_t w,uint32_t h,uint32_t*fb){if(!d||!fb||w==0||h==0||w>8192u||h>8192u)return -1;d->width=w;d->height=h;d->stride=w;d->framebuffer=fb;d->active=0;return 0;}
int pros_display_set_active(struct pros_display_device*d,uint8_t a){if(!d)return -1;d->active=a?1u:0u;return 0;}
int pros_input_validate(const struct pros_input_event*e){if(!e||e->type==0)return -1;return 0;}
int pros_display_input_selftest(void){uint32_t fb[4]={0};struct pros_display_device d;struct pros_input_event e={1,30,1,1};if(pros_display_init(&d,2,2,fb)!=0||pros_display_set_active(&d,1)!=0||!d.active||pros_input_validate(&e)!=0)return -1;return 0;}
