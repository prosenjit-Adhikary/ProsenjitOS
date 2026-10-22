#include "keymap.h"

static const char plain_map[128] = {
    [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
    [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
    [0x0C]='-', [0x0D]='=', [0x0E]='\b', [0x0F]='\t',
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
    [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1A]='[', [0x1B]=']', [0x1C]='\n',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
    [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
    [0x27]=';', [0x28]='\'', [0x29]='`',
    [0x2B]='\\', [0x2C]='z', [0x2D]='x', [0x2E]='c',
    [0x2F]='v', [0x30]='b', [0x31]='n', [0x32]='m',
    [0x33]=',', [0x34]='.', [0x35]='/', [0x39]=' '
};

static const char shifted_map[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
    [0x0C]='_', [0x0D]='+', [0x0E]='\b', [0x0F]='\t',
    [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
    [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
    [0x1A]='{', [0x1B]='}', [0x1C]='\n',
    [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
    [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
    [0x27]=':', [0x28]='"', [0x29]='~',
    [0x2B]='|', [0x2C]='Z', [0x2D]='X', [0x2E]='C',
    [0x2F]='V', [0x30]='B', [0x31]='N', [0x32]='M',
    [0x33]='<', [0x34]='>', [0x35]='?', [0x39]=' '
};

void pros_keymap_reset(struct pros_keymap_state *state)
{
    if (!state) return;
    state->left_shift = 0u;
    state->right_shift = 0u;
    state->caps_lock = 0u;
    state->caps_pressed = 0u;
    state->control = 0u;
    state->right_control = 0u;
    state->alt = 0u;
    state->right_alt = 0u;
}

uint32_t pros_keymap_translate(struct pros_keymap_state *state,
                               const struct pros_input_event *event)
{
    if (!state || !event || event->version != PROSENJIT_INPUT_ABI_VERSION ||
        event->type != PROS_INPUT_KEY)
        return 0u;

    /* Reject unknown ABI flags and out-of-range scan codes before decoding. */
    const uint32_t allowed_flags = PROS_INPUT_FLAG_PRESS | PROS_INPUT_FLAG_RELEASE;
    if ((event->flags & ~allowed_flags) != 0u ||
        event->value0 < 0 || (uint32_t)event->value0 > 0x1FFu)
        return 0u;

    uint32_t edge = event->flags & allowed_flags;
    if (edge != PROS_INPUT_FLAG_PRESS && edge != PROS_INPUT_FLAG_RELEASE)
        return 0u;

    uint32_t raw = (uint32_t)event->value0;
    /* The PS/2 decoder strips the break bit; it must never reach this ABI. */
    if ((raw & 0x80u) != 0u)
        return 0u;
    uint8_t extended = (raw & 0x100u) != 0u;
    uint8_t code = (uint8_t)(raw & 0x7Fu);
    uint8_t pressed = edge == PROS_INPUT_FLAG_PRESS;
    uint8_t shifted;

    if (!extended && (code == 0x2Au || code == 0x36u)) {
        if (code == 0x2Au) state->left_shift = pressed;
        else state->right_shift = pressed;
        return 0u;
    }
    if (!extended && code == 0x3Au) {
        /* Typematic make codes must not toggle Caps Lock repeatedly. */
        if (pressed && !state->caps_pressed)
            state->caps_lock = (uint8_t)!state->caps_lock;
        state->caps_pressed = pressed;
        return 0u;
    }
    if (code == 0x1Du) {
        if (extended) state->right_control = pressed;
        else state->control = pressed;
        return 0u;
    }
    if (code == 0x38u) {
        if (extended) state->right_alt = pressed;
        else state->alt = pressed;
        return 0u;
    }
    if (!pressed || extended || state->control || state->right_control ||
        state->alt || state->right_alt)
        return 0u;

    shifted = (uint8_t)(state->left_shift || state->right_shift);
    char ch = shifted ? shifted_map[code] : plain_map[code];
    if (ch >= 'a' && ch <= 'z' && (shifted != state->caps_lock))
        ch = (char)(ch - 'a' + 'A');
    return (uint8_t)ch;
}
