#include "driver.h"
static void copy_name(char *d,const char *s){uint32_t i=0;if(!s){d[0]='\0';return;}while(s[i]!='\0'&&i+1u<PROS_DRIVER_NAME_MAX){d[i]=s[i];++i;}d[i]='\0';}
int driver_init(struct pros_driver *d,const char *n,enum pros_device_bus b,const struct pros_device_id *ids,uint32_t count,pros_driver_probe_fn p,pros_driver_remove_fn rm){if(!d||!n||!ids||count==0u||count>PROS_DRIVER_MAX_IDS||!p)return -1;copy_name(d->name,n);d->bus=b;d->ids=ids;d->id_count=count;d->probe=p;d->remove=rm;d->registered=0;return 0;}
int driver_supports_device(const struct pros_driver *d,const struct pros_device *dev){if(!d||!dev||d->bus!=dev->id.bus)return 0;for(uint32_t i=0;i<d->id_count;++i)if(device_matches_id(dev,&d->ids[i]))return 1;return 0;}
int driver_bind(struct pros_driver *d,struct pros_device *dev){if(!d||!dev||d->registered==0u||dev->claimed!=0u||!driver_supports_device(d,dev))return -1;int rc=d->probe(d,dev);if(rc==0)dev->claimed=1u;return rc;}
int driver_unbind(struct pros_driver *d,struct pros_device *dev){if(!d||!dev||dev->claimed==0u)return -1;int rc=d->remove?d->remove(d,dev):0;if(rc==0)dev->claimed=0u;return rc;}
