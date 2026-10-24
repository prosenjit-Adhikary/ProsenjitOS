#include <stdio.h>
#include "pci.h"

int main(void)
{
    int rc = pci_selftest();
    if (rc != PROS_PCI_OK) {
        fprintf(stderr, "PCI decoder self-test failed: %d\n", rc);
        return 1;
    }
    puts("PCI BAR decoder contract tests passed");
    return 0;
}
