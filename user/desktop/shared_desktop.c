#include "shared_desktop.h"
#include <string.h>
static struct pros_sd_process *pf(struct pros_sd_context *c,uint64_t id){uint32_t i;if(!c||!id)return 0;for(i=0;i<PROS_SD_MAX_PROCESSES;i++)if(c->processes[i].id==id&&c->processes[i].state)return &c->processes[i];return 0;}
static struct pros_sd_process *pl(struct pros_sd_context *c,uint64_t id){struct pros_sd_process*p=pf(c,id);return p&&p->state==PROS_SD_PROC_CONNECTED?p:0;}
static struct pros_sd_window *wf(struct pros_sd_context*c,uint64_t h){uint32_t i;if(!c||!h)return 0;for(i=0;i<PROS_SD_MAX_WINDOWS;i++)if(c->windows[i].handle==h&&c->windows[i].state)return &c->windows[i];return 0;}
static struct pros_sd_audio_session *af(struct pros_sd_context*c,uint64_t h){uint32_t i;if(!c||!h)return 0;for(i=0;i<PROS_SD_AUDIO_MAX_SESSIONS;i++)if(c->audio[i].handle==h&&c->audio[i].active)return &c->audio[i];return 0;}
static uint64_t hn(struct pros_sd_context*c){++c->next_handle;if(!c->next_handle)++c->next_handle;return c->next_handle;}
int pros_sd_init(struct pros_sd_context*c){if(!c)return PROS_SD_EINVAL;memset(c,0,sizeof(*c));c->next_handle=0x53440000u;return 0;}
int pros_sd_connect(struct pros_sd_context*c,uint64_t id,uint32_t sub,uint32_t rights){uint32_t i;if(!c||!id||(sub!=PROS_SD_NATIVE&&sub!=PROS_SD_WINDOWS)||(rights&~(PROS_SD_CLIPBOARD_READ|PROS_SD_CLIPBOARD_WRITE)))return PROS_SD_EINVAL;if(pf(c,id))return PROS_SD_ESTATE;for(i=0;i<PROS_SD_MAX_PROCESSES;i++)if(!c->processes[i].state){c->processes[i].id=id;c->processes[i].generation++;c->processes[i].subsystem=sub;c->processes[i].state=PROS_SD_PROC_CONNECTED;c->processes[i].clipboard_rights=rights;return 0;}return PROS_SD_EFULL;}
int pros_sd_disconnect(struct pros_sd_context*c,uint64_t id){struct pros_sd_process*p;uint32_t i;if(!c||!(p=pl(c,id)))return PROS_SD_ENOENT;p->state=PROS_SD_PROC_DISCONNECTED;for(i=0;i<PROS_SD_MAX_WINDOWS;i++)if(c->windows[i].state&&c->windows[i].owner_process==id){if(c->focused_window==c->windows[i].handle)c->focused_window=0;c->windows[i].state=0;c->windows[i].handle=0;}for(i=0;i<PROS_SD_AUDIO_MAX_SESSIONS;i++)if(c->audio[i].active&&c->audio[i].owner_process==id){c->audio[i].active=0;c->audio[i].handle=0;}c->disconnected_mask|=1u<<(p->subsystem-1u);return 0;}
int pros_sd_process_exit(struct pros_sd_context*c,uint64_t id){struct pros_sd_process*p=pf(c,id);int r;if(!p||p->state==PROS_SD_PROC_EXITED)return PROS_SD_ENOENT;if(p->state==PROS_SD_PROC_CONNECTED){r=pros_sd_disconnect(c,id);if(r)return r;}p->state=PROS_SD_PROC_EXITED;return 0;}
int pros_sd_window_create(struct pros_sd_context*c,uint64_t owner,uint64_t*out){uint32_t i;if(!c||!out)return PROS_SD_EINVAL;*out=0;if(!pl(c,owner))return PROS_SD_ENOENT;for(i=0;i<PROS_SD_MAX_WINDOWS;i++)if(!c->windows[i].state){c->windows[i].generation++;c->windows[i].handle=hn(c);c->windows[i].owner_process=owner;c->windows[i].state=PROS_SD_WIN_LIVE;*out=c->windows[i].handle;return 0;}return PROS_SD_EFULL;}
int pros_sd_window_focus(struct pros_sd_context*c,uint64_t req,uint64_t h){struct pros_sd_window*w;if(!c||!h)return PROS_SD_EINVAL;if(!pl(c,req))return PROS_SD_ENOENT;if(!(w=wf(c,h))||w->state!=PROS_SD_WIN_LIVE)return PROS_SD_ESTALE;if(!pl(c,w->owner_process))return PROS_SD_EDISCONNECTED;c->focused_window=h;return 0;}
int pros_sd_window_destroy(struct pros_sd_context*c,uint64_t req,uint64_t h){struct pros_sd_window*w;if(!c||!h)return PROS_SD_EINVAL;if(!pl(c,req))return PROS_SD_ENOENT;if(!(w=wf(c,h))||w->state!=PROS_SD_WIN_LIVE)return PROS_SD_ESTALE;if(w->owner_process!=req)return PROS_SD_EPERM;if(c->focused_window==h)c->focused_window=0;w->state=0;w->handle=0;w->owner_process=0;return 0;}
int pros_sd_input_route(struct pros_sd_context*c,uint32_t type,int32_t v0,int32_t v1,struct pros_sd_event*out){struct pros_sd_window*w;if(!c||!out||(type!=PROS_SD_EVENT_KEY&&type!=PROS_SD_EVENT_POINTER))return PROS_SD_EINVAL;memset(out,0,sizeof(*out));if(!c->focused_window)return PROS_SD_ENOENT;if(!(w=wf(c,c->focused_window))||w->state!=PROS_SD_WIN_LIVE){c->focused_window=0;return PROS_SD_ESTALE;}if(!pl(c,w->owner_process)){c->focused_window=0;return PROS_SD_EDISCONNECTED;}out->abi_version=PROS_SD_ABI_VERSION;out->struct_size=sizeof(*out);out->type=type;out->target_handle=w->handle;out->owner_process=w->owner_process;out->value0=v0;out->value1=v1;c->event_count++;return 0;}
int pros_sd_clipboard_write(struct pros_sd_context*c,uint64_t owner,const void*data,uint32_t size){struct pros_sd_process*p;if(!c||(!data&&size))return PROS_SD_EINVAL;if(size>PROS_SD_CLIPBOARD_MAX)return PROS_SD_EBOUNDS;if(!(p=pl(c,owner)))return PROS_SD_ENOENT;if(!(p->clipboard_rights&PROS_SD_CLIPBOARD_WRITE))return PROS_SD_EPERM;if(size)memcpy(c->clipboard,data,size);if(size<c->clipboard_size)memset(c->clipboard+size,0,c->clipboard_size-size);c->clipboard_size=size;return 0;}
int pros_sd_clipboard_read(struct pros_sd_context*c,uint64_t reader,void*out,uint32_t cap,uint32_t*size){struct pros_sd_process*p;if(!c||!size||(!out&&cap))return PROS_SD_EINVAL;*size=0;if(!(p=pl(c,reader)))return PROS_SD_ENOENT;if(!(p->clipboard_rights&PROS_SD_CLIPBOARD_READ))return PROS_SD_EPERM;if(cap<c->clipboard_size)return PROS_SD_EBOUNDS;if(c->clipboard_size)memcpy(out,c->clipboard,c->clipboard_size);*size=c->clipboard_size;return 0;}
int pros_sd_audio_open(struct pros_sd_context*c,uint64_t owner,uint64_t*out){uint32_t i;if(!c||!out)return PROS_SD_EINVAL;*out=0;if(!pl(c,owner))return PROS_SD_ENOENT;for(i=0;i<PROS_SD_AUDIO_MAX_SESSIONS;i++)if(!c->audio[i].active){c->audio[i].generation++;c->audio[i].handle=hn(c);c->audio[i].owner_process=owner;c->audio[i].active=1;*out=c->audio[i].handle;return 0;}return PROS_SD_EFULL;}
int pros_sd_audio_close(struct pros_sd_context*c,uint64_t owner,uint64_t h){struct pros_sd_audio_session*a;if(!c||!h)return PROS_SD_EINVAL;if(!(a=af(c,h)))return PROS_SD_ESTALE;if(a->owner_process!=owner)return PROS_SD_EPERM;a->active=0;a->handle=0;a->owner_process=0;return 0;}
int pros_sd_validate_event(const struct pros_sd_event*e){if(!e||e->abi_version!=PROS_SD_ABI_VERSION||e->struct_size!=sizeof(*e))return PROS_SD_EINVAL;if(e->type<PROS_SD_EVENT_FOCUS||e->type>PROS_SD_EVENT_SUBSYSTEM_LOST)return PROS_SD_EINVAL;if((e->type==PROS_SD_EVENT_KEY||e->type==PROS_SD_EVENT_POINTER)&&(!e->target_handle||!e->owner_process))return PROS_SD_EINVAL;return 0;}

int pros_sd_validate_request(const struct pros_desktop_message_header *h, uint64_t bytes)
{
    if (!h || !pros_desktop_message_fits(h, bytes))
        return PROS_SD_EINVAL;
    switch (h->type) {
    case PROS_DESKTOP_MSG_WINDOW_CREATE:
    case PROS_DESKTOP_MSG_WINDOW_DESTROY:
    case PROS_DESKTOP_MSG_WINDOW_MOVE:
    case PROS_DESKTOP_MSG_WINDOW_RESIZE:
    case PROS_DESKTOP_MSG_WINDOW_FOCUS:
        return pros_desktop_window_request_valid(
            (const struct pros_desktop_window_request *)h, bytes)
            ? PROS_SD_OK : PROS_SD_EINVAL;
    case PROS_DESKTOP_MSG_INPUT_KEY:
    case PROS_DESKTOP_MSG_INPUT_POINTER:
        return pros_desktop_input_event_valid(
            (const struct pros_desktop_input_event *)h, bytes)
            ? PROS_SD_OK : PROS_SD_EINVAL;
    case PROS_DESKTOP_MSG_CLIPBOARD_READ:
    case PROS_DESKTOP_MSG_CLIPBOARD_WRITE: {
        const struct pros_desktop_clipboard_request *r =
            (const struct pros_desktop_clipboard_request *)h;
        if (h->struct_size != sizeof(*r) || bytes < sizeof(*r) ||
            r->reserved != 0u ||
            r->mime_type < PROS_DESKTOP_CLIPBOARD_MIME_TEXT_UTF8 ||
            r->mime_type > PROS_DESKTOP_CLIPBOARD_MIME_OCTET_STREAM ||
            !pros_desktop_clipboard_size_valid(h->payload_size))
            return PROS_SD_EINVAL;
        if (h->type == PROS_DESKTOP_MSG_CLIPBOARD_READ && h->payload_size != 0u)
            return PROS_SD_EINVAL;
        return PROS_SD_OK;
    }
    case PROS_DESKTOP_MSG_AUDIO_SUBMIT:
        return pros_desktop_audio_request_valid(
            (const struct pros_desktop_audio_request *)h, bytes)
            ? PROS_SD_OK : PROS_SD_EINVAL;
    case PROS_DESKTOP_MSG_FRAME_PRESENT:
    case PROS_DESKTOP_MSG_AUDIO_CONTROL:
        /* Part 1 defines these message types but no complete v1 typed payload. */
        return PROS_SD_ENOTSUP;
    default:
        return PROS_SD_EINVAL;
    }
}
