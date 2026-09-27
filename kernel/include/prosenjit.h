#ifndef PROSENJIT_H
#define PROSENJIT_H

#include <stdint.h>

#define PROSENJIT_OS_VERSION "0.1.0"

void kernel_main(uint32_t boot_magic, uint32_t boot_info);

#endif
