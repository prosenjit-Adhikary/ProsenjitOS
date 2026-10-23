#include "memory.h"

#define MULTIBOOT2_TAG_END 0u
#define MULTIBOOT2_TAG_MMAP 6u
#define MULTIBOOT2_MEMORY_AVAILABLE 1u

#define MAX_PAGES (PROSENJIT_PHYS_LIMIT / PROSENJIT_PAGE_SIZE)
#define BITMAP_WORDS ((MAX_PAGES + 63ULL) / 64ULL)

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct multiboot_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

/*
 * The current bootstrap maps only physical addresses below 1 GiB. Validate
 * the boot-info header before dereferencing it, then require the entire tagged
 * structure to remain inside that identity-mapped range.
 */
static int multiboot_info_header_address_valid(uint64_t address) {
    return address != 0 &&
           (address & 7ULL) == 0 &&
           address <= PROSENJIT_PHYS_LIMIT - 8ULL;
}

static int multiboot_info_range_valid(uint64_t address, uint32_t total_size) {
    if (!multiboot_info_header_address_valid(address) ||
        total_size < 16u || total_size > (16u * 1024u * 1024u) ||
        address > UINT64_MAX - (uint64_t)total_size)
        return 0;
    return (uint64_t)total_size <= PROSENJIT_PHYS_LIMIT - address;
}

extern char __kernel_start[];
extern char __kernel_end[];

static uint64_t page_bitmap[BITMAP_WORDS];
/* managed_bitmap distinguishes allocator-owned pages from permanently reserved pages. */
static uint64_t managed_bitmap[BITMAP_WORDS];
/* Only explicitly user-allocated pages may be mapped into CPL3. */
static uint64_t user_bitmap[BITMAP_WORDS];
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t failed_allocations;
static int allocator_initialized;

/*
 * IRQ-save spin lock: disabling local maskable interrupts prevents an IRQ on
 * this CPU from recursively taking the allocator lock; the atomic exchange
 * serializes other CPUs. NMI handlers must not call this allocator.
 */
static volatile uint32_t allocator_lock;

static inline uint64_t allocator_lock_acquire(void) {
    uint64_t flags = 0;
#ifndef PROSENJIT_MEMORY_HOST_TEST
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
#endif
    while (__atomic_exchange_n(&allocator_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile ("pause" ::: "memory");
    }
    return flags;
}

static inline void allocator_lock_release(uint64_t flags) {
    __atomic_store_n(&allocator_lock, 0u, __ATOMIC_RELEASE);
#ifndef PROSENJIT_MEMORY_HOST_TEST
    if ((flags & (1ULL << 9)) != 0)
        __asm__ volatile ("sti" ::: "memory");
#else
    (void)flags;
#endif
}

static inline void bitmap_set(uint64_t page) {
    page_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
}

static inline void bitmap_clear(uint64_t page) {
    page_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
}

static inline int bitmap_test(uint64_t page) {
    return (page_bitmap[page >> 6] & (1ULL << (page & 63ULL))) != 0;
}

static inline void managed_set(uint64_t page) {
    managed_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
}

static inline void managed_clear(uint64_t page) {
    managed_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
}

static inline int managed_test(uint64_t page) {
    return (managed_bitmap[page >> 6] & (1ULL << (page & 63ULL))) != 0;
}

/*
 * Keep the kernel freestanding: GCC may lower __builtin_popcountll() to
 * the external libgcc helper __popcountdi2 when the target lacks POPCNT.
 * The kernel is linked with -nostdlib and deliberately does not depend on
 * libgcc, so use a local implementation instead.
 */
static inline uint64_t bitmap_popcount64(uint64_t value) {
    value = value - ((value >> 1) & 0x5555555555555555ULL);
    value = (value & 0x3333333333333333ULL) +
            ((value >> 2) & 0x3333333333333333ULL);
    value = (value + (value >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (value * 0x0101010101010101ULL) >> 56;
}

static inline uint64_t bitmap_ctz64(uint64_t value) {
    uint64_t count = 0;
    while ((value & 1ULL) == 0) {
        value >>= 1;
        ++count;
    }
    return count;
}

static void reserve_range(uint64_t base, uint64_t length) {
    if (length == 0 || base >= PROSENJIT_PHYS_LIMIT) return;

    uint64_t end = base + length;
    if (end < base || end > PROSENJIT_PHYS_LIMIT) end = PROSENJIT_PHYS_LIMIT;

    uint64_t first = base / PROSENJIT_PAGE_SIZE;
    uint64_t last = (end + PROSENJIT_PAGE_SIZE - 1ULL) / PROSENJIT_PAGE_SIZE;
    if (last > MAX_PAGES) last = MAX_PAGES;

    for (uint64_t page = first; page < last; ++page) {
        if (managed_test(page)) {
            managed_clear(page);
            if (bitmap_test(page)) {
                bitmap_clear(page);
                if (free_pages != 0) --free_pages;
            }
        }
    }
}

static void mark_available(uint64_t base, uint64_t length) {
    if (length == 0 || base >= PROSENJIT_PHYS_LIMIT) return;

    uint64_t end = base + length;
    if (end < base || end > PROSENJIT_PHYS_LIMIT) end = PROSENJIT_PHYS_LIMIT;

    uint64_t first = (base + PROSENJIT_PAGE_SIZE - 1ULL) / PROSENJIT_PAGE_SIZE;
    uint64_t last = end / PROSENJIT_PAGE_SIZE;
    if (last > MAX_PAGES) last = MAX_PAGES;

    for (uint64_t page = first; page < last; ++page) {
        if (!managed_test(page)) managed_set(page);
        if (!bitmap_test(page)) {
            bitmap_set(page);
            ++free_pages;
        }
    }
}

int physical_memory_init(uint64_t multiboot_info_address) {
    if (multiboot_info_address == 0) return -1;
#ifndef PROSENJIT_MEMORY_HOST_TEST
    /* Never dereference a physical boot-info pointer before range-checking it. */
    if (!multiboot_info_header_address_valid(multiboot_info_address))
        return -2;
#endif

    uint32_t total_size = *(const uint32_t *)(uintptr_t)multiboot_info_address;
    if (total_size < 16u || total_size > (16u * 1024u * 1024u)) return -2;
    if (multiboot_info_address > UINT64_MAX - (uint64_t)total_size)
        return -2;
#ifndef PROSENJIT_MEMORY_HOST_TEST
    /* The complete tag chain must be readable through the bootstrap identity map. */
    if (!multiboot_info_range_valid(multiboot_info_address, total_size))
        return -2;
#endif

    uint64_t current = multiboot_info_address + 8ULL;
    uint64_t end = multiboot_info_address + (uint64_t)total_size;
    int saw_end_tag = 0;

    /*
     * First pass: validate the complete tag chain before using any map entry.
     * Subtract-before-add checks prevent wrapped tag sizes from passing bounds
     * validation, and the mmap payload must fit inside its enclosing tag.
     */
    while (current <= end && end - current >= 8ULL) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(uintptr_t)current;
        if (tag->size < 8u || (uint64_t)tag->size > end - current)
            return -3;

        if (tag->type == MULTIBOOT2_TAG_END) {
            if (tag->size != 8u) return -3;
            saw_end_tag = 1;
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            const struct multiboot_tag_mmap *mmap =
                (const struct multiboot_tag_mmap *)tag;
            if (mmap->size < 16u ||
                mmap->entry_size < 24u || mmap->entry_version != 0u ||
                ((mmap->size - 16u) % mmap->entry_size) != 0u)
                return -4;
        }

        uint64_t next = current + (uint64_t)tag->size;
        if (next > UINT64_MAX - 7ULL) return -3;
        current = (next + 7ULL) & ~7ULL;
    }
    if (!saw_end_tag) return -3;

    /* Serialize initialization against allocators and other init callers. */
    uint64_t lock_flags = allocator_lock_acquire();
    /* Validate malformed input first, but never reset a live allocator. */
    if (allocator_initialized) {
        allocator_lock_release(lock_flags);
        return -6;
    }

    /* Only mutate allocator state after the complete boot tag chain validates. */
    for (uint64_t i = 0; i < BITMAP_WORDS; ++i) {
        page_bitmap[i] = 0;
        managed_bitmap[i] = 0;
        user_bitmap[i] = 0;
    }
    total_pages = 0;
    free_pages = 0;
    failed_allocations = 0;

    /*
     * Second pass: admit only complete, validated available ranges. Reserved
     * firmware ranges are applied in a later pass so they always win if a
     * malformed or overlapping map describes the same physical page as both
     * available and reserved.
     */
    current = multiboot_info_address + 8ULL;
    while (current < end) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(uintptr_t)current;
        if (tag->type == MULTIBOOT2_TAG_END) break;

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            const struct multiboot_tag_mmap *mmap =
                (const struct multiboot_tag_mmap *)tag;
            uint64_t entries_start = current + 16ULL;
            uint64_t entries_end = current + mmap->size;

            for (uint64_t p = entries_start; p < entries_end;
                 p += mmap->entry_size) {
                const struct multiboot_mmap_entry *entry =
                    (const struct multiboot_mmap_entry *)(uintptr_t)p;
                if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE)
                    mark_available(entry->base_addr, entry->length);
            }
        }

        uint64_t next = current + (uint64_t)tag->size;
        current = (next + 7ULL) & ~7ULL;
    }

    /*
     * Third pass: non-available entries override overlapping available entries.
     * This is conservative: ambiguous firmware memory is never handed to the
     * allocator merely because an available entry was encountered later.
     */
    current = multiboot_info_address + 8ULL;
    while (current < end) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(uintptr_t)current;
        if (tag->type == MULTIBOOT2_TAG_END) break;

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            const struct multiboot_tag_mmap *mmap =
                (const struct multiboot_tag_mmap *)tag;
            uint64_t entries_start = current + 16ULL;
            uint64_t entries_end = current + mmap->size;

            for (uint64_t p = entries_start; p < entries_end;
                 p += mmap->entry_size) {
                const struct multiboot_mmap_entry *entry =
                    (const struct multiboot_mmap_entry *)(uintptr_t)p;
                if (entry->type != MULTIBOOT2_MEMORY_AVAILABLE)
                    reserve_range(entry->base_addr, entry->length);
            }
        }

        uint64_t next = current + (uint64_t)tag->size;
        current = (next + 7ULL) & ~7ULL;
    }

    /*
     * Keep low memory and all boot/kernel storage unavailable to the allocator.
     * The current bootstrap page tables identity-map only the first 1 GiB, so
     * this first allocator deliberately exposes only that range.
     */
    reserve_range(0, 0x100000ULL);
    reserve_range((uint64_t)(uintptr_t)__kernel_start,
                  (uint64_t)((uintptr_t)__kernel_end - (uintptr_t)__kernel_start));

    /* The Multiboot information structure must remain readable by the kernel. */
    reserve_range(multiboot_info_address, total_size);

    /* Report the number of pages currently managed after reservations. */
    total_pages = 0;
    for (uint64_t word = 0; word < BITMAP_WORDS; ++word)
        total_pages += bitmap_popcount64(managed_bitmap[word]);
    if (free_pages == 0) {
        for (uint64_t i = 0; i < BITMAP_WORDS; ++i) {
            page_bitmap[i] = 0;
            managed_bitmap[i] = 0;
            user_bitmap[i] = 0;
        }
        total_pages = 0;
        free_pages = 0;
        failed_allocations = 0;
        allocator_lock_release(lock_flags);
        return -5;
    }
    allocator_initialized = 1;
    allocator_lock_release(lock_flags);
    return 0;
}

static uint64_t physical_page_alloc_locked(void) {
    if (!allocator_initialized) {
        ++failed_allocations;
        return 0;
    }
    for (uint64_t word = 0; word < BITMAP_WORDS; ++word) {
        uint64_t bits = page_bitmap[word];
        if (bits != 0) {
            uint64_t bit = bitmap_ctz64(bits);
            uint64_t page = (word << 6) + bit;
            if (page >= MAX_PAGES || !managed_test(page)) {
                /* A free bit outside managed RAM is metadata corruption: fail closed. */
                ++failed_allocations;
                return 0;
            }

            bitmap_clear(page);
            user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
            if (free_pages == 0) {
                bitmap_set(page);
                ++failed_allocations;
                return 0;
            }
            --free_pages;
            return page * PROSENJIT_PAGE_SIZE;
        }
    }
    ++failed_allocations;
    return 0;
}

uint64_t physical_page_alloc(void) {
    uint64_t flags = allocator_lock_acquire();
    uint64_t physical = physical_page_alloc_locked();
    allocator_lock_release(flags);
    return physical;
}

uint64_t physical_page_alloc_user(void) {
    uint64_t flags = allocator_lock_acquire();
    uint64_t physical = physical_page_alloc_locked();
    if (physical != 0) {
        uint64_t page = physical / PROSENJIT_PAGE_SIZE;
        user_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
    }
    allocator_lock_release(flags);
    return physical;
}

int physical_page_is_allocated(uint64_t physical_address) {
    uint64_t flags = allocator_lock_acquire();
    int result = 0;
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) == 0) {
        uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
        if (allocator_initialized && page < MAX_PAGES && managed_test(page))
            result = !bitmap_test(page);
    }
    allocator_lock_release(flags);
    return result;
}

int physical_page_is_user_mappable(uint64_t physical_address) {
    uint64_t flags = allocator_lock_acquire();
    int result = 0;
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) == 0) {
        uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
        if (allocator_initialized && page < MAX_PAGES && managed_test(page) &&
            !bitmap_test(page))
            result = (user_bitmap[page >> 6] & (1ULL << (page & 63ULL))) != 0;
    }
    allocator_lock_release(flags);
    return result;
}

void physical_page_free(uint64_t physical_address) {
    uint64_t flags = allocator_lock_acquire();
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0) {
        allocator_lock_release(flags);
        return;
    }

    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (!allocator_initialized || page >= MAX_PAGES ||
        !managed_test(page) || bitmap_test(page)) {
        allocator_lock_release(flags);
        return;
    }

    user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
    bitmap_set(page);
    ++free_pages;
    allocator_lock_release(flags);
}

uint64_t physical_memory_total_pages(void) {
    uint64_t flags = allocator_lock_acquire();
    uint64_t value = total_pages;
    allocator_lock_release(flags);
    return value;
}

uint64_t physical_memory_free_pages(void) {
    uint64_t flags = allocator_lock_acquire();
    uint64_t value = free_pages;
    allocator_lock_release(flags);
    return value;
}

uint64_t physical_memory_failed_allocations(void) {
    uint64_t flags = allocator_lock_acquire();
    uint64_t value = failed_allocations;
    allocator_lock_release(flags);
    return value;
}


int physical_memory_selftest(void) {
    uint64_t before = free_pages;
    uint64_t a = physical_page_alloc();
    uint64_t b = physical_page_alloc();
    if (!a || !b || a == b || free_pages != before - 2ULL)
        return -1;

    physical_page_free(a);
    if (free_pages != before - 1ULL)
        return -2;

    /* User classification is explicit and is cleared on free. */
    uint64_t user = physical_page_alloc_user();
    if (!user || !physical_page_is_user_mappable(user) ||
        !physical_page_is_allocated(user)) {
        if (user) physical_page_free(user);
        return -3;
    }
    physical_page_free(user);
    if (physical_page_is_user_mappable(user) ||
        physical_page_is_allocated(user))
        return -3;

    /* Double-free must not inflate the free-page count. */
    physical_page_free(a);
    if (free_pages != before - 1ULL)
        return -3;

    physical_page_free(b);
    if (free_pages != before)
        return -4;

    /* Page zero is permanently reserved by physical_memory_init(). */
    physical_page_free(0);
    if (free_pages != before || managed_test(0))
        return -5;

    return 0;
}
