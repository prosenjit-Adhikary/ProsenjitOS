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
compare="$(line_of 'cmp rax, rdx')"
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
# saved thread frames contain only GPRs followed by RIP/CS/RFLAGS.
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
