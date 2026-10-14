#ifndef PROSENJIT_ABI_INPUT_H
#define PROSENJIT_ABI_INPUT_H
#include <stddef.h>
#include <stdint.h>
#define PROSENJIT_INPUT_ABI_VERSION 1u
enum pros_input_type { PROS_INPUT_KEY=1, PROS_INPUT_POINTER=2 };
struct pros_input_event { uint32_t version,type; uint64_t timestamp; int32_t value0,value1; uint32_t flags,reserved; };
#define PROS_INPUT_FLAG_PRESS 0x0001u
#define PROS_INPUT_FLAG_RELEASE 0x0002u
#define PROS_INPUT_FLAG_RELATIVE 0x0004u

/* Freeze the public event layout for separately compiled user processes. */
_Static_assert(offsetof(struct pros_input_event, timestamp) == 8u,
               "pros_input_event.timestamp ABI offset changed");
_Static_assert(offsetof(struct pros_input_event, flags) == 24u,
               "pros_input_event.flags ABI offset changed");
_Static_assert(sizeof(struct pros_input_event) == 32u,
               "pros_input_event ABI size changed");
#endif
