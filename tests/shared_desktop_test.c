#include "shared_desktop.h"
#include <stdio.h>
#include <string.h>
#define C(x,n) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",n,#x);return n;}}while(0)
int main(void){struct pros_sd_context c;struct pros_sd_event e;uint64_t w1,w2,au;char b[32]={0};uint32_t n=0;
C(pros_sd_init(&c)==0,1);C(pros_sd_connect(&c,101,PROS_SD_NATIVE,3)==0,2);C(pros_sd_connect(&c,202,PROS_SD_WINDOWS,PROS_SD_CLIPBOARD_READ)==0,3);C(pros_sd_connect(&c,303,9,0)==PROS_SD_EINVAL,4);
C(pros_sd_window_create(&c,101,&w1)==0&&w1,5);C(pros_sd_window_create(&c,202,&w2)==0&&w2!=w1,6);C(pros_sd_window_focus(&c,202,w1)==0,7);C(pros_sd_input_route(&c,PROS_SD_EVENT_KEY,42,1,&e)==0&&e.target_handle==w1&&e.owner_process==101,8);C(pros_sd_validate_event(&e)==0,9);
C(pros_sd_window_focus(&c,101,0)==PROS_SD_EINVAL,10);C(pros_sd_window_destroy(&c,202,w1)==PROS_SD_EPERM,11);C(pros_sd_window_focus(&c,202,0xdeadbeef)==PROS_SD_ESTALE,12);
C(pros_sd_clipboard_write(&c,101,"secret",6)==0,13);C(pros_sd_clipboard_read(&c,202,b,sizeof(b),&n)==0&&n==6&&!memcmp(b,"secret",6),14);C(pros_sd_clipboard_write(&c,202,"no",2)==PROS_SD_EPERM,15);C(pros_sd_clipboard_read(&c,101,b,2,&n)==PROS_SD_EBOUNDS,16);C(pros_sd_clipboard_write(&c,101,b,PROS_SD_CLIPBOARD_MAX+1)==PROS_SD_EBOUNDS,17);
C(pros_sd_audio_open(&c,101,&au)==0&&au,18);C(pros_sd_audio_close(&c,202,au)==PROS_SD_EPERM,19);C(pros_sd_disconnect(&c,101)==0,20);C(pros_sd_input_route(&c,PROS_SD_EVENT_POINTER,1,2,&e)==PROS_SD_ENOENT,21);C(pros_sd_window_destroy(&c,202,w1)==PROS_SD_ESTALE,22);C(pros_sd_audio_close(&c,202,au)==PROS_SD_ESTALE,23);C(pros_sd_process_exit(&c,202)==0,24);C(pros_sd_process_exit(&c,202)==PROS_SD_ENOENT,25);C(pros_sd_window_create(&c,202,&w1)==PROS_SD_ENOENT,26);C(pros_sd_validate_event(0)==PROS_SD_EINVAL,27);
{
struct pros_desktop_window_request r; memset(&r,0,sizeof(r));
r.header.version=PROSENJIT_DESKTOP_ABI_VERSION;
r.header.struct_size=sizeof(r); r.header.type=PROS_DESKTOP_MSG_WINDOW_CREATE;
r.bounds.width=640; r.bounds.height=480;
C(pros_sd_validate_request(&r.header,sizeof(r))==PROS_SD_OK,29);
r.header.flags=1;
C(pros_sd_validate_request(&r.header,sizeof(r))==PROS_SD_EINVAL,30);
r.header.flags=0; r.header.type=PROS_DESKTOP_MSG_FRAME_PRESENT;
C(pros_sd_validate_request(&r.header,sizeof(r))==PROS_SD_ENOTSUP,31);
}
memset(&e,0,sizeof(e));e.abi_version=99;e.struct_size=sizeof(e);e.type=PROS_SD_EVENT_KEY;C(pros_sd_validate_event(&e)==PROS_SD_EINVAL,32);puts("shared desktop integration tests passed (host state model only)");return 0;}
