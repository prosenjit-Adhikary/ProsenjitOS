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
    line="$(grep -nF "$pattern" <<<"$isr" | head -n1 | cut -d: -f1)"
    [[ -n "$line" ]] || { echo "missing ISR32 invariant: $pattern" >&2; exit 1; }
    printf '%s\n' "$line"
}

preempt="$(line_of 'call timer_should_preempt')"
save_decision="$(line_of 'mov r12d, eax')"
eoi="$(line_of 'call pic_eoi_master')"
schedule="$(line_of 'call thread_schedule_interrupt')"
compare="$(line_of 'cmp rax, rdx')"
no_switch="$(line_of '.timer_no_switch:')"
skip_words="$(line_of 'add rsp, 24')"
restore_switched="$(line_of '.timer_restore_switched:')"

(( preempt < save_decision && save_decision < eoi && eoi < schedule ))
(( schedule < compare && compare < no_switch && no_switch < skip_words && skip_words < restore_switched ))

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

echo "ProsenjitOS: timer IRQ assembly source invariants passed"
