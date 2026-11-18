#ifndef PROSENJIT_PANIC_HANDOFF_H
#define PROSENJIT_PANIC_HANDOFF_H

#include <stddef.h>
#include <stdint.h>

/*
 * Versioned, normalized exception snapshot. The assembly entry must populate
 * every field before publishing/reading the frame. rsp/ss are valid only when
 * PROS_PANIC_FRAME_HAS_OLD_RSP_SS is set; CR2 is valid only for #PF snapshots
 * when PROS_PANIC_FRAME_HAS_CR2 is set.
 */
#define PROS_PANIC_FRAME_VERSION 1u
#define PROS_PANIC_STACK_SIZE 16384u
#define PROS_PANIC_FRAME_STORAGE_SIZE 128u

#define PROS_PANIC_FRAME_HAS_OLD_RSP_SS (1ULL << 0)
#define PROS_PANIC_FRAME_HAS_CR2        (1ULL << 1)
#define PROS_PANIC_FRAME_FROM_USER      (1ULL << 2)
#define PROS_PANIC_FRAME_USED_IST       (1ULL << 3)

struct pros_panic_frame {
    uint32_t version;
    uint32_t size;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
    uint64_t cr2;
    uint64_t flags;
    uint64_t reserved[6];
};

_Static_assert(sizeof(struct pros_panic_frame) ==
               PROS_PANIC_FRAME_STORAGE_SIZE,
               "panic frame storage ABI size mismatch");
_Static_assert(offsetof(struct pros_panic_frame, vector) == 8u,
               "panic frame vector offset mismatch");
_Static_assert(offsetof(struct pros_panic_frame, rip) == 24u,
               "panic frame RIP offset mismatch");
_Static_assert(offsetof(struct pros_panic_frame, rsp) == 48u,
               "panic frame RSP offset mismatch");
_Static_assert(offsetof(struct pros_panic_frame, cr2) == 64u,
               "panic frame CR2 offset mismatch");
_Static_assert(offsetof(struct pros_panic_frame, flags) == 72u,
               "panic frame flags offset mismatch");

/* Isolated Phase A test surface; production IDT/exception routing does not call it. */
int pros_panic_handoff_validate(const struct pros_panic_frame *frame);
int pros_panic_handoff_try_claim(void);
void pros_panic_handoff_reset_test(void);
void pros_panic_handoff_enter_test(const struct pros_panic_frame *frame);
int pros_panic_handoff_selftest(void);
extern volatile uint32_t pros_panic_handoff_test_marker;

#endif
