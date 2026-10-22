#include <stdio.h>
#include "hw_stack.h"
int main(void) { int rc=pros_hw_stack_selftest(); if(rc!=PROS_DRIVER_OK){fprintf(stderr,"hardware stack contract tests failed: %d\n",rc);return 1;} puts("native hardware stack contract tests passed"); return 0; }
