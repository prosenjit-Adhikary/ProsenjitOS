#include "pe_inspect.h"

int main(void)
{
    return pros_pe_selftest() == PROS_DRIVER_OK ? 0 : 1;
}
