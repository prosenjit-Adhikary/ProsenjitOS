#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THREAD_H="$ROOT/kernel/include/thread.h"
THREAD_C="$ROOT/kernel/thread.c"
KERNEL_C="$ROOT/kernel/kernel.c"

grep -Fq 'uint64_t kernel_stack_pages[4];' "$THREAD_H"
grep -Fq 'uint32_t kernel_stack_slot;' "$THREAD_H"
grep -Fq '#define PROS_THREAD_STACK_ARENA_BASE 0xFFFF800800000000ULL' "$THREAD_C"
grep -Fq '#define PROS_THREAD_STACK_SLOT_PAGES 6ULL' "$THREAD_C"
grep -Fq '#define PROS_THREAD_STACK_PAGE_COUNT 4u' "$THREAD_C"
grep -Fq 'ensure_table(kernel_pml4, 256u, PROSENJIT_PAGE_WRITABLE)' "$ROOT/kernel/paging.c"
grep -Fq 'ensure_table(kernel_high_pdpt, 32u, PROSENJIT_PAGE_WRITABLE)' "$ROOT/kernel/paging.c"
grep -Fq 'paging_map_page(stack_base +' "$THREAD_C"
grep -Fq 'PROSENJIT_PAGE_WRITABLE | PROSENJIT_PAGE_NX' "$THREAD_C"
grep -Fq 'paging_translate(slot_base) != 0u' "$THREAD_C"
grep -Fq 'paging_translate(slot_base + 5u * PROSENJIT_PAGE_SIZE) != 0u' "$THREAD_C"
grep -Fq 'thread_kernel_stack_valid(thread)' "$THREAD_C"
grep -Fq 'thread_kernel_stack_release(thread)' "$THREAD_C"
grep -Fq 'physical_page_free(thread->kernel_stack_pages[i])' "$THREAD_C"
if grep -Fq 'kmalloc(PROS_THREAD_KERNEL_STACK_SIZE)' "$THREAD_C"; then
    echo "thread kernel stacks must not use the general heap" >&2
    exit 1
fi
grep -Fq 'ProsenjitOS: kernel thread stack guard-page mapping passed' "$KERNEL_C"
echo "ProsenjitOS: guarded thread-stack source invariants passed"
