#include "panic_handoff.h"

extern int pros_panic_handoff_validate(const struct pros_panic_frame *frame);
extern int pros_panic_handoff_try_claim(void);
extern void pros_panic_handoff_reset_test(void);
extern int pros_panic_handoff_stack_probe(void);
extern char __panic_stack_bottom[];
extern char __panic_stack_top[];
extern struct pros_panic_frame __panic_frame_storage;

/*
 * Classify only. This helper has no side effects and is deliberately not
 * called by the production ISR/exception dispatcher yet. User-origin faults
 * remain pending until a scheduler-safe process-fault handoff exists.
 */
enum pros_exception_disposition
pros_exception_classify(uint64_t vector, uint64_t cs_rpl,
                        int usercopy_fixup_active) {
    if (vector > 31u || (cs_rpl != 0u && cs_rpl != 3u))
        return PROS_EXCEPTION_INVALID;

    if (vector == 2u || vector == 8u || vector == 18u)
        return PROS_EXCEPTION_MACHINE_FATAL;

    if (vector == 15u || (vector >= 22u && vector <= 27u) ||
        vector == 31u || vector == 9u)
        return PROS_EXCEPTION_RESERVED_FAILSTOP;

    if (vector == 14u && cs_rpl == 0u && usercopy_fixup_active)
        return PROS_EXCEPTION_USERCOPY_RECOVERABLE;

    switch (vector) {
    case 0u: case 1u: case 3u: case 4u: case 5u: case 6u: case 7u:
    case 10u: case 11u: case 12u: case 13u: case 14u:
    case 16u: case 17u: case 19u: case 20u: case 21u:
    case 28u: case 29u: case 30u:
        return cs_rpl == 3u ? PROS_EXCEPTION_USER_FAULT_PENDING
                            : PROS_EXCEPTION_KERNEL_FATAL;
    default:
        return PROS_EXCEPTION_RESERVED_FAILSTOP;
    }
}

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
    if (pros_panic_handoff_validate(
            (const struct pros_panic_frame *)((const uint8_t *)&frame + 1u)) == 0)
        return -14;

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
    frame.flags = 1ULL << 8;
    if (pros_panic_handoff_validate(&frame) == 0)
        return -15;

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

    /* NMI and double-fault gates use IST stacks with old RSP/SS present. */
    frame.vector = 2u;
    frame.cs = 0x08u;
    frame.rip = 0xFFFFFFFF80002000ULL;
    frame.rsp = 0xFFFF800000020000ULL;
    frame.ss = 0x10u;
    frame.cr2 = 0u;
    frame.error_code = 0u;
    frame.flags = PROS_PANIC_FRAME_HAS_OLD_RSP_SS |
                  PROS_PANIC_FRAME_USED_IST;
    if (pros_panic_handoff_validate(&frame) != 0)
        return -20;
    frame.vector = 8u;
    frame.error_code = 0x2u;
    if (pros_panic_handoff_validate(&frame) != 0)
        return -21;

    /* User-origin #PF has both the IST-supplied old stack and user selectors. */
    frame.vector = 14u;
    frame.error_code = 0x5u;
    frame.cs = 0x1Bu;
    frame.rip = 0x0000000000400000ULL;
    frame.rsp = 0x00007FFFFFFFE000ULL;
    frame.ss = 0x23u;
    frame.cr2 = 0x0000000000000000ULL;
    frame.flags = PROS_PANIC_FRAME_HAS_OLD_RSP_SS |
                  PROS_PANIC_FRAME_HAS_CR2 |
                  PROS_PANIC_FRAME_FROM_USER |
                  PROS_PANIC_FRAME_USED_IST;
    if (pros_panic_handoff_validate(&frame) != 0)
        return -22;

    pros_panic_handoff_reset_test();
    if (pros_panic_handoff_try_claim() != 0 ||
        pros_panic_handoff_try_claim() == 0)
        return -12;
    pros_panic_handoff_reset_test();
    if (pros_panic_handoff_try_claim() != 0)
        return -13;
    pros_panic_handoff_reset_test();

    /* Invalid isolated entries must restore the caller stack and never claim. */
    {
        struct pros_panic_frame invalid_frame = frame;
        volatile uint64_t invalid_entry_sentinel = 0xCAFEBABE76543210ULL;
        invalid_frame.version++;
        if (pros_panic_handoff_enter_test(&invalid_frame) == 0 ||
            invalid_entry_sentinel != 0xCAFEBABE76543210ULL ||
            pros_panic_handoff_enter_test(
                (const struct pros_panic_frame *)
                ((const uint8_t *)&frame + 1u)) == 0 ||
            pros_panic_handoff_try_claim() != 0)
            return -23;
        pros_panic_handoff_reset_test();
    }

    /* Exercise the dedicated stack switch and SysV call alignment, then
     * verify that the caller's original stack was restored on return. */
    volatile uint64_t caller_stack_sentinel = 0xA55AA55A11223344ULL;
    uint64_t panic_stack_bottom = (uint64_t)(uintptr_t)__panic_stack_bottom;
    uint64_t panic_stack_top = (uint64_t)(uintptr_t)__panic_stack_top;
    if (pros_panic_handoff_stack_probe() != 0 ||
        caller_stack_sentinel != 0xA55AA55A11223344ULL ||
        pros_panic_handoff_test_stack_seen < panic_stack_bottom ||
        pros_panic_handoff_test_stack_seen >= panic_stack_top ||
        (pros_panic_handoff_test_stack_seen & 0xFULL) != 8u ||
        pros_panic_handoff_test_stack_marker != 0x5354414Bu)
        return -16;

    /*
     * Execute the complete first-owner entry/copy/stack-switch sequence in a
     * reversible test-only continuation. Production mode still halts forever.
     */
    pros_panic_handoff_reset_test();
    pros_panic_handoff_test_return_mode = 1u;
    caller_stack_sentinel = 0x11223344A55AA55AULL;
    if (pros_panic_handoff_enter_test(&frame) != 0) {
        pros_panic_handoff_test_return_mode = 0u;
        pros_panic_handoff_reset_test();
        return -17;
    }
    pros_panic_handoff_test_return_mode = 0u;
    if (caller_stack_sentinel != 0x11223344A55AA55AULL ||
        pros_panic_handoff_test_marker != 0x50414E49u ||
        pros_panic_handoff_test_stack_seen < panic_stack_bottom ||
        pros_panic_handoff_test_stack_seen >= panic_stack_top ||
        (pros_panic_handoff_test_stack_seen & 0xFULL) != 8u)
        return -18;
    {
        const uint8_t *expected = (const uint8_t *)&frame;
        const uint8_t *copied =
            (const uint8_t *)&__panic_frame_storage;
        for (uint32_t i = 0u; i < sizeof(frame); ++i)
            if (copied[i] != expected[i])
                return -19;
    }
    pros_panic_handoff_reset_test();

    /*
     * Configured-vector shape matrix. This validates normalized snapshot
     * shapes only; it does not claim that production ISR stubs produce them.
     * The current IDT assigns IST1/2/3 to vectors 2/8/14 respectively.
     */
    for (uint32_t vector = 0u; vector < 32u; ++vector) {
        for (uint32_t from_user = 0u; from_user < 2u; ++from_user) {
            uint64_t flags = 0u;
            uint32_t uses_ist =
                (vector == 2u || vector == 8u || vector == 14u);

            frame = (struct pros_panic_frame){0};
            frame.version = PROS_PANIC_FRAME_VERSION;
            frame.size = sizeof(frame);
            frame.vector = vector;
            frame.rip = from_user ? 0x0000000000401000ULL
                                  : 0xFFFFFFFF80001000ULL;
            frame.cs = from_user ? 0x1Bu : 0x08u;
            frame.rflags = 0x202u;

            /* Match the repository's current ISR_WITH_ERROR declarations. */
            if (vector == 8u || (vector >= 10u && vector <= 14u) ||
                vector == 17u)
                frame.error_code = 0xA5000000ULL | vector;

            if (from_user) {
                flags |= PROS_PANIC_FRAME_FROM_USER |
                         PROS_PANIC_FRAME_HAS_OLD_RSP_SS;
                frame.rsp = 0x00007FFFFFFFE000ULL;
                frame.ss = 0x23u;
            } else if (uses_ist) {
                flags |= PROS_PANIC_FRAME_HAS_OLD_RSP_SS;
                frame.rsp = 0xFFFF800000020000ULL;
                frame.ss = 0x10u;
            }

            if (uses_ist)
                flags |= PROS_PANIC_FRAME_USED_IST;
            if (vector == 14u) {
                flags |= PROS_PANIC_FRAME_HAS_CR2;
                frame.cr2 = 0xDEADBEEFULL;
            }
            frame.flags = flags;

            if (pros_panic_handoff_validate(&frame) != 0)
                return -24;
        }
    }


    /*
     * Exhaustive pure disposition matrix. This deliberately does not connect
     * the policy to isr_common: CPL3 termination is not implemented yet.
     */
    for (uint64_t vector = 0u; vector < 32u; ++vector) {
        enum pros_exception_disposition kernel_expected;
        enum pros_exception_disposition user_expected;

        if (vector == 2u || vector == 8u || vector == 18u) {
            kernel_expected = PROS_EXCEPTION_MACHINE_FATAL;
            user_expected = PROS_EXCEPTION_MACHINE_FATAL;
        } else if (vector == 9u || vector == 15u ||
                   (vector >= 22u && vector <= 27u) || vector == 31u) {
            kernel_expected = PROS_EXCEPTION_RESERVED_FAILSTOP;
            user_expected = PROS_EXCEPTION_RESERVED_FAILSTOP;
        } else {
            kernel_expected = PROS_EXCEPTION_KERNEL_FATAL;
            user_expected = PROS_EXCEPTION_USER_FAULT_PENDING;
        }

        if (pros_exception_classify(vector, 0u, 0) != kernel_expected ||
            pros_exception_classify(vector, 3u, 0) != user_expected)
            return -25;
    }

    if (pros_exception_classify(14u, 0u, 1) !=
            PROS_EXCEPTION_USERCOPY_RECOVERABLE ||
        pros_exception_classify(14u, 0u, 0) !=
            PROS_EXCEPTION_KERNEL_FATAL ||
        pros_exception_classify(14u, 3u, 1) !=
            PROS_EXCEPTION_USER_FAULT_PENDING ||
        pros_exception_classify(32u, 0u, 0) != PROS_EXCEPTION_INVALID ||
        pros_exception_classify(0u, 1u, 0) != PROS_EXCEPTION_INVALID ||
        pros_exception_classify(0u, 2u, 0) != PROS_EXCEPTION_INVALID)
        return -26;

    return 0;
}
