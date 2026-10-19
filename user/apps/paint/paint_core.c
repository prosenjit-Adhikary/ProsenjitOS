#include "paint_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define BMP_HEADER_SIZE 54u
#define PAINT_MAX_PIXELS ((size_t)PROS_PAINT_MAX_DIMENSION * PROS_PAINT_MAX_DIMENSION)
static size_t pixel_count(const struct pros_paint_canvas *c){return (size_t)c->width*c->height;}
static int valid(const struct pros_paint_canvas *c){return c&&c->pixels&&c->width&&c->height&&c->width<=PROS_PAINT_MAX_DIMENSION&&c->height<=PROS_PAINT_MAX_DIMENSION;}
static int coordinate_valid(int32_t v){return v>=-(int32_t)PROS_PAINT_MAX_DIMENSION&&v<=(int32_t)(2u*PROS_PAINT_MAX_DIMENSION);}
static void discard_redo(struct pros_paint_canvas *c){while(c->history_count>c->history_cursor){free(c->history[--c->history_count]);c->history[c->history_count]=NULL;}}
static int snapshot(struct pros_paint_canvas *c){
 size_t bytes; uint32_t *copy; if(!valid(c))return PROS_PAINT_EINVAL; discard_redo(c);
 if(c->history_count==PROS_PAINT_HISTORY_DEPTH){free(c->history[0]);memmove(&c->history[0],&c->history[1],(PROS_PAINT_HISTORY_DEPTH-1u)*sizeof(c->history[0]));--c->history_count;if(c->history_cursor)--c->history_cursor;}
 bytes=pixel_count(c)*sizeof(*copy);copy=(uint32_t*)malloc(bytes);if(!copy)return PROS_PAINT_ENOMEM;memcpy(copy,c->pixels,bytes);c->history[c->history_count++]=copy;c->history_cursor=c->history_count;return PROS_PAINT_OK;
}
int pros_paint_init(struct pros_paint_canvas *c,uint32_t w,uint32_t h,uint32_t bg){
 size_t n;if(!c||!w||!h||w>PROS_PAINT_MAX_DIMENSION||h>PROS_PAINT_MAX_DIMENSION)return PROS_PAINT_EINVAL;memset(c,0,sizeof(*c));n=(size_t)w*h;if(n>PAINT_MAX_PIXELS||n>SIZE_MAX/sizeof(uint32_t))return PROS_PAINT_EINVAL;
 c->pixels=(uint32_t*)malloc(n*sizeof(uint32_t));if(!c->pixels)return PROS_PAINT_ENOMEM;c->width=w;c->height=h;for(size_t i=0;i<n;++i)c->pixels[i]=bg;return PROS_PAINT_OK;
}
void pros_paint_destroy(struct pros_paint_canvas *c){if(!c)return;free(c->pixels);for(uint32_t i=0;i<c->history_count;++i)free(c->history[i]);memset(c,0,sizeof(*c));}
int pros_paint_clear(struct pros_paint_canvas *c,uint32_t color){int rc=snapshot(c);if(rc)return rc;for(size_t i=0;i<pixel_count(c);++i)c->pixels[i]=color;return PROS_PAINT_OK;}
int pros_paint_set_pixel(struct pros_paint_canvas *c,int32_t x,int32_t y,uint32_t color){if(!valid(c))return PROS_PAINT_EINVAL;if(x<0||y<0||(uint32_t)x>=c->width||(uint32_t)y>=c->height)return PROS_PAINT_OK;c->pixels[(size_t)y*c->width+(uint32_t)x]=color;return PROS_PAINT_OK;}
static void thick_pixel(struct pros_paint_canvas *c,int32_t x,int32_t y,uint32_t color,uint32_t t){int32_t l=(int32_t)(t/2u),r=(int32_t)((t-1u)/2u);for(int32_t yy=y-l;yy<=y+r;++yy)for(int32_t xx=x-l;xx<=x+r;++xx)(void)pros_paint_set_pixel(c,xx,yy,color);}
int pros_paint_line(struct pros_paint_canvas *c,int32_t x0,int32_t y0,int32_t x1,int32_t y1,uint32_t color,uint32_t t){
 int32_t dx,sx,dy,sy,err;int rc;if(!valid(c)||!t||t>128u||!coordinate_valid(x0)||!coordinate_valid(y0)||!coordinate_valid(x1)||!coordinate_valid(y1))return PROS_PAINT_EINVAL;rc=snapshot(c);if(rc)return rc;dx=abs(x1-x0);sx=x0<x1?1:-1;dy=-abs(y1-y0);sy=y0<y1?1:-1;err=dx+dy;
 for(;;){thick_pixel(c,x0,y0,color,t);if(x0==x1&&y0==y1)break;{int32_t e2=2*err;if(e2>=dy){err+=dy;x0+=sx;}if(e2<=dx){err+=dx;y0+=sy;}}}return PROS_PAINT_OK;
}
int pros_paint_rectangle(struct pros_paint_canvas *c,int32_t x0,int32_t y0,int32_t x1,int32_t y1,uint32_t color,uint32_t t,int filled){
 int32_t x,y,l,r,top,bottom;int rc;if(!valid(c)||!t||t>128u||!coordinate_valid(x0)||!coordinate_valid(y0)||!coordinate_valid(x1)||!coordinate_valid(y1))return PROS_PAINT_EINVAL;rc=snapshot(c);if(rc)return rc;l=x0<x1?x0:x1;r=x0>x1?x0:x1;top=y0<y1?y0:y1;bottom=y0>y1?y0:y1;
 if(filled){for(y=top;y<=bottom;++y)for(x=l;x<=r;++x)(void)pros_paint_set_pixel(c,x,y,color);}else for(uint32_t i=0;i<t;++i){for(x=l;x<=r;++x){(void)pros_paint_set_pixel(c,x,top+(int32_t)i,color);(void)pros_paint_set_pixel(c,x,bottom-(int32_t)i,color);}for(y=top;y<=bottom;++y){(void)pros_paint_set_pixel(c,l+(int32_t)i,y,color);(void)pros_paint_set_pixel(c,r-(int32_t)i,y,color);}}return PROS_PAINT_OK;
}
int pros_paint_ellipse(struct pros_paint_canvas *c,int32_t x0,int32_t y0,int32_t x1,int32_t y1,uint32_t color,uint32_t t,int filled){
 int32_t l,r,top,bottom;double cx,cy,rx,ry;int rc;if(!valid(c)||!t||t>128u||!coordinate_valid(x0)||!coordinate_valid(y0)||!coordinate_valid(x1)||!coordinate_valid(y1))return PROS_PAINT_EINVAL;rc=snapshot(c);if(rc)return rc;l=x0<x1?x0:x1;r=x0>x1?x0:x1;top=y0<y1?y0:y1;bottom=y0>y1?y0:y1;cx=((double)l+r)/2.0;cy=((double)top+bottom)/2.0;rx=((double)r-l+1.0)/2.0;ry=((double)bottom-top+1.0)/2.0;
 if(rx<0.5||ry<0.5) return PROS_PAINT_OK;
 for(int32_t y=top;y<=bottom;++y) {
  for(int32_t x=l;x<=r;++x) {
   double nx=(x-cx)/rx,ny=(y-cy)/ry,d=nx*nx+ny*ny;
   if((filled&&d<=1.0)||(!filled&&d<=1.0&&d>=1.0-(double)t*(2.0/rx+2.0/ry)))
    (void)pros_paint_set_pixel(c,x,y,color);
  }
 }
 return PROS_PAINT_OK;
}
int pros_paint_flood_fill(struct pros_paint_canvas *c,int32_t x,int32_t y,uint32_t color){
 size_t n,head=0,tail=0,*q;
 uint32_t target;
 int rc;
 if(!valid(c)||x<0||y<0||(uint32_t)x>=c->width||(uint32_t)y>=c->height)
  return PROS_PAINT_EINVAL;
 target=c->pixels[(size_t)y*c->width+(uint32_t)x];
 if(target==color)return PROS_PAINT_OK;
 n=pixel_count(c);
 if(n>SIZE_MAX/sizeof(*q))return PROS_PAINT_ENOMEM;
 q=(size_t*)malloc(n*sizeof(*q));
 if(!q)return PROS_PAINT_ENOMEM;
 rc=snapshot(c);
 if(rc){free(q);return rc;}
 q[tail++]=(size_t)y*c->width+(uint32_t)x;
 c->pixels[q[0]]=color;
 while(head<tail){
  size_t p=q[head++],xx=p%c->width,yy=p/c->width,nb[4];
  uint32_t count=0;
  if(xx)nb[count++]=p-1;
  if(xx+1<c->width)nb[count++]=p+1;
  if(yy)nb[count++]=p-c->width;
  if(yy+1<c->height)nb[count++]=p+c->width;
  for(uint32_t i=0;i<count;++i){
   size_t z=nb[i];
   if(c->pixels[z]==target){
    c->pixels[z]=color;
    q[tail++]=z;
   }
  }
 }
 free(q);
 return PROS_PAINT_OK;
}
int pros_paint_undo(struct pros_paint_canvas *c){
 size_t bytes;uint32_t *current;uint32_t idx;if(!valid(c))return PROS_PAINT_EINVAL;if(!c->history_cursor)return PROS_PAINT_ENOHISTORY;
 bytes=pixel_count(c)*sizeof(*current);current=(uint32_t*)malloc(bytes);if(!current)return PROS_PAINT_ENOMEM;memcpy(current,c->pixels,bytes);idx=--c->history_cursor;
 memcpy(c->pixels,c->history[idx],bytes);free(c->history[idx]);c->history[idx]=current;return PROS_PAINT_OK;
}
int pros_paint_redo(struct pros_paint_canvas *c){
 size_t bytes;uint32_t *current;uint32_t idx;if(!valid(c))return PROS_PAINT_EINVAL;if(c->history_cursor>=c->history_count)return PROS_PAINT_ENOHISTORY;
 bytes=pixel_count(c)*sizeof(*current);current=(uint32_t*)malloc(bytes);if(!current)return PROS_PAINT_ENOMEM;memcpy(current,c->pixels,bytes);idx=c->history_cursor++;
 memcpy(c->pixels,c->history[idx],bytes);free(c->history[idx]);c->history[idx]=current;return PROS_PAINT_OK;
}
static void put16(uint8_t*p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint16_t get16(const uint8_t*p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t get32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int pros_paint_save_bmp(const struct pros_paint_canvas*c,const char*path){
 FILE*f;uint8_t h[BMP_HEADER_SIZE]={0};size_t row,pad;uint64_t total;if(!valid(c)||!path)return PROS_PAINT_EINVAL;row=(size_t)c->width*3;pad=(4-row%4)%4;total=BMP_HEADER_SIZE+(uint64_t)(row+pad)*c->height;if(total>UINT32_MAX)return PROS_PAINT_EINVAL;f=fopen(path,"wb");if(!f)return PROS_PAINT_EIO;
 h[0]='B';h[1]='M';put32(h+2,(uint32_t)total);put32(h+10,BMP_HEADER_SIZE);put32(h+14,40);put32(h+18,c->width);put32(h+22,c->height);put16(h+26,1);put16(h+28,24);put32(h+34,(uint32_t)(total-BMP_HEADER_SIZE));if(fwrite(h,1,sizeof(h),f)!=sizeof(h)){fclose(f);return PROS_PAINT_EIO;}
 for(uint32_t y=0;y<c->height;++y){uint8_t zero[3]={0};uint32_t yy=c->height-1-y;for(uint32_t x=0;x<c->width;++x){uint32_t p=c->pixels[(size_t)yy*c->width+x];uint8_t bgr[3]={(uint8_t)p,(uint8_t)(p>>8),(uint8_t)(p>>16)};if(fwrite(bgr,1,3,f)!=3){fclose(f);return PROS_PAINT_EIO;}}if(pad&&fwrite(zero,1,pad,f)!=pad){fclose(f);return PROS_PAINT_EIO;}}if(fclose(f))return PROS_PAINT_EIO;return PROS_PAINT_OK;
}
int pros_paint_load_bmp(struct pros_paint_canvas*c,const char*path){
 FILE*f;uint8_t h[BMP_HEADER_SIZE];uint32_t w,off,compression,hs;uint16_t bpp;int32_t sh;size_t row,pad;uint32_t*np;if(!valid(c)||!path)return PROS_PAINT_EINVAL;f=fopen(path,"rb");if(!f)return PROS_PAINT_EIO;
 if(fread(h,1,sizeof(h),f)!=sizeof(h)||h[0]!='B'||h[1]!='M'||get32(h+14)<40||get16(h+26)!=1){fclose(f);return PROS_PAINT_EFORMAT;}off=get32(h+10);w=get32(h+18);sh=(int32_t)get32(h+22);bpp=get16(h+28);compression=get32(h+30);
 if(off<54||get32(h+14)>off-14u||!w||!sh||sh==INT32_MIN||w>PROS_PAINT_MAX_DIMENSION||(uint32_t)(sh<0?-sh:sh)>PROS_PAINT_MAX_DIMENSION||(bpp!=24&&bpp!=32)||compression){fclose(f);return PROS_PAINT_EFORMAT;}hs=(uint32_t)(sh<0?-sh:sh);if((size_t)w*hs>PAINT_MAX_PIXELS){fclose(f);return PROS_PAINT_EFORMAT;}row=(size_t)w*(bpp/8);pad=(4-row%4)%4;np=(uint32_t*)malloc((size_t)w*hs*sizeof(uint32_t));if(!np){fclose(f);return PROS_PAINT_ENOMEM;}if(fseek(f,(long)off,SEEK_SET)){free(np);fclose(f);return PROS_PAINT_EIO;}
 for(uint32_t y=0;y<hs;++y){uint32_t dy=sh>0?hs-1-y:y;for(uint32_t x=0;x<w;++x){uint8_t px[4];if(fread(px,1,bpp/8,f)!=bpp/8){free(np);fclose(f);return PROS_PAINT_EFORMAT;}np[(size_t)dy*w+x]=0xff000000u|((uint32_t)px[2]<<16)|((uint32_t)px[1]<<8)|px[0];}if(pad){uint8_t padding[3];if(fread(padding,1,pad,f)!=pad){free(np);fclose(f);return PROS_PAINT_EFORMAT;}}}fclose(f);
 if(w!=c->width||hs!=c->height){uint32_t*resized=(uint32_t*)realloc(c->pixels,(size_t)w*hs*sizeof(uint32_t));if(!resized){free(np);return PROS_PAINT_ENOMEM;}c->pixels=resized;c->width=w;c->height=hs;}memcpy(c->pixels,np,(size_t)w*hs*sizeof(uint32_t));free(np);for(uint32_t i=0;i<c->history_count;++i)free(c->history[i]);memset(c->history,0,sizeof(c->history));c->history_count=0;c->history_cursor=0;return PROS_PAINT_OK;
}
