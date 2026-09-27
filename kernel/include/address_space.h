#ifndef PROSENJIT_ADDRESS_SPACE_H
#define PROSENJIT_ADDRESS_SPACE_H

#include <stdint.h>

#define PROSENJIT_USER_TEST_VA   0x0000008000001000ULL
#define PROSENJIT_USER_CODE_VA   0x0000008000000000ULL
#define PROSENJIT_USER_STACK_VA  0x0000008000002000ULL
#define PROSENJIT_USER_STACK_TOP 0x0000008000003000ULL

struct pros_address_space {
    uint64_t *root;
};

int address_space_create(struct pros_address_space *space);
int address_space_map_user_page(struct pros_address_space *space,
                                uint64_t virtual_address,
                                uint64_t physical_address,
                                uint64_t flags);
int address_space_switch(struct pros_address_space *space);

#endif
