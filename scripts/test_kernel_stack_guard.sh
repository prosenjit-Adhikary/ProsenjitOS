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
grep -Fq 'paging_query_physical_checked(slot_base, &guard_physical)' "$THREAD_C"
grep -Fq 'paging_query_physical_checked(slot_base + 5u * PROSENJIT_PAGE_SIZE' "$THREAD_C"
grep -Fq 'thread_kernel_stack_valid(thread)' "$THREAD_C"
grep -Fq 'thread_kernel_stack_release(thread)' "$THREAD_C"
grep -Fq 'paging_unmap_pages_expected_and_free(' "$THREAD_C"
grep -Fq 'physical_page_free_batch_preflight(' "$ROOT/kernel/memory.c"
grep -Fq 'paging_unmap_pages_expected_and_free(' "$ROOT/kernel/paging.c"
if grep -Fq 'paging_unmap_pages_expected(virtual_pages, thread->kernel_stack_pages' "$THREAD_C" ||
   grep -Fq 'physical_page_free_batch(thread->kernel_stack_pages' "$THREAD_C"; then
    echo "thread stack teardown must use the combined unmap/free transaction" >&2
    exit 1
fi
grep -Fq 'thread_registry_validate()' "$THREAD_C"
grep -Fq 'if (thread_runq_validate() != 0) return -1;' "$THREAD_C"
grep -Fq 'if (thread_runq_validate() != 0) return 0;' "$THREAD_C"
grep -Fq 'Do not let a corrupt/cyclic run queue trap the interrupt scheduler.' "$THREAD_C"
grep -Fq 'A forward-link cycle must be rejected without entering an unbounded probe.' "$THREAD_C"
grep -Fq 'thread_address_space_disjoint_preflight(' "$THREAD_C"
grep -Fq 'thread_address_space_disjoint_preflight(' "$THREAD_H"
grep -Fq 'thread_address_space_disjoint_preflight(&process->address_space)' "$ROOT/kernel/process.c"
grep -Fq 'thread_address_space_disjoint_preflight(&child->address_space)' "$ROOT/kernel/process.c"
grep -Fq 'struct pros_owned_page *stack_alias' "$THREAD_C"
grep -Fq 'invalid_owned_link_rejected' "$THREAD_C"
grep -Fq 'address_space_owned_pages_preflight(space)' "$ROOT/kernel/address_space.c"
grep -Fq 'heap_is_live_allocation' "$ROOT/kernel/heap.c"
grep -Fq 'thread_address_space_disjoint_preflight(&alias_space) == 0' "$THREAD_C"
grep -Fq 'thread_stack_quarantined_slots' "$THREAD_C"
grep -Fq 'thread_irq_save()' "$THREAD_C"
grep -Fq 'thread_irq_restore(irq_flags)' "$THREAD_C"
grep -Fq 'thread->user_interrupt_frame_storage = *user_frame;' "$THREAD_C"
awk '
    /thread->user_interrupt_frame_storage = \*user_frame;/ { frame = NR }
    /thread_registry = thread;/ { publish = NR }
    END { if (!frame || !publish || frame >= publish) exit 1 }
' "$THREAD_C"
if grep -Fq 'kmalloc(PROS_THREAD_KERNEL_STACK_SIZE)' "$THREAD_C"; then
    echo "thread kernel stacks must not use the general heap" >&2
    exit 1
fi
grep -Fq 'ProsenjitOS: kernel thread stack guard-page mapping passed' "$KERNEL_C"
echo "ProsenjitOS: guarded thread-stack source invariants passed"
