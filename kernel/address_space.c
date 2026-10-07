#include "address_space.h"
#include "memory.h"
#include "paging.h"
#include "heap.h"

struct pros_owned_page {
    uint64_t virtual_address;
    uint64_t physical_address;
    struct pros_owned_page *next;
};

#define PAGE_MASK 0x000FFFFFFFFFF000ULL
#define ENTRIES 512u
#define PT_INDEX(va) (((va) >> 12) & 0x1FFu)
#define PD_INDEX(va) (((va) >> 21) & 0x1FFu)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FFu)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FFu)
#define LARGE_PAGE_FLAG (1ULL << 7)
#define USER_MIN_VA PROSENJIT_USER_CODE_VA

extern uint64_t *paging_kernel_root(void);

static void zero_page(uint64_t *page) {
    for (uint32_t i = 0; i < ENTRIES; ++i) page[i] = 0;
}

static int user_va_valid(uint64_t virtual_address) {
    return virtual_address >= USER_MIN_VA &&
           virtual_address < 0x0000800000000000ULL &&
           (virtual_address & 0xFFFULL) == 0;
}

static int physical_already_mapped(const struct pros_address_space *space,
                                   uint64_t physical_address) {
    if (!space || !space->root) return 1;
    for (uint32_t pml4 = 1; pml4 < 256; ++pml4) {
        uint64_t pml4e = space->root[pml4];
        if (!(pml4e & PROSENJIT_PAGE_PRESENT) || (pml4e & LARGE_PAGE_FLAG)) continue;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PAGE_MASK);
        for (uint32_t pdpti = 0; pdpti < ENTRIES; ++pdpti) {
            uint64_t pdpte = pdpt[pdpti];
            if (!(pdpte & PROSENJIT_PAGE_PRESENT) || (pdpte & LARGE_PAGE_FLAG)) continue;
            uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PAGE_MASK);
            for (uint32_t pdi = 0; pdi < ENTRIES; ++pdi) {
                uint64_t pde = pd[pdi];
                if (!(pde & PROSENJIT_PAGE_PRESENT) || (pde & LARGE_PAGE_FLAG)) continue;
                uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PAGE_MASK);
                for (uint32_t pti = 0; pti < ENTRIES; ++pti) {
                    uint64_t pte = pt[pti];
                    if ((pte & PROSENJIT_PAGE_PRESENT) &&
                        (pte & PAGE_MASK) == physical_address)
                        return 1;
                }
            }
        }
    }
    return 0;
}

static uint64_t *alloc_table(void) {
    uint64_t physical = physical_page_alloc();
    if (!physical) return 0;
    uint64_t *table = (uint64_t *)(uintptr_t)physical;
    zero_page(table);
    return table;
}

static uint64_t *ensure_table(uint64_t *parent, uint32_t index) {
    uint64_t entry = parent[index];
    if (entry & PROSENJIT_PAGE_PRESENT) {
        if (entry & LARGE_PAGE_FLAG) return 0;
        return (uint64_t *)(uintptr_t)(entry & PAGE_MASK);
    }

    uint64_t *table = alloc_table();
    if (!table) return 0;
    parent[index] = ((uint64_t)(uintptr_t)table & PAGE_MASK) |
                    PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_WRITABLE |
                    PROSENJIT_PAGE_USER;
    return table;
}

int address_space_create(struct pros_address_space *space) {
    if (!space) return -1;
    uint64_t *root = alloc_table();
    if (!root) return -1;

    uint64_t *kernel_root = paging_kernel_root();
    if (!kernel_root) {
        physical_page_free((uint64_t)(uintptr_t)root);
        return -1;
    }

    /*
     * PML4[0] is shared with the kernel's low identity map so CPL3
     * interrupts can still enter kernel code and use the TSS RSP0 stack.
     * Keep this top-level entry supervisor-only: user pages live at
     * PML4[1] (0x0000008000000000), so they do not need USER on PML4[0].
     */ 
    root[0] = kernel_root[0] & ~PROSENJIT_PAGE_USER;
    for (uint32_t i = 256; i < ENTRIES; ++i)
        root[i] = kernel_root[i];

    space->root = root;
    space->owned_pages = 0;
    return 0;
}

int address_space_map_user_page(struct pros_address_space *space,
                                uint64_t virtual_address,
                                uint64_t physical_address,
                                uint64_t flags) {
    if (!space || !space->root || !user_va_valid(virtual_address)) return -1;
    if ((physical_address & 0xFFFULL) ||
        (physical_address & ~PAGE_MASK) != 0 || physical_address == 0 ||
        (flags & ~(PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX)) != 0 ||
        ((flags & PROSENJIT_PAGE_WRITABLE) && !(flags & PROSENJIT_PAGE_NX)))
        return -1;

    uint32_t pml4i = PML4_INDEX(virtual_address);
    uint32_t pdpti = PDPT_INDEX(virtual_address);
    uint32_t pdi = PD_INDEX(virtual_address);
    uint32_t pti = PT_INDEX(virtual_address);
    uint64_t *root = space->root;
    uint64_t old_pml4e = root[pml4i];
    uint64_t *pdpt = ensure_table(root, pml4i);
    if (!pdpt) return -1;
    int created_pdpt = !(old_pml4e & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pdpte = pdpt[pdpti];
    uint64_t *pd = ensure_table(pdpt, pdpti);
    if (!pd) {
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            root[pml4i] = 0;
        }
        return -1;
    }
    int created_pd = !(old_pdpte & PROSENJIT_PAGE_PRESENT);

    uint64_t old_pde = pd[pdi];
    uint64_t *pt = ensure_table(pd, pdi);
    if (!pt) {
        if (created_pd) {
            physical_page_free((uint64_t)(uintptr_t)pd);
            pdpt[pdpti] = 0;
        }
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            root[pml4i] = 0;
        }
        return -1;
    }
    int created_pt = !(old_pde & PROSENJIT_PAGE_PRESENT);

    if (pt[pti] & PROSENJIT_PAGE_PRESENT) {
        if (created_pt) {
            physical_page_free((uint64_t)(uintptr_t)pt);
            pd[pdi] = 0;
        }
        if (created_pd) {
            physical_page_free((uint64_t)(uintptr_t)pd);
            pdpt[pdpti] = 0;
        }
        if (created_pdpt) {
            physical_page_free((uint64_t)(uintptr_t)pdpt);
            root[pml4i] = 0;
        }
        return -1;
    }

    pt[pti] =
        (physical_address & PAGE_MASK) |
        PROSENJIT_PAGE_PRESENT |
        PROSENJIT_PAGE_USER |
        (flags & (PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX));
    return 0;
}

int address_space_map_user_page_owned(struct pros_address_space *space,
                                      uint64_t virtual_address,
                                      uint64_t physical_address,
                                      uint64_t flags) {
    if (!space || !space->root || !user_va_valid(virtual_address) ||
        (physical_address & 0xFFFULL) ||
        (physical_address & ~PAGE_MASK) != 0 || physical_address == 0 ||
        (flags & ~(PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX)) != 0 ||
        ((flags & PROSENJIT_PAGE_WRITABLE) && !(flags & PROSENJIT_PAGE_NX)))
        return -1;
    if (physical_already_mapped(space, physical_address))
        return -1;
    struct pros_owned_page *owned = (struct pros_owned_page *)kmalloc(sizeof(*owned));
    if (!owned)
        return -1;
    if (address_space_map_user_page(space, virtual_address, physical_address, flags) != 0) {
        kfree(owned);
        return -1;
    }
    owned->virtual_address = virtual_address;
    owned->physical_address = physical_address;
    owned->next = space->owned_pages;
    space->owned_pages = owned;
    return 0;
}

static void destroy_table(uint64_t *table, uint32_t level) {
    if (!table) return;

    for (uint32_t i = 0; i < ENTRIES; ++i) {
        uint64_t entry = table[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT)) continue;
        if (entry & LARGE_PAGE_FLAG) continue;

        uint64_t *child = (uint64_t *)(uintptr_t)(entry & PAGE_MASK);
        if (level > 1) {
            destroy_table(child, level - 1);
            physical_page_free((uint64_t)(uintptr_t)child);
        }
        /* At level 1, entries are leaf mappings; their physical pages
         * are owned by the caller and are intentionally not freed here. */
    }
}

int address_space_unmap_user_page(struct pros_address_space *space,
                                  uint64_t virtual_address,
                                  uint64_t *physical_address_out) {
    if (!space || !space->root || !user_va_valid(virtual_address))
        return -1;
    uint64_t e = space->root[PML4_INDEX(virtual_address)];
    if (!(e & PROSENJIT_PAGE_PRESENT) || !(e & PROSENJIT_PAGE_USER) ||
        (e & LARGE_PAGE_FLAG)) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(e & PAGE_MASK);
    e = pdpt[PDPT_INDEX(virtual_address)];
    if (!(e & PROSENJIT_PAGE_PRESENT) || !(e & PROSENJIT_PAGE_USER) ||
        (e & LARGE_PAGE_FLAG)) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(e & PAGE_MASK);
    e = pd[PD_INDEX(virtual_address)];
    if (!(e & PROSENJIT_PAGE_PRESENT) || !(e & PROSENJIT_PAGE_USER) ||
        (e & LARGE_PAGE_FLAG)) return -1;
    uint64_t *pt = (uint64_t *)(uintptr_t)(e & PAGE_MASK);
    e = pt[PT_INDEX(virtual_address)];
    if (!(e & PROSENJIT_PAGE_PRESENT) || !(e & PROSENJIT_PAGE_USER))
        return -1;
    uint64_t physical = e & PAGE_MASK;
    if (physical_address_out) *physical_address_out = physical;
    struct pros_owned_page **link = &space->owned_pages;
    while (*link) {
        if ((*link)->virtual_address == virtual_address) {
            struct pros_owned_page *owned = *link;
            *link = owned->next;
            kfree(owned);
            break;
        }
        link = &(*link)->next;
    }
    pt[PT_INDEX(virtual_address)] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 0;
}

int address_space_destroy(struct pros_address_space *space) {
    if (!space || !space->root) return -1;

    /*
     * PML4[0] and PML4[256..511] reference shared kernel mappings and are
     * never owned by this address space. Only user-space PML4[1..255] is
     * recursively owned and released.
     */
    for (uint32_t i = 1; i < 256; ++i) {
        uint64_t entry = space->root[i];
        if (!(entry & PROSENJIT_PAGE_PRESENT) ||
            (entry & LARGE_PAGE_FLAG))
            continue;

        uint64_t *pdpt = (uint64_t *)(uintptr_t)(entry & PAGE_MASK);
        destroy_table(pdpt, 3);
        physical_page_free((uint64_t)(uintptr_t)pdpt);
    }

    struct pros_owned_page *owned = space->owned_pages;
    while (owned) {
        struct pros_owned_page *next = owned->next;
        /* Only still-mapped pages remain in the ownership list. */
        physical_page_free(owned->physical_address);
        kfree(owned);
        owned = next;
    }
    space->owned_pages = 0;
    uint64_t root = (uint64_t)(uintptr_t)space->root;
    space->root = 0;
    physical_page_free(root);
    return 0;
}

int address_space_switch(struct pros_address_space *space) {
    if (!space || !space->root) return -1;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)(uintptr_t)space->root) : "memory");
    return 0;
}

int address_space_translate_user(const struct pros_address_space *space,
                                 uint64_t virtual_address,
                                 uint64_t *physical_address_out,
                                 uint64_t *flags_out) {
    if (!space || !space->root || !physical_address_out ||
        virtual_address < USER_MIN_VA || virtual_address >= 0x0000800000000000ULL)
        return -1;
    uint64_t pml4e = space->root[PML4_INDEX(virtual_address)];
    if ((pml4e & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
        return -1;
    if (pml4e & LARGE_PAGE_FLAG) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PAGE_MASK);
    uint64_t pdpte = pdpt[PDPT_INDEX(virtual_address)];
    if ((pdpte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
        return -1;
    if (pdpte & LARGE_PAGE_FLAG) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PAGE_MASK);
    uint64_t pde = pd[PD_INDEX(virtual_address)];
    if ((pde & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
        return -1;
    if (pde & LARGE_PAGE_FLAG) return -1;
    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PAGE_MASK);
    uint64_t pte = pt[PT_INDEX(virtual_address)];
    if ((pte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
        (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
        return -1;
    *physical_address_out = (pte & PAGE_MASK) | (virtual_address & 0xFFFULL);
    if (flags_out)
        *flags_out = pte & (PROSENJIT_PAGE_USER | PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX);
    return 0;
}

int address_space_validate_user_range(const struct pros_address_space *space,
                                      uint64_t virtual_address,
                                      uint64_t size,
                                      int write_access) {
    if (!space || !space->root || size == 0 || virtual_address < USER_MIN_VA) return -1;

    uint64_t end = virtual_address + size - 1ULL;
    if (end < virtual_address || (virtual_address >> 47) != 0 ||
        (end >> 47) != 0)
        return -1;

    for (uint64_t va = virtual_address & PAGE_MASK;; va += 0x1000ULL) {
        uint64_t pml4e = space->root[PML4_INDEX(va)];
        if ((pml4e & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
            return -1;

        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & PAGE_MASK);
        uint64_t pdpte = pdpt[PDPT_INDEX(va)];
        if ((pdpte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER) ||
            (pdpte & LARGE_PAGE_FLAG))
            return -1;

        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & PAGE_MASK);
        uint64_t pde = pd[PD_INDEX(va)];
        if ((pde & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER) ||
            (pde & LARGE_PAGE_FLAG))
            return -1;

        uint64_t *pt = (uint64_t *)(uintptr_t)(pde & PAGE_MASK);
        uint64_t pte = pt[PT_INDEX(va)];
        if ((pte & (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER)) !=
            (PROSENJIT_PAGE_PRESENT | PROSENJIT_PAGE_USER))
            return -1;
        if (write_access && !(pte & PROSENJIT_PAGE_WRITABLE))
            return -1;

        if (va >= (end & PAGE_MASK)) break;
    }

    return 0;
}


int address_space_selftest(void) {
    struct pros_address_space space = { 0, 0 };
    uint64_t physical = 0;
    uint64_t translated = 0;
    uint64_t flags = 0;

    if (address_space_create(&space) != 0)
        return -1;

    physical = physical_page_alloc();
    if (!physical) {
        address_space_destroy(&space);
        return -2;
    }

    if (address_space_map_user_page_owned(&space, PROSENJIT_USER_TEST_VA,
                                          physical,
                                          PROSENJIT_PAGE_WRITABLE |
                                          PROSENJIT_PAGE_NX) != 0) {
        physical_page_free(physical);
        address_space_destroy(&space);
        return -3;
    }

    if (address_space_translate_user(&space, PROSENJIT_USER_TEST_VA,
                                     &translated, &flags) != 0 ||
        translated != physical ||
        !(flags & PROSENJIT_PAGE_WRITABLE) ||
        !(flags & PROSENJIT_PAGE_NX) ||
        address_space_validate_user_range(&space, PROSENJIT_USER_TEST_VA,
                                          PROSENJIT_PAGE_SIZE, 1) != 0) {
        address_space_destroy(&space);
        return -4;
    }

    if (address_space_map_user_page(&space, PROSENJIT_USER_TEST_VA,
                                    physical, PROSENJIT_PAGE_NX) == 0 ||
        address_space_map_user_page_owned(&space, PROSENJIT_USER_TEST_VA + 0x1000ULL,
                                          physical, PROSENJIT_PAGE_WRITABLE |
                                          PROSENJIT_PAGE_NX) == 0 ||
        address_space_validate_user_range(&space, PROSENJIT_USER_TEST_VA,
                                          PROSENJIT_PAGE_SIZE, 0) != 0) {
        address_space_destroy(&space);
        return -5;
    }

    if (address_space_unmap_user_page(&space, PROSENJIT_USER_TEST_VA,
                                      &translated) != 0 ||
        translated != physical ||
        address_space_translate_user(&space, PROSENJIT_USER_TEST_VA,
                                     &translated, &flags) == 0) {
        physical_page_free(physical);
        address_space_destroy(&space);
        return -6;
    }

    physical_page_free(physical);

    if (address_space_validate_user_range(&space,
                                           PROSENJIT_USER_TEST_VA,
                                           0, 0) == 0 ||
        address_space_validate_user_range(&space,
                                           0x00007FFFFFFFF000ULL,
                                           0x2000ULL, 0) == 0 ||
        address_space_map_user_page(&space,
                                    0x0000800000000000ULL,
                                    physical,
                                    PROSENJIT_PAGE_NX) == 0) {
        address_space_destroy(&space);
        return -7;
    }

    if (address_space_destroy(&space) != 0 || space.root != 0 ||
        space.owned_pages != 0)
        return -8;

    return 0;
}
