#include "recovery.h"
int pros_recovery_begin(struct pros_recovery_record *r,uint64_t seq){if(!r)return -1;r->state=PROS_RECOVERY_DETECTED;r->repair_allowed=0;r->evidence=0;r->sequence=seq;return 0;}
int pros_recovery_verify(struct pros_recovery_record *r,uint32_t evidence){if(!r||r->state!=PROS_RECOVERY_DETECTED||!evidence)return -1;r->evidence=evidence;r->state=PROS_RECOVERY_VERIFIED;return 0;}
int pros_recovery_repair(struct pros_recovery_record *r){if(!r||r->state!=PROS_RECOVERY_VERIFIED||!r->repair_allowed)return -1;r->state=PROS_RECOVERY_REPAIRING;return 0;}
int pros_recovery_verify_repair(struct pros_recovery_record *r,uint32_t evidence){if(!r||r->state!=PROS_RECOVERY_REPAIRING||!evidence)return -1;r->evidence=evidence;r->state=PROS_RECOVERY_HEALTHY;return 0;}
int pros_recovery_rollback(struct pros_recovery_record *r){if(!r||r->state!=PROS_RECOVERY_REPAIRING)return -1;r->state=PROS_RECOVERY_ROLLBACK;r->repair_allowed=0;return 0;}
int pros_recovery_selftest(void){struct pros_recovery_record r;if(pros_recovery_begin(&r,1)!=0||pros_recovery_verify(&r,1)!=0||pros_recovery_repair(&r)==0)return -1;r.repair_allowed=1;if(pros_recovery_repair(&r)!=0||pros_recovery_verify_repair(&r,1)!=0||r.state!=PROS_RECOVERY_HEALTHY)return -2;return 0;}
