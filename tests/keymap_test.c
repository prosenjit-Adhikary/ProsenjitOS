#include <assert.h>
#include <stdint.h>
#include "../user/desktop/keymap.h"

static struct pros_input_event ev(uint32_t code, uint32_t flags)
{
    struct pros_input_event e = {
        PROSENJIT_INPUT_ABI_VERSION, PROS_INPUT_KEY, 0u,
        (int32_t)code, 0, flags
    };
    return e;
}

int main(void)
{
    struct pros_keymap_state s;
    pros_keymap_reset(&s);

    struct pros_input_event e = ev(0x1Eu, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 'a');
    e = ev(0x1Eu, PROS_INPUT_FLAG_RELEASE);
    assert(pros_keymap_translate(&s, &e) == 0u);

    e = ev(0x2Au, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 0u);
    e = ev(0x1Eu, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 'A');
    e = ev(0x2Au, PROS_INPUT_FLAG_RELEASE);
    (void)pros_keymap_translate(&s, &e);

    e = ev(0x3Au, PROS_INPUT_FLAG_PRESS);
    (void)pros_keymap_translate(&s, &e);
    e = ev(0x1Eu, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 'A');
    e = ev(0x2Eu, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 'C');
    e = ev(0x03u, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == '2');

    e = ev(0x100u | 0x48u, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 0u);
    e = ev(0x1Du, PROS_INPUT_FLAG_PRESS);
    (void)pros_keymap_translate(&s, &e);
    e = ev(0x2Eu, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 0u);
    e = ev(0x1Du, PROS_INPUT_FLAG_RELEASE);
    (void)pros_keymap_translate(&s, &e);

    e = ev(0x1Eu, PROS_INPUT_FLAG_PRESS | PROS_INPUT_FLAG_RELEASE);
    assert(pros_keymap_translate(&s, &e) == 0u);
    e.version = 999u;
    assert(pros_keymap_translate(&s, &e) == 0u);

    /* Reject malformed raw scan codes and ABI flags, not just unknown versions. */
    e = ev((uint32_t)-1, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 0u);
    e = ev(0x200u, PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 0u);
    e = ev(0x1Eu, PROS_INPUT_FLAG_PRESS | PROS_INPUT_FLAG_RELATIVE);
    assert(pros_keymap_translate(&s, &e) == 0u);
    e = ev(0x1Eu, 0x80000000u | PROS_INPUT_FLAG_PRESS);
    assert(pros_keymap_translate(&s, &e) == 0u);
    pros_keymap_reset(0);
    return 0;
}
