#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../user/security/telemetry_service.h"

struct fixture { uint32_t calls; int corrupt_list; };

static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void w64(uint8_t *p, uint64_t v) { w32(p,(uint32_t)v); w32(p+4,(uint32_t)(v>>32)); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) | ((uint64_t)r32(p+4)<<32); }

static int status_cb(void *ctx, uint8_t out[24]) {
    struct fixture *f=ctx; ++f->calls; memset(out,0,24); w16(out,1); w16(out+2,1); return SCC_STATUS_OK;
}
static int list_cb(void *ctx, uint64_t after, uint32_t mask, uint16_t max,
                   uint8_t out[SCC_RESPONSE_PAYLOAD_MAX], uint16_t *len, uint64_t *cursor) {
    struct fixture *f=ctx; (void)mask; ++f->calls;
    if (max != 1) return SCC_STATUS_INVALID_ARGUMENT;
    if (after == 0) { memset(out,0,SCC_EVENT_RECORD_SIZE); w64(out,f->corrupt_list ? 0 : 1); w64(out+8,0x123456789ULL); *len=SCC_EVENT_RECORD_SIZE; *cursor=1; }
    else { *len=0; *cursor=after; }
    return SCC_STATUS_OK;
}
static int event_cb(void *ctx, uint64_t seq, uint8_t out[SCC_EVENT_RECORD_SIZE]) {
    struct fixture *f=ctx; ++f->calls; memset(out,0,SCC_EVENT_RECORD_SIZE); w64(out,seq); return SCC_STATUS_OK;
}
static void request(uint8_t *out, uint16_t op, uint32_t id, uint16_t n) {
    memset(out,0,64); w16(out,1); w16(out+2,op); w32(out+4,id); w16(out+8,n);
}

int main(void) {
    struct fixture f={0,0};
    struct scc_service_backend b={&f,status_cb,list_cb,event_cb};
    uint8_t req[64], resp[64];
    size_t n=999;

    request(req,SCC_OP_GET_STATUS,42,0);
    assert(scc_service_dispatch(&b,1,req,16,resp,sizeof(resp),&n)==0);
    assert(n==48 && r16(resp+2)==SCC_OP_GET_STATUS && r32(resp+4)==42);
    assert(r32(resp+8)==SCC_STATUS_OK && r16(resp+12)==24 && r64(resp+16)==0);

    request(req,SCC_OP_GET_STATUS,43,0);
    assert(scc_service_dispatch(&b,0,req,16,resp,sizeof(resp),&n)==0);
    assert(r32(resp+8)==SCC_STATUS_DENIED && r16(resp+12)==0 && r64(resp+16)==0);
    assert(f.calls==1);

    /* Invalid truthy/negative authorization values must fail closed. */
    assert(scc_service_dispatch(&b,-1,req,16,resp,sizeof(resp),&n)==0);
    assert(r32(resp+8)==SCC_STATUS_DENIED && r16(resp+12)==0);
    assert(f.calls==1);
    assert(scc_service_dispatch(&b,2,req,16,resp,sizeof(resp),&n)==0);
    assert(r32(resp+8)==SCC_STATUS_DENIED && r16(resp+12)==0);
    assert(f.calls==1);

    request(req,SCC_OP_LIST_EVENTS,44,16);
    w64(req+16,0); w32(req+24,0); w16(req+28,1); w16(req+30,0);
    assert(scc_service_dispatch(&b,1,req,32,resp,sizeof(resp),&n)==0);
    assert(r32(resp+8)==SCC_STATUS_OK && r16(resp+12)==24 && r64(resp+16)==1);
    assert(r64(resp+24)==1);

    request(req,SCC_OP_GET_EVENT,45,8); w64(req+16,7);
    assert(scc_service_dispatch(&b,1,req,24,resp,sizeof(resp),&n)==0);
    assert(r32(resp+8)==SCC_STATUS_OK && r16(resp+12)==24 && r64(resp+16)==7);
    assert(r64(resp+24)==7);

    request(req,SCC_OP_GET_STATUS,46,0);
    req[10]=1;
    assert(scc_service_dispatch(&b,1,req,16,resp,sizeof(resp),&n)==0);
    assert(r32(resp+8)==SCC_STATUS_INVALID_ARGUMENT && r16(resp+12)==0);

    f.corrupt_list=1;
    request(req,SCC_OP_LIST_EVENTS,47,16);
    w64(req+16,0); w32(req+24,0); w16(req+28,1); w16(req+30,0);
    assert(scc_service_dispatch(&b,1,req,32,resp,sizeof(resp),&n)==0);
    assert(r32(resp+8)==SCC_STATUS_INTERNAL_ERROR && r16(resp+12)==0 && r64(resp+16)==0);
    f.corrupt_list=0;

    request(req,SCC_OP_GET_STATUS,48,0);
    memset(resp,0xA5,sizeof(resp)); n=999;
    assert(scc_service_dispatch(&b,1,req,16,resp,23,&n)==-1);
    assert(n==0);
    assert(f.calls==4);
    return 0;
}
