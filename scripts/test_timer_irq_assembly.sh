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
    'sizeof(struct pros_interrupt_frame) == 160'; do
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
    'thread->interrupt_frame_kind != PROS_INTERRUPT_FRAME_USER' \\
    'thread->interrupt_frame, PROS_INTERRUPT_FRAME_USER)' \\
    'thread->interrupt_frame->rip -= 2'; do
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
(( syscall_cs_test < syscall_kernel_kind &&
   syscall_kernel_kind < syscall_user_kind &&
   syscall_user_kind < syscall_schedule )) || {
    echo "syscall frame-kind tag is not derived before scheduler handoff" >&2
    exit 1
}

echo "ProsenjitOS: typed CPL0/CPL3 frame ABI and IPC wakeup source invariants passed"
