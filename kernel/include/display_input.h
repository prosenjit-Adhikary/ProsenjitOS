#ifndef PROSENJIT_DISPLAY_INPUT_H
#define PROSENJIT_DISPLAY_INPUT_H
#include <stdint.h>
#define PROS_DISPLAY_OK 0
#define PROS_DISPLAY_EINVAL -1
struct pros_display_device {uint32_t width,height,stride;uint32_t *framebuffer;uint8_t active;};
struct pros_input_event {uint16_t type;uint16_t code;int32_t value;uint64_t timestamp;};
int pros_display_init(struct pros_display_device *,uint32_t,uint32_t,uint32_t *);
int pros_display_set_active(struct pros_display_device *,uint8_t);
int pros_input_validate(const struct pros_input_event *);
int pros_display_input_selftest(void);
#endif
