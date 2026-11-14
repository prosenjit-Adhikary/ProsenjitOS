#include "usb_msc.h"
#include "prosenjit.h"
#define CBW_LEN 31u
#define CSW_LEN 13u
#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u
#define SENSE_LEN 18u
#define INQUIRY_LEN 36u
#define CAP16_LEN 32u
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t be64(const uint8_t *p){return ((uint64_t)be32(p)<<32)|be32(p+4u);}
static void put_le32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void put_be16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put_be32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void put_be64(uint8_t *p,uint64_t v){put_be32(p,(uint32_t)(v>>32));put_be32(p+4u,(uint32_t)v);}
static void zero_bytes(void *p,uint32_t n){uint8_t *b=(uint8_t *)p;for(uint32_t i=0;i<n;++i)b[i]=0u;}
static int msc_command_try_lock(struct pros_usb_msc *m)
{
    return __atomic_exchange_n(&m->command_lock, 1u, __ATOMIC_ACQUIRE) == 0u;
}
static void msc_command_unlock(struct pros_usb_msc *m)
{
    __atomic_store_n(&m->command_lock, 0u, __ATOMIC_RELEASE);
}
#define MSC_TRANSPORT_RETRY_LIMIT 64u
#define MSC_READ_STAGE_MAX_BYTES (64u * 1024u)
static uint8_t msc_read_stage[MSC_READ_STAGE_MAX_BYTES];
static uint32_t msc_read_stage_lock;
static int msc_read_stage_try_acquire(void)
{
    return __atomic_exchange_n(&msc_read_stage_lock, 1u, __ATOMIC_ACQUIRE) == 0u;
}
static void msc_read_stage_release(void)
{
    __atomic_store_n(&msc_read_stage_lock, 0u, __ATOMIC_RELEASE);
}
static int transport_clear_halt_retry(struct pros_usb_msc *m, uint8_t endpoint)
{
    int rc;
    for (uint32_t attempt = 0u; attempt < MSC_TRANSPORT_RETRY_LIMIT; ++attempt) {
        rc = m->transport.clear_halt(m->transport.context, endpoint);
        if (rc != PROS_USB_MSC_EBUSY)
            return rc;
        __asm__ volatile ("pause" ::: "memory");
    }
    return PROS_USB_MSC_EBUSY;
}
static int transport_reset_recovery_retry(struct pros_usb_msc *m)
{
    int rc;
    for (uint32_t attempt = 0u; attempt < MSC_TRANSPORT_RETRY_LIMIT; ++attempt) {
        rc = m->transport.reset_recovery(m->transport.context,
            m->interface_number, m->endpoint_in, m->endpoint_out);
        if (rc != PROS_USB_MSC_EBUSY)
            return rc;
        __asm__ volatile ("pause" ::: "memory");
    }
    return PROS_USB_MSC_EBUSY;
}
static int recover_bot(struct pros_usb_msc *m){
    if(transport_reset_recovery_retry(m)!=0)m->faulted=1u;
    m->busy=0u;return PROS_USB_MSC_EIO;
}
/* EBUSY means no transfer was posted; retry the current BOT phase, not its CBW. */
static int bulk_out_retry(struct pros_usb_msc *m, uint8_t endpoint,
                          const void *buffer, uint32_t length,
                          uint32_t *actual)
{
    int rc;
    for (uint32_t attempt = 0u; attempt < MSC_TRANSPORT_RETRY_LIMIT; ++attempt) {
        *actual = 0u;
        rc = m->transport.bulk_out(m->transport.context, endpoint, buffer,
                                   length, actual);
        if (rc != PROS_USB_MSC_EBUSY || *actual != 0u)
            return rc;
        __asm__ volatile ("pause" ::: "memory");
    }
    return PROS_USB_MSC_EBUSY;
}
static int bulk_in_retry(struct pros_usb_msc *m, uint8_t endpoint,
                         void *buffer, uint32_t length, uint32_t *actual)
{
    int rc;
    for (uint32_t attempt = 0u; attempt < MSC_TRANSPORT_RETRY_LIMIT; ++attempt) {
        *actual = 0u;
        rc = m->transport.bulk_in(m->transport.context, endpoint, buffer,
                                  length, actual);
        if (rc != PROS_USB_MSC_EBUSY || *actual != 0u)
            return rc;
        __asm__ volatile ("pause" ::: "memory");
    }
    return PROS_USB_MSC_EBUSY;
}
static int issue(struct pros_usb_msc *m,const uint8_t *cdb,uint8_t cdb_len,uint8_t data_in,void *data,uint32_t data_len){
    uint8_t cbw[CBW_LEN],csw[CSW_LEN];uint32_t tag,actual=0u,data_actual=0u,residue;uint8_t data_stalled=0u;int rc;
    if(!m||!cdb||cdb_len==0u||cdb_len>16u||(data_len&&!data))return PROS_USB_MSC_EINVAL;
    if(m->faulted)return PROS_USB_MSC_EIO;
    if(m->busy)return PROS_USB_MSC_EBUSY;
    m->busy=1u;tag=m->next_tag?m->next_tag:1u;m->next_tag=tag+1u;if(m->next_tag==0u)m->next_tag=1u;
    zero_bytes(cbw,sizeof(cbw));put_le32(cbw,CBW_SIG);put_le32(cbw+4u,tag);put_le32(cbw+8u,data_len);
    cbw[12]=data_len?(data_in?0x80u:0u):0u;cbw[13]=m->lun;cbw[14]=cdb_len;
    for(uint32_t i=0;i<cdb_len;++i)cbw[15u+i]=cdb[i];
    actual=0u;rc=bulk_out_retry(m,m->endpoint_out,cbw,sizeof(cbw),&actual);
    if (rc == PROS_USB_MSC_EBUSY && actual == 0u) { m->busy = 0u; return PROS_USB_MSC_EBUSY; }
    if(rc!=0||actual!=sizeof(cbw))return recover_bot(m);
    if(data_len){
        if(data_in){
            actual=0u;rc=bulk_in_retry(m,m->endpoint_in,data,data_len,&actual);
            if(rc==PROS_USB_MSC_XFER_STALL){
                if(transport_clear_halt_retry(m,m->endpoint_in)!=0)return recover_bot(m);
                data_stalled=1u;if(actual>data_len)return recover_bot(m);
                data_actual=actual;
            }else if(rc!=0||actual>data_len)return recover_bot(m);
            else data_actual=actual;
        }else{
            actual=0u;rc=bulk_out_retry(m,m->endpoint_out,data,data_len,&actual);
            if(rc==PROS_USB_MSC_XFER_STALL){
                if(transport_clear_halt_retry(m,m->endpoint_out)!=0)return recover_bot(m);
                data_stalled=1u;if(actual>data_len)return recover_bot(m);
                data_actual=actual;
            }else if(rc!=0||actual>data_len)return recover_bot(m);
            else data_actual=actual;
        }
    }
    actual=0u;rc=bulk_in_retry(m,m->endpoint_in,csw,sizeof(csw),&actual);
    if(rc!=0||actual!=sizeof(csw))return recover_bot(m);
    if(le32(csw)!=CSW_SIG||le32(csw+4u)!=tag)return recover_bot(m);
    residue=le32(csw+8u);if(residue>data_len||csw[12]>2u)return recover_bot(m);
    if(data_len&&(uint64_t)data_actual+(uint64_t)residue!=(uint64_t)data_len)
        return recover_bot(m);
    if(csw[12]==2u)return recover_bot(m);
    m->busy=0u;if(csw[12]==1u)return PROS_USB_MSC_ECOMMAND;
    if(data_stalled)return PROS_USB_MSC_EIO;
    return residue==0u?PROS_USB_MSC_OK:PROS_USB_MSC_EIO;
}
static int request_sense(struct pros_usb_msc *m){
    uint8_t cdb[6]={0x03u,0u,0u,0u,SENSE_LEN,0u},sense[SENSE_LEN];int rc;
    zero_bytes(sense,sizeof(sense));rc=issue(m,cdb,sizeof(cdb),1u,sense,sizeof(sense));
    if(rc==PROS_USB_MSC_OK){m->last_sense_key=sense[2]&0x0Fu;m->last_asc=sense[12];m->last_ascq=sense[13];}
    return rc;
}
static int command(struct pros_usb_msc *m,const uint8_t *cdb,uint8_t cdb_len,uint8_t in,void *data,uint32_t len){
    int rc;
    if (!m)
        return PROS_USB_MSC_EINVAL;
    /* Keep the failed command and its REQUEST SENSE indivisible on the BOT stream. */
    if (!msc_command_try_lock(m))
        return PROS_USB_MSC_EBUSY;
    rc=issue(m,cdb,cdb_len,in,data,len);
    if(rc==PROS_USB_MSC_ECOMMAND&&cdb&&cdb[0]!=0x03u)(void)request_sense(m);
    msc_command_unlock(m);
    return rc;
}
static int test_ready(struct pros_usb_msc *m){
    uint8_t cdb[6]={0};int rc=command(m,cdb,sizeof(cdb),0u,0,0u);
    if(rc==PROS_USB_MSC_ECOMMAND&&m->last_sense_key==0x06u)rc=command(m,cdb,sizeof(cdb),0u,0,0u);
    return rc;
}
static int read_capacity(struct pros_usb_msc *m){
    uint8_t cdb10[10]={0x25u},r10[8];uint64_t last;uint32_t sector;int rc;
    rc=command(m,cdb10,sizeof(cdb10),1u,r10,sizeof(r10));if(rc!=PROS_USB_MSC_OK)return rc;
    last=be32(r10);sector=be32(r10+4u);
    if(last==UINT32_MAX){
        uint8_t cdb16[16]={0x9Eu,0x10u},r16[CAP16_LEN];put_be32(cdb16+10u,sizeof(r16));
        rc=command(m,cdb16,sizeof(cdb16),1u,r16,sizeof(r16));
        if(rc!=PROS_USB_MSC_OK)return PROS_USB_MSC_EUNSUPPORTED;
        last=be64(r16);sector=be32(r16+8u);m->use_rw16=last>UINT32_MAX?1u:0u;
    }
    if(last==UINT64_MAX||sector<512u||sector>4096u||(sector&(sector-1u)))return PROS_USB_MSC_EUNSUPPORTED;
    m->sector_count=last+1u;m->sector_size=sector;return PROS_USB_MSC_OK;
}
static void probe_write_protect(struct pros_usb_msc *m){
    uint8_t cdb[6]={0x1Au,0x08u,0x3Fu,0u,4u,0u},r[4];
    if(command(m,cdb,sizeof(cdb),1u,r,sizeof(r))==PROS_USB_MSC_OK)m->read_only=(r[2]&0x80u)?1u:0u;
}
#define MSC_IO_DETACHING 0x80000000u
#define MSC_IO_COUNT_MASK 0x7FFFFFFFu

static int msc_io_enter(struct pros_usb_msc *m)
{
    uint32_t state;
    if (!m)
        return PROS_USB_MSC_EINVAL;
    for (;;) {
        state = __sync_val_compare_and_swap(&m->io_state, 0u, 0u);
        if (state & MSC_IO_DETACHING)
            return PROS_USB_MSC_EBUSY;
        if ((state & MSC_IO_COUNT_MASK) == MSC_IO_COUNT_MASK)
            return PROS_USB_MSC_EOVERFLOW;
        if (__sync_val_compare_and_swap(&m->io_state, state, state + 1u) == state)
            return PROS_USB_MSC_OK;
    }
}

static void msc_io_leave(struct pros_usb_msc *m)
{
    (void)__sync_fetch_and_sub(&m->io_state, 1u);
}

static int rw(struct pros_usb_msc *m,uint64_t lba,uint32_t count,void *buffer,uint8_t write){
    uint8_t *cursor=(uint8_t *)buffer;uint32_t remaining=count;int rc;
    if(!m)return PROS_BLOCK_EINVAL;
    rc=msc_io_enter(m);
    if(rc!=PROS_USB_MSC_OK)return PROS_BLOCK_EIO;
    if(!m->initialized||!buffer||count==0u||!block_device_lba_range_valid(&m->block,lba,count)){
        msc_io_leave(m);return PROS_BLOCK_EINVAL;
    }
    if(m->faulted){msc_io_leave(m);return PROS_BLOCK_EIO;}
    if(write&&m->read_only){msc_io_leave(m);return PROS_BLOCK_EINVAL;}
    while(remaining){
        uint8_t cdb[16];uint8_t cdb_len;uint32_t chunk,max_count,bytes;
        zero_bytes(cdb,sizeof(cdb));max_count=UINT32_MAX/m->sector_size;
        chunk=remaining<max_count?remaining:max_count;
        if (!write) {
            uint32_t max_read_sectors = MSC_READ_STAGE_MAX_BYTES / m->sector_size;
            if (max_read_sectors == 0u) {
                msc_io_leave(m);
                return PROS_BLOCK_EINVAL;
            }
            if (chunk > max_read_sectors)
                chunk = max_read_sectors;
        }
        if(m->use_rw16||lba>UINT32_MAX){
            cdb[0]=write?0x8Au:0x88u;put_be64(cdb+2u,lba);put_be32(cdb+10u,chunk);cdb_len=16u;
        }else{
            uint64_t addressable=(uint64_t)UINT32_MAX-lba+1u;
            if (chunk > 65535u)
                chunk = 65535u;
            if ((uint64_t)chunk > addressable)
                chunk = (uint32_t)addressable;
            if(chunk==0u){msc_io_leave(m);return PROS_BLOCK_EINVAL;}
            cdb[0]=write?0x2Au:0x28u;put_be32(cdb+2u,(uint32_t)lba);put_be16(cdb+7u,(uint16_t)chunk);cdb_len=10u;
        }
        bytes=chunk*m->sector_size;
        if (write) {
            rc=command(m,cdb,cdb_len,0u,cursor,bytes);
        } else {
            /*
             * A USB bulk-IN transfer may deliver bytes before BOT rejects its
             * CSW/residue. Keep that uncommitted payload out of the caller's
             * buffer. The kernel heap is not internally serialized, so use a
             * bounded shared staging area and serialize ownership across
             * devices. Copy out only after the complete BOT command succeeds.
             */
            if (!msc_read_stage_try_acquire()) {
                msc_io_leave(m);
                return PROS_BLOCK_EIO;
            }
            rc=command(m,cdb,cdb_len,1u,msc_read_stage,bytes);
            if (rc == PROS_USB_MSC_OK) {
                for (uint32_t i = 0u; i < bytes; ++i)
                    cursor[i] = msc_read_stage[i];
            }
            msc_read_stage_release();
        }
        if(rc!=PROS_USB_MSC_OK){msc_io_leave(m);return PROS_BLOCK_EIO;}
        cursor+=bytes;lba+=chunk;remaining-=chunk;
    }
    msc_io_leave(m);
    return PROS_BLOCK_OK;
}
static int block_read(struct pros_block_device *d,uint64_t l,uint32_t n,void *b){
    if(!d||!d->private_data)return PROS_BLOCK_EINVAL;
    return rw((struct pros_usb_msc *)d->private_data,l,n,b,0u);
}
static int block_write(struct pros_block_device *d,uint64_t l,uint32_t n,const void *b){
    if(!d||!d->private_data)return PROS_BLOCK_EINVAL;
    return rw((struct pros_usb_msc *)d->private_data,l,n,(void *)b,1u);
}
static int block_flush(struct pros_block_device *d){
    struct pros_usb_msc *m;uint8_t cdb[10]={0x35u};int rc;
    if (!d || !d->private_data)
        return PROS_BLOCK_EINVAL;
    m = (struct pros_usb_msc *)d->private_data;
    rc=msc_io_enter(m);
    if(rc!=PROS_USB_MSC_OK)return PROS_BLOCK_EIO;
    if(!m->initialized||m->faulted){msc_io_leave(m);return PROS_BLOCK_EIO;}
    rc=command(m,cdb,sizeof(cdb),0u,0,0u)==PROS_USB_MSC_OK?PROS_BLOCK_OK:PROS_BLOCK_EIO;
    msc_io_leave(m);
    return rc;
}
int pros_usb_msc_init(struct pros_usb_msc *m,const struct pros_usb_msc_transport *t,
                      uint8_t iface,uint8_t ep_in,uint8_t ep_out,uint8_t lun){
    uint8_t cdb[6]={0x12u,0u,0u,0u,INQUIRY_LEN,0u},inq[INQUIRY_LEN];int rc;
    if(!m||!t||!t->bulk_in||!t->bulk_out||!t->clear_halt||!t->reset_recovery||lun>15u||
       !(ep_in&0x80u)||(ep_in&0x0Fu)==0u||(ep_in&0x70u)||(ep_out&0x80u)||(ep_out&0x0Fu)==0u||(ep_out&0x70u))
        return PROS_USB_MSC_EINVAL;
    zero_bytes(m,sizeof(*m));m->transport=*t;m->interface_number=iface;m->endpoint_in=ep_in;
    m->endpoint_out=ep_out;m->lun=lun;m->next_tag=1u;
    rc=command(m,cdb,sizeof(cdb),1u,inq,sizeof(inq));if(rc!=PROS_USB_MSC_OK)return rc;
    if((inq[0]&0x1Fu)!=0u&&(inq[0]&0x1Fu)!=0x0Eu)return PROS_USB_MSC_EUNSUPPORTED;
    rc=test_ready(m);if(rc!=PROS_USB_MSC_OK)return rc;
    rc=read_capacity(m);if(rc!=PROS_USB_MSC_OK)return rc;
    probe_write_protect(m);
    if(block_device_init(&m->block,m->sector_size,m->sector_count,block_read,m->read_only?0:block_write,m)!=PROS_BLOCK_OK||
       block_device_set_flush(&m->block,block_flush)!=PROS_BLOCK_OK)return PROS_USB_MSC_EIO;
    m->initialized=1u;return PROS_USB_MSC_OK;
}
int pros_usb_msc_detach(struct pros_usb_msc *m){
    int unregister_rc;
    if (!m || !m->initialized)
        return PROS_USB_MSC_EINVAL;
    /*
     * Atomically close admission before checking the in-flight count. The
     * high bit prevents new callbacks from entering; a nonzero low count
     * means an existing multi-command read/write/flush has not drained.
     */
    if (__sync_val_compare_and_swap(&m->io_state, 0u, MSC_IO_DETACHING) != 0u)
        return PROS_USB_MSC_EBUSY;
    if (m->busy) {
        (void)__sync_lock_test_and_set(&m->io_state, 0u);
        return PROS_USB_MSC_EBUSY;
    }
    /*
     * Remove the published pointer before invalidating the embedded device.
     * ENOENT is valid for an initialized instance that was never published;
     * any other registry error must leave the object intact and retryable.
     */
    unregister_rc = block_device_unregister(&m->block);
    if (unregister_rc != PROS_BLOCK_OK &&
        unregister_rc != PROS_BLOCK_ENOENT) {
        (void)__sync_lock_test_and_set(&m->io_state, 0u);
        return PROS_USB_MSC_EIO;
    }
    /* Keep callback pointers stable; DETACHING makes stale callbacks fail closed. */
    m->initialized=0u;
    return PROS_USB_MSC_OK;
}
struct pros_block_device *pros_usb_msc_block_device(struct pros_usb_msc *m){return m&&m->initialized?&m->block:0;}

struct usb_msc_fake {
    uint8_t storage[16u*512u],stage,data_in,status,fail_next,bad_csw,short_data;
    uint8_t write_protected,sense_key,sense_asc,sense_ascq,cdb[16],cdb_len,stall_data,stall_out_data,fail_clear_halt,fail_reset,bad_out_residue,busy_cbw,busy_data_out,busy_data_in,busy_csw,busy_clear_halt,busy_reset;
    uint32_t tag,data_len,resets,csw_residue,clear_halts,stall_out_actual;
};
static uint64_t fake_lba(const struct usb_msc_fake *f){return (f->cdb[0]==0x88u||f->cdb[0]==0x8Au)?be64(f->cdb+2u):be32(f->cdb+2u);}
static uint32_t fake_count(const struct usb_msc_fake *f){return (f->cdb[0]==0x88u||f->cdb[0]==0x8Au)?be32(f->cdb+10u):((uint32_t)f->cdb[7]<<8)|f->cdb[8];}
static int fake_out(void *ctx,uint8_t ep,const void *buf,uint32_t len,uint32_t *actual){
    struct usb_msc_fake *f=(struct usb_msc_fake *)ctx;const uint8_t *b=(const uint8_t *)buf;
    if (!f || ep != 0x02u || !buf || !actual)
        return -1;
    *actual = 0u;
    if(f->stage==0u&&len==CBW_LEN){
        if (f->busy_cbw) { f->busy_cbw=0u; return PROS_USB_MSC_EBUSY; }
        if (le32(b) != CBW_SIG)
            return -1;
        f->tag = le32(b + 4u);
        f->data_len = le32(b + 8u);
        f->data_in = (b[12] & 0x80u) != 0u;
        f->cdb_len=b[14];if(!f->cdb_len||f->cdb_len>16u)return -1;
        for(uint32_t i=0;i<16u;++i)f->cdb[i]=b[15u+i];
        f->status=f->fail_next?1u:0u;f->fail_next=0u;f->stage=f->data_len?1u:2u;*actual=len;return 0;
    }
    if(f->stage==1u&&!f->data_in&&(f->cdb[0]==0x2Au||f->cdb[0]==0x8Au)){
         if (f->busy_data_out) { f->busy_data_out=0u; return PROS_USB_MSC_EBUSY; }
        if(f->stall_out_data){f->stall_out_data=0u;f->stage=2u;*actual=f->stall_out_actual;f->csw_residue=f->stall_out_actual<=f->data_len?f->data_len-f->stall_out_actual:0u;if(f->bad_out_residue&&f->csw_residue<f->data_len)++f->csw_residue;f->status=1u;return PROS_USB_MSC_XFER_STALL;}
        uint64_t l=fake_lba(f);uint32_t n=fake_count(f);
        if(len!=f->data_len||n*512u!=len||l>=16u||n>16u-l)return -1;
        for (uint32_t i = 0; i < len; ++i)
            f->storage[(uint32_t)(l * 512u) + i] = b[i];
        f->stage = 2u;*actual=len;
        return 0;
    }return -1;
}
static int fake_in(void *ctx,uint8_t ep,void *buf,uint32_t len,uint32_t *actual){
    struct usb_msc_fake *f=(struct usb_msc_fake *)ctx;uint8_t *b=(uint8_t *)buf;
    if(!f||ep!=0x81u||!buf||!actual)return -1;
    if(f->stage==1u&&f->data_in){
         if (f->busy_data_in) { f->busy_data_in=0u; return PROS_USB_MSC_EBUSY; }
        if(f->stall_data){f->stall_data=0u;f->stage=2u;f->csw_residue=f->data_len;*actual=0u;return PROS_USB_MSC_XFER_STALL;}
        if(f->short_data){f->short_data=0u;f->stage=2u;f->csw_residue=0u;*actual=len/2u;for(uint32_t i=0u;i<*actual;++i)b[i]=0xA7u;return 0;}
        zero_bytes(b,len);
        switch(f->cdb[0]){
        case 0x12u:if(len!=INQUIRY_LEN)return -1;b[2]=5u;b[3]=2u;b[4]=31u;break;
        case 0x25u:if(len!=8u)return -1;put_be32(b,UINT32_MAX);put_be32(b+4u,512u);break;
        case 0x9Eu:if(len!=CAP16_LEN)return -1;put_be64(b,0x100000010ULL);put_be32(b+8u,512u);break;
        case 0x1Au:if(len!=4u)return -1;b[0]=3u;b[2]=f->write_protected?0x80u:0u;break;
        case 0x03u:if(len!=SENSE_LEN)return -1;b[0]=0x70u;b[2]=f->sense_key;b[7]=10u;b[12]=f->sense_asc;b[13]=f->sense_ascq;break;
        case 0x28u:case 0x88u:{uint64_t l=fake_lba(f);uint32_t n=fake_count(f);if(n*512u!=len)return -1;
            if (l < 16u && n <= 16u - l) {
                for (uint32_t i = 0; i < len; ++i)
                    b[i] = f->storage[(uint32_t)(l * 512u) + i];
            }
            break;
        }
        default:return -1;
        }f->stage=2u;*actual=len;return 0;
    }
    if(f->stage==2u&&len==CSW_LEN){
         if (f->busy_csw) { f->busy_csw=0u; return PROS_USB_MSC_EBUSY; }
        zero_bytes(b,CSW_LEN);put_le32(b,CSW_SIG);put_le32(b+4u,f->tag+(f->bad_csw?1u:0u));
        put_le32(b+8u,f->csw_residue);b[12]=f->status;f->bad_csw=0u;f->csw_residue=0u;f->stage=0u;*actual=CSW_LEN;return 0;
    }return -1;
}
static int fake_clear_halt(void *ctx,uint8_t ep){
    struct usb_msc_fake *f=(struct usb_msc_fake *)ctx;
    if (!f || (ep != 0x81u && ep != 0x02u))
        return -1;
    ++f->clear_halts;
    if (f->busy_clear_halt) { --f->busy_clear_halt; return PROS_USB_MSC_EBUSY; }
    return f->fail_clear_halt ? -1 : 0;
}
static int fake_reset(void *ctx,uint8_t iface,uint8_t in,uint8_t out){
    struct usb_msc_fake *f=(struct usb_msc_fake *)ctx;(void)iface;
    if (!f || in != 0x81u || out != 0x02u)
        return -1;
    if (f->busy_reset) { --f->busy_reset; return PROS_USB_MSC_EBUSY; }
    f->stage = 0u;
    ++f->resets;
    if(f->fail_reset)return -1;
    f->csw_residue=0u;
    return 0;
}
int pros_usb_msc_selftest(void){
    static struct usb_msc_fake fake;struct pros_usb_msc m,ro;
    struct pros_usb_msc_transport t;uint8_t in[512],out[512];
    zero_bytes(&fake,sizeof(fake));for(uint32_t i=0;i<sizeof(fake.storage);++i)fake.storage[i]=(uint8_t)((i*17u)^(i>>2)^0x5Au);
    fake.sense_key=3u;fake.sense_asc=0x11u;t.context=&fake;t.bulk_out=fake_out;t.bulk_in=fake_in;t.clear_halt=fake_clear_halt;t.reset_recovery=fake_reset;
    if(pros_usb_msc_init(&m,&t,1u,0x81u,0x02u,0u)!=PROS_USB_MSC_OK||m.sector_size!=512u||
       m.sector_count!=0x100000011ULL||!m.use_rw16||m.read_only)return -1;
    /* A contending BOT caller must fail before touching the transport. */
    if (!msc_command_try_lock(&m)) return -20;
    { uint32_t before=fake.tag;
      if (block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_EIO ||
          fake.tag!=before) { msc_command_unlock(&m); return -21; } }
    msc_command_unlock(&m);
    /* Shared staging contention must fail fast, without spinning while the
     * owner may be preempted; the caller buffer must remain untouched. */
    for (uint32_t i = 0u; i < sizeof(out); ++i) out[i] = 0x5Au;
    __atomic_store_n(&msc_read_stage_lock, 1u, __ATOMIC_RELEASE);
    if (block_device_read(&m.block, 2u, 1u, out) != PROS_BLOCK_EIO) {
        __atomic_store_n(&msc_read_stage_lock, 0u, __ATOMIC_RELEASE);
        return -34;
    }
    __atomic_store_n(&msc_read_stage_lock, 0u, __ATOMIC_RELEASE);
    for (uint32_t i = 0u; i < sizeof(out); ++i)
        if (out[i] != 0x5Au) return -35;
    /* A transport-lock collision before CBW submission is safe to retry:
     * the CBW was not posted, so retry it without class reset recovery. */
    fake.busy_cbw = 1u;
    { uint32_t resets = fake.resets;
      if (block_device_read(&m.block,2u,1u,out) != PROS_BLOCK_OK ||
          fake.resets != resets || fake.busy_cbw != 0u || m.faulted)
          return -22; }
    for(uint32_t i=0;i<512u;++i)in[i]=(uint8_t)(0xC3u^i);
    {
        uint32_t resets = fake.resets;
        fake.busy_data_out = 1u;
        if (block_device_write(&m.block, 2u, 1u, in) != PROS_BLOCK_OK ||
            fake.busy_data_out != 0u || fake.resets != resets)
            return -26;
        fake.busy_data_in = 1u;
        if (block_device_read(&m.block, 2u, 1u, out) != PROS_BLOCK_OK ||
            fake.busy_data_in != 0u || fake.resets != resets)
            return -27;
        fake.busy_csw = 1u;
        if (block_device_read(&m.block, 2u, 1u, out) != PROS_BLOCK_OK ||
            fake.busy_csw != 0u || fake.resets != resets)
            return -28;
    }
    if(block_device_write(&m.block,2u,1u,in)!=PROS_BLOCK_OK||block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_OK)return -2;
    for(uint32_t i=0;i<512u;++i)if(in[i]!=out[i])return -3;
    for(uint32_t i=0;i<512u;++i)out[i]=(uint8_t)~in[i];
    { uint32_t clears=fake.clear_halts;fake.stall_out_data=1u;fake.stall_out_actual=0u;
      if(block_device_write(&m.block,2u,1u,out)!=PROS_BLOCK_EIO||
         fake.clear_halts!=clears+1u||
         block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_OK)return -16;
      for(uint32_t i=0;i<512u;++i)if(out[i]!=in[i])return -17; }
    /* xHCI may briefly own the event lock during endpoint recovery. */
    { uint32_t clears=fake.clear_halts,resets=fake.resets;
      fake.stall_out_data=1u;fake.stall_out_actual=0u;fake.busy_clear_halt=1u;
      if(block_device_write(&m.block,2u,1u,out)!=PROS_BLOCK_EIO||
         fake.clear_halts!=clears+2u||fake.resets!=resets||
         fake.busy_clear_halt!=0u)return -29; }
    /* A transient recovery-lock collision must not immediately poison BOT. */
    { uint32_t resets=fake.resets;
      /* Force a short OUT transfer and then corrupt the CSW residue so BOT
       * recovery must run; the first reset attempt deliberately returns EBUSY. */
      /* A partial transfer leaves room for an impossible CSW residue. */
      fake.stall_out_data=1u;fake.stall_out_actual=256u;
      fake.bad_out_residue=1u;fake.busy_reset=1u;
      if(block_device_write(&m.block,2u,1u,out)!=PROS_BLOCK_EIO)
          return -30;
      if(fake.resets==resets)
          return -31;
      if(fake.resets!=resets+1u)
          return -33;
      if(fake.busy_reset!=0u)
          return -32;
      fake.bad_out_residue=0u; }
    { uint32_t clears=fake.clear_halts;fake.stall_out_data=1u;fake.stall_out_actual=256u;
      if(block_device_write(&m.block,2u,1u,out)!=PROS_BLOCK_EIO||fake.clear_halts!=clears+1u||
         m.last_sense_key!=3u||m.last_asc!=0x11u||
         block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_OK)return -21;
      for(uint32_t i=0;i<512u;++i)if(out[i]!=in[i])return -22; }
    { uint32_t resets=fake.resets;fake.stall_out_data=1u;fake.stall_out_actual=256u;fake.bad_out_residue=1u;
      if(block_device_write(&m.block,2u,1u,out)!=PROS_BLOCK_EIO||fake.resets!=resets+1u)return -23;
      fake.bad_out_residue=0u;
      if(block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_OK)return -24;
      for(uint32_t i=0;i<512u;++i)if(out[i]!=in[i])return -25; }
    { uint32_t resets=fake.resets;fake.stall_out_data=1u;fake.fail_clear_halt=1u;
      if(block_device_write(&m.block,2u,1u,out)!=PROS_BLOCK_EIO||fake.resets!=resets+1u)return -18;
      fake.fail_clear_halt=0u;
      if(block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_OK)return -19;
      for(uint32_t i=0;i<512u;++i)if(out[i]!=in[i])return -20; }
    if(block_device_flush(&m.block)!=PROS_BLOCK_OK)return -4;
    fake.fail_next=1u;fake.stall_data=1u;if(block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_EIO||m.last_sense_key!=3u||m.last_asc!=0x11u)return -5;
    for(uint32_t i=0u;i<512u;++i)out[i]=0x5Au;
    fake.short_data=1u;
    if(block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_EIO||!fake.resets)return -6;
    for(uint32_t i=0u;i<512u;++i)if(out[i]!=0x5Au)return -30;
    fake.bad_csw=1u;if(block_device_flush(&m.block)!=PROS_BLOCK_EIO||fake.resets<2u)return -7;
    fake.fail_reset=1u;fake.short_data=1u;
    if(block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_EIO)return -8;
    { uint32_t resets=fake.resets;
      if(block_device_read(&m.block,2u,1u,out)!=PROS_BLOCK_EIO||fake.resets!=resets)return -9; }
    {
        uint32_t registry_before = block_device_registry_count();
        if (block_device_register(&m.block) != PROS_BLOCK_OK ||
            block_device_registry_count() != registry_before + 1u ||
            block_device_registry_get(registry_before) != &m.block)
            return -10;

        /* Detach must refuse while a BOT command is active and keep the
         * published block device intact until the command has drained. */
        m.busy = 1u;
        if (pros_usb_msc_detach(&m) != PROS_USB_MSC_EBUSY ||
            block_device_registry_count() != registry_before + 1u ||
            block_device_registry_get(registry_before) != &m.block) {
            m.busy = 0u;
            (void)block_device_unregister(&m.block);
            return -11;
        }
        m.busy = 0u;
        /* The admission gate must also reject a new I/O after detach closes it. */
        m.io_state = MSC_IO_DETACHING;
        if (block_device_read(&m.block, 2u, 1u, out) != PROS_BLOCK_EIO ||
            pros_usb_msc_detach(&m) != PROS_USB_MSC_EBUSY) {
            m.io_state = 0u;
            (void)block_device_unregister(&m.block);
            return -12;
        }
        m.io_state = 0u;
        /* Model one operation holding its reference across a multi-command
         * transfer: detach must not unregister until the reference is gone. */
        m.io_state = 1u;
        if (pros_usb_msc_detach(&m) != PROS_USB_MSC_EBUSY ||
            block_device_registry_get(registry_before) != &m.block) {
            m.io_state = 0u;
            (void)block_device_unregister(&m.block);
            return -13;
        }
        m.io_state = 0u;
        if (pros_usb_msc_detach(&m) != PROS_USB_MSC_OK ||
            block_device_registry_count() != registry_before ||
            block_device_registry_get(registry_before) != 0 ||
            m.block.read != block_read || m.block.write != block_write ||
            m.block.flush != block_flush || m.block.private_data != &m ||
            block_device_read(&m.block, 2u, 1u, out) != PROS_BLOCK_EIO)
            return -14;
    }
    zero_bytes(&fake,sizeof(fake));fake.write_protected=1u;t.context=&fake;fake.sense_key=5u;
    if(pros_usb_msc_init(&ro,&t,1u,0x81u,0x02u,0u)!=PROS_USB_MSC_OK||!ro.read_only||ro.block.write!=0)return -13;
    if(block_device_write(&ro.block,0u,1u,in)!=PROS_BLOCK_EINVAL)return -14;
    if(pros_usb_msc_detach(&ro)!=PROS_USB_MSC_OK)return -15;
    return PROS_USB_MSC_OK;
}
