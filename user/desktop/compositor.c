#include "compositor.h"
#define UI_MAX_DIMENSION 4096u
#define DESKTOP 0xFF182536u
#define PANEL 0xFF101923u
#define WINDOW 0xFFF0F2F5u
#define BORDER 0xFF52657Au
#define TITLE 0xFF34506Eu
#define FOCUSED 0xFF167D9Au
#define WHITE 0xFFFFFFFFu
#define CLOSE 0xFFD95555u
static int surface_valid(const struct pros_ui_surface *s) {
 if(!s||!s->pixels||!s->width||!s->height||s->width>UI_MAX_DIMENSION||
    s->height>UI_MAX_DIMENSION||s->pitch_pixels<s->width||
    (size_t)s->pitch_pixels>SIZE_MAX/(size_t)s->height) return 0;
 return (size_t)s->pitch_pixels*(size_t)s->height<=s->capacity_pixels;
}
static int valid(const struct pros_ui_compositor *u) {
 uint8_t seen[PROS_UI_MAX_WINDOWS]={0};
 uint32_t used=0;
 if(!u||u->magic!=PROS_UI_MAGIC||!surface_valid(&u->surface)||
    u->window_count>PROS_UI_MAX_WINDOWS) return 0;
 for(uint32_t i=0;i<PROS_UI_MAX_WINDOWS;i++) {
  const struct pros_ui_window *w=&u->windows[i];
  if(!w->used) continue;
  used++;
  if(!w->id||w->width<48u||w->height<40u||
     w->width>u->surface.width||w->height>u->surface.height||
     !title_valid(w->title)) return 0;
  for(uint32_t j=0;j<i;j++)
   if(u->windows[j].used&&u->windows[j].id==w->id) return 0;
 }
 if(used!=u->window_count) return 0;
 for(uint32_t i=0;i<u->window_count;i++) {
  uint32_t slot=u->z_order[i];
  if(slot>=PROS_UI_MAX_WINDOWS||!u->windows[slot].used||seen[slot]) return 0;
  seen[slot]=1u;
 }
 for(uint32_t i=u->window_count;i<PROS_UI_MAX_WINDOWS;i++)
  if(u->z_order[i]!=0u) return 0;
 if(u->focused_id&&find(u,u->focused_id)<0) return 0;
 return 1;
}
static int title_valid(const char *s) {
 if(!s||!s[0])return 0;
 for(uint32_t i=0;i<PROS_UI_TITLE_MAX;i++){unsigned char c=(unsigned char)s[i];if(!c)return 1;if(c<0x20||c>0x7e)return 0;}
 return 0;
}
static int find(const struct pros_ui_compositor *u,uint32_t id) {
 for(uint32_t i=0;i<PROS_UI_MAX_WINDOWS;i++)if(u->windows[i].used&&u->windows[i].id==id)return (int)i;
 return -1;
}
int pros_ui_init(struct pros_ui_compositor *u,const struct pros_ui_surface *s) {
 if(!u||!surface_valid(s))return PROS_UI_EINVAL;
 u->magic=0;u->surface=*s;
 for(uint32_t i=0;i<PROS_UI_MAX_WINDOWS;i++){u->windows[i]=(struct pros_ui_window){0};u->z_order[i]=0;}
 u->window_count=0;u->next_id=1;u->focused_id=0;u->magic=PROS_UI_MAGIC;return PROS_UI_OK;
}
int pros_ui_window_create(struct pros_ui_compositor *u,const char *title,int32_t x,int32_t y,uint32_t w,uint32_t h,uint32_t *out) {
 if(!valid(u)||!title_valid(title)||!out||w<48||h<40||w>u->surface.width||h>u->surface.height||
    x<-(int32_t)w||y<-(int32_t)h||(int64_t)x>=u->surface.width||(int64_t)y>=u->surface.height||!u->next_id)return PROS_UI_EINVAL;
 if(u->window_count>=PROS_UI_MAX_WINDOWS)return PROS_UI_ENOSPC;
 int slot=-1;for(uint32_t i=0;i<PROS_UI_MAX_WINDOWS;i++)if(!u->windows[i].used){slot=(int)i;break;}
 if(slot<0)return PROS_UI_ENOSPC;
 struct pros_ui_window *v=&u->windows[slot];v->id=u->next_id++;v->x=x;v->y=y;v->width=w;v->height=h;
 for(uint32_t i=0;i<PROS_UI_TITLE_MAX;i++){v->title[i]=title[i];if(!title[i])break;}
 v->used=1;u->z_order[u->window_count++]=(uint8_t)slot;u->focused_id=v->id;*out=v->id;return PROS_UI_OK;
}
int pros_ui_window_focus(struct pros_ui_compositor *u,uint32_t id) {
 if(!valid(u)||!id)return PROS_UI_EINVAL;int slot=find(u,id);if(slot<0)return PROS_UI_ENOENT;
 uint32_t p;for(p=0;p<u->window_count;p++)if(u->z_order[p]==(uint8_t)slot)break;
 if(p==u->window_count)return PROS_UI_EINVAL;
 for(;p+1<u->window_count;p++)u->z_order[p]=u->z_order[p+1];
 u->z_order[u->window_count-1]=(uint8_t)slot;u->focused_id=id;return PROS_UI_OK;
}
int pros_ui_window_close(struct pros_ui_compositor *u,uint32_t id) {
 if(!valid(u)||!id)return PROS_UI_EINVAL;int slot=find(u,id);if(slot<0)return PROS_UI_ENOENT;
 uint32_t p;for(p=0;p<u->window_count;p++)if(u->z_order[p]==(uint8_t)slot)break;
 if(p==u->window_count)return PROS_UI_EINVAL;
 for(;p+1<u->window_count;p++)u->z_order[p]=u->z_order[p+1];
 u->z_order[--u->window_count]=0;u->windows[slot]=(struct pros_ui_window){0};
 if(u->focused_id==id)u->focused_id=u->window_count?u->windows[u->z_order[u->window_count-1]].id:0;
 return PROS_UI_OK;
}
int pros_ui_pointer_press(struct pros_ui_compositor *u,int32_t x,int32_t y,uint32_t *hit) {
 if(!valid(u)||!hit)return PROS_UI_EINVAL;*hit=0;
 for(uint32_t z=u->window_count;z>0;z--){const struct pros_ui_window *w=&u->windows[u->z_order[z-1]];
  if(x>=w->x&&y>=w->y&&(int64_t)x<(int64_t)w->x+w->width&&(int64_t)y<(int64_t)w->y+w->height){*hit=w->id;return pros_ui_window_focus(u,w->id);}}
 return PROS_UI_OK;
}
static void rect(struct pros_ui_surface *s,int64_t x,int64_t y,int64_t w,int64_t h,uint32_t c) {
 int64_t l=x<0?0:x,t=y<0?0:y,r=x+w,b=y+h;if(r>s->width)r=s->width;if(b>s->height)b=s->height;
 if(l>=r||t>=b)return;
 for(int64_t py=t;py<b;py++){uint32_t *row=s->pixels+(size_t)py*s->pitch_pixels;for(int64_t px=l;px<r;px++)row[px]=c;}
}
static void draw(struct pros_ui_compositor *u,const struct pros_ui_window *w) {
 int64_t x=w->x,y=w->y;uint32_t tc=w->id==u->focused_id?FOCUSED:TITLE;
 rect(&u->surface,x,y,w->width,w->height,BORDER);rect(&u->surface,x+1,y+1,w->width-2,w->height-2,WINDOW);
 rect(&u->surface,x+1,y+1,w->width-2,22,tc);rect(&u->surface,x+(int64_t)w->width-19,y+5,10,10,CLOSE);
 for(uint32_t i=0;i<20&&w->title[i];i++)rect(&u->surface,x+8+(int64_t)i*6,y+8,3,7,WHITE);
 rect(&u->surface,x+10,y+34,w->width>32?w->width-20:0,2,0xFFD2D8DFu);
 rect(&u->surface,x+10,y+42,w->width>50?w->width-42:0,2,0xFFD2D8DFu);
}
int pros_ui_render(struct pros_ui_compositor *u) {
 if(!valid(u))return PROS_UI_EINVAL;rect(&u->surface,0,0,u->surface.width,u->surface.height,DESKTOP);
 rect(&u->surface,0,(int64_t)u->surface.height-28,u->surface.width,28,PANEL);
 rect(&u->surface,8,(int64_t)u->surface.height-22,16,16,FOCUSED);
 for(uint32_t z=0;z<u->window_count;z++){uint32_t slot=u->z_order[z];if(slot>=PROS_UI_MAX_WINDOWS||!u->windows[slot].used)return PROS_UI_EINVAL;draw(u,&u->windows[slot]);}
 return PROS_UI_OK;
}
