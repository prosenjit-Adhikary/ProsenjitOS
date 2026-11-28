#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASM="$ROOT/kernel/idt.asm"
KERNEL_C="$ROOT/kernel/kernel.c"
[[ -f "$ASM" ]] || { echo "missing $ASM" >&2; exit 1; }

# Multiboot2 leaves data-segment selectors to the loader. The entry stub must
# install its own GDT/data selectors before the first BSS memory operation.
BOOT_ENTRY="$ROOT/boot/entry.asm"
boot_entry="$(sed -n '/^_start:/,/^BITS 64/p' "$BOOT_ENTRY")"
boot_saved_eax="$(grep -nF 'mov esi, eax' <<<"$boot_entry" | head -n1 | cut -d: -f1)"
boot_saved_ebx="$(grep -nF 'mov edx, ebx' <<<"$boot_entry" | head -n1 | cut -d: -f1)"
boot_gdt="$(grep -nF 'lgdt [gdt64.pointer]' <<<"$boot_entry" | head -n1 | cut -d: -f1)"
boot_ds="$(grep -nF 'mov ds, ax' <<<"$boot_entry" | head -n1 | cut -d: -f1)"
boot_es="$(grep -nF 'mov es, ax' <<<"$boot_entry" | head -n1 | cut -d: -f1)"
boot_ss="$(grep -nF 'mov ss, ax' <<<"$boot_entry" | head -n1 | cut -d: -f1)"
boot_bss_clear="$(grep -nF 'rep stosd' <<<"$boot_entry" | head -n1 | cut -d: -f1)"
[[ -n "$boot_saved_eax" && -n "$boot_saved_ebx" && -n "$boot_gdt" &&
   -n "$boot_ds" && -n "$boot_es" && -n "$boot_ss" &&
   -n "$boot_bss_clear" ]] &&
    (( boot_saved_eax < boot_gdt && boot_saved_ebx < boot_gdt &&
       boot_gdt < boot_ds && boot_ds < boot_bss_clear &&
       boot_gdt < boot_es && boot_es < boot_bss_clear &&
       boot_gdt < boot_ss && boot_ss < boot_bss_clear )) || {
    echo "boot entry touches BSS before installing known data/stack segments" >&2
    exit 1
}
echo "ProsenjitOS: Multiboot2 data-segment initialization precedes BSS access"

isr="$(sed -n '/^isr32:/,/^%macro ISR_NO_ERROR/p' "$ASM")"
[[ -n "$isr" ]] || { echo "isr32 block not found" >&2; exit 1; }

line_of() {
    local pattern="$1"
    local line
    line="$(grep -nFm1 "$pattern" <<<"$isr" | cut -d: -f1)"
    [[ -n "$line" ]] || { echo "missing ISR32 invariant: $pattern" >&2; exit 1; }
    printf '%s\n' "$line"
}

saved_stack="$(line_of 'mov rbx, rsp')"
frame_arg="$(line_of 'lea rdi, [rbx + 16]')"
dispatch="$(line_of 'call timer_dispatch')"
preempt="$(line_of 'call timer_should_preempt')"
save_decision="$(line_of 'mov r12d, eax')"
eoi="$(line_of 'call pic_eoi_master')"
schedule="$(line_of 'call thread_schedule_interrupt')"
frame_kind="$(line_of 'mov esi, 1')"
user_cpl="$(line_of 'test qword [rbx + 144], 3')"
user_preempt_gate="$(line_of 'jnz .timer_no_switch')"
frame_kind_test="$(grep -nF 'test qword [rbx + 144], 3' <<<"$isr" | tail -n1 | cut -d: -f1)"
compare="$(line_of 'cmp rax, rdx')"
(( user_cpl < user_preempt_gate && user_preempt_gate < frame_kind &&
   frame_kind < frame_kind_test && frame_kind_test < schedule )) || {
    echo "timer entry does not gate CPL3 preemption before scheduler handoff" >&2
    exit 1
}

syscall_isr="$(sed -n '/^isr128:/,/^isr32:/p' "$ASM")"
grep -Fq 'test qword [rbx + 128], 3' <<<"$syscall_isr" || {
    echo "syscall entry does not derive frame kind from saved CS" >&2
    exit 1
}
grep -Fq 'call thread_schedule_interrupt' <<<"$syscall_isr" || {
    echo "syscall entry does not use the typed scheduler handoff" >&2
    exit 1
}
no_switch="$(line_of '.timer_no_switch:')"
skip_words="$(line_of 'add rsp, 16')"
restore_switched="$(line_of '.timer_restore_switched:')"

(( saved_stack < frame_arg && frame_arg < dispatch && dispatch < preempt ))
(( preempt < save_decision && save_decision < eoi && eoi < schedule ))
(( schedule < compare && compare < no_switch && no_switch < skip_words && skip_words < restore_switched ))

alignment_count="$(grep -Fc 'and rsp, -16' <<<"$isr")"
[[ "$alignment_count" -eq 4 ]] || {
    echo "expected four explicitly aligned C-call sites in ISR32; found $alignment_count" >&2
    exit 1
}

assert_aligned_call_and_restore() {
    local target="$1"
    awk -v target="call $target" '
        /and rsp, -16/ { aligned = 1; next }
        index($0, target) {
            if (!aligned) exit 1
            if (calls++) exit 1
            aligned = 0
            expect_restore = 1
            next
        }
        /call[[:space:]]/ { aligned = 0; next }
        expect_restore {
            if ($0 !~ /^[[:space:]]*mov rsp, rbx[[:space:]]*$/) exit 1
            expect_restore = 0
        }
        END {
            if (calls != 1 || expect_restore) exit 1
        }
    ' <<<"$isr" || {
        echo "C call $target is not dynamically aligned/restored" >&2
        exit 1
    }
}

assert_aligned_call_and_restore timer_dispatch
assert_aligned_call_and_restore timer_should_preempt
assert_aligned_call_and_restore pic_eoi_master
assert_aligned_call_and_restore thread_schedule_interrupt

eoi_count="$(grep -Fc 'call pic_eoi_master' <<<"$isr")"
[[ "$eoi_count" -eq 1 ]] || {
    echo "expected exactly one early master-PIC EOI in ISR32; found $eoi_count" >&2
    exit 1
}

grep -Fq 'test r12d, r12d' <<<"$isr" || {
    echo "preemption decision is not tested after EOI" >&2
    exit 1
}
grep -Fq 'je .timer_no_switch' <<<"$isr" || {
    echo "unchanged IRQ frame does not branch to vector/error cleanup" >&2
    exit 1
}

# The switched-frame restore path must not skip the IRQ vector/error pair:
# saved thread frames start at RAX and IRETQ consumes 3-word CPL0 or
# 5-word CPL3 tails according to the saved CS privilege level.
switched_block="$(sed -n '/^\.timer_restore_switched:/,/^%macro ISR_NO_ERROR/p' <<<"$isr")"
[[ -n "$switched_block" ]] || {
    echo "switched-frame restore block not found" >&2
    exit 1
}
if grep -Fq 'add rsp, 24' <<<"$switched_block"; then
    echo "switched-frame restore incorrectly skips vector/error words" >&2
    exit 1
fi
grep -Fq 'pop r15' <<<"$switched_block" || {
    echo "switched-frame restore does not restore the final saved GPR" >&2
    exit 1
}
grep -Fq 'iretq' <<<"$switched_block" || {
    echo "switched-frame restore does not return with IRETQ" >&2
    exit 1
}

echo "ProsenjitOS: timer IRQ assembly source invariants passed"


# Cross-check the C/assembly frame-kind ABI, not just the timer stub.
HEADER="$ROOT/kernel/include/thread.h"
THREAD_C="$ROOT/kernel/thread.c"
[[ -f "$HEADER" && -f "$THREAD_C" ]] || {
    echo "missing scheduler frame ABI source" >&2
    exit 1
}

for invariant in \
    'PROS_INTERRUPT_FRAME_NONE = 0' \
    'PROS_INTERRUPT_FRAME_KERNEL = 1' \
    'PROS_INTERRUPT_FRAME_USER = 2' \
    'PROS_INTERRUPT_FRAME_USER_IST = 3' \
    'offsetof(struct pros_interrupt_frame, rip) == 120' \
    'offsetof(struct pros_interrupt_frame, user_rsp) == 144' \
    'sizeof(struct pros_interrupt_frame) == 160' \
    'struct pros_interrupt_frame user_interrupt_frame_storage'; do
    grep -Fq "$invariant" "$HEADER" || {
        echo "missing interrupt-frame ABI declaration: $invariant" >&2
        exit 1
    }
done

for invariant in \
    'return frame->cs == 0x08u' \
    'frame->cs == 0x1Bu' \
    'frame->user_ss == 0x23u' \
    'frame->user_rsp < 0x0000800000000000ULL' \
    'thread_interrupt_frame_valid(current_frame, effective_frame_kind)' \
    'thread_interrupt_frame_valid(' \
    'thread_make_ist_kernel_frame(next, 0)' \
    'frame->user_rsp = resume_rsp' \
    'frame->user_ss = resume_ss' \
    'int ist_entry = frame_kind == PROS_INTERRUPT_FRAME_USER_IST' \
    'next->interrupt_frame_kind != PROS_INTERRUPT_FRAME_NONE' \
    'thread->interrupt_frame_kind != PROS_INTERRUPT_FRAME_USER' \
    'thread->interrupt_frame, PROS_INTERRUPT_FRAME_USER)' \
    'thread_capture_interrupt_frame(' \
    'destination[i] = source[i]' \
    'return &thread->user_interrupt_frame_storage' \
    'thread->interrupt_frame->rip = original_rip - 2u' \
    'thread->interrupt_frame->rax = blocked_syscall' \
    'thread->interrupt_frame->rip = original_rip' \
    'thread->interrupt_frame->rax = original_rax' \
    'thread->state = PROS_THREAD_READY' \
    'thread->state = PROS_THREAD_BLOCKED'; do
    grep -Fq "$invariant" "$THREAD_C" || {
        echo "missing scheduler frame validation/wakeup invariant: $invariant" >&2
        exit 1
    }
done

# The syscall stub's frame starts at saved RAX: RIP is +120 and CS is +128.
syscall_cs_test="$(grep -nF 'test qword [rbx + 128], 3' <<<"$syscall_isr" | cut -d: -f1)"
syscall_kernel_kind="$(grep -nF 'mov esi, 1' <<<"$syscall_isr" | cut -d: -f1)"
syscall_user_kind="$(grep -nF 'mov esi, 2' <<<"$syscall_isr" | cut -d: -f1)"
syscall_schedule="$(grep -nF 'call thread_schedule_interrupt' <<<"$syscall_isr" | cut -d: -f1)"
[[ -n "$syscall_cs_test" && -n "$syscall_kernel_kind" && \
   -n "$syscall_user_kind" && -n "$syscall_schedule" ]] || {
    echo "syscall frame-kind dispatch is incomplete" >&2
    exit 1
}
(( syscall_kernel_kind < syscall_cs_test &&
   syscall_cs_test < syscall_user_kind &&
   syscall_user_kind < syscall_schedule )) || {
    echo "syscall frame-kind tag is not derived from saved CS before scheduler handoff" >&2
    exit 1
}

# IPC receiver resume metadata must be published before waiter registration.
SYSCALL_C="$ROOT/kernel/syscall.c"
ipc_wait_block="$(sed -n '/ipc_receive_retry:/,/if (rc != PROSENJIT_IPC_OK)/p' "$SYSCALL_C")"
[[ -n "$ipc_wait_block" ]] || {
    echo "IPC receive retry/block section not found" >&2
    exit 1
}
# Compare all publication/rollback positions inside one AWK process.
# Avoid splitting captured line numbers through shell command substitution/read.
awk '
    /current->interrupt_frame = 0;/ { frame_invalidate = NR }
    /current->interrupt_frame_kind = PROS_INTERRUPT_FRAME_NONE;/ { frame_kind_clear = NR }
    /current->blocked_syscall = PROSENJIT_SYS_IPC_RECEIVE;/ { resume_publish = NR }
    /ipc_channel_wait\(/ { wait_register = NR }
    /current->blocked_syscall = 0;/ {
        clear_count++
        if (clear_count == 1) already_clear = NR
        if (clear_count == 2) error_clear = NR
    }
    END {
        if (clear_count != 2 ||
            !frame_invalidate || !frame_kind_clear || !resume_publish || !wait_register ||
            !(frame_invalidate < frame_kind_clear &&
              frame_kind_clear < resume_publish &&
              resume_publish < wait_register &&
              wait_register < already_clear &&
              already_clear < error_clear)) {
            print "IPC wait must invalidate stale frame before publication and clear resume state on retry/error" > "/dev/stderr"
            exit 1
        }
    }
' <<<"$ipc_wait_block" || exit 1

IPC_C="$ROOT/kernel/ipc.c"
KERNEL_C="$ROOT/kernel/kernel.c"
grep -Fq 'IPC early-wakeup stale-frame retry regression passed' "$KERNEL_C" || {
    echo "runtime IPC early-wakeup regression is missing" >&2
    exit 1
}
grep -Fq 'thread_schedule_interrupt(&scheduler_init_frame, PROS_INTERRUPT_FRAME_USER)' "$KERNEL_C" || {
    echo "runtime IPC regression does not exercise the interrupt scheduler handoff" >&2
    exit 1
}
grep -Fq 'channel->waiter_count == 0 || channel->count == 0' "$IPC_C" || {
    echo "IPC wakeup can resume a receiver without a queued message" >&2
    exit 1
}
grep -Fq 'ipc_channel_wake_one(' "$THREAD_C" || {
    echo "scheduler does not retry an early IPC wake after frame publication" >&2
    exit 1
}
grep -Fq 'if (prev && prev->state == PROS_THREAD_RUNNING)' "$THREAD_C" || {
    echo "scheduler may enqueue an IPC-woken predecessor twice" >&2
    exit 1
}
frame_store="$(grep -nF 'prev->interrupt_frame = thread_capture_interrupt_frame(' "$THREAD_C" | head -n1 | cut -d: -f1)"
pending_wake="$(grep -nF 'ipc_channel_wake_one(' "$THREAD_C" | head -n1 | cut -d: -f1)"
[[ -n "$frame_store" && -n "$pending_wake" ]] && (( frame_store < pending_wake )) || {
    echo "scheduler retries IPC wake before publishing the fresh interrupt frame" >&2
    exit 1
}

# Verify the TSS IST contract as well as the interrupt-frame ABI.
IDT_C="$ROOT/kernel/kernel.c"
ENTRY_ASM="$ROOT/boot/entry.asm"
for invariant in \
    'idt_set_gate(i, handlers[i], 4)' \
    'idt_set_gate(2, handlers[2], 1)' \
    'idt_set_gate(8, handlers[8], 2)' \
    'idt_set_gate(13, handlers[13], 4)' \
    'idt_set_gate(14, handlers[14], 3)' \
    'idt_set_gate(128, isr128, 0)'; do
    grep -Fq "$invariant" "$IDT_C" || {
        echo "missing IDT IST assignment: $invariant" >&2
        exit 1
    }
done
for invariant in \
    'mov [tss64 + 36], rax' \
    'mov [tss64 + 44], rax' \
    'mov [tss64 + 52], rax' \
    'mov [tss64 + 60], rax' \
    'ist1_stack_bottom:' \
    'ist2_stack_bottom:' \
    'ist3_stack_bottom:' \
    'ist4_stack_bottom:'; do
    grep -Fq "$invariant" "$ENTRY_ASM" || {
        echo "missing dedicated TSS IST stack: $invariant" >&2
        exit 1
    }
done

# SYS_EXIT has already switched to the kernel CR3 in process_exit().
# Its terminal trampoline must not make an unaligned redundant C call, and
# must enter the C function with the SysV x86-64 stack alignment contract.
exit_block="$(sed -n '/^\.exit_to_kernel:/,/^isr32:/p' "$ASM")"
[[ -n "$exit_block" ]] || {
    echo "SYS_EXIT assembly terminal block not found" >&2
    exit 1
}
grep -Fq 'mov rsp, stack_top' <<<"$exit_block" || {
    echo "SYS_EXIT does not switch to the bootstrap kernel stack" >&2
    exit 1
}
grep -Fq 'call user_mode_exit_trampoline' <<<"$exit_block" || {
    echo "SYS_EXIT does not enter the C trampoline with a call frame" >&2
    exit 1
}
if grep -Fq 'call paging_switch_kernel' <<<"$exit_block"; then
    echo "SYS_EXIT redundantly calls paging_switch_kernel from an unaligned frame" >&2
    exit 1
fi
stack_switch="$(grep -nF 'mov rsp, stack_top' <<<"$exit_block" | cut -d: -f1)"
trampoline_call="$(grep -nF 'call user_mode_exit_trampoline' <<<"$exit_block" | cut -d: -f1)"
(( stack_switch < trampoline_call )) || {
    echo "SYS_EXIT trampoline call occurs before bootstrap stack activation" >&2
    exit 1
}

echo "ProsenjitOS: SYS_EXIT trampoline stack alignment invariants passed"

echo "ProsenjitOS: typed CPL0/CPL3 frame ABI and IPC wakeup source invariants passed"
echo "ProsenjitOS: NMI/double-fault/page-fault IST assignments passed"

# Common exception entry preserves all interrupted GPRs and dynamically aligns
# the C stack without changing the original raw vector/error/IRET frame.
exception_isr="$(sed -n '/^isr_common:/,/^section .note/p' "$ASM")"
[[ -n "$exception_isr" ]] || {
    echo "common exception entry block not found" >&2
    exit 1
}
expected_pushes=(r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax)
expected_pops=(rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15)
for reg in "${expected_pushes[@]}"; do
    grep -Fq "push $reg" <<<"$exception_isr" || {
        echo "common exception entry does not preserve $reg" >&2
        exit 1
    }
done
for reg in "${expected_pops[@]}"; do
    grep -Fq "pop $reg" <<<"$exception_isr" || {
        echo "common exception return does not restore $reg" >&2
        exit 1
    }
done
grep -Fq 'and rsp, -16' <<<"$exception_isr" || {
    echo "common exception C stack is not dynamically aligned" >&2
    exit 1
}
grep -Fq 'lea rdi, [rbx + 120]' <<<"$exception_isr" || {
    echo "exception_dispatch does not receive the original raw frame" >&2
    exit 1
}
grep -Fq 'call exception_dispatch' <<<"$exception_isr" || {
    echo "common exception entry does not call exception_dispatch" >&2
    exit 1
}
grep -Fq 'mov rsp, rbx' <<<"$exception_isr" || {
    echo "common exception entry does not restore the interrupted stack" >&2
    exit 1
}
grep -Fq 'add rsp, 16' <<<"$exception_isr" || {
    echo "common exception return does not discard vector/error words" >&2
    exit 1
}
grep -Fq 'iretq' <<<"$exception_isr" || {
    echo "common exception return does not use IRETQ" >&2
    exit 1
}
echo "ProsenjitOS: common exception GPR preservation and stack alignment invariants passed"

# Architectural exception vectors that push an error code must not receive a
# synthetic zero. In particular, modern #CP (21), AMD #VC (29), and #SX (30)
# share the same hardware-error-code frame contract as the legacy vectors.
for vector in 8 10 11 12 13 14 17 21 29 30; do
    grep -Fxq "ISR_WITH_ERROR $vector" "$ASM" || {
        echo "exception vector $vector must preserve its hardware error code" >&2
        exit 1
    }
    if grep -Fxq "ISR_NO_ERROR $vector" "$ASM"; then
        echo "exception vector $vector incorrectly receives a synthetic error code" >&2
        exit 1
    fi
done
for vector in 0 1 2 3 4 5 6 7 9 15 16 18 19 20 22 23 24 25 26 27 28 31; do
    grep -Fxq "ISR_NO_ERROR $vector" "$ASM" || {
        echo "exception vector $vector must receive a synthetic zero error code" >&2
        exit 1
    }
done
echo "ProsenjitOS: legacy and modern x86 exception error-code vector ABI passed"

# Interrupt/trap gates do not provide the SysV C ABI's DF-clear guarantee.
# Clear DF before every assembly-to-C entry; IRET restores the interrupted
# RFLAGS (including the original DF bit) from the untouched hardware frame.
for entry in isr128 isr32 isr_common; do
    case "$entry" in
        isr128) block="$(sed -n '/^isr128:/,/^isr32:/p' "$ASM")" ;;
        isr32) block="$(sed -n '/^isr32:/,/^%macro ISR_NO_ERROR/p' "$ASM")" ;;
        isr_common) block="$(sed -n '/^isr_common:/,/^section .note/p' "$ASM")" ;;
    esac
    cli_line="$(grep -nFx '    cli' <<<"$block" | head -n1 | cut -d: -f1)"
    cld_line="$(grep -nFx '    cld' <<<"$block" | head -n1 | cut -d: -f1)"
    first_call="$(grep -nE '^[[:space:]]*call[[:space:]]' <<<"$block" | head -n1 | cut -d: -f1)"
    [[ -n "$cli_line" && -n "$cld_line" && -n "$first_call" ]] &&
        (( cli_line < cld_line && cld_line < first_call )) || {
        echo "$entry does not clear DF before entering C" >&2
        exit 1
    }
done
echo "ProsenjitOS: interrupt/trap C entries clear DF before dispatch passed"

# Recoverable user-copy #PF must be fixed up before optional reporting, and a
# fatal record must be captured before serial/filesystem diagnostics.
exception_c="$(sed -n '/^struct exception_dispatch_return exception_dispatch(uint64_t \*frame) {/,/^static void nested_panic_owner_selftest/p' "$KERNEL_C")"
[[ -n "$exception_c" ]] || {
    echo "normalized exception_dispatch C handler not found" >&2
    exit 1
}
grep -Fq 'pros_exception_classify(vector, frame[3] & 3u, 0u)' <<<"$exception_c" &&
    grep -Fq 'if (exception_disposition == PROS_EXCEPTION_USER_FAULT_PENDING)' <<<"$exception_c" || {
    echo "CPL3 synchronous fault policy-classifier path not found" >&2
    exit 1
}
usercopy_gate="$(grep -nF 'usercopy_fault_matches(frame[2], cr2)' <<<"$exception_c" | cut -d: -f1)"
first_report="$(grep -nF 'serial_write("ProsenjitOS: CPU exception vector' <<<"$exception_c" | cut -d: -f1)"
crash_capture="$(grep -nF 'pros_crash_record_capture(' <<<"$exception_c" | cut -d: -f1)"
[[ -n "$usercopy_gate" && -n "$first_report" && -n "$crash_capture" ]] &&
    (( usercopy_gate < first_report && crash_capture < first_report )) || {
    echo "exception dispatch performs reporting before usercopy fixup or fatal capture" >&2
    exit 1
}
usercopy_block="$(sed -n '/usercopy_fault_matches(frame\[2\], cr2)/,/^    }/p' <<<"$exception_c")"
if grep -Eq 'serial_write\(|pros_health_|pros_crash_record_|prosenjitfs_' <<<"$usercopy_block"; then
    echo "recoverable user-copy fault path performs optional diagnostics" >&2
    exit 1
fi
grep -Fq 'test rdx, rdx' "$ROOT/kernel/idt.asm" || {
    echo "exception return ABI discriminator missing" >&2
    exit 1
}
raw_return_block="$(sed -n '/    mov rsp, rax/,/^\.nested_owner_fail_stop:/p' "$ROOT/kernel/idt.asm")"
grep -Fq 'jz .restore_normalized_frame' <<<"$raw_return_block" &&
    grep -Fq 'jmp .restore_raw_exception_frame' <<<"$raw_return_block" || {
    echo "raw exception return must branch to raw-frame restore, never fall through to nested-owner fail-stop" >&2
    exit 1
}
grep -Fq '.restore_normalized_frame:' "$ROOT/kernel/idt.asm" || {
    echo "normalized scheduler-frame restore path missing" >&2
    exit 1
}
grep -Fq '.restore_raw_exception_frame:' "$ROOT/kernel/idt.asm" || {
    echo "raw vector/error frame restore path missing" >&2
    exit 1
}
grep -Fq 'discarded_user_frame' "$ROOT/kernel/thread.c" || {
    echo "discarded CPL3 frame scheduler path missing" >&2
    exit 1
}
grep -Fq 'CPL3 user-fault exit/wait/reap transaction passed' "$KERNEL_C" || {
    echo "CPL3 user-fault exit/wait/reap regression missing" >&2
    exit 1
}
grep -Fq 'PROS_INTERRUPT_FRAME_USER_IST = 3' "$ROOT/kernel/include/thread.h" || {
    echo "IST-origin scheduler frame kind missing" >&2
    exit 1
}
grep -Fq 'thread_make_ist_kernel_frame(next, 0)' "$ROOT/kernel/thread.c" || {
    echo "full IST-to-kernel IRET frame construction missing" >&2
    exit 1
}
grep -Fq 'frame->user_rsp = resume_rsp;' "$ROOT/kernel/thread.c" || {
    echo "IST kernel return RSP field missing" >&2
    exit 1
}
grep -Fq 'frame->user_ss = resume_ss;' "$ROOT/kernel/thread.c" || {
    echo "IST kernel return SS field missing" >&2
    exit 1
}
echo "ProsenjitOS: early user-copy fixup, CPL3 fault termination, and raw/normalized/IST return-frame contracts passed"

# Production panic routing must remain separate from reversible test helpers.
PANIC_ASM="$ROOT/kernel/panic_handoff.asm"
PANIC_HEADER="$ROOT/kernel/include/panic_handoff.h"
[[ -f "$PANIC_ASM" && -f "$PANIC_HEADER" ]] || {
    echo "missing production panic handoff sources" >&2
    exit 1
}
exception_isr="$(sed -n '/^isr_common:/,/^section .note/p' "$ASM")"
owner_check="$(grep -nF 'cmp dword [rel pros_panic_handoff_owner_state], 0' <<<"$exception_isr" | cut -d: -f1)"
owner_branch="$(grep -nF 'jne .nested_owner_fail_stop' <<<"$exception_isr" | cut -d: -f1)"
c_dispatch="$(grep -nF 'call exception_dispatch' <<<"$exception_isr" | cut -d: -f1)"
nested_halt="$(sed -n '/^\.nested_owner_fail_stop:/,/^\.restore_raw_exception_frame:/p' <<<"$exception_isr")"
[[ -n "$owner_check" && -n "$owner_branch" && -n "$c_dispatch" ]] &&
    (( owner_check < owner_branch && owner_branch < c_dispatch )) || {
    echo "nested panic-owner check must precede every C exception dispatch" >&2
    exit 1
}
grep -Fq 'cli' <<<"$nested_halt" && grep -Fq 'hlt' <<<"$nested_halt" || {
    echo "nested panic-owner branch is not an assembly-only cli/hlt fail-stop" >&2
    exit 1
}
production_entry="$(sed -n '/^pros_panic_handoff_enter_production:/,/^pros_panic_handoff_production_probe:/p' "$PANIC_ASM")"
prepare_entry="$(sed -n '/^pros_panic_handoff_prepare_and_switch:/,/^pros_panic_handoff_production_terminal:/p' "$PANIC_ASM")"
terminal_entry="$(sed -n '/^pros_panic_handoff_production_terminal:/,/^pros_panic_handoff_production_probe_continuation:/p' "$PANIC_ASM")"
probe_entry="$(sed -n '/^pros_panic_handoff_production_probe:/,/^pros_panic_handoff_prepare_and_switch:/p' "$PANIC_ASM")"
[[ -n "$production_entry" && -n "$prepare_entry" && -n "$terminal_entry" && -n "$probe_entry" ]] || {
    echo "production panic entry/shared transition/runtime probe missing" >&2
    exit 1
}
grep -Fq 'jmp pros_panic_handoff_prepare_and_switch' <<<"$production_entry" || {
    echo "production fatal entry does not use the shared first-owner transition" >&2
    exit 1
}
for invariant in \
    'call pros_panic_handoff_validate' \
    'call pros_panic_handoff_try_claim' \
    'and rsp, -16' \
    'rep movsq' \
    'lea rsp, [rel __panic_stack_top]' \
    'sub rsp, 16'; do
    grep -Fq "$invariant" <<<"$prepare_entry" || {
        echo "shared production panic transition missing invariant: $invariant" >&2
        exit 1
    }
done
grep -Fq 'jmp pros_panic_handoff_prepare_and_switch' <<<"$probe_entry" || {
    echo "runtime probe does not exercise the shared production transition" >&2
    exit 1
}
grep -Fq 'PANIC_PRODUCTION_STACK_SWITCH' "$PANIC_ASM" || {
    echo "production transition runtime probe marker is missing" >&2
    exit 1
}
if grep -Eq 'test_return_mode|test_nested_mode|test_continuation|serial_|prosenjitfs_|block_device_|ata_|out dx|call ' <<<"$terminal_entry"; then
    echo "production terminal continuation contains test hooks or I/O" >&2
    exit 1
fi
grep -Fq 'pros_panic_frame_normalize(frame, cr2, &fatal_frame)' "$KERNEL_C" || {
    echo "fatal exception route does not normalize the raw CPU frame" >&2
    exit 1
}
grep -Fq 'pros_panic_handoff_enter_production(&fatal_frame)' "$KERNEL_C" || {
    echo "fatal exception route does not enter the production panic handoff" >&2
    exit 1
}
fatal_route="$(grep -nF 'pros_panic_handoff_enter_production(&fatal_frame)' "$KERNEL_C" | cut -d: -f1)"
fatal_capture="$(grep -nF 'pros_crash_record_capture(' "$KERNEL_C" | head -n1 | cut -d: -f1)"
fatal_serial="$(grep -nF 'serial_write("ProsenjitOS: CPU exception vector' "$KERNEL_C" | head -n1 | cut -d: -f1)"
usercopy_route="$(grep -nF 'usercopy_fault_matches(frame[2], cr2)' "$KERNEL_C" | head -n1 | cut -d: -f1)"
cpl3_route="$(grep -nF 'if (exception_disposition == PROS_EXCEPTION_USER_FAULT_PENDING)' "$KERNEL_C" | head -n1 | cut -d: -f1)"
grep -Fq 'pros_exception_classify(vector, frame[3] & 3u, 0u)' "$KERNEL_C" || {
    echo "CPL3 fault routing does not use the shared exception policy classifier" >&2
    exit 1
}
[[ -n "$fatal_route" && -n "$fatal_capture" && -n "$fatal_serial" &&
   -n "$usercopy_route" && -n "$cpl3_route" ]] &&
    (( usercopy_route < cpl3_route && cpl3_route < fatal_capture &&
       fatal_capture < fatal_route && fatal_route < fatal_serial )) || {
    echo "fatal panic routing must follow usercopy/CPL3 handling and RAM capture, before diagnostics" >&2
    exit 1
}
grep -Fq 'pros_panic_handoff_production_probe(&frame)' "$ROOT/kernel/panic_handoff.c" || {
    echo "runtime probe does not exercise the shared production transition" >&2
    exit 1
}
echo "ProsenjitOS: production panic-stack handoff and assembly nested-owner invariants passed"
