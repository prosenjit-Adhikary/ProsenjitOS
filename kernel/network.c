#include "network.h"

static uint16_t net_be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint16_t fold_checksum(uint32_t sum) { while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16); return (uint16_t)~sum; }
int pros_net_device_init(struct pros_net_device *d,const char *name,const uint8_t mac[6],uint32_t mtu,pros_net_rx_fn rx,pros_net_tx_fn tx,void *private_data){
    if(!d||!name||!mac||mtu<576u||mtu>PROS_NET_MAX_FRAME||!tx)return PROS_NET_EINVAL;
    d->name=name; for(uint32_t i=0;i<6u;++i)d->mac[i]=mac[i]; d->mtu=mtu; d->up=0; d->receive=rx; d->transmit=tx; d->private_data=private_data; return PROS_NET_OK;
}
int pros_net_set_up(struct pros_net_device *d,uint8_t up){if(!d)return PROS_NET_EINVAL; d->up=up?1u:0u; return PROS_NET_OK;}
int pros_net_ethernet_validate(const uint8_t *frame,uint32_t length){if(!frame||length<14u||length>PROS_NET_MAX_FRAME)return PROS_NET_EINVAL; uint16_t type=net_be16(frame+12); if(type<0x0600u)return PROS_NET_EINVAL; return PROS_NET_OK;}
uint16_t pros_net_ipv4_checksum(const void *data,uint32_t length){const uint8_t *p=(const uint8_t*)data;uint32_t sum=0;for(uint32_t i=0;i+1u<length;i+=2u)sum+=(uint32_t)net_be16(p+i);if(length&1u)sum+=(uint32_t)p[length-1u]<<8;return fold_checksum(sum);}
static int net_test_tx(struct pros_net_device *d,const uint8_t *f,uint32_t n){(void)d;(void)f;(void)n;return PROS_NET_OK;}
int pros_net_selftest(void){static const uint8_t mac[6]={0,1,2,3,4,5};uint8_t frame[14]={0};struct pros_net_device d;if(pros_net_device_init(&d,"test0",mac,1500u,0,net_test_tx,0)!=0)return -1;if(pros_net_set_up(&d,1)!=0||!d.up)return -2;frame[12]=0x08;frame[13]=0x00;if(pros_net_ethernet_validate(frame,sizeof(frame))!=0)return -3;frame[12]=0x00;frame[13]=0x01;if(pros_net_ethernet_validate(frame,sizeof(frame))==0)return -4;return 0;}
