#include "driver_bundle.h"

static struct pros_driver_bundle_entry entries[PROS_DRIVER_BUNDLE_MAX_ENTRIES];

static int valid_source(enum pros_driver_bundle_source s) { return s <= PROS_DRIVER_BUNDLE_LOCAL; }
static struct pros_driver_bundle_entry *find(uint32_t id) {
    for (uint32_t i=0;i<PROS_DRIVER_BUNDLE_MAX_ENTRIES;++i) if(entries[i].package_id==id) return &entries[i];
    return 0;
}
int pros_driver_bundle_init(void) {
    for(uint32_t i=0;i<PROS_DRIVER_BUNDLE_MAX_ENTRIES;++i){ entries[i].package_id=0; entries[i].content_sha256_valid=0; entries[i].version_major=entries[i].version_minor=entries[i].version_patch=0; }
    return PROS_DRIVER_OK;
}
int pros_driver_bundle_add(uint32_t id, enum pros_driver_bundle_source s,uint32_t ma,uint32_t mi,uint32_t pa,const uint8_t *d) {
    if(!id||!valid_source(s)||!d) return PROS_DRIVER_EINVAL;
    struct pros_driver_bundle_entry *e=find(id);
    if(!e) for(uint32_t i=0;i<PROS_DRIVER_BUNDLE_MAX_ENTRIES;++i) if(!entries[i].package_id){e=&entries[i];break;}
    if(!e) return PROS_DRIVER_ENOSPC;
    e->package_id=id;e->source=s;e->version_major=ma;e->version_minor=mi;e->version_patch=pa;
    for(uint32_t i=0;i<PROS_FILE_SHA256_BYTES;++i)e->content_sha256[i]=d[i];
    e->content_sha256_valid=1; return PROS_DRIVER_OK;
}
int pros_driver_bundle_find(uint32_t id, struct pros_driver_bundle_entry *out) {
    if(!id||!out)return PROS_DRIVER_EINVAL; struct pros_driver_bundle_entry *e=find(id);
    if(!e)return PROS_DRIVER_ENODEV; *out=*e; return PROS_DRIVER_OK;
}
int pros_driver_bundle_compare_version(uint32_t a,uint32_t b,int *out) {
    if(!a||!b||!out)return PROS_DRIVER_EINVAL; struct pros_driver_bundle_entry *x=find(a),*y=find(b);
    if(!x||!y)return PROS_DRIVER_ENODEV;
    if(x->version_major!=y->version_major)*out=x->version_major>y->version_major?1:-1;
    else if(x->version_minor!=y->version_minor)*out=x->version_minor>y->version_minor?1:-1;
    else if(x->version_patch!=y->version_patch)*out=x->version_patch>y->version_patch?1:-1; else *out=0;
    return PROS_DRIVER_OK;
}
int pros_driver_bundle_selftest(void) {
    uint8_t a[PROS_FILE_SHA256_BYTES],b[PROS_FILE_SHA256_BYTES];
    for(uint32_t i=0;i<PROS_FILE_SHA256_BYTES;++i){a[i]=(uint8_t)i;b[i]=(uint8_t)(i^0x55u);}
    if(pros_driver_bundle_init()!=0)return -1;
    if(pros_driver_bundle_add(1,PROS_DRIVER_BUNDLE_ISO,1,2,3,a)!=0)return -2;
    if(pros_driver_bundle_add(2,PROS_DRIVER_BUNDLE_REMOVABLE,1,3,0,b)!=0)return -3;
    int cmp=0;if(pros_driver_bundle_compare_version(1,2,&cmp)!=0||cmp>=0)return -4;
    struct pros_driver_bundle_entry e;if(pros_driver_bundle_find(1,&e)!=0)return -5;
    for(uint32_t i=0;i<PROS_FILE_SHA256_BYTES;++i)if(e.content_sha256[i]!=a[i])return -6;
    return 0;
}
