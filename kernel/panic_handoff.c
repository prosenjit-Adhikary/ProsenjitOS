#include "panic_handoff.h"

extern int pros_panic_handoff_validate(const struct pros_panic_frame *frame);
extern int pros_panic_handoff_try_claim(void);
extern void pros_panic_handoff_reset_test(void);

int pros_panic_handoff_selftest(void) {
    struct pros_panic_frame frame = {0};

    frame.version = PROS_PANIC_FRAME_VERSION;
    frame.size = sizeof(frame);
    frame.vector = 13u;
    frame.rip = 0xFFFFFFFF80001000ULL;
    frame.cs = 0x08u;
    frame.rflags = 0x202u;
    if (pros_panic_handoff_validate(&frame) != 0)
        return -1;

    frame.version++;
    if (pros_panic_handoff_validate(&frame) == 0)
        return -2;
    frame.version = PROS_PANIC_FRAME_VERSION;
    frame.size--;
    if (pros_panic_handoff_validate(&frame) == 0)
        return -3;
    frame.size = sizeof(frame);

    frame.cs = 0x1Bu;
    if (pros_panic_handoff_validate(&frame) == 0)
        return -4;
    frame.cs = 0x08u;

    frame.flags = PROS_PANIC_FRAME_HAS_OLD_RSP_SS;
    frame.rsp = 0xFFFF800000010000ULL;
    frame.ss = 0x10u;
    if (pros_panic_handoff_validate(&frame) != 0)
        return -5;
    frame.flags = 0;
    if (pros_panic_handoff_validate(&frame) == 0)
        return -6;
    frame.rsp = 0;
    frame.ss = 0;

    frame.vector = 14u;
    if (pros_panic_handoff_validate(&frame) == 0)
        return -7;
    frame.rsp = 0xFFFF800000010000ULL;
    frame.ss = 0x10u;
    frame.cr2 = 0xDEADBEEFULL;
    frame.flags = PROS_PANIC_FRAME_HAS_OLD_RSP_SS |
                  PROS_PANIC_FRAME_HAS_CR2 |
                  PROS_PANIC_FRAME_USED_IST;
    if (pros_panic_handoff_validate(&frame) != 0)
        return -8;

    frame.flags |= PROS_PANIC_FRAME_FROM_USER;
    if (pros_panic_handoff_validate(&frame) == 0)
        return -9;
    frame.flags &= ~PROS_PANIC_FRAME_FROM_USER;
    frame.reserved[2] = 1u;
    if (pros_panic_handoff_validate(&frame) == 0)
        return -10;
    frame.reserved[2] = 0u;

    frame.vector = 6u;
    frame.cr2 = 0;
    frame.cs = 0x1Bu;
    frame.rip = 0x0000000000400000ULL;
    frame.rsp = 0x00007FFFFFFFE000ULL;
    frame.ss = 0x23u;
    frame.flags = PROS_PANIC_FRAME_HAS_OLD_RSP_SS |
                  PROS_PANIC_FRAME_FROM_USER;
    if (pros_panic_handoff_validate(&frame) != 0)
        return -11;

    pros_panic_handoff_reset_test();
    if (pros_panic_handoff_try_claim() != 0 ||
        pros_panic_handoff_try_claim() == 0)
        return -12;
    pros_panic_handoff_reset_test();
    if (pros_panic_handoff_try_claim() != 0)
        return -13;
    pros_panic_handoff_reset_test();
    return 0;
}
