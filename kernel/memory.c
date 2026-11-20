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

extern char __kernel_start[];
extern char __kernel_end[];

static uint64_t page_bitmap[BITMAP_WORDS];
/* managed_bitmap distinguishes allocator-owned pages from permanently reserved pages. */
static uint64_t managed_bitmap[BITMAP_WORDS];
/* Only explicitly user-allocated pages may be mapped into CPL3. */
static uint64_t user_bitmap[BITMAP_WORDS];
/* Authoritative type registry: allocator-owned pages currently used as paging structures. */
static uint64_t page_table_bitmap[BITMAP_WORDS];
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t failed_allocations;
/* One-shot PMM allocation fault injection for transactional-growth tests. */
static uint32_t pmm_test_fail_after;
static uint32_t pmm_test_fail_armed;

/*
 * The PMM is shared by preemptible kernel contexts on a single CPU. Keep
 * bitmap, page-type and free-page-counter mutations indivisible with respect
 * to timer-driven context switches. This preserves the caller's IF state and
 * is deliberately not presented as an SMP lock.
 */
static inline uint64_t pmm_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void pmm_irq_restore(uint64_t flags) {
    __asm__ volatile ("pushq %0; popq" : : "r"(flags) : "memory", "cc");
}

void physical_memory_test_fail_alloc_after(uint32_t successful_allocations) {
    __atomic_store_n(&pmm_test_fail_after, successful_allocations,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&pmm_test_fail_armed, 1u, __ATOMIC_RELEASE);
}

void physical_memory_test_fail_clear(void) {
    __atomic_store_n(&pmm_test_fail_armed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&pmm_test_fail_after, 0u, __ATOMIC_RELEASE);
}

static int pmm_test_should_fail_alloc(void) {
    if (!__atomic_load_n(&pmm_test_fail_armed, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t remaining = __atomic_load_n(&pmm_test_fail_after,
                                         __ATOMIC_ACQUIRE);
    while (remaining != 0u) {
        if (__atomic_compare_exchange_n(&pmm_test_fail_after, &remaining,
                                        remaining - 1u, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }
    uint32_t expected = 1u;
    return __atomic_compare_exchange_n(&pmm_test_fail_armed, &expected, 0u,
                                       0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
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

static inline void page_table_set(uint64_t page) {
    page_table_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
}

static inline void page_table_clear(uint64_t page) {
    page_table_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
}

static inline int page_table_test(uint64_t page) {
    return (page_table_bitmap[page >> 6] & (1ULL << (page & 63ULL))) != 0;
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
        page_table_bitmap[i] = 0;
    }
    total_pages = 0;
    free_pages = 0;
    failed_allocations = 0;
    pmm_test_fail_after = 0u;
    pmm_test_fail_armed = 0u;

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

static uint64_t physical_page_alloc_unlocked(void) {
    if (pmm_test_should_fail_alloc()) {
        ++failed_allocations;
        return 0u;
    }
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
    uint64_t flags = pmm_irq_save();
    uint64_t page = physical_page_alloc_unlocked();
    pmm_irq_restore(flags);
    return page;
}

static uint64_t physical_page_alloc_table_unlocked(void) {
    uint64_t physical = physical_page_alloc();
    if (!physical) return 0u;
    uint64_t page = physical / PROSENJIT_PAGE_SIZE;
    if (page_table_test(page)) {
        physical_page_free(physical);
        return 0u;
    }
    page_table_set(page);
    return physical;
}

int physical_page_is_page_table(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u)
        return 0;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES || !managed_test(page) || bitmap_test(page))
        return 0;
    return page_table_test(page);
}

uint64_t physical_page_alloc_table(void) {
    uint64_t flags = pmm_irq_save();
    uint64_t page = physical_page_alloc_table_unlocked();
    pmm_irq_restore(flags);
    return page;
}

static int physical_page_free_table_unlocked(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u)
        return -1;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES || !managed_test(page) || bitmap_test(page) ||
        !page_table_test(page))
        return -1;
    page_table_clear(page);
    user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
    bitmap_set(page);
    ++free_pages;
    return 0;
}

int physical_page_free_table(uint64_t physical_address) {
    uint64_t flags = pmm_irq_save();
    int rc = physical_page_free_table_unlocked(physical_address);
    pmm_irq_restore(flags);
    return rc;
}

static uint64_t physical_page_alloc_contiguous_unlocked(uint64_t page_count,
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

uint64_t physical_page_alloc_contiguous(uint64_t page_count,
                                        uint64_t alignment_pages) {
    uint64_t flags = pmm_irq_save();
    uint64_t page = physical_page_alloc_contiguous_unlocked(page_count,
                                                             alignment_pages);
    pmm_irq_restore(flags);
    return page;
}

static int physical_page_free_contiguous_unlocked(uint64_t physical_address,
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
        if (!managed_test(page) || bitmap_test(page) || page_table_test(page))
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

/*
 * Atomically release a bounded non-contiguous set of allocator-owned pages.
 * Validate the complete input (including duplicates) before changing a bit.
 * The 256-page limit covers the maximum 64 guarded thread stacks (4 pages
 * each) in one all-or-nothing process reap transaction.
 */
int physical_page_free_contiguous(uint64_t physical_address,
                                  uint64_t page_count) {
    uint64_t flags = pmm_irq_save();
    int rc = physical_page_free_contiguous_unlocked(physical_address, page_count);
    pmm_irq_restore(flags);
    return rc;
}

static int physical_page_free_batch_preflight_unlocked(const uint64_t *physical_addresses,
                                      uint64_t page_count) {
    if (!physical_addresses || page_count == 0u || page_count > 256u)
        return -1;

    for (uint64_t i = 0u; i < page_count; ++i) {
        uint64_t address = physical_addresses[i];
        if ((address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u)
            return -1;
        uint64_t page = address / PROSENJIT_PAGE_SIZE;
        if (page >= MAX_PAGES || !managed_test(page) || bitmap_test(page) ||
            page_table_test(page))
            return -1;
        for (uint64_t j = 0u; j < i; ++j)
            if (physical_addresses[j] == address)
                return -1;
    }
    return 0;
}

int physical_page_free_batch_preflight(const uint64_t *physical_addresses,
                                      uint64_t page_count) {
    uint64_t flags = pmm_irq_save();
    int rc = physical_page_free_batch_preflight_unlocked(physical_addresses,
                                                          page_count);
    pmm_irq_restore(flags);
    return rc;
}

static int physical_page_free_batch_unlocked(const uint64_t *physical_addresses,
                             uint64_t page_count) {
    if (physical_page_free_batch_preflight(physical_addresses, page_count) != 0)
        return -1;

    for (uint64_t i = 0u; i < page_count; ++i) {
        uint64_t page = physical_addresses[i] / PROSENJIT_PAGE_SIZE;
        user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
        bitmap_set(page);
    }
    free_pages += page_count;
    return 0;
}

int physical_page_free_batch(const uint64_t *physical_addresses,
                             uint64_t page_count) {
    uint64_t flags = pmm_irq_save();
    int rc = physical_page_free_batch_unlocked(physical_addresses, page_count);
    pmm_irq_restore(flags);
    return rc;
}

static uint64_t physical_page_alloc_user_unlocked(void) {
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

uint64_t physical_page_alloc_user(void) {
    uint64_t flags = pmm_irq_save();
    uint64_t page = physical_page_alloc_user_unlocked();
    pmm_irq_restore(flags);
    return page;
}

static void physical_page_free_unlocked(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0) return;

    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES || !managed_test(page) || bitmap_test(page) ||
        page_table_test(page)) return;

    user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
    bitmap_set(page);
    ++free_pages;
}

void physical_page_free(uint64_t physical_address) {
    uint64_t flags = pmm_irq_save();
    physical_page_free_unlocked(physical_address);
    pmm_irq_restore(flags);
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
    uint64_t entry_flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(entry_flags) : : "memory");
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

    /* Non-contiguous batch free is all-or-nothing and rejects aliases. */
    uint64_t batch_before = free_pages;
    uint64_t batch_a = physical_page_alloc();
    uint64_t batch_b = physical_page_alloc();
    if (!batch_a || !batch_b || batch_a == batch_b)
        return -9;
    uint64_t batch_pages[2] = {batch_a, batch_b};
    if (physical_page_free_batch(batch_pages, 2u) != 0 ||
        free_pages != batch_before ||
        physical_page_is_allocated(batch_a) ||
        physical_page_is_allocated(batch_b))
        return -10;

    batch_a = physical_page_alloc();
    batch_b = physical_page_alloc();
    if (!batch_a || !batch_b)
        return -11;
    batch_pages[0] = batch_a;
    batch_pages[1] = batch_a; /* Duplicate must reject before any free. */
    if (physical_page_free_batch(batch_pages, 2u) == 0 ||
        free_pages != batch_before - 2u ||
        !physical_page_is_allocated(batch_a) ||
        !physical_page_is_allocated(batch_b))
        return -12;
    batch_pages[1] = batch_b;
    batch_pages[0] = batch_a + 1u; /* Misaligned member rejects whole batch. */
    if (physical_page_free_batch(batch_pages, 2u) == 0 ||
        free_pages != batch_before - 2u ||
        !physical_page_is_allocated(batch_a) ||
        !physical_page_is_allocated(batch_b))
        return -13;
    batch_pages[0] = batch_a;
    if (physical_page_free_batch(batch_pages, 2u) != 0 ||
        free_pages != batch_before)
        return -14;

    /*
     * The full 256-page transaction limit is required for 64 four-page
     * thread stacks. A duplicate at the end must reject without freeing any
     * earlier page; the corrected batch must then free all 256 pages.
     */
    {
        static uint64_t large_batch[256];
        uint64_t large_before = free_pages;
        uint32_t allocated = 0u;
        for (; allocated < 256u; ++allocated) {
            large_batch[allocated] = physical_page_alloc();
            if (!large_batch[allocated]) break;
        }
        if (allocated != 256u) {
            for (uint32_t i = 0u; i < allocated; ++i)
                physical_page_free(large_batch[i]);
            return -19;
        }
        uint64_t last_page = large_batch[255];
        large_batch[255] = large_batch[0];
        int duplicate_rejected =
            physical_page_free_batch(large_batch, 256u) != 0;
        large_batch[255] = last_page;
        if (!duplicate_rejected || free_pages != large_before - 256u) {
            for (uint32_t i = 0u; i < 256u; ++i)
                if (physical_page_is_allocated(large_batch[i]))
                    physical_page_free(large_batch[i]);
            return -20;
        }
        if (physical_page_free_batch(large_batch, 256u) != 0 ||
            free_pages != large_before)
            return -21;
    }

    /* Page-table ownership is a distinct allocator type and ordinary frees reject it. */
    uint64_t table_before = free_pages;
    uint64_t table_page = physical_page_alloc_table();
    if (!table_page || !physical_page_is_page_table(table_page) ||
        !physical_page_is_allocated(table_page) || free_pages != table_before - 1u)
        return -15;
    physical_page_free(table_page);
    uint64_t table_batch[1] = {table_page};
    if (!physical_page_is_page_table(table_page) ||
        !physical_page_is_allocated(table_page) ||
        free_pages != table_before - 1u ||
        physical_page_free_batch(table_batch, 1u) == 0 ||
        physical_page_free_contiguous(table_page, 1u) == 0)
        return -16;
    if (physical_page_free_table(table_page) != 0 ||
        physical_page_is_page_table(table_page) ||
        physical_page_is_allocated(table_page) ||
        free_pages != table_before)
        return -17;
    if (physical_page_free_table(table_page) == 0)
        return -18;

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

    uint64_t exit_flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(exit_flags) : : "memory");
    if ((entry_flags ^ exit_flags) & (1ULL << 9))
        return -22; /* Every allocator operation preserves the caller's IF. */

    return 0;
}
