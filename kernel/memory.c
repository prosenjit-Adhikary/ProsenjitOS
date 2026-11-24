#include "memory.h"
#include "abi/boot.h"

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
    for (uint64_t i = 0; i < BITMAP_WORDS; ++i) {
        page_bitmap[i] = 0;
        managed_bitmap[i] = 0;
        user_bitmap[i] = 0;
    }
    total_pages = 0;
    free_pages = 0;
    failed_allocations = 0;

    if (multiboot_info_address == 0) return -1;

    uint32_t total_size = *(const uint32_t *)(uintptr_t)multiboot_info_address;
    if (total_size < 16u || total_size > (16u * 1024u * 1024u)) return -2;
    if (multiboot_info_address > UINT64_MAX - (uint64_t)total_size)
        return -2;

    uint64_t current = multiboot_info_address + 8ULL;
    uint64_t end = multiboot_info_address + (uint64_t)total_size;

    while (current <= end && end - current >= 8ULL) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)(uintptr_t)current;
        if (tag->type == MULTIBOOT2_TAG_END) break;
        if (tag->size < 8u || current + tag->size > end) return -3;

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            const struct multiboot_tag_mmap *mmap =
                (const struct multiboot_tag_mmap *)tag;
            if (mmap->size < 16u || mmap->entry_size < 24u) return -4;

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
    return free_pages != 0 ? 0 : -5;
}

/*
 * Initialize the allocator from a fully validated Boot Protocol v1 view.
 * The v1 parser guarantees sorted, non-overlapping descriptors and verifies
 * ownership of the handoff, modules, and copied ACPI RSDP. The allocator
 * deliberately manages only its existing 1-GiB bootstrap window; high RAM
 * remains described by the ABI but is not exposed until the PMM is extended.
 */
int physical_memory_init_from_boot_v1(const struct pros_boot_v1_view *view,
                                      uint64_t handoff_physical_address) {
    if (!view || !view->header || !view->mmap ||
        view->header->mmap_count == 0u ||
        view->header->mmap_count > PROSENJIT_BOOT_V1_MAX_MMAP_ENTRIES ||
        view->header->modules_count > PROSENJIT_BOOT_V1_MAX_MODULES ||
        (view->header->modules_count != 0u && !view->modules) ||
        handoff_physical_address != view->header->self_phys ||
        view->header->total_size < PROSENJIT_BOOT_V1_HEADER_SIZE ||
        handoff_physical_address > UINT64_MAX - view->header->total_size)
        return -1;

    /* Validate the complete borrowed view before mutating allocator state. */
    uint64_t previous_end = 0u;
    for (uint32_t i = 0u; i < view->header->mmap_count; ++i) {
        uint64_t end = view->mmap[i].base + view->mmap[i].length;
        if (!view->mmap[i].length || end < view->mmap[i].base ||
            view->mmap[i].attributes != 0u ||
            (i != 0u && view->mmap[i].base < previous_end))
            return -2;
        previous_end = end;
    }
    for (uint32_t i = 0u; i < view->header->modules_count; ++i) {
        uint64_t end = view->modules[i].start + view->modules[i].length;
        if (!view->modules[i].start || !view->modules[i].length ||
            end < view->modules[i].start)
            return -3;
    }

    for (uint64_t i = 0u; i < BITMAP_WORDS; ++i) {
        page_bitmap[i] = 0u;
        managed_bitmap[i] = 0u;
        user_bitmap[i] = 0u;
    }
    total_pages = 0u;
    free_pages = 0u;
    failed_allocations = 0u;

    /* Only explicitly usable records enter the allocator. Every other type,
     * unknown type, and every gap remains unavailable by construction. */
    for (uint32_t i = 0u; i < view->header->mmap_count; ++i) {
        if (view->mmap[i].type == PROSENJIT_BOOT_MEMORY_USABLE)
            mark_available(view->mmap[i].base, view->mmap[i].length);
    }

    reserve_range(0u, 0x100000u);
    reserve_range((uint64_t)(uintptr_t)__kernel_start,
                  (uint64_t)((uintptr_t)__kernel_end -
                             (uintptr_t)__kernel_start);
    reserve_range(handoff_physical_address, view->header->total_size);

    for (uint32_t i = 0u; i < view->header->modules_count; ++i)
        reserve_range(view->modules[i].start, view->modules[i].length);
    if (view->header->acpi_rsdp_size != 0u)
        reserve_range(view->header->acpi_rsdp_phys,
                      view->header->acpi_rsdp_size);
    if (view->header->framebuffer_size != 0u)
        reserve_range(view->header->framebuffer_phys,
                      view->header->framebuffer_size);

    for (uint64_t word = 0u; word < BITMAP_WORDS; ++word)
        total_pages += bitmap_popcount64(managed_bitmap[word]);
    return free_pages != 0u ? 0 : -5;
}

uint64_t physical_page_alloc(void) {
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

uint64_t physical_page_alloc_contiguous(uint64_t page_count,
                                        uint64_t alignment_pages) {
    if (page_count == 0u || page_count > MAX_PAGES ||
        alignment_pages == 0u || alignment_pages > MAX_PAGES ||
        (alignment_pages & (alignment_pages - 1u)) != 0u) {
        ++failed_allocations;
        return 0u;
    }

    if (page_count > free_pages) {
        ++failed_allocations;
        return 0u;
    }

    for (uint64_t start = 0u; start <= MAX_PAGES - page_count;) {
        uint64_t remainder = start & (alignment_pages - 1u);
        if (remainder != 0u) {
            uint64_t adjustment = alignment_pages - remainder;
            if (start > MAX_PAGES - adjustment)
                break;
            start += adjustment;
            continue;
        }

        uint64_t offset = 0u;
        while (offset < page_count) {
            uint64_t page = start + offset;
            if (!managed_test(page) || !bitmap_test(page))
                break;
            ++offset;
        }
        if (offset == page_count) {
            for (uint64_t i = 0u; i < page_count; ++i) {
                uint64_t page = start + i;
                bitmap_clear(page);
                user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
            }
            free_pages -= page_count;
            return start * PROSENJIT_PAGE_SIZE;
        }
        start += offset + 1u;
    }

    ++failed_allocations;
    return 0u;
}

int physical_page_free_contiguous(uint64_t physical_address,
                                  uint64_t page_count) {
    if (page_count == 0u ||
        (physical_address & (PROSENJIT_PAGE_SIZE - 1u)) != 0u)
        return -1;

    uint64_t first = physical_address / PROSENJIT_PAGE_SIZE;
    if (first >= MAX_PAGES || page_count > MAX_PAGES - first)
        return -1;

    /* Validate the complete range before mutating any allocator state. */
    for (uint64_t i = 0u; i < page_count; ++i) {
        uint64_t page = first + i;
        if (!managed_test(page) || bitmap_test(page))
            return -1;
    }

    for (uint64_t i = 0u; i < page_count; ++i) {
        uint64_t page = first + i;
        user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
        bitmap_set(page);
    }
    free_pages += page_count;
    return 0;
}

uint64_t physical_page_alloc_user(void) {
    uint64_t physical = physical_page_alloc();
    if (!physical) return 0;
    uint64_t page = physical / PROSENJIT_PAGE_SIZE;
    user_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
    return physical;
}

int physical_page_is_allocated(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return 0;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES || !managed_test(page))
        return 0;
    return !bitmap_test(page);
}

int physical_page_is_user_mappable(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return 0;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES || !managed_test(page) || bitmap_test(page))
        return 0;
    return (user_bitmap[page >> 6] & (1ULL << (page & 63ULL))) != 0;
}

void physical_page_free(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0) return;

    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES || !managed_test(page) || bitmap_test(page)) return;

    user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
    bitmap_set(page);
    ++free_pages;
}

uint64_t physical_memory_total_pages(void) {
    return total_pages;
}

uint64_t physical_memory_free_pages(void) {
    return free_pages;
}

uint64_t physical_memory_failed_allocations(void) {
    return failed_allocations;
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

    /* DMA-style allocations require contiguous, physically aligned pages. */
    uint64_t dma_before = free_pages;
    uint64_t dma = physical_page_alloc_contiguous(3u, 4u);
    if (!dma || ((dma / PROSENJIT_PAGE_SIZE) & 3u) != 0u ||
        free_pages != dma_before - 3u ||
        !physical_page_is_allocated(dma) ||
        !physical_page_is_allocated(dma + PROSENJIT_PAGE_SIZE) ||
        !physical_page_is_allocated(dma + 2u * PROSENJIT_PAGE_SIZE))
        return -6;
    if (physical_page_free_contiguous(dma, 3u) != 0 ||
        free_pages != dma_before ||
        physical_page_is_allocated(dma))
        return -7;
    if (physical_page_alloc_contiguous(0u, 1u) != 0u ||
        physical_page_alloc_contiguous(1u, 3u) != 0u ||
        physical_page_free_contiguous(dma + 1u, 2u) == 0)
        return -8;

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
