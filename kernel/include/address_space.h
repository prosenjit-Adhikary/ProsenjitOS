#ifndef PROSENJIT_ADDRESS_SPACE_H
#define PROSENJIT_ADDRESS_SPACE_H

#include <stdint.h>

#define PROSENJIT_USER_TEST_VA   0x0000008000001000ULL
#define PROSENJIT_USER_CODE_VA   0x0000008000000000ULL
#define PROSENJIT_USER_STACK_VA  0x0000008000002000ULL
#define PROSENJIT_USER_STACK_TOP 0x0000008000003000ULL

/* Kernel-internal ownership record; exposed for integrity diagnostics/tests. */
struct pros_owned_page {
    uint64_t virtual_address;
    uint64_t physical_address;
    struct pros_owned_page *next;
};

struct pros_address_space {
    uint64_t *root;
    struct pros_owned_page *owned_pages;
};

int address_space_create(struct pros_address_space *space);
int address_space_map_user_page(struct pros_address_space *space,
                                uint64_t virtual_address,
                                uint64_t physical_address,
                                uint64_t flags);
int address_space_map_user_page_owned(struct pros_address_space *space,
                                      uint64_t virtual_address,
                                      uint64_t physical_address,
                                      uint64_t flags);
int address_space_unmap_user_page(struct pros_address_space *space,
                                  uint64_t virtual_address,
                                  uint64_t *physical_address_out);
/* Atomically unmap/free a bounded set of owned user VAs; other mappings stay intact. */
int address_space_unmap_owned_pages(struct pros_address_space *space,
                                    const uint64_t *virtual_pages,
                                    uint64_t page_count);
int address_space_last_map_failure(void);
int address_space_owned_pages_preflight(const struct pros_address_space *space);
int address_space_destroy_preflight(const struct pros_address_space *space);
int address_space_destroy(struct pros_address_space *space);
int address_space_switch(struct pros_address_space *space);
int address_space_translate_user(const struct pros_address_space *space,
                                 uint64_t virtual_address,
                                 uint64_t *physical_address_out,
                                 uint64_t *flags_out);
int address_space_validate_user_range(const struct pros_address_space *space,
                                      uint64_t virtual_address,
                                      uint64_t size,
                                      int write_access);
int address_space_selftest(void);

#endif
