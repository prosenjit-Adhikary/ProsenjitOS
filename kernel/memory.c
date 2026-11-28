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
/* Authoritative type registry: allocator-owned pages currently used as paging structures. */
static uint64_t page_table_bitmap[BITMAP_WORDS];
/* Scratch uniqueness bitmap for one IRQ-serialized mixed free transaction. */
static uint64_t mixed_free_seen[BITMAP_WORDS];
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
#ifdef PROSENJIT_PMM_HOST_TEST
/* Host-only PMM regression harness: there is no local interrupt state. */
static inline uint64_t pmm_irq_save(void) { return 0u; }
static inline void pmm_irq_restore(uint64_t flags) { (void)flags; }
#else
static inline uint64_t pmm_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void pmm_irq_restore(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}
#endif

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
    int end_tag_found = 0;

    while (current <= end && end - current >= 8ULL) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)(uintptr_t)current;
        if (tag->type == MULTIBOOT2_TAG_END) {
            if (tag->size != 8u || current + 8ULL != end)
                return -3;
            end_tag_found = 1;
            break;
        }
        if (tag->size < 8u || current + tag->size > end) return -3;

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            const struct multiboot_tag_mmap *mmap =
                (const struct multiboot_tag_mmap *)tag;
            if (mmap->size < 16u || mmap->entry_size < 24u ||
                ((mmap->size - 16u) % mmap->entry_size) != 0u)
                return -4;

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
    if (!end_tag_found) return -3;

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
        page_table_bitmap[i] = 0u;
        mixed_free_seen[i] = 0u;
    }
    total_pages = 0u;
    free_pages = 0u;
    failed_allocations = 0u;
    pmm_test_fail_after = 0u;
    pmm_test_fail_armed = 0u;

    /* Only explicitly usable records enter the allocator. Every other type,
     * unknown type, and every gap remains unavailable by construction. */
    for (uint32_t i = 0u; i < view->header->mmap_count; ++i) {
        if (view->mmap[i].type == PROSENJIT_BOOT_MEMORY_USABLE)
            mark_available(view->mmap[i].base, view->mmap[i].length);
    }

    reserve_range(0u, 0x100000u);
    reserve_range((uint64_t)(uintptr_t)__kernel_start,
                  (uint64_t)((uintptr_t)__kernel_end -
                             (uintptr_t)__kernel_start));
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

static void physical_page_free_unlocked(uint64_t physical_address);

static uint64_t physical_page_alloc_table_unlocked(void) {
    uint64_t physical = physical_page_alloc_unlocked();
    if (!physical) return 0u;
    uint64_t page = physical / PROSENJIT_PAGE_SIZE;
    if (page_table_test(page)) {
        physical_page_free_unlocked(physical);
        return 0u;
    }
    page_table_set(page);
    return physical;
}

static int physical_page_is_page_table_unlocked(uint64_t physical_address) {
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

int physical_page_is_page_table(uint64_t physical_address) {
    uint64_t flags = pmm_irq_save();
    int result = physical_page_is_page_table_unlocked(physical_address);
    pmm_irq_restore(flags);
    return result;
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
    if (physical_page_free_batch_preflight_unlocked(physical_addresses,
                                                    page_count) != 0)
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

/*
 * Atomically release user/data pages and page-table pages in one PMM
 * transaction. User pages retain the existing 256-page batch bound; the
 * page-table side is bounded by the physical address-space limit. A shared
 * bitmap rejects duplicates both within and across the two input arrays.
 */
static int physical_page_free_mixed_preflight_unlocked(
    const uint64_t *user_pages, uint64_t user_count,
    const uint64_t *table_pages, uint64_t table_count) {
    if ((user_count != 0u && !user_pages) ||
        (table_count != 0u && !table_pages) ||
        (user_count == 0u && table_count == 0u) ||
        user_count > 256u || table_count > MAX_PAGES ||
        table_count > MAX_PAGES - user_count)
        return -1;

    for (uint64_t word = 0u; word < BITMAP_WORDS; ++word)
        mixed_free_seen[word] = 0u;

    for (uint64_t i = 0u; i < user_count; ++i) {
        uint64_t address = user_pages[i];
        if ((address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u)
            return -1;
        uint64_t page = address / PROSENJIT_PAGE_SIZE;
        if (page >= MAX_PAGES || !managed_test(page) || bitmap_test(page) ||
            page_table_test(page))
            return -1;
        uint64_t mask = 1ULL << (page & 63ULL);
        uint64_t *word = &mixed_free_seen[page >> 6];
        if (*word & mask)
            return -1;
        *word |= mask;
    }

    for (uint64_t i = 0u; i < table_count; ++i) {
        uint64_t address = table_pages[i];
        if ((address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u)
            return -1;
        uint64_t page = address / PROSENJIT_PAGE_SIZE;
        if (page >= MAX_PAGES)
            return -1;
        uint64_t mask = 1ULL << (page & 63ULL);
        uint64_t *word = &mixed_free_seen[page >> 6];
        if (*word & mask)
            return -1;
        if (!managed_test(page) || bitmap_test(page) ||
            !page_table_test(page))
            return -1;
        *word |= mask;
    }
    return 0;
}

int physical_page_free_mixed_preflight(const uint64_t *user_pages,
                                       uint64_t user_count,
                                       const uint64_t *table_pages,
                                       uint64_t table_count) {
    uint64_t flags = pmm_irq_save();
    int rc = physical_page_free_mixed_preflight_unlocked(
        user_pages, user_count, table_pages, table_count);
    pmm_irq_restore(flags);
    return rc;
}

int physical_page_free_mixed(const uint64_t *user_pages,
                             uint64_t user_count,
                             const uint64_t *table_pages,
                             uint64_t table_count) {
    uint64_t flags = pmm_irq_save();
    int rc = physical_page_free_mixed_preflight_unlocked(
        user_pages, user_count, table_pages, table_count);
    if (rc == 0) {
        for (uint64_t i = 0u; i < user_count; ++i) {
            uint64_t page = user_pages[i] / PROSENJIT_PAGE_SIZE;
            user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
            bitmap_set(page);
        }
        for (uint64_t i = 0u; i < table_count; ++i) {
            uint64_t page = table_pages[i] / PROSENJIT_PAGE_SIZE;
            page_table_clear(page);
            user_bitmap[page >> 6] &= ~(1ULL << (page & 63ULL));
            bitmap_set(page);
        }
        free_pages += user_count + table_count;
    }
    pmm_irq_restore(flags);
    return rc;
}

/*
 * Address-space teardown already has validated physical-page sets in fixed
 * bitmaps. Consume those sets directly so teardown never needs a fallible
 * heap allocation after process-wide resource preflight.
 */
static int physical_page_free_mixed_bitmaps_preflight_unlocked(
    const uint64_t *user_page_bits, const uint64_t *table_page_bits) {
    if (!user_page_bits || !table_page_bits)
        return -1;
    uint64_t user_count = 0u;
    uint64_t table_count = 0u;
    for (uint64_t word = 0u; word < BITMAP_WORDS; ++word) {
        uint64_t user_bits = user_page_bits[word];
        uint64_t table_bits = table_page_bits[word];
        if (user_bits & table_bits)
            return -1;
        while (user_bits) {
            uint64_t bit = bitmap_ctz64(user_bits);
            uint64_t page = (word << 6) + bit;
            uint64_t mask = 1ULL << bit;
            if (page >= MAX_PAGES || ++user_count > MAX_PAGES ||
                !managed_test(page) || bitmap_test(page) ||
                page_table_test(page) ||
                !(user_bitmap[page >> 6] & mask))
                return -1;
            user_bits &= user_bits - 1ULL;
        }
        while (table_bits) {
            uint64_t bit = bitmap_ctz64(table_bits);
            uint64_t page = (word << 6) + bit;
            if (page >= MAX_PAGES || ++table_count > MAX_PAGES ||
                !managed_test(page) || bitmap_test(page) ||
                !page_table_test(page))
                return -1;
            table_bits &= table_bits - 1ULL;
        }
    }
    if (user_count + table_count == 0u ||
        user_count + table_count > MAX_PAGES)
        return -1;
    return 0;
}

int physical_page_free_mixed_bitmaps_preflight(
    const uint64_t *user_page_bits, const uint64_t *table_page_bits) {
    uint64_t flags = pmm_irq_save();
    int rc = physical_page_free_mixed_bitmaps_preflight_unlocked(
        user_page_bits, table_page_bits);
    pmm_irq_restore(flags);
    return rc;
}

int physical_page_free_mixed_bitmaps(
    const uint64_t *user_page_bits, const uint64_t *table_page_bits) {
    uint64_t flags = pmm_irq_save();
    int rc = physical_page_free_mixed_bitmaps_preflight_unlocked(
        user_page_bits, table_page_bits);
    if (rc == 0) {
        uint64_t released = 0u;
        for (uint64_t word = 0u; word < BITMAP_WORDS; ++word) {
            uint64_t user_bits = user_page_bits[word];
            uint64_t table_bits = table_page_bits[word];
            released += bitmap_popcount64(user_bits);
            released += bitmap_popcount64(table_bits);
            while (user_bits) {
                uint64_t bit = bitmap_ctz64(user_bits);
                uint64_t page = (word << 6) + bit;
                user_bitmap[page >> 6] &= ~(1ULL << bit);
                bitmap_set(page);
                user_bits &= user_bits - 1ULL;
            }
            while (table_bits) {
                uint64_t bit = bitmap_ctz64(table_bits);
                uint64_t page = (word << 6) + bit;
                page_table_clear(page);
                user_bitmap[page >> 6] &= ~(1ULL << bit);
                bitmap_set(page);
                table_bits &= table_bits - 1ULL;
            }
        }
        free_pages += released;
    }
    pmm_irq_restore(flags);
    return rc;
}

static uint64_t physical_page_alloc_user_unlocked(void) {
    uint64_t physical = physical_page_alloc_unlocked();
    if (!physical) return 0;
    uint64_t page = physical / PROSENJIT_PAGE_SIZE;
    user_bitmap[page >> 6] |= 1ULL << (page & 63ULL);
    return physical;
}

static int physical_page_is_allocated_unlocked(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return 0;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES || !managed_test(page))
        return 0;
    return !bitmap_test(page);
}

static int physical_page_is_user_mappable_unlocked(uint64_t physical_address) {
    if ((physical_address & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0)
        return 0;
    uint64_t page = physical_address / PROSENJIT_PAGE_SIZE;
    if (page >= MAX_PAGES || !managed_test(page) || bitmap_test(page))
        return 0;
    return (user_bitmap[page >> 6] & (1ULL << (page & 63ULL))) != 0;
}

int physical_page_is_allocated(uint64_t physical_address) {
    uint64_t flags = pmm_irq_save();
    int result = physical_page_is_allocated_unlocked(physical_address);
    pmm_irq_restore(flags);
    return result;
}

int physical_page_is_user_mappable(uint64_t physical_address) {
    uint64_t flags = pmm_irq_save();
    int result = physical_page_is_user_mappable_unlocked(physical_address);
    pmm_irq_restore(flags);
    return result;
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
    uint64_t flags = pmm_irq_save();
    uint64_t result = free_pages;
    pmm_irq_restore(flags);
    return result;
}

uint64_t physical_memory_failed_allocations(void) {
    uint64_t flags = pmm_irq_save();
    uint64_t result = failed_allocations;
    pmm_irq_restore(flags);
    return result;
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

    /*
     * Mixed user/table release validates both allocator types and the
     * cross-array uniqueness contract before changing any allocator bit.
     */
    {
        uint64_t mixed_before = free_pages;
        uint64_t mixed_user[2] = {
            physical_page_alloc_user(), physical_page_alloc()
        };
        uint64_t mixed_table[2] = {
            physical_page_alloc_table(), physical_page_alloc_table()
        };
        if (!mixed_user[0] || !mixed_user[1] ||
            !mixed_table[0] || !mixed_table[1]) {
            for (uint32_t i = 0u; i < 2u; ++i) {
                if (mixed_user[i] && physical_page_is_allocated(mixed_user[i]))
                    physical_page_free(mixed_user[i]);
                if (mixed_table[i] && physical_page_is_page_table(mixed_table[i]))
                    physical_page_free_table(mixed_table[i]);
            }
            return -23;
        }
        int mixed_rc = physical_page_free_mixed_preflight(
            mixed_user, 2u, mixed_table, 2u);
        if (mixed_rc == 0)
            mixed_rc = physical_page_free_mixed(mixed_user, 2u,
                                                mixed_table, 2u);
        if (mixed_rc != 0 || free_pages != mixed_before ||
            physical_page_is_allocated(mixed_user[0]) ||
            physical_page_is_allocated(mixed_user[1]) ||
            physical_page_is_allocated(mixed_table[0]) ||
            physical_page_is_allocated(mixed_table[1]) ||
            physical_page_is_page_table(mixed_table[0]) ||
            physical_page_is_page_table(mixed_table[1])) {
            for (uint32_t i = 0u; i < 2u; ++i) {
                if (mixed_user[i] && physical_page_is_allocated(mixed_user[i]))
                    physical_page_free(mixed_user[i]);
                if (mixed_table[i] && physical_page_is_page_table(mixed_table[i]))
                    physical_page_free_table(mixed_table[i]);
            }
            return -23;
        }
    }

    {
        uint64_t generic = physical_page_alloc();
        uint64_t table = physical_page_alloc_table();
        uint64_t mixed_before = free_pages;
        uint64_t wrong_user[1] = {table};
        uint64_t wrong_table[1] = {generic};
        if (!generic || !table ||
            physical_page_free_mixed(wrong_user, 1u,
                                     wrong_table, 1u) == 0 ||
            free_pages != mixed_before ||
            !physical_page_is_allocated(generic) ||
            !physical_page_is_allocated(table) ||
            !physical_page_is_page_table(table)) {
            if (generic && physical_page_is_allocated(generic))
                physical_page_free(generic);
            if (table && physical_page_is_page_table(table))
                physical_page_free_table(table);
            return -24;
        }
        uint64_t duplicate_user[2] = {generic, generic};
        uint64_t duplicate_table[2] = {table, table};
        uint64_t cross_alias_user[1] = {generic};
        uint64_t cross_alias_table[1] = {generic};
        uint64_t misaligned_user[1] = {generic + 1u};
        if (physical_page_free_mixed_preflight(duplicate_user, 2u, 0, 0u) == 0 ||
            physical_page_free_mixed_preflight(0, 0u,
                                               duplicate_table, 2u) == 0 ||
            physical_page_free_mixed_preflight(cross_alias_user, 1u,
                                               cross_alias_table, 1u) == 0 ||
            physical_page_free_mixed_preflight(misaligned_user, 1u,
                                               &table, 1u) == 0 ||
            free_pages != mixed_before ||
            !physical_page_is_allocated(generic) ||
            !physical_page_is_allocated(table)) {
            physical_page_free(generic);
            physical_page_free_table(table);
            return -25;
        }
        physical_page_free(generic);
        physical_page_free_table(table);
        if (free_pages != mixed_before + 2u)
            return -26;
    }

    {
        uint64_t valid = physical_page_alloc();
        uint64_t stale = physical_page_alloc();
        if (!valid || !stale) {
            if (valid) physical_page_free(valid);
            if (stale) physical_page_free(stale);
            return -27;
        }
        physical_page_free(stale);
        uint64_t before_reject = free_pages;
        uint64_t mixed_user[2] = {valid, stale};
        if (physical_page_free_mixed(mixed_user, 2u, 0, 0u) == 0 ||
            free_pages != before_reject ||
            !physical_page_is_allocated(valid) ||
            physical_page_is_allocated(stale)) {
            if (physical_page_is_allocated(valid))
                physical_page_free(valid);
            return -28;
        }
        physical_page_free(valid);
    }


    /* Bitmap mixed-release variant used by allocation-free address teardown. */
    {
        static uint64_t user_bits[BITMAP_WORDS];
        static uint64_t table_bits[BITMAP_WORDS];
        for (uint64_t i = 0u; i < BITMAP_WORDS; ++i) {
            user_bits[i] = 0u;
            table_bits[i] = 0u;
        }
        uint64_t before_bitmap_mixed = free_pages;
        uint64_t user_page = physical_page_alloc_user();
        uint64_t table_page = physical_page_alloc_table();
        if (!user_page || !table_page) {
            if (user_page) physical_page_free(user_page);
            if (table_page) physical_page_free_table(table_page);
            return -29;
        }
        uint64_t user_index = user_page / PROSENJIT_PAGE_SIZE;
        uint64_t table_index = table_page / PROSENJIT_PAGE_SIZE;
        user_bits[user_index >> 6] |= 1ULL << (user_index & 63u);
        table_bits[table_index >> 6] |= 1ULL << (table_index & 63u);
        if (physical_page_free_mixed_bitmaps_preflight(user_bits,
                                                       table_bits) != 0 ||
            physical_page_free_mixed_bitmaps(user_bits, table_bits) != 0 ||
            free_pages != before_bitmap_mixed ||
            physical_page_is_allocated(user_page) ||
            physical_page_is_allocated(table_page) ||
            physical_page_is_page_table(table_page))
            return -30;

        for (uint64_t i = 0u; i < BITMAP_WORDS; ++i) {
            user_bits[i] = 0u;
            table_bits[i] = 0u;
        }
        uint64_t generic_page = physical_page_alloc();
        uint64_t wrong_table_page = physical_page_alloc_table();
        if (!generic_page || !wrong_table_page) {
            if (generic_page) physical_page_free(generic_page);
            if (wrong_table_page) physical_page_free_table(wrong_table_page);
            return -31;
        }
        uint64_t generic_index = generic_page / PROSENJIT_PAGE_SIZE;
        uint64_t wrong_table_index = wrong_table_page / PROSENJIT_PAGE_SIZE;
        user_bits[wrong_table_index >> 6] |= 1ULL << (wrong_table_index & 63u);
        if (physical_page_free_mixed_bitmaps_preflight(user_bits,
                                                       table_bits) == 0) {
            physical_page_free(generic_page);
            physical_page_free_table(wrong_table_page);
            return -32;
        }
        user_bits[wrong_table_index >> 6] = 0u;
        table_bits[generic_index >> 6] |= 1ULL << (generic_index & 63u);
        if (physical_page_free_mixed_bitmaps_preflight(user_bits,
                                                       table_bits) == 0) {
            physical_page_free(generic_page);
            physical_page_free_table(wrong_table_page);
            return -33;
        }
        table_bits[generic_index >> 6] = 0u;
        user_bits[generic_index >> 6] |= 1ULL << (generic_index & 63u);
        table_bits[generic_index >> 6] |= 1ULL << (generic_index & 63u);
        if (physical_page_free_mixed_bitmaps_preflight(user_bits,
                                                       table_bits) == 0 ||
            !physical_page_is_allocated(generic_page) ||
            !physical_page_is_allocated(wrong_table_page)) {
            physical_page_free(generic_page);
            physical_page_free_table(wrong_table_page);
            return -34;
        }
        for (uint64_t i = 0u; i < BITMAP_WORDS; ++i) {
            user_bits[i] = 0u;
            table_bits[i] = 0u;
        }

        /* A stale member rejects the complete set without freeing its peer. */
        uint64_t valid_user = physical_page_alloc_user();
        uint64_t stale_user = physical_page_alloc_user();
        if (!valid_user || !stale_user) {
            if (valid_user) physical_page_free(valid_user);
            if (stale_user) physical_page_free(stale_user);
            physical_page_free(generic_page);
            physical_page_free_table(wrong_table_page);
            return -35;
        }
        physical_page_free(stale_user);
        uint64_t valid_index = valid_user / PROSENJIT_PAGE_SIZE;
        uint64_t stale_index = stale_user / PROSENJIT_PAGE_SIZE;
        user_bits[valid_index >> 6] |= 1ULL << (valid_index & 63u);
        user_bits[stale_index >> 6] |= 1ULL << (stale_index & 63u);
        if (physical_page_free_mixed_bitmaps_preflight(user_bits,
                                                       table_bits) == 0 ||
            !physical_page_is_allocated(valid_user) ||
            physical_page_is_allocated(stale_user)) {
            physical_page_free(valid_user);
            physical_page_free(generic_page);
            physical_page_free_table(wrong_table_page);
            return -36;
        }
        for (uint64_t i = 0u; i < BITMAP_WORDS; ++i)
            user_bits[i] = 0u;
        physical_page_free(valid_user);
        physical_page_free(generic_page);
        physical_page_free_table(wrong_table_page);
        if (free_pages != before_bitmap_mixed)
            return -37;
    }

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
