#ifndef PROSENJIT_ABI_DISPLAY_H
#define PROSENJIT_ABI_DISPLAY_H

#include <stdint.h>
#define PROSENJIT_DISPLAY_ABI_VERSION 1u
enum pros_display_format { PROS_DISPLAY_XRGB8888 = 1 };
struct pros_display_info { uint32_t version,width,height,pitch,format; uint64_t framebuffer,framebuffer_bytes; };
struct pros_display_event { uint32_t type,flags; uint64_t timestamp,value0,value1; };
#define PROS_DISPLAY_EVENT_NONE 0u
#define PROS_DISPLAY_EVENT_RESIZE 1u
#define PROS_DISPLAY_EVENT_HOTPLUG 2u
#define PROS_DISPLAY_EVENT_VSYNC 3u
#define PROS_DISPLAY_EVENT_DEVICE_LOST 4u
#endif
