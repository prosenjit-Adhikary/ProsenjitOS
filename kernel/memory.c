#include "memory.h"

#define MULTIBOOT2_TAG_END 0u
#define MULTIBOOT2_TAG_MMAP 6u
#define MULTIBOOT2_TAG_FRAMEBUFFER 8u
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

struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
} __attribute__((packed));

struct multiboot_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

extern char __kernel_start[];
extern char __kernel_end[];

static uint64_t page_bitmap[BITMAP_WORDS];
/* managed_bitmap distinguishes allocator-owned pages from permanently reserved pages. */
static uint64_t managed_bitmap[BITMAP_WORDS];
/* Only explicitly user-allocated pages may be mapped into CPL3. */
static uint64_t user_bitmap[BITMAP_WORDS];
/* Marks every page in a contiguous allocation, not just its run head. */
static uint64_t contiguous_bitmap[BITMAP_WORDS];
static uint16_t contiguous_run_length[MAX_PAGES];
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t failed_allocations;
static uint32_t page_allocator_lock;

/* IRQ-safe spin lock: allocator calls may occur from kernel paths on multiple CPUs. */
static uint64_t page_allocator_irq_save(void) {
#if __STDC_HOSTED__
    return 0u;
#else
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
#endif
}
static void page_allocator_irq_restore(uint64_t flags) {
#if __STDC_HOSTED__
    (void)flags;
#else
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
#endif
}
static uint64_t page_allocator_lock_acquire(void) {
    uint64_t flags = page_allocator_irq_save();
    while (__atomic_exchange_n(&page_allocator_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    return flags;
}
static void page_allocator_lock_release(uint64_t flags) {
    __atomic_store_n(&page_allocator_lock, 0u, __ATOMIC_RELEASE);
    page_allocator_irq_restore(flags);
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
static inline void contiguous_set(uint64_t page) {
    contiguous_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
}
static inline void contiguous_clear(uint64_t page) {
    contiguous_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
}
static inline int contiguous_test(uint64_t page) {
    return (contiguous_bitmap[page >> 6] & (1ULL << (page & 63ULL))) != 0;
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
    for (uint64_t i = 0; i < BITMAP_WORDS; ++i) {
        page_bitmap[i] = 0;
        managed_bitmap[i] = 0;
        user_bitmap[i] = 0;
        contiguous_bitmap[i] = 0;
    }
    for (uint64_t i = 0; i < MAX_PAGES; ++i)
        contiguous_run_length[i] = 0;
    total_pages = 0;
    free_pages = 0;
    failed_allocations = 0;

    if (multiboot_info_address == 0) return -1;

    uint32_t total_size = *(const uint32_t *)(uintptr_t)multiboot_info_address;
    if (total_size < 16u || total_size > (16u * 1024u * 1024u)) return -2;
    if (multiboot_info_address > UINT64_MAX - (uint64_t)total_size)
        return -2;

    uint64_t framebuffer_base = 0u;
    uint64_t framebuffer_length = 0u;
    uint64_t current = multiboot_info_address + 8ULL;
    uint64_t end = multiboot_info_address + (uint64_t)total_size;

    while (current <= end && end - current >= 8ULL) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)(uintptr_t)current;
        if (tag->type == MULTIBOOT2_TAG_END) break;
        if (tag->size < 8u || current + tag->size > end) return -3;

        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER &&
            tag->size >= sizeof(struct multiboot_tag_framebuffer)) {
            const struct multiboot_tag_framebuffer *fb =
                (const struct multiboot_tag_framebuffer *)tag;
            uint64_t bytes = (uint64_t)fb->pitch * (uint64_t)fb->height;
            if (fb->pitch != 0u && fb->height != 0u &&
                bytes != 0u && bytes <= (256ULL * 1024ULL * 1024ULL) &&
                fb->address <= 0x000FFFFFFFFFFFFFULL &&
                bytes - 1u <= 0x000FFFFFFFFFFFFFULL - fb->address) {
                framebuffer_base = fb->address;
                framebuffer_length = bytes;
            }
        }

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            const struct multiboot_tag_mmap *mmap =
                (const struct multiboot_tag_mmap *)tag;
            if (mmap->size < 16u || mmap->entry_size < 24u) return -4;
            /* A memory-map tag must contain whole entries, never a truncated tail. */
            if (((mmap->size - 16u) % mmap->entry_size) != 0u) return -4;

            uint64_t entries_start = current + 16ULL;
            uint64_t entries_end = current + mmap->size;

            for (uint64_t p = entries_start;
                 p <= entries_end &&
                 entries_end - p >= mmap->entry_size;
                 p += mmap->entry_size) {
                const struct multiboot_mmap_entry *entry =
                    (const struct multiboot_mmap_entry *)(uintptr_t)p;
                if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE)
                    mark_available(entry->base_addr, entry->length);
            }
        }

        uint64_t next = current + (uint64_t)tag->size;
        if (next > UINT64_MAX - 7ULL)
            return -3;
        current = (next + 7ULL) & ~7ULL;
    }

    /*
     * Firmware memory-map entries should not overlap, but treat every
     * non-available range as reserved even if a malformed/overlapping map
     * also described the same pages as available. Do this in a second pass
     * so the final result does not depend on entry ordering.
     */
    current = multiboot_info_address + 8ULL;
    while (current <= end && end - current >= 8ULL) {
        const struct multiboot_tag *tag =
            (const struct multiboot_tag *)(uintptr_t)current;
        if (tag->type == MULTIBOOT2_TAG_END) break;
        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            const struct multiboot_tag_mmap *mmap =
                (const struct multiboot_tag_mmap *)tag;
            uint64_t entries_start = current + 16ULL;
            uint64_t entries_end = current + mmap->size;
            for (uint64_t p = entries_start;
                 p <= entries_end &&
                 entries_end - p >= mmap->entry_size;
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

    /* The bootloader framebuffer must never be returned as ordinary RAM. */
    if (framebuffer_length != 0u)
        reserve_range(framebuffer_base, framebuffer_length);

    /* The Multiboot information structure must remain readable by the kernel. */
    reserve_range(multiboot_info_address, total_size);

    /* Report the number of pages currently managed after reservations. */
    total_pages = 0;
    for (uint64_t word = 0; word < BITMAP_WORDS; ++word)
        total_pages += bitmap_popcount64(managed_bitmap[word]);
    return free_pages != 0 ? 0 : -5;
}

static uint64_t physical_page_alloc_locked(void) {
    for (uint64_t word = 0; word < BITMAP_WORDS; ++word) {
        uint64_t bits = page_bitmap[word];
        if (bits != 0) {
            uint64_t bit = bitmap_ctz64(bits);
            uint64_t page = (word << 6) + bit;
            if (page >= MAX_PAGES) {
                ++failed_allocations;
                return 0;
            }

            bitmap_clear(page);
            --free_pages;
            return page * PROSENJIT_PAGE_SIZE;
        }
    }
    ++failed_allocations;
    return 0;
}

uint64_t physical_page_alloc(void) {
    uint64_t flags = page_allocator_lock_acquire();
    uint64_t physical = physical_page_alloc_locked();
    page_allocator_lock_release(flags);
    return physical;
}

/*
 * Allocate a physically contiguous run for future DMA-capable consumers.
 * max_address_exclusive is a physical-address ceiling; zero is invalid.
 * Alignment is expressed in pages and must be a power of two.
 * This API does not itself make a buffer DMA-safe: cache coherency,
 * pinning, device ownership and IOMMU/address-mask handling remain separate.
 */
static void physical_page_record_failed_allocation(void) {
    uint64_t flags = page_allocator_lock_acquire();
    ++failed_allocations;
    page_allocator_lock_release(flags);
}

uint64_t physical_pages_alloc_contiguous(uint64_t page_count,
                                         uint64_t alignment_pages,
                                         uint64_t max_address_exclusive) {
    if (page_count == 0 || page_count > UINT16_MAX || alignment_pages == 0 ||
        (alignment_pages & (alignment_pages - 1ULL)) != 0 ||
        max_address_exclusive < PROSENJIT_PAGE_SIZE) {
        physical_page_record_failed_allocation();
        return 0;
    }

    if (max_address_exclusive > PROSENJIT_PHYS_LIMIT)
        max_address_exclusive = PROSENJIT_PHYS_LIMIT;
    uint64_t limit_pages = max_address_exclusive / PROSENJIT_PAGE_SIZE;
    if (page_count > limit_pages) {
        physical_page_record_failed_allocation();
        return 0;
    }

    uint64_t flags = page_allocator_lock_acquire();
    uint64_t candidate = alignment_pages;
    while (candidate <= limit_pages - page_count) {
        uint64_t i = 0;
        while (i < page_count) {
            uint64_t page = candidate + i;
            if (!managed_test(page) || !bitmap_test(page))
                break;
            ++i;
        }
        if (i == page_count) {
            for (uint64_t j = 0; j < page_count; ++j) {
                bitmap_clear(candidate + j);
                contiguous_set(candidate + j);
            }
            contiguous_run_length[candidate] = (uint16_t)page_count;
            free_pages -= page_count;
            page_allocator_lock_release(flags);
            return candidate * PROSENJIT_PAGE_SIZE;
        }
        /* Skip past the first unavailable page, then realign. */
        candidate += i + 1ULL;
        uint64_t remainder = candidate & (alignment_pages - 1ULL);
        if (remainder != 0)
            candidate += alignment_pages - remainder;
    }
    ++failed_allocations;
    page_allocator_lock_release(flags);
    return 0;
}

/* Release only a wholly allocated, managed run; invalid runs change nothing. */
int physical_pages_free_contiguous(uint64_t physical_address,
                                   uint64_t page_count) {
    if (page_count == 0 ||
        (physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return -1;
    uint64_t first = physical_address / PROSENJIT_PAGE_SIZE;
    if (first == 0 || first >= MAX_PAGES || page_count > MAX_PAGES - first)
        return -1;

    uint64_t flags = page_allocator_lock_acquire();
    if (contiguous_run_length[first] != page_count) {
        page_allocator_lock_release(flags);
        return -3;
    }
    for (uint64_t i = 0; i < page_count; ++i) {
        uint64_t page = first + i;
        if (!managed_test(page) || bitmap_test(page) || !contiguous_test(page)) {
            page_allocator_lock_release(flags);
            return -2;
        }
    }
    contiguous_run_length[first] = 0;
    for (uint64_t i = 0; i < page_count; ++i) {
        uint64_t page = first + i;
        user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
        contiguous_clear(page);
        bitmap_set(page);
    }
    free_pages += page_count;
    page_allocator_lock_release(flags);
    return 0;
}

uint64_t physical_page_alloc_user(void) {
    uint64_t flags = page_allocator_lock_acquire();
    uint64_t physical = physical_page_alloc_locked();
    if (physical) {
        uint64_t page = physical / PROSENJIT_PAGE_SIZE;
        user_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
    }
    page_allocator_lock_release(flags);
    return physical;
}

int physical_page_is_allocated(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return 0;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES) return 0;
    uint64_t flags = page_allocator_lock_acquire();
    int allocated = managed_test(page) && !bitmap_test(page);
    page_allocator_lock_release(flags);
    return allocated;
}

int physical_page_is_user_mappable(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return 0;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES) return 0;
    uint64_t flags = page_allocator_lock_acquire();
    int mappable = managed_test(page) && !bitmap_test(page) &&
        (user_bitmap[page >> 6] & (1ULL << (page & 63ULL))) != 0;
    page_allocator_lock_release(flags);
    return mappable;
}

void physical_page_free(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0) return;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES) return;

    uint64_t flags = page_allocator_lock_acquire();
    if (managed_test(page) && !bitmap_test(page) && !contiguous_test(page)) {
        user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
        bitmap_set(page);
        ++free_pages;
    }
    page_allocator_lock_release(flags);
}

uint64_t physical_memory_total_pages(void) {
    uint64_t flags = page_allocator_lock_acquire();
    uint64_t value = total_pages;
    page_allocator_lock_release(flags);
    return value;
}

uint64_t physical_memory_free_pages(void) {
    uint64_t flags = page_allocator_lock_acquire();
    uint64_t value = free_pages;
    page_allocator_lock_release(flags);
    return value;
}

uint64_t physical_memory_failed_allocations(void) {
    uint64_t flags = page_allocator_lock_acquire();
    uint64_t value = failed_allocations;
    page_allocator_lock_release(flags);
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
