#include "heap.h"
#include "memory.h"
#include "paging.h"

#define BLOCK_MAGIC 0x50524F5348454150ULL
#define BLOCK_FREE  0ULL
#define BLOCK_USED  1ULL
#define ALIGNMENT   16ULL

struct heap_block {
    uint64_t magic;
    uint64_t size;
    uint64_t state;
    struct heap_block *next;
    struct heap_block *prev;
    uint64_t reserved;
};

static struct heap_block *heap_head;
static uint64_t heap_end;
static uint64_t heap_used;
/* One-shot fault injection inside multi-page heap growth. */
static uint32_t heap_growth_fail_after;
static uint32_t heap_growth_fail_armed;
static uint32_t heap_growth_fail_triggered;
static uint32_t heap_growth_poisoned;

/* One-shot rollback rejection seam for the fail-closed heap self-test. */
static uint32_t heap_growth_rollback_fail_armed;

void heap_test_growth_rollback_fail_once(void)
{
    __atomic_store_n(&heap_growth_rollback_fail_armed, 1u, __ATOMIC_RELEASE);
}

int heap_test_growth_is_poisoned(void)
{
    return __atomic_load_n(&heap_growth_poisoned, __ATOMIC_ACQUIRE) != 0u;
}

/* Self-test recovery only; call after manually releasing the injected mapping. */
void heap_test_growth_clear_poison(void)
{
    __atomic_store_n(&heap_growth_poisoned, 0u, __ATOMIC_RELEASE);
}

static int heap_growth_rollback_should_fail(void)
{
    return __atomic_exchange_n(&heap_growth_rollback_fail_armed, 0u,
                               __ATOMIC_ACQ_REL) != 0u;
}

void heap_test_growth_fail_after(uint32_t successful_mappings)
{
    __atomic_store_n(&heap_growth_fail_triggered, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&heap_growth_fail_after, successful_mappings,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&heap_growth_fail_armed, 1u, __ATOMIC_RELEASE);
}

int heap_test_growth_fail_was_triggered(void)
{
    return __atomic_load_n(&heap_growth_fail_triggered, __ATOMIC_ACQUIRE) != 0u;
}

static int heap_growth_should_fail(void)
{
    if (!__atomic_load_n(&heap_growth_fail_armed, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t remaining = __atomic_load_n(&heap_growth_fail_after,
                                         __ATOMIC_ACQUIRE);
    while (remaining != 0u) {
        if (__atomic_compare_exchange_n(&heap_growth_fail_after, &remaining,
                                        remaining - 1u, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }
    uint32_t expected = 1u;
    if (__atomic_compare_exchange_n(&heap_growth_fail_armed, &expected, 0u,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&heap_growth_fail_triggered, 1u, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}

static uint64_t heap_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory", "cc");
    return flags;
}

static void heap_irq_restore(uint64_t flags)
{
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* One-shot deterministic allocation failure seam for kernel self-tests. */
#define HEAP_TEST_FAIL_ARMED 0x80000000u
#define HEAP_TEST_FAIL_COUNT_MASK 0x7FFFFFFFu
static uint32_t heap_test_fail_state;
static uint32_t heap_test_fail_triggered;

void heap_test_fail_after(uint32_t successful_allocations)
{
    if (successful_allocations > HEAP_TEST_FAIL_COUNT_MASK)
        successful_allocations = HEAP_TEST_FAIL_COUNT_MASK;
    __atomic_store_n(&heap_test_fail_triggered, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&heap_test_fail_state,
                     HEAP_TEST_FAIL_ARMED | successful_allocations,
                     __ATOMIC_RELEASE);
}

void heap_test_fail_clear(void)
{
    __atomic_store_n(&heap_test_fail_state, 0u, __ATOMIC_RELEASE);
}

int heap_test_fail_was_triggered(void)
{
    return __atomic_load_n(&heap_test_fail_triggered, __ATOMIC_ACQUIRE) != 0u;
}

static int heap_test_should_fail_allocation(void)
{
    uint32_t state = __atomic_load_n(&heap_test_fail_state, __ATOMIC_ACQUIRE);

    while ((state & HEAP_TEST_FAIL_ARMED) != 0u) {
        uint32_t remaining = state & HEAP_TEST_FAIL_COUNT_MASK;
        uint32_t desired = remaining == 0u ? 0u : state - 1u;

        if (__atomic_compare_exchange_n(&heap_test_fail_state, &state,
                                        desired, 1, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (remaining == 0u) {
                __atomic_store_n(&heap_test_fail_triggered, 1u,
                                 __ATOMIC_RELEASE);
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (value > UINT64_MAX - (alignment - 1ULL)) return 0;
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

/*
 * Validate the complete allocator chain before following its links. Every
 * block begins at the committed heap base or at a previously validated
 * block-end/page frontier; a damaged next pointer must never become an
 * arbitrary kernel dereference. Coalescing can preserve gaps from earlier heap
 * growth epochs, so links need only be monotonic, non-overlapping and bounded.
 * Monotonic bounded addresses also make cycles impossible.
 */
static int heap_validate_chain_locked(void)
{
    uint64_t limit = PROSENJIT_HEAP_BASE + PROSENJIT_HEAP_MAX;
    if (limit < PROSENJIT_HEAP_BASE ||
        heap_end < PROSENJIT_HEAP_BASE || heap_end > limit ||
        (heap_end & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u)
        return 0;

    if (!heap_head)
        return heap_end == PROSENJIT_HEAP_BASE && heap_used == 0u;

    if ((uint64_t)(uintptr_t)heap_head != PROSENJIT_HEAP_BASE ||
        heap_end - PROSENJIT_HEAP_BASE < sizeof(struct heap_block))
        return 0;

    uint64_t cursor = PROSENJIT_HEAP_BASE;
    uint64_t used = 0u;
    uint64_t visited = 0u;
    uint64_t max_blocks =
        (heap_end - PROSENJIT_HEAP_BASE) /
        (sizeof(struct heap_block) + ALIGNMENT) + 1u;
    struct heap_block *previous = 0;

    while (cursor < heap_end) {
        if (++visited > max_blocks ||
            cursor > heap_end - sizeof(struct heap_block) ||
            (cursor & (ALIGNMENT - 1ULL)) != 0u)
            return 0;

        struct heap_block *block = (struct heap_block *)(uintptr_t)cursor;
        if (block->magic != BLOCK_MAGIC ||
            (block->state != BLOCK_FREE && block->state != BLOCK_USED) ||
            block->size == 0u ||
            (block->size & (ALIGNMENT - 1ULL)) != 0u ||
            block->prev != previous ||
            block->size > heap_end - cursor - sizeof(struct heap_block))
            return 0;

        uint64_t block_end = cursor + sizeof(struct heap_block) + block->size;
        if (block->state == BLOCK_USED) {
            if (used > UINT64_MAX - block->size)
                return 0;
            used += block->size;
        }

        if (!block->next) {
            if (used != heap_used)
                return 0;
            return 1;
        }

        uint64_t next = (uint64_t)(uintptr_t)block->next;
        if (next <= cursor || next > heap_end - sizeof(struct heap_block) ||
            (next & (ALIGNMENT - 1ULL)) != 0u || next < block_end)
            return 0;
        previous = block;
        cursor = next;
    }

    return 0;
}

/*
 * Grow the mapped heap transactionally. heap_end is the committed frontier:
 * it is not advanced until kmalloc has linked the new block. If any page
 * allocation or mapping fails, unmap and free every page mapped by this
 * attempt in bounded all-or-nothing batches.
 *
 * This allocator is currently uniprocessor-only. Interrupt exclusion keeps
 * interrupt-context allocations from observing the temporary mappings.
 */
static int heap_grow(uint64_t required_end) {
    uint64_t irq_flags = heap_irq_save();
    uint64_t original_end = heap_end;
    uint64_t mapped_end = original_end;
    int failure = 0;

    if (heap_growth_poisoned || required_end < original_end ||
        required_end - PROSENJIT_HEAP_BASE > PROSENJIT_HEAP_MAX) {
        heap_irq_restore(irq_flags);
        return -1;
    }

    while (mapped_end < required_end) {
        if (mapped_end - PROSENJIT_HEAP_BASE >= PROSENJIT_HEAP_MAX ||
            heap_growth_should_fail()) {
            failure = 1;
            break;
        }
        uint64_t physical = physical_page_alloc();
        if (!physical) {
            failure = 1;
            break;
        }
        if (paging_map_page(mapped_end, physical,
                            PROSENJIT_PAGE_WRITABLE |
                            PROSENJIT_PAGE_NX) != 0) {
            /*
             * The data page was allocated but never became a heap mapping.
             * Use the checked PMM batch path rather than the void best-effort
             * free API. If ownership validation fails, poison the heap so no
             * later allocation can reuse a potentially leaked page.
             */
            if (paging_unmap_pages_expected_and_free(
                    0, 0, 0u, &physical, 1u) != 0) {
                heap_growth_poisoned = 1u;
                heap_irq_restore(irq_flags);
                return -2;
            }
            failure = 1;
            break;
        }
        mapped_end += PROSENJIT_PAGE_SIZE;
    }

    if (!failure) {
        heap_irq_restore(irq_flags);
        return 0;
    }

    uint64_t rollback = original_end;
    while (rollback < mapped_end) {
        uint64_t virtual_pages[64];
        uint64_t physical_pages[64];
        uint64_t count = 0u;
        while (rollback < mapped_end && count < 64u) {
            uint64_t physical = 0u;
            if (paging_query_physical_checked(rollback, &physical) != 0 ||
                physical == 0u || !physical_page_is_allocated(physical)) {
                heap_growth_poisoned = 1u;
                heap_irq_restore(irq_flags);
                return -2;
            }
            virtual_pages[count] = rollback;
            physical_pages[count] = physical;
            ++count;
            rollback += PROSENJIT_PAGE_SIZE;
        }
        /*
         * Commit rollback as one mapping/ownership transaction. The combined
         * helper validates every VA->PA leaf and the complete PMM free batch
         * before clearing the first PTE; if the final PMM commit unexpectedly
         * rejects, it restores the saved PTEs before returning.
         */
        if (heap_growth_rollback_should_fail() ||
            paging_unmap_pages_expected_and_free(
                virtual_pages, physical_pages, count,
                physical_pages, count) != 0) {
            heap_growth_poisoned = 1u;
            heap_irq_restore(irq_flags);
            return -2;
        }
    }

    heap_irq_restore(irq_flags);
    return -1;
}

static void split_block(struct heap_block *block, uint64_t requested) {
    uint64_t minimum = sizeof(struct heap_block) + ALIGNMENT;
    if (block->size < requested + minimum) return;

    struct heap_block *tail =
        (struct heap_block *)((uint8_t *)block + sizeof(struct heap_block) + requested);
    tail->magic = BLOCK_MAGIC;
    tail->size = block->size - requested - sizeof(struct heap_block);
    tail->state = BLOCK_FREE;
    tail->next = block->next;
    tail->prev = block;
    if (tail->next) tail->next->prev = tail;
    block->next = tail;
    block->size = requested;
}

int heap_init(void) {
    heap_head = 0;
    heap_end = PROSENJIT_HEAP_BASE;
    heap_used = 0;
    heap_growth_poisoned = 0u;
    heap_growth_fail_armed = 0u;
    heap_growth_fail_triggered = 0u;
    heap_growth_fail_after = 0u;
    __atomic_store_n(&heap_test_fail_state, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&heap_test_fail_triggered, 0u, __ATOMIC_RELEASE);
    return 0;
}

static void *kmalloc_locked(uint64_t size) {
    if (heap_growth_poisoned || size == 0 ||
        !heap_validate_chain_locked()) return 0;
    if (size > PROSENJIT_HEAP_MAX - sizeof(struct heap_block)) return 0;
    size = align_up(size, ALIGNMENT);
    if (size == 0 || size > PROSENJIT_HEAP_MAX - sizeof(struct heap_block)) return 0;
    if (heap_test_should_fail_allocation())
        return 0;

    for (struct heap_block *block = heap_head; block; block = block->next) {
        if (block->magic == BLOCK_MAGIC && block->state == BLOCK_FREE && block->size >= size) {
            split_block(block, size);
            block->state = BLOCK_USED;
            heap_used += block->size;
            return (uint8_t *)block + sizeof(struct heap_block);
        }
    }

    uint64_t block_start = align_up(heap_end, ALIGNMENT);
    uint64_t total = sizeof(struct heap_block) + size;
    if (block_start == 0 || total > UINT64_MAX - block_start) return 0;
    uint64_t required_end = align_up(block_start + total, PROSENJIT_PAGE_SIZE);
    if (required_end == 0 || required_end < block_start ||
        required_end - PROSENJIT_HEAP_BASE > PROSENJIT_HEAP_MAX)
        return 0;
    if (heap_grow(required_end) != 0) return 0;

    struct heap_block *block = (struct heap_block *)block_start;
    block->magic = BLOCK_MAGIC;
    block->size = size;
    block->state = BLOCK_USED;
    block->next = 0;

    if (heap_head) {
        struct heap_block *tail = heap_head;
        while (tail->next) tail = tail->next;
        block->prev = tail;
        tail->next = block;
    } else {
        block->prev = 0;
        heap_head = block;
    }

    heap_end = required_end;
    heap_used += size;
    return (uint8_t *)block + sizeof(struct heap_block);
}

/* Serialize allocator metadata and the heap frontier against interrupt reentry. */
void *kmalloc(uint64_t size)
{
    uint64_t irq_flags = heap_irq_save();
    void *result = kmalloc_locked(size);
    heap_irq_restore(irq_flags);
    return result;
}

static void kfree_locked(void *address) {
    if (!address || !heap_validate_chain_locked()) return;
    uint64_t addr = (uint64_t)(uintptr_t)address;
    if (addr < PROSENJIT_HEAP_BASE + sizeof(struct heap_block) ||
        addr >= heap_end || (addr & (ALIGNMENT - 1ULL)) != 0)
        return;

    struct heap_block *block = 0;
    for (struct heap_block *candidate = heap_head; candidate; candidate = candidate->next) {
        uint64_t payload = (uint64_t)(uintptr_t)candidate + sizeof(struct heap_block);
        if (payload == addr) {
            block = candidate;
            break;
        }
    }
    if (!block || block->magic != BLOCK_MAGIC || block->state != BLOCK_USED) return;

    block->state = BLOCK_FREE;
    if (heap_used >= block->size) heap_used -= block->size;

    if (block->next && block->next->state == BLOCK_FREE) {
        struct heap_block *next = block->next;
        block->size += sizeof(struct heap_block) + next->size;
        block->next = next->next;
        if (block->next) block->next->prev = block;
    }

    if (block->prev && block->prev->state == BLOCK_FREE) {
        struct heap_block *prev = block->prev;
        prev->size += sizeof(struct heap_block) + block->size;
        prev->next = block->next;
        if (prev->next) prev->next->prev = prev;
        block = prev;
    }

    /*
     * Keep the virtual heap frontier monotonic. Mapped pages are retained
     * for reuse; lowering heap_end here would cause the next allocation to
     * remap already-mapped virtual pages.
     */
}

void kfree(void *address)
{
    uint64_t irq_flags = heap_irq_save();
    kfree_locked(address);
    heap_irq_restore(irq_flags);
}

/*
 * Validate a caller-supplied pointer as the exact payload address of a live
 * heap block. Range checks happen before touching the candidate header, so an
 * arbitrary/unmapped owned-list link is rejected without dereferencing it.
 * The adjacent block links are checked as additional allocator-membership
 * evidence. This allocator remains single-CPU and IRQ-serialized.
 */
int heap_is_live_allocation(const void *address, uint64_t minimum_size)
{
    uint64_t irq_flags = heap_irq_save();
    int valid = 0;
    uint64_t payload = (uint64_t)(uintptr_t)address;
    uint64_t heap_limit = PROSENJIT_HEAP_BASE + PROSENJIT_HEAP_MAX;

    if (!address || minimum_size == 0u ||
        heap_end < PROSENJIT_HEAP_BASE || heap_end > heap_limit ||
        (heap_end & (PROSENJIT_PAGE_SIZE - 1ULL)) != 0u ||
        payload < PROSENJIT_HEAP_BASE + sizeof(struct heap_block) ||
        payload >= heap_end || (payload & (ALIGNMENT - 1ULL)) != 0u)
        goto out;

    uint64_t block_address = payload - sizeof(struct heap_block);
    if (block_address < PROSENJIT_HEAP_BASE ||
        block_address > heap_end - sizeof(struct heap_block) ||
        (block_address & (ALIGNMENT - 1ULL)) != 0u)
        goto out;

    struct heap_block *block = (struct heap_block *)(uintptr_t)block_address;
    if (block->magic != BLOCK_MAGIC || block->state != BLOCK_USED ||
        block->size < minimum_size || block->size == 0u ||
        (block->size & (ALIGNMENT - 1ULL)) != 0u ||
        block->size > heap_end - block_address - sizeof(struct heap_block))
        goto out;

    if (!block->prev) {
        if (heap_head != block)
            goto out;
    } else {
        uint64_t previous_address = (uint64_t)(uintptr_t)block->prev;
        if (previous_address < PROSENJIT_HEAP_BASE ||
            previous_address >= block_address ||
            block_address - previous_address < sizeof(struct heap_block) ||
            previous_address > heap_end - sizeof(struct heap_block) ||
            (previous_address & (ALIGNMENT - 1ULL)) != 0u)
            goto out;
        struct heap_block *previous = block->prev;
        if (previous->magic != BLOCK_MAGIC ||
            (previous->state != BLOCK_FREE && previous->state != BLOCK_USED) ||
            previous->size == 0u ||
            (previous->size & (ALIGNMENT - 1ULL)) != 0u ||
            previous->next != block ||
            previous->size > block_address - previous_address -
                             sizeof(struct heap_block))
            goto out;
    }

    if (block->next) {
        uint64_t next_address = (uint64_t)(uintptr_t)block->next;
        if (next_address <= block_address ||
            next_address > heap_end - sizeof(struct heap_block) ||
            (next_address & (ALIGNMENT - 1ULL)) != 0u)
            goto out;
        struct heap_block *next = block->next;
        if (next->magic != BLOCK_MAGIC ||
            (next->state != BLOCK_FREE && next->state != BLOCK_USED) ||
            next->size == 0u ||
            (next->size & (ALIGNMENT - 1ULL)) != 0u ||
            next->prev != block ||
            next->size > heap_end - next_address -
                         sizeof(struct heap_block) ||
            block_address + sizeof(struct heap_block) + block->size >
                next_address)
            goto out;
    }

    valid = 1;
out:
    heap_irq_restore(irq_flags);
    return valid;
}

uint64_t heap_used_bytes(void) {
    return heap_used;
}


int heap_selftest(void) {
    uint64_t before = heap_used;
    void *a = kmalloc(17);
    void *b = kmalloc(64);
    if (!a || !b || a == b ||
        ((uint64_t)(uintptr_t)a & (ALIGNMENT - 1ULL)) != 0 ||
        ((uint64_t)(uintptr_t)b & (ALIGNMENT - 1ULL)) != 0)
        return -1;

    if (!heap_is_live_allocation(a, 17u) ||
        !heap_is_live_allocation(b, 64u) ||
        heap_is_live_allocation(a, 33u) ||
        heap_is_live_allocation((const uint8_t *)a + 16u, 1u) ||
        heap_is_live_allocation((const void *)(uintptr_t)1u, 1u))
        return -21;

    /*
     * A corrupt forward link must be rejected before kmalloc/kfree follows it.
     * Restore the fixture before any later heap operation and verify that the
     * live allocation accounting and complete chain remain unchanged.
     */
    struct heap_block *a_block =
        (struct heap_block *)((uint8_t *)a - sizeof(struct heap_block));
    struct heap_block *saved_a_next = a_block->next;
    uint64_t corrupt_chain_used = heap_used;
    a_block->next = (struct heap_block *)(uintptr_t)1u;
    void *rejected_corrupt_alloc = kmalloc(16u);
    kfree(b);
    if (rejected_corrupt_alloc != 0 || heap_used != corrupt_chain_used) {
        a_block->next = saved_a_next;
        return -26;
    }
    a_block->next = saved_a_next;
    if (!heap_validate_chain_locked() ||
        !heap_is_live_allocation(a, 17u) ||
        !heap_is_live_allocation(b, 64u))
        return -27;

    ((volatile uint8_t *)a)[0] = 0xA5;
    ((volatile uint8_t *)a)[16] = 0x5A;
    if (((volatile uint8_t *)a)[0] != 0xA5 ||
        ((volatile uint8_t *)a)[16] != 0x5A)
        return -2;

    uint64_t used_after_alloc = heap_used;
    kfree(a);
    if (heap_used >= used_after_alloc ||
        heap_is_live_allocation(a, 1u))
        return -3;

    kfree(b);
    if (heap_used != before)
        return -4;

    void *c = kmalloc(80);
    if (!c || heap_used == before)
        return -5;
    kfree(c);
    if (heap_used != before)
        return -6;

    if (kmalloc(PROSENJIT_HEAP_MAX) != 0)
        return -7;

    /*
     * Force a multi-page growth to fail after one successful mapping. The
     * failed allocation must leave the committed frontier, allocator usage,
     * and heap data-page ownership unchanged. Up to three newly-created
     * empty paging-structure pages may remain cached by the shared kernel
     * page-table hierarchy after leaf rollback.
     */
    uint64_t saved_end = heap_end;
    uint64_t saved_free_pages = physical_memory_free_pages();
    uint64_t saved_used = heap_used;
    if (saved_end < PROSENJIT_HEAP_BASE ||
        saved_end - PROSENJIT_HEAP_BASE >=
            PROSENJIT_HEAP_MAX - sizeof(struct heap_block))
        return -8;
    uint64_t growth_request = PROSENJIT_HEAP_MAX -
        (saved_end - PROSENJIT_HEAP_BASE) - sizeof(struct heap_block);

    /* Failure before the first mapping must leave allocator state untouched. */
    heap_test_growth_fail_after(0u);
    void *failed_before_first_map = kmalloc(growth_request);
    if (failed_before_first_map != 0 ||
        !heap_test_growth_fail_was_triggered() ||
        heap_end != saved_end || heap_used != saved_used ||
        physical_memory_free_pages() != saved_free_pages)
        return -8;
    uint64_t first_map_physical = 0u;
    if (paging_query_physical_checked(saved_end, &first_map_physical) != 0 ||
        first_map_physical != 0u)
        return -13;

    /*
     * Exercise the real PMM failure path: fail the first physical allocation
     * requested by heap_grow, then prove no heap mapping or allocator page
     * ownership was committed.
     */
    uint64_t failed_allocs_before = physical_memory_failed_allocations();
    physical_memory_test_fail_alloc_after(0u);
    void *failed_pmm_growth = kmalloc(growth_request);
    physical_memory_test_fail_clear();
    if (failed_pmm_growth != 0 ||
        physical_memory_failed_allocations() != failed_allocs_before + 1u ||
        heap_end != saved_end || heap_used != saved_used ||
        physical_memory_free_pages() != saved_free_pages)
        return -14;
    uint64_t pmm_failure_physical = 0u;
    if (paging_query_physical_checked(saved_end, &pmm_failure_physical) != 0 ||
        pmm_failure_physical != 0u)
        return -15;

    /*
     * Fail the second real PMM allocation, after one heap page was mapped.
     * Rollback must remove that leaf and return its data page; at most the
     * three empty shared page-table pages may remain cached.
     */
    failed_allocs_before = physical_memory_failed_allocations();
    physical_memory_test_fail_alloc_after(1u);
    void *failed_mid_pmm_growth = kmalloc(growth_request);
    physical_memory_test_fail_clear();
    uint64_t mid_pmm_free_pages = physical_memory_free_pages();
    if (failed_mid_pmm_growth != 0 ||
        physical_memory_failed_allocations() != failed_allocs_before + 1u ||
        heap_end != saved_end || heap_used != saved_used ||
        mid_pmm_free_pages > saved_free_pages ||
        saved_free_pages - mid_pmm_free_pages > 3u)
        return -16;
    pmm_failure_physical = 0u;
    if (paging_query_physical_checked(saved_end, &pmm_failure_physical) != 0 ||
        pmm_failure_physical != 0u)
        return -17;

    /*
     * Fail page-table allocation exactly when growth reaches the next 2 MiB
     * page-table boundary. Earlier data pages must be rolled back, and the
     * just-allocated-but-not-mapped data page must be returned by checked
     * batch-free rather than the legacy void free path.
     */
    uint64_t next_table_boundary =
        (saved_end & ~(0x200000ULL - 1ULL)) + 0x200000ULL;
    uint64_t block_start = align_up(saved_end, ALIGNMENT);
    uint64_t heap_limit = PROSENJIT_HEAP_BASE + PROSENJIT_HEAP_MAX;
    if (next_table_boundary <= block_start + sizeof(struct heap_block) ||
        next_table_boundary > heap_limit - PROSENJIT_PAGE_SIZE)
        return -18;
    uint64_t map_failure_request =
        next_table_boundary - block_start - sizeof(struct heap_block) + 1u;
    paging_test_table_alloc_fail_after(0u);
    void *failed_map_growth = kmalloc(map_failure_request);
    paging_test_table_alloc_fail_clear();
    if (failed_map_growth != 0 || heap_end != saved_end ||
        heap_used != saved_used ||
        physical_memory_free_pages() != saved_free_pages)
        return -19;
    uint64_t map_failure_leaf = 0u;
    if (paging_query_physical_checked(saved_end, &map_failure_leaf) != 0 ||
        map_failure_leaf != 0u)
        return -20;

    heap_test_growth_fail_after(1u);
    void *failed_growth = kmalloc(growth_request);
    if (failed_growth != 0 || !heap_test_growth_fail_was_triggered() ||
        heap_end != saved_end || heap_used != saved_used ||
        physical_memory_free_pages() > saved_free_pages ||
        saved_free_pages - physical_memory_free_pages() > 3u)
        return -8;

    /*
     * The failed growth must leave the committed frontier unmapped. Checking
     * only heap_end/free-page counts could miss a leaked leaf mapping whose
     * physical page was not returned to the allocator.
     */
    uint64_t rolled_back_physical = 0u;
    if (paging_query_physical_checked(saved_end, &rolled_back_physical) != 0 ||
        rolled_back_physical != 0u)
        return -11;

    /*
     * Force a fresh heap extension rather than allowing kmalloc to satisfy
     * the retry from one of the small free blocks created above.
     */
    void *retry = kmalloc(8192u);
    if (!retry)
        return -9;
    uint64_t retry_physical = 0u;
    if (paging_query_physical_checked(saved_end, &retry_physical) != 0 ||
        retry_physical == 0u || !physical_page_is_allocated(retry_physical))
        return -12;
    kfree(retry);
    if (heap_used != saved_used)
        return -10;

    /*
     * Force the rollback commit itself to reject after one new heap page has
     * been mapped. The allocator must poison itself, and a later large
     * allocation must fail without advancing the committed frontier or
     * changing heap usage. The test then explicitly releases the deliberately
     * retained mapping before clearing the poison flag.
     */
    saved_end = heap_end;
    saved_used = heap_used;
    if (saved_end < PROSENJIT_HEAP_BASE ||
        saved_end - PROSENJIT_HEAP_BASE >=
            PROSENJIT_HEAP_MAX - sizeof(struct heap_block))
        return -22;
    growth_request = PROSENJIT_HEAP_MAX -
        (saved_end - PROSENJIT_HEAP_BASE) - sizeof(struct heap_block);
    uint64_t poison_free_pages = physical_memory_free_pages();
    heap_test_growth_rollback_fail_once();
    heap_test_growth_fail_after(1u);
    void *rollback_rejected = kmalloc(growth_request);
    uint64_t retained_physical = 0u;
    if (rollback_rejected != 0 || !heap_test_growth_fail_was_triggered() ||
        !heap_test_growth_is_poisoned() ||
        heap_end != saved_end || heap_used != saved_used ||
        paging_query_physical_checked(saved_end, &retained_physical) != 0 ||
        retained_physical == 0u ||
        !physical_page_is_allocated(retained_physical))
        return -22;

    void *poisoned_retry = kmalloc(growth_request);
    if (poisoned_retry != 0 || heap_end != saved_end ||
        heap_used != saved_used ||
        !physical_page_is_allocated(retained_physical))
        return -23;

    const uint64_t retained_va = saved_end;
    if (paging_unmap_pages_expected_and_free(
            &retained_va, &retained_physical, 1u,
            &retained_physical, 1u) != 0 ||
        physical_page_is_allocated(retained_physical))
        return -24;
    heap_test_growth_clear_poison();
    uint64_t absent_physical = 0u;
    uint64_t poison_cleanup_free_pages = physical_memory_free_pages();
    if (heap_test_growth_is_poisoned() ||
        paging_query_physical_checked(saved_end, &absent_physical) != 0 ||
        absent_physical != 0u ||
        poison_cleanup_free_pages > poison_free_pages ||
        poison_free_pages - poison_cleanup_free_pages > 3u)
        return -25;

    return 0;
}
