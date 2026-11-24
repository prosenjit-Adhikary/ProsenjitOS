#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASM="$ROOT/kernel/idt.asm"
[[ -f "$ASM" ]] || { echo "missing $ASM" >&2; exit 1; }

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
skip_words="$(line_of 'add rsp, 24')"
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
    'thread_interrupt_frame_valid(current_frame, frame_kind)' \
    'thread_interrupt_frame_valid(' \
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
    'idt_set_gate(2, handlers[2], 1)' \
    'idt_set_gate(8, handlers[8], 2)' \
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
    'ist1_stack_bottom:' \
    'ist2_stack_bottom:' \
    'ist3_stack_bottom:'; do
    grep -Fq "$invariant" "$ENTRY_ASM" || {
        echo "missing dedicated TSS IST stack: $invariant" >&2
        exit 1
    }
done

echo "ProsenjitOS: typed CPL0/CPL3 frame ABI and IPC wakeup source invariants passed"
echo "ProsenjitOS: NMI/double-fault/page-fault IST assignments passed"
