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

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (value > UINT64_MAX - (alignment - 1ULL)) return 0;
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static int heap_grow(uint64_t required_end) {
    while (heap_end < required_end) {
        if (heap_end - PROSENJIT_HEAP_BASE >= PROSENJIT_HEAP_MAX) return -1;
        uint64_t physical = physical_page_alloc();
        if (!physical) return -1;
        if (paging_map_page(heap_end, physical, PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
            physical_page_free(physical);
            return -1;
        }
        heap_end += PROSENJIT_PAGE_SIZE;
    }
    return 0;
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
    return 0;
}

void *kmalloc(uint64_t size) {
    if (size == 0) return 0;
    if (size > PROSENJIT_HEAP_MAX - sizeof(struct heap_block)) return 0;
    size = align_up(size, ALIGNMENT);
    if (size == 0 || size > PROSENJIT_HEAP_MAX - sizeof(struct heap_block)) return 0;

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

void kfree(void *address) {
    if (!address) return;
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

    ((volatile uint8_t *)a)[0] = 0xA5;
    ((volatile uint8_t *)a)[16] = 0x5A;
    if (((volatile uint8_t *)a)[0] != 0xA5 ||
        ((volatile uint8_t *)a)[16] != 0x5A)
        return -2;

    uint64_t used_after_alloc = heap_used;
    kfree(a);
    if (heap_used >= used_after_alloc)
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

    return 0;
}
