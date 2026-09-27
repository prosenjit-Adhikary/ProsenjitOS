#ifndef PROSENJIT_PIC_H
#define PROSENJIT_PIC_H

#include <stdint.h>
int pic_init_for_timer(void);
void pic_eoi_master(void);
#endif
