#include "elf_loader.h"
#include "address_space.h"
#include "memory.h"
#include "paging.h"

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EV_CURRENT 1u
#define ELFOSABI_SYSV 0u
#define EM_X86_64 62u
#define ET_EXEC 2u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_TLS 7u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u
#define ELF_PAGE 4096ULL
#define ELF_USER_MAX 0x0000800000000000ULL
#define ELF_TEST_CODE_VA PROSENJIT_USER_CODE_VA
#define ELF_MAX_PHNUM 128u
#define ELF_MAX_IMAGE 0x02000000ULL
#define ELF_MAX_ALIGN 0x10000000ULL

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

static int range_ok(uint64_t start, uint64_t length, uint64_t limit,
                    uint64_t *end_out)
{
    if (length > limit || start > limit - length)
        return 0;
    if (end_out) *end_out = start + length;
    return 1;
}

static int canonical_user(uint64_t va)
{
    return va < ELF_USER_MAX;
}

static int phdr_table_ok(const struct elf64_ehdr *eh, uint64_t size)
{
    uint64_t bytes;
    if (eh->phnum == 0 || eh->phnum > ELF_MAX_PHNUM ||
        eh->phentsize != sizeof(struct elf64_phdr))
        return 0;
    bytes = (uint64_t)eh->phnum * sizeof(struct elf64_phdr);
    return eh->phoff <= size && bytes <= size - eh->phoff;
}

static int validate_load_segments(const uint8_t *image, uint64_t size,
                                  uint64_t *entry_out,
                                  uint64_t *image_start,
                                  uint64_t *image_end)
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    uint64_t start = UINT64_MAX, end = 0;
    int entry_exec = 0;
    uint32_t loads = 0;

    if (!phdr_table_ok(eh, size))
        return PROS_ELF_EBOUNDS;

    for (uint16_t i = 0; i < eh->phnum; ++i) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->phoff +
                                        (uint64_t)i * sizeof(*ph));
        uint64_t seg_end, page_start, page_end;

        if (ph->type == PT_DYNAMIC || ph->type == PT_INTERP ||
            ph->type == PT_TLS)
            return PROS_ELF_ENOTSUP;
        if (ph->type != PT_LOAD)
            continue;
        ++loads;

        if (ph->filesz > ph->memsz ||
            ph->offset > size || ph->filesz > size - ph->offset)
            return PROS_ELF_EBOUNDS;
        if (ph->memsz == 0)
            return PROS_ELF_EFORMAT;
        if (ph->vaddr < PROSENJIT_USER_CODE_VA ||
            ph->vaddr > ELF_USER_MAX ||
            !range_ok(ph->vaddr, ph->memsz, ELF_USER_MAX, &seg_end) ||
            ph->vaddr + ph->memsz == 0)
            return PROS_ELF_EBOUNDS;
        if ((ph->flags & ~(PF_R | PF_W | PF_X)) != 0 ||
            (ph->flags & (PF_W | PF_X)) == (PF_W | PF_X))
            return PROS_ELF_EPERM;
        if (ph->align != 0 && (ph->align & (ph->align - 1)) != 0)
            return PROS_ELF_EFORMAT;
        if (ph->align > ELF_MAX_ALIGN)
            return PROS_ELF_EFORMAT;
        /* ELF requires every PT_LOAD p_vaddr/p_offset pair to be
         * congruent modulo the page size, even when p_align is 0 or 1.
         * A stronger p_align constraint is checked separately below. */
        if ((ph->vaddr & (ELF_PAGE - 1ULL)) !=
            (ph->offset & (ELF_PAGE - 1ULL)))
            return PROS_ELF_EFORMAT;
        if (ph->align > 1 &&
            ((ph->vaddr & (ph->align - 1)) !=
             (ph->offset & (ph->align - 1))))
            return PROS_ELF_EFORMAT;
        if (ph->vaddr < PROSENJIT_USER_STACK_TOP &&
            seg_end > PROSENJIT_USER_STACK_VA)
            return PROS_ELF_ECONFLICT;

        /* Loadable virtual ranges must not overlap. Overlap would make
         * page permissions ambiguous and could cause one segment to overwrite
         * another during loading. */
        for (uint16_t j = 0; j < i; ++j) {
            const struct elf64_phdr *prev =
                (const struct elf64_phdr *)(image + eh->phoff +
                                            (uint64_t)j * sizeof(*prev));
            uint64_t prev_end;
            if (prev->type != PT_LOAD)
                continue;
            if (!range_ok(prev->vaddr, prev->memsz, ELF_USER_MAX, &prev_end))
                return PROS_ELF_EBOUNDS;
            if (ph->vaddr < prev_end && prev->vaddr < seg_end)
                return PROS_ELF_ECONFLICT;
        }

        page_start = ph->vaddr & ~(ELF_PAGE - 1);
        if (seg_end > UINT64_MAX - (ELF_PAGE - 1))
            return PROS_ELF_EOVERFLOW;
        page_end = (seg_end + ELF_PAGE - 1) & ~(ELF_PAGE - 1);
        if (page_end > ELF_USER_MAX)
            return PROS_ELF_EBOUNDS;

        if (eh->entry >= ph->vaddr && eh->entry < seg_end &&
            (ph->flags & PF_X))
            entry_exec = 1;
        if (page_start < start) start = page_start;
        if (page_end > end) end = page_end;
    }

    if (loads == 0 || !entry_exec)
        return PROS_ELF_EFORMAT;
    if (!canonical_user(eh->entry) || eh->entry < PROSENJIT_USER_CODE_VA)
        return PROS_ELF_EBOUNDS;
    if (entry_out) *entry_out = eh->entry;
    if (image_start) *image_start = start;
    if (image_end) *image_end = end;
    return PROS_ELF_OK;
}

static int segment_page_flags(const struct elf64_phdr *ph, uint64_t va)
{
    uint64_t end;
    if (!range_ok(ph->vaddr, ph->memsz, ELF_USER_MAX, &end))
        return -1;
    if (va < (ph->vaddr & ~(ELF_PAGE - 1)) ||
        va >= ((end + ELF_PAGE - 1) & ~(ELF_PAGE - 1)))
        return -1;
    return (ph->flags & PF_W ? PROSENJIT_PAGE_WRITABLE : 0) |
           ((ph->flags & PF_X) ? 0 : PROSENJIT_PAGE_NX);
}

int pros_elf_validate(const uint8_t *image, uint64_t size)
{
    const struct elf64_ehdr *eh;
    if (!image || size < sizeof(struct elf64_ehdr) || size > ELF_MAX_IMAGE)
        return PROS_ELF_EBOUNDS;
    eh = (const struct elf64_ehdr *)image;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != ELFCLASS64 || eh->ident[5] != ELFDATA2LSB ||
        eh->ident[6] != EV_CURRENT ||
        (eh->ident[7] != ELFOSABI_SYSV && eh->ident[7] != 3) ||
        eh->type != ET_EXEC ||
        eh->machine != EM_X86_64 || eh->version != EV_CURRENT ||
        eh->ehsize != sizeof(struct elf64_ehdr))
        return PROS_ELF_EFORMAT;
    return validate_load_segments(image, size, 0, 0, 0);
}

static int page_mapped(const struct pros_address_space *space, uint64_t va)
{
    return address_space_validate_user_range(space, va, ELF_PAGE, 0) == 0;
}

static void rollback_user_pages(struct pros_process *process,
                                uint64_t start, uint64_t end)
{
    for (uint64_t va = start; va < end; va += ELF_PAGE) {
        uint64_t physical = 0;
        if (address_space_unmap_user_page(&process->address_space, va,
                                          &physical) == 0)
            physical_page_free(physical);
    }
}

int pros_elf_selftest(void)
{
    uint8_t image[ELF_PAGE + 16u] = {0};
    struct elf64_ehdr *eh = (struct elf64_ehdr *)image;
    struct elf64_phdr *ph = (struct elf64_phdr *)(image + sizeof(*eh));
    const uint64_t data_off = ELF_PAGE;

    eh->ident[0] = 0x7F; eh->ident[1] = 'E'; eh->ident[2] = 'L'; eh->ident[3] = 'F';
    eh->ident[4] = ELFCLASS64; eh->ident[5] = ELFDATA2LSB; eh->ident[6] = EV_CURRENT;
    eh->ident[7] = ELFOSABI_SYSV;
    eh->type = ET_EXEC; eh->machine = EM_X86_64; eh->version = EV_CURRENT;
    eh->entry = ELF_TEST_CODE_VA; eh->phoff = sizeof(*eh); eh->ehsize = sizeof(*eh);
    eh->phentsize = sizeof(*ph); eh->phnum = 1;
    ph[0].type = PT_LOAD; ph[0].flags = PF_R | PF_X; ph[0].offset = data_off;
    ph[0].vaddr = ELF_TEST_CODE_VA; ph[0].filesz = 4; ph[0].memsz = 0x1000;
    ph[0].align = ELF_PAGE;

    if (pros_elf_validate(image, data_off + 4) != PROS_ELF_OK) return -1;

    /* PT_LOAD page-congruence regression: p_align=1 does not waive the
     * mandatory p_vaddr/p_offset congruence modulo page size. */
    ph[0].align = 1;
    ph[0].offset = data_off + 1;
    if (pros_elf_validate(image, data_off + 5) != PROS_ELF_EFORMAT) return -15;
    ph[0].offset = data_off;
    ph[0].align = ELF_PAGE;
    eh->type = 3u;
    if (pros_elf_validate(image, data_off + 4) != PROS_ELF_EFORMAT) return -2;
    eh->type = ET_EXEC;
    if (pros_elf_validate(image, sizeof(*eh) - 1) != PROS_ELF_EBOUNDS) return -2;

    image[0] = 0;
    if (pros_elf_validate(image, data_off + 4) == PROS_ELF_OK) return -3;
    image[0] = 0x7F;

    ph[0].flags = PF_R | PF_W | PF_X;
    if (pros_elf_validate(image, data_off + 4) != PROS_ELF_EPERM) return -4;
    ph[0].flags = PF_R | PF_X;

    ph[0].type = PT_DYNAMIC;
    if (pros_elf_validate(image, data_off + 4) != PROS_ELF_ENOTSUP) return -5;
    ph[0].type = PT_LOAD;

    ph[0].vaddr = PROSENJIT_USER_STACK_VA;
    if (pros_elf_validate(image, data_off + 4) != PROS_ELF_ECONFLICT) return -5;
    ph[0].vaddr = ELF_TEST_CODE_VA;

    ph[0].align = ELF_MAX_ALIGN + 1ULL;
    if (pros_elf_validate(image, data_off + 4) != PROS_ELF_EFORMAT) return -6;
    ph[0].align = ELF_PAGE;

    eh->entry = ELF_TEST_CODE_VA + 0x1000ULL;
    if (pros_elf_validate(image, data_off + 4) == PROS_ELF_OK) return -7;
    eh->entry = ELF_TEST_CODE_VA;

    eh->phnum = 2;
    /* Keep the second segment page-congruent while overlapping the first:
     * a misaligned address would correctly fail EFORMAT before overlap logic. */
    ph[0].memsz = 0x1800ULL;
    ph[1] = ph[0];
    ph[1].vaddr = ELF_TEST_CODE_VA + ELF_PAGE;
    ph[1].align = 0;
    if (pros_elf_validate(image, data_off + 4) != PROS_ELF_ECONFLICT) return -8;

    eh->phnum = 1;
    /* filesz must not mask the zero-memsz format regression as EBOUNDS. */
    ph[0].filesz = 0;
    ph[0].memsz = 0;
    if (pros_elf_validate(image, data_off + 4) != PROS_ELF_EFORMAT) return -9;
    ph[0].filesz = 4;
    ph[0].memsz = 0x1000ULL;

    /* Rollback isolation regression: an existing mapping must survive a
     * rejected load that targets the same PT_LOAD page. */
    {
        struct pros_process process;
        struct pros_elf_load_result loaded;
        uint64_t existing = 0, translated = 0, flags = 0;
        int rc = process_init(&process, 0xE1F2u, PROS_SECURITY_LIMITED);
        if (rc != 0) return -14;
        existing = physical_page_alloc_user();
        if (!existing ||
            address_space_map_user_page_owned(&process.address_space,
                                              ELF_TEST_CODE_VA, existing,
                                              PROSENJIT_PAGE_WRITABLE |
                                              PROSENJIT_PAGE_NX) != 0) {
            if (existing) physical_page_free(existing);
            (void)address_space_destroy(&process.address_space);
            (void)object_release(&process.object);
            return -11;
        }
        ph[0].flags = PF_R | PF_X;
        ph[0].filesz = 2;
        ph[0].memsz = ELF_PAGE;
        ph[0].vaddr = ELF_TEST_CODE_VA;
        ph[0].align = ELF_PAGE;
        eh->entry = ELF_TEST_CODE_VA;
        image[data_off + 0] = 0x90;
        image[data_off + 1] = 0xC3;
        rc = pros_elf_load(&process, image, data_off + 2, &loaded);
        if (rc != PROS_ELF_ECONFLICT ||
            address_space_translate_user(&process.address_space,
                                         ELF_TEST_CODE_VA, &translated,
                                         &flags) != 0 ||
            translated != existing) {
            (void)address_space_destroy(&process.address_space);
            (void)object_release(&process.object);
            return -12;
        }
        if (address_space_destroy(&process.address_space) != 0 ||
            object_release(&process.object) != 0)
            return -13;
    }

    /* Positive mapping test: verify PT_LOAD bytes, zero-filled BSS, stack
     * construction, entry publication, and owned-page teardown. */
    {
        struct pros_process process;
        struct pros_elf_load_result loaded;
        const char *argv[] = {"elf-selftest", "ok"};
        const char *envp[] = {"PFS=1"};
        uint64_t physical = 0, flags = 0;
        int rc = process_init(&process, 0xE1F1u, PROS_SECURITY_LIMITED);
        if (rc != 0) return -10;
        image[data_off + 0] = 0x90;
        image[data_off + 1] = 0xC3;
        ph[0].flags = PF_R | PF_X;
        ph[0].filesz = 2;
        ph[0].memsz = ELF_PAGE;
        ph[0].vaddr = ELF_TEST_CODE_VA;
        ph[0].align = ELF_PAGE;
        eh->entry = ELF_TEST_CODE_VA;
        /* Invalid process-object metadata must be rejected before mapping. */
        process.object.reserved = 1u;
        rc = pros_elf_load(&process, image, data_off + 2, &loaded);
        process.object.reserved = 0u;
        if (rc != PROS_ELF_EINVAL ||
            address_space_translate_user(&process.address_space,
                                         ELF_TEST_CODE_VA, &physical,
                                         &flags) == 0) {
            (void)address_space_destroy(&process.address_space);
            (void)object_release(&process.object);
            return -33;
        }
        rc = pros_elf_load_with_args(&process, image, data_off + 2,
                                     argv, 2u, envp, 1u, &loaded);
        if (rc != PROS_ELF_OK) {
            (void)address_space_destroy(&process.address_space);
            (void)object_release(&process.object);
            return -11;
        }
        if (loaded.entry != ELF_TEST_CODE_VA ||
            loaded.mapped_pages < 2u ||
            process.user_entry != ELF_TEST_CODE_VA ||
            process.user_stack_top != loaded.stack_top ||
            address_space_translate_user(&process.address_space,
                                         ELF_TEST_CODE_VA, &physical,
                                         &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_USER) ||
            (flags & PROSENJIT_PAGE_WRITABLE) ||
            (flags & PROSENJIT_PAGE_NX) ||
            ((uint8_t *)(uintptr_t)physical)[0] != 0x90 ||
            ((uint8_t *)(uintptr_t)physical)[1] != 0xC3 ||
            ((uint8_t *)(uintptr_t)physical)[2] != 0 ||
            address_space_translate_user(&process.address_space,
                                         PROSENJIT_USER_STACK_VA,
                                         &physical, &flags) != 0 ||
            !(flags & PROSENJIT_PAGE_WRITABLE) ||
            !(flags & PROSENJIT_PAGE_NX) ||
            process.user_stack_top < PROSENJIT_USER_STACK_VA ||
            process.user_stack_top > PROSENJIT_USER_STACK_TOP ||
            ((uint64_t *)(uintptr_t)(physical & ~(ELF_PAGE - 1ULL)))[
                (process.user_stack_top - PROSENJIT_USER_STACK_VA) / sizeof(uint64_t)] != 2u) {
            (void)address_space_destroy(&process.address_space);
            (void)object_release(&process.object);
            return -12;
        }
        if (address_space_destroy(&process.address_space) != 0 ||
            object_release(&process.object) != 0)
            return -13;
    }

    return PROS_ELF_OK;
}

static int build_initial_stack(struct pros_process *process,
                                  const char *const *argv, uint32_t argc,
                                  const char *const *envp, uint32_t envc) {
    if (!process || argc > 64u || envc > 64u ||
        (argc && !argv) || (envc && !envp))
        return PROS_ELF_EINVAL;

    uint64_t physical = 0, flags = 0;
    if (address_space_translate_user(&process->address_space,
                                     PROSENJIT_USER_STACK_VA,
                                     &physical, &flags) != 0 ||
        !(flags & PROSENJIT_PAGE_WRITABLE))
        return PROS_ELF_EINVAL;

    uint8_t *page = (uint8_t *)(uintptr_t)(physical & ~(ELF_PAGE - 1ULL));
    uint64_t argv_ptrs[64];
    uint64_t env_ptrs[64];
    uint64_t string_bytes = 0;

    /* Preflight every string before modifying the stack page. */
    for (uint32_t pass = 0; pass < 2u; ++pass) {
        const char *const *list = pass == 0u ? argv : envp;
        uint32_t count = pass == 0u ? argc : envc;
        for (uint32_t i = 0; i < count; ++i) {
            const char *str = list[i];
            if (!str) return PROS_ELF_EINVAL;
            uint64_t len = 0;
            while (len <= 4096u && str[len] != '\0') ++len;
            if (len > 4096u) return PROS_ELF_EBOUNDS;
            if (len == UINT64_MAX || string_bytes > UINT64_MAX - (len + 1u))
                return PROS_ELF_EOVERFLOW;
            string_bytes += len + 1u;
        }
    }

    /* Validate the pointer-vector footprint independently so the stack
     * construction cannot rely on an arithmetic-only capacity check. */
    if (argc > 64u || envc > 64u ||
        (uint64_t)argc + (uint64_t)envc > 128u)
        return PROS_ELF_EINVAL;

    uint64_t pointer_words = (uint64_t)argc + (uint64_t)envc + 3u;
    if (pointer_words > UINT64_MAX / sizeof(uint64_t))
        return PROS_ELF_EOVERFLOW;
    uint64_t pointer_bytes = pointer_words * sizeof(uint64_t);
    uint64_t stack_capacity = PROSENJIT_USER_STACK_TOP - PROSENJIT_USER_STACK_VA;

    /* Account for both required data and worst-case 16-byte alignment padding. */
    if (string_bytes > UINT64_MAX - pointer_bytes ||
        string_bytes + pointer_bytes > UINT64_MAX - 15u ||
        string_bytes + pointer_bytes + 15u > stack_capacity)
        return PROS_ELF_EOVERFLOW;

    /* Start at the top of the stack page; the loop below subtracts each
     * string exactly once. Pre-subtracting string_bytes here would underflow
     * the page offset for non-empty argv/envp and write outside the stack. */
    uint64_t sp = PROSENJIT_USER_STACK_TOP;
    for (uint32_t pass = 0; pass < 2u; ++pass) {
        const char *const *list = pass == 0u ? argv : envp;
        uint32_t count = pass == 0u ? argc : envc;
        uint64_t *out = pass == 0u ? argv_ptrs : env_ptrs;
        for (uint32_t i = count; i > 0u; --i) {
            const char *str = list[i - 1u];
            uint64_t len = 0;
            while (len <= 4096u && str[len] != '\0') ++len;
            sp -= len + 1u;
            uint64_t off = sp - PROSENJIT_USER_STACK_VA;
            for (uint64_t j = 0; j <= len; ++j)
                page[off + j] = (uint8_t)str[j];
            out[i - 1u] = sp;
        }
    }

    /* Place the pointer block below the strings and align the final
     * initial stack pointer down to 16 bytes. */
    if (pointer_bytes > sp - PROSENJIT_USER_STACK_VA)
        return PROS_ELF_EBOUNDS;
    sp -= pointer_bytes;
    sp &= ~0xFULL;
    if (sp < PROSENJIT_USER_STACK_VA)
        return PROS_ELF_EBOUNDS;

    uint64_t *stack = (uint64_t *)(page + (sp - PROSENJIT_USER_STACK_VA));
    *stack++ = argc;
    for (uint32_t i = 0; i < argc; ++i) *stack++ = argv_ptrs[i];
    *stack++ = 0;
    for (uint32_t i = 0; i < envc; ++i) *stack++ = env_ptrs[i];
    *stack++ = 0;

    /* Keep an ABI-safe 16-byte aligned initial stack pointer. */
    if ((sp & 0xFULL) != 0)
        return PROS_ELF_EBOUNDS;
    process->user_stack_top = sp;
    return PROS_ELF_OK;
}

int pros_elf_load_with_args(struct pros_process *process,
                             const uint8_t *image,
                             uint64_t size,
                             const char *const *argv,
                             uint32_t argc,
                             const char *const *envp,
                             uint32_t envc,
                             struct pros_elf_load_result *result) {
    if (argc > 64u || envc > 64u || (argc && !argv) || (envc && !envp))
        return PROS_ELF_EINVAL;
    int rc = pros_elf_load(process, image, size, result);
    if (rc != PROS_ELF_OK) return rc;
    rc = build_initial_stack(process, argv, argc, envp, envc);
    if (rc != PROS_ELF_OK) {
        rollback_user_pages(process, result->image_start, result->image_end);
        uint64_t physical = 0;
        if (address_space_unmap_user_page(&process->address_space,
                                          PROSENJIT_USER_STACK_VA,
                                          &physical) == 0)
            physical_page_free(physical);
        process->user_entry = 0;
        process->user_stack_top = 0;
        result->mapped_pages = 0;
        return rc;
    }
    result->stack_top = process->user_stack_top;
    return PROS_ELF_OK;
}

int pros_elf_load(struct pros_process *process, const uint8_t *image,
                  uint64_t size, struct pros_elf_load_result *result)
{
    const struct elf64_ehdr *eh;
    uint64_t start, end, entry;
    uint64_t mapped = 0;

    if (!process || !image || !result || process->pid == 0u ||
        process->state == PROS_PROCESS_DEAD ||
        process->state == PROS_PROCESS_ZOMBIE ||
        process_state(process) == PROS_PROCESS_DEAD)
        return PROS_ELF_EINVAL;
    {
        int validation_rc = pros_elf_validate(image, size);
        if (validation_rc != PROS_ELF_OK)
            return validation_rc;
    }

    eh = (const struct elf64_ehdr *)image;
    {
        int validation_rc =
            validate_load_segments(image, size, &entry, &start, &end);
        if (validation_rc != PROS_ELF_OK)
            return validation_rc;
    }

    /* Preflight all pages that this image would actually map. This makes
     * rollback ownership-safe: after this point, every conflicting page has
     * already been rejected and every mapped page belongs to this load. */
    for (uint64_t va = start; va < end; va += ELF_PAGE) {
        int flags = -1;
        for (uint16_t i = 0; i < eh->phnum; ++i) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)(image + eh->phoff +
                                            (uint64_t)i * sizeof(*ph));
            if (ph->type != PT_LOAD) continue;
            int f = segment_page_flags(ph, va);
            if (f < 0) continue;
            if (flags < 0) flags = f;
            else if (flags != f)
                return PROS_ELF_ECONFLICT;
        }
        if (flags >= 0 && page_mapped(&process->address_space, va))
            return PROS_ELF_ECONFLICT;
    }

    for (uint64_t va = start; va < end; va += ELF_PAGE) {
        int flags = -1;
        for (uint16_t i = 0; i < eh->phnum; ++i) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)(image + eh->phoff +
                                            (uint64_t)i * sizeof(*ph));
            if (ph->type != PT_LOAD) continue;
            int f = segment_page_flags(ph, va);
            if (f < 0) continue;
            if (flags < 0) flags = f;
            else if (flags != f) {
                rollback_user_pages(process, start, end);
                return PROS_ELF_ECONFLICT;
            }
        }
        if (flags < 0) continue;
        uint64_t physical = physical_page_alloc_user();
        if (!physical) {
            rollback_user_pages(process, start, end);
            return PROS_ELF_ENOMEM;
        }
        for (uint64_t off = 0; off < ELF_PAGE; ++off)
            ((uint8_t *)(uintptr_t)physical)[off] = 0;

        if (address_space_map_user_page_owned(&process->address_space, va,
                                              physical, (uint64_t)flags) != 0) {
            physical_page_free(physical);
            rollback_user_pages(process, start, end);
            return PROS_ELF_EINVAL;
        }
        ++mapped;

        for (uint16_t i = 0; i < eh->phnum; ++i) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)(image + eh->phoff +
                                            (uint64_t)i * sizeof(*ph));
            if (ph->type != PT_LOAD) continue;
            uint64_t seg_end = 0, page_end = 0;
            if (!range_ok(ph->vaddr, ph->memsz, ELF_USER_MAX, &seg_end) ||
                seg_end > UINT64_MAX - (ELF_PAGE - 1ULL)) {
                rollback_user_pages(process, start, end);
                return PROS_ELF_EOVERFLOW;
            }
            page_end = (seg_end + ELF_PAGE - 1ULL) & ~(ELF_PAGE - 1ULL);
            uint64_t page_base = ph->vaddr & ~(ELF_PAGE - 1ULL);
            if (va < page_base || va >= page_end)
                continue;
            uint64_t src_delta = (va > ph->vaddr) ? va - ph->vaddr : 0;
            uint64_t dst_delta = (ph->vaddr > va) ? ph->vaddr - va : 0;
            if (dst_delta >= ELF_PAGE)
                continue;
            uint64_t copy_len = 0;
            if (src_delta < ph->filesz)
                copy_len = ph->filesz - src_delta;
            if (copy_len > ELF_PAGE - dst_delta)
                copy_len = ELF_PAGE - dst_delta;
            if (copy_len != 0) {
                if (ph->offset > UINT64_MAX - src_delta) {
                    rollback_user_pages(process, start, end);
                    return PROS_ELF_EOVERFLOW;
                }
                uint64_t file_off = ph->offset + src_delta;
                if (file_off > size || copy_len > size - file_off) {
                    rollback_user_pages(process, start, end);
                    return PROS_ELF_EBOUNDS;
                }
                for (uint64_t j = 0; j < copy_len; ++j)
                    ((uint8_t *)(uintptr_t)physical)[dst_delta + j] =
                        image[file_off + j];
            }
        }
    }

    /*
     * Reserve a dedicated initial user stack page.  The stack is deliberately
     * outside the ELF image range so segment permissions cannot accidentally
     * make it executable.  A later userspace startup layer may grow/replace
     * this page after constructing argv/env; the loader itself never trusts
     * caller-supplied stack addresses.
     */
    if (page_mapped(&process->address_space, PROSENJIT_USER_STACK_VA)) {
        rollback_user_pages(process, start, end);
        return PROS_ELF_ECONFLICT;
    }
    {
        uint64_t stack_page = physical_page_alloc_user();
        if (!stack_page) {
            rollback_user_pages(process, start, end);
            return PROS_ELF_ENOMEM;
        }
        for (uint64_t off = 0; off < ELF_PAGE; ++off)
            ((uint8_t *)(uintptr_t)stack_page)[off] = 0;
        if (address_space_map_user_page_owned(&process->address_space,
                                              PROSENJIT_USER_STACK_VA,
                                              stack_page,
                                              PROSENJIT_PAGE_WRITABLE |
                                              PROSENJIT_PAGE_NX) != 0) {
            physical_page_free(stack_page);
            rollback_user_pages(process, start, end);
            return PROS_ELF_EINVAL;
        }
        ++mapped;
    }

    process->user_entry = entry;
    process->user_stack_top = PROSENJIT_USER_STACK_TOP;
    result->entry = entry;
    result->image_start = start;
    result->image_end = end;
    result->stack_top = PROSENJIT_USER_STACK_TOP;
    result->mapped_pages = mapped;
    return PROS_ELF_OK;
}
