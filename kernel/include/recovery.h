#ifndef PROSENJIT_RECOVERY_H
#define PROSENJIT_RECOVERY_H
#include <stdint.h>
enum pros_recovery_state { PROS_RECOVERY_HEALTHY=0, PROS_RECOVERY_DETECTED, PROS_RECOVERY_VERIFIED, PROS_RECOVERY_REPAIRING, PROS_RECOVERY_ROLLBACK, PROS_RECOVERY_DEGRADED };
struct pros_recovery_record { enum pros_recovery_state state; uint32_t repair_allowed; uint32_t evidence; uint64_t sequence; };
int pros_recovery_begin(struct pros_recovery_record *,uint64_t);
int pros_recovery_verify(struct pros_recovery_record *,uint32_t);
int pros_recovery_repair(struct pros_recovery_record *);
int pros_recovery_verify_repair(struct pros_recovery_record *,uint32_t);
int pros_recovery_rollback(struct pros_recovery_record *);
int pros_recovery_selftest(void);
#endif
