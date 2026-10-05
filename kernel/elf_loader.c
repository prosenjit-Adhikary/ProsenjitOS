#include "elf_loader.h"
#include "elf.h"
#include "memory.h"
#include "address_space.h"
#include "paging.h"

#define LOAD_PAGE_MAX 256u
#define ELF_USER_LIMIT 0x0000800000000000ULL

static uint64_t rd64(const uint8_t *p) {
    uint64_t lo = (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
                  ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24);
    uint64_t hi = (uint64_t)p[4] | ((uint64_t)p[5] << 8) |
                  ((uint64_t)p[6] << 16) | ((uint64_t)p[7] << 24);
    return lo | (hi << 32);
}

static int add_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return 0;
    if (out) *out = a + b;
    return 1;
}

static uint64_t align_down(uint64_t v) {
    return v & ~(PROSENJIT_PAGE_SIZE - 1ULL);
}

static int align_up(uint64_t v, uint64_t *out) {
    if (v > UINT64_MAX - (PROSENJIT_PAGE_SIZE - 1ULL)) return -1;
    *out = (v + PROSENJIT_PAGE_SIZE - 1ULL) &
           ~(PROSENJIT_PAGE_SIZE - 1ULL);
    return 0;
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

    if (!process || !image || !entry_out || !stack_top_out || size == 0)
        return PROS_ELF_LOAD_EINVAL;
    if (process_state(process) == PROS_PROCESS_DEAD ||
        process_state(process) == PROS_PROCESS_ZOMBIE)
        return PROS_ELF_LOAD_EINVAL;
    if (pros_elf_validate_load_segments(image, size, &h) != PROS_ELF_OK)
        return PROS_ELF_LOAD_EFORMAT;

    /*
     * The current address-space ABI uses fixed non-PIE ET_EXEC mappings.
     * ET_DYN is rejected by the validator until a load-bias/relocation
     * contract is implemented.
     */
    if (h.type != PROS_ELF_ET_EXEC) return PROS_ELF_LOAD_EFORMAT;

    const uint64_t stack_va = PROSENJIT_USER_STACK_VA;
    const uint64_t stack_top = PROSENJIT_USER_STACK_TOP;
    uint64_t stack_end = 0;
    if (!add_ok(stack_va, PROSENJIT_PAGE_SIZE, &stack_end) ||
        stack_end != stack_top || stack_end > ELF_USER_LIMIT)
        return PROS_ELF_LOAD_EOVERLAP;

    for (uint16_t i = 0; i < h.phnum; ++i) {
        const uint8_t *q = bytes + h.phoff + (uint64_t)i * h.phentsize;
        uint32_t type = (uint32_t)(q[0] | ((uint32_t)q[1] << 8) |
                                   ((uint32_t)q[2] << 16) |
                                   ((uint32_t)q[3] << 24));
        if (type != PROS_ELF_PT_LOAD) continue;

        uint32_t pf = (uint32_t)(q[4] | ((uint32_t)q[5] << 8) |
                                 ((uint32_t)q[6] << 16) |
                                 ((uint32_t)q[7] << 24));
        uint64_t offset = rd64(q + 8);
        uint64_t vaddr = rd64(q + 16);
        uint64_t filesz = rd64(q + 32);
        uint64_t memsz = rd64(q + 40);
        uint64_t vend = 0, end = 0;

        if (memsz < filesz || !add_ok(vaddr, memsz, &vend) ||
            vaddr < PROSENJIT_USER_CODE_VA || vend > ELF_USER_LIMIT)
            return PROS_ELF_LOAD_EOVERLAP;
        if (offset > size || filesz > size - offset)
            return PROS_ELF_LOAD_EFORMAT;

        uint64_t start = align_down(vaddr);
        if (align_up(vend, &end) != 0 || end <= start)
            return PROS_ELF_LOAD_EOVERLAP;
        if (start < PROSENJIT_USER_CODE_VA || end > ELF_USER_LIMIT)
            return PROS_ELF_LOAD_EOVERLAP;
        if (start < stack_end && stack_va < end) {
            rollback(process, pages, page_count);
            return PROS_ELF_LOAD_EOVERLAP;
        }

        uint64_t page_span = (end - start) / PROSENJIT_PAGE_SIZE;
        if (page_span > LOAD_PAGE_MAX - page_count) {
            rollback(process, pages, page_count);
            return PROS_ELF_LOAD_ENOMEM;
        }

        for (uint64_t va = start; va < end; va += PROSENJIT_PAGE_SIZE) {
            for (uint32_t n = 0; n < page_count; ++n) {
                if (pages[n].va == va) {
                    rollback(process, pages, page_count);
                    return PROS_ELF_LOAD_EOVERLAP;
                }
            }

            uint64_t pa = physical_page_alloc();
            if (!pa) {
                rollback(process, pages, page_count);
                return PROS_ELF_LOAD_ENOMEM;
            }

            uint8_t *dst = (uint8_t *)(uintptr_t)pa;
            for (uint32_t z = 0; z < PROSENJIT_PAGE_SIZE; ++z) dst[z] = 0;

            uint64_t page_end = va + PROSENJIT_PAGE_SIZE;
            uint64_t file_start = vaddr > va ? vaddr : va;
            uint64_t file_end = 0;
            if (!add_ok(vaddr, filesz, &file_end)) {
                physical_page_free(pa);
                rollback(process, pages, page_count);
                return PROS_ELF_LOAD_EFORMAT;
            }
            if (file_end > page_end) file_end = page_end;
            if (file_start < file_end) {
                uint64_t src_off = 0;
                if (!add_ok(offset, file_start - vaddr, &src_off) ||
                    src_off > size || file_end - file_start > size - src_off) {
                    physical_page_free(pa);
                    rollback(process, pages, page_count);
                    return PROS_ELF_LOAD_EFORMAT;
                }
                uint64_t count = file_end - file_start;
                for (uint64_t n = 0; n < count; ++n)
                    dst[(file_start - va) + n] = bytes[src_off + n];
            }

            uint64_t flags = (pf & PROS_ELF_PF_W) ? PROSENJIT_PAGE_WRITABLE : 0;
            if (!(pf & PROS_ELF_PF_X)) flags |= PROSENJIT_PAGE_NX;

            if (address_space_map_user_page_owned(&process->address_space, va, pa, flags) != 0) {
                physical_page_free(pa);
                rollback(process, pages, page_count);
                return PROS_ELF_LOAD_EOVERLAP;
            }
            pages[page_count].va = va;
            pages[page_count].pa = pa;
            ++page_count;
        }
    }

    uint64_t stack_pa = physical_page_alloc();
    if (!stack_pa) {
        rollback(process, pages, page_count);
        return PROS_ELF_LOAD_ENOMEM;
    }
    for (uint32_t z = 0; z < PROSENJIT_PAGE_SIZE; ++z)
        ((uint8_t *)(uintptr_t)stack_pa)[z] = 0;

    if (address_space_map_user_page_owned(&process->address_space, stack_va, stack_pa,
                                    PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX) != 0) {
        physical_page_free(stack_pa);
        rollback(process, pages, page_count);
        return PROS_ELF_LOAD_EOVERLAP;
    }

    if (h.entry < PROSENJIT_USER_CODE_VA ||
        h.entry >= ELF_USER_LIMIT ||
        address_space_validate_user_range(&process->address_space, h.entry, 1, 0) != 0) {
        uint64_t pa = 0;
        (void)address_space_unmap_user_page(&process->address_space, stack_va, &pa);
        if (pa) physical_page_free(pa);
        rollback(process, pages, page_count);
        return PROS_ELF_LOAD_EFORMAT;
    }

    *entry_out = h.entry;
    *stack_top_out = stack_top;
    return PROS_ELF_LOAD_OK;
}

int pros_elf_loader_selftest(void) {
    return pros_elf_selftest() == PROS_ELF_OK ? 0 : -1;
}
