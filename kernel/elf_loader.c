#include "elf_loader.h"
#include "elf.h"
#include "memory.h"
#include "address_space.h"
#include "paging.h"

#define LOAD_PAGE_MAX 256u

static uint64_t align_down(uint64_t v) {
    return v & ~(PROSENJIT_PAGE_SIZE - 1ULL);
}

static int range_overlap(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz) {
    if (asz == 0 || bsz == 0) return 0;
    if (a + asz < a || b + bsz < b) return 1;
    return a < b + bsz && b < a + asz;
}

struct loaded_page {
    uint64_t va;
    uint64_t pa;
};

static void rollback(struct pros_process *process,
                     struct loaded_page *pages, uint32_t count) {
    for (uint32_t i = count; i != 0; --i) {
        uint64_t pa = 0;
        if (address_space_unmap_user_page(&process->address_space,
                                          pages[i - 1].va, &pa) == 0 && pa)
            physical_page_free(pa);
    }
}

int pros_elf_load_process(struct pros_process *process,
                          const void *image, uint64_t size,
                          uint64_t *entry_out, uint64_t *stack_top_out) {
    struct pros_elf_header64 h;
    const uint8_t *bytes = (const uint8_t *)image;
    struct loaded_page pages[LOAD_PAGE_MAX];
    uint32_t page_count = 0;

    if (!process || !image || !entry_out || !stack_top_out)
        return PROS_ELF_LOAD_EINVAL;
    if (process_state(process) == PROS_PROCESS_DEAD ||
        process_state(process) == PROS_PROCESS_ZOMBIE)
        return PROS_ELF_LOAD_EINVAL;
    if (pros_elf_validate_load_segments(image, size, &h) != PROS_ELF_OK)
        return PROS_ELF_LOAD_EFORMAT;

    uint64_t stack_va = PROSENJIT_USER_STACK_VA;
    for (uint16_t i = 0; i < h.phnum; ++i) {
        const uint8_t *q = bytes + h.phoff + (uint64_t)i * h.phentsize;
        uint32_t type = (uint32_t)(q[0] | ((uint32_t)q[1] << 8) |
                                   ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24));
        if (type != PROS_ELF_PT_LOAD) continue;
        uint64_t vaddr = *(const uint64_t *)(const void *)(q + 16);
        uint64_t memsz = *(const uint64_t *)(const void *)(q + 40);
        uint64_t start = align_down(vaddr);
        uint64_t end = (vaddr + memsz + PROSENJIT_PAGE_SIZE - 1ULL) &
                       ~(PROSENJIT_PAGE_SIZE - 1ULL);
        if (start < PROSENJIT_USER_CODE_VA || end <= start ||
            range_overlap(start, end - start, stack_va, PROSENJIT_PAGE_SIZE))
            return PROS_ELF_LOAD_EOVERLAP;
        if ((end - start) / PROSENJIT_PAGE_SIZE > LOAD_PAGE_MAX - page_count)
            return PROS_ELF_LOAD_ENOMEM;

        for (uint64_t va = start; va < end; va += PROSENJIT_PAGE_SIZE) {
            for (uint32_t n = 0; n < page_count; ++n)
                if (pages[n].va == va) return PROS_ELF_LOAD_EOVERLAP;

            uint64_t pa = physical_page_alloc();
            if (!pa) {
                rollback(process, pages, page_count);
                return PROS_ELF_LOAD_ENOMEM;
            }
            uint8_t *dst = (uint8_t *)(uintptr_t)pa;
            for (uint32_t z = 0; z < PROSENJIT_PAGE_SIZE; ++z) dst[z] = 0;

            uint64_t page_end = va + PROSENJIT_PAGE_SIZE;
            uint64_t file_start = vaddr > va ? vaddr : va;
            uint64_t file_end = vaddr + *(const uint64_t *)(const void *)(q + 32);
            if (file_end > page_end) file_end = page_end;
            if (file_start < file_end) {
                uint64_t src_off = *(const uint64_t *)(const void *)(q + 8) +
                                   (file_start - vaddr);
                uint64_t count = file_end - file_start;
                for (uint64_t n = 0; n < count; ++n)
                    dst[(file_start - va) + n] = bytes[src_off + n];
            }

            uint32_t flags = 0;
            uint32_t pf = (uint32_t)(q[4] | ((uint32_t)q[5] << 8) |
                                     ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24));
            if (pf & PROS_ELF_PF_W) flags |= PROSENJIT_PAGE_WRITABLE;
            if (!(pf & PROS_ELF_PF_X)) flags |= PROSENJIT_PAGE_NX;

            if (address_space_map_user_page(&process->address_space, va, pa, flags) != 0) {
                physical_page_free(pa);
                rollback(process, pages, page_count);
                return PROS_ELF_LOAD_EOVERLAP;
            }
            pages[page_count++].va = va;
            pages[page_count - 1].pa = pa;
        }
    }

    uint64_t stack_pa = physical_page_alloc();
    if (!stack_pa) {
        rollback(process, pages, page_count);
        return PROS_ELF_LOAD_ENOMEM;
    }
    for (uint32_t z = 0; z < PROSENJIT_PAGE_SIZE; ++z)
        ((uint8_t *)(uintptr_t)stack_pa)[z] = 0;
    if (address_space_map_user_page(&process->address_space, stack_va, stack_pa,
                                    PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        physical_page_free(stack_pa);
        rollback(process, pages, page_count);
        return PROS_ELF_LOAD_EOVERLAP;
    }
    if (h.entry < PROSENJIT_USER_CODE_VA ||
        address_space_validate_user_range(&process->address_space, h.entry, 1, 0) != 0) {
        uint64_t pa = 0;
        (void)address_space_unmap_user_page(&process->address_space, stack_va, &pa);
        if (pa) physical_page_free(pa);
        rollback(process, pages, page_count);
        return PROS_ELF_LOAD_EFORMAT;
    }

    *entry_out = h.entry;
    *stack_top_out = PROSENJIT_USER_STACK_TOP;
    return PROS_ELF_LOAD_OK;
}

int pros_elf_loader_selftest(void) {
    /*
     * The full loader requires initialized physical memory and an address
     * space, so the deterministic unit contract is exercised by the ELF
     * validator self-test. Runtime loading is gated by the caller.
     */
    return pros_elf_selftest() == PROS_ELF_OK ? 0 : -1;
}
