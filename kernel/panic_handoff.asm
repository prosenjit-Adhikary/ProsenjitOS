BITS 64
DEFAULT REL

SECTION .bss align=4
global pros_panic_handoff_owner_state
global pros_panic_handoff_test_marker
pros_panic_handoff_owner_state: resd 1
pros_panic_handoff_test_marker: resd 1

SECTION .text
global pros_panic_handoff_validate
global pros_panic_handoff_try_claim
global pros_panic_handoff_reset_test
global pros_panic_handoff_enter_test
global pros_panic_handoff_test_continuation
extern __panic_stack_top
extern __panic_frame_storage

; int pros_panic_handoff_validate(const struct pros_panic_frame *frame)
; The normalized ABI is validated without dereferencing optional raw-frame data.
pros_panic_handoff_validate:
    test rdi, rdi
    jz .invalid
    test rdi, 7
    jnz .invalid
    cmp dword [rdi + 0], 1
    jne .invalid
    cmp dword [rdi + 4], 128
    jne .invalid

    mov rax, [rdi + 8]             ; vector
    cmp rax, 31
    ja .invalid
    mov rdx, [rdi + 32]            ; CS
    test rdx, rdx
    jz .invalid
    cmp rdx, 0xFFFF
    ja .invalid
    mov rcx, [rdi + 40]            ; RFLAGS bit 1 is always set
    test rcx, 2
    jz .invalid
    cmp qword [rdi + 24], 0       ; RIP
    je .invalid

    mov r8, [rdi + 72]             ; flags
    mov rcx, r8
    shr rcx, 4
    jnz .invalid

    ; FROM_USER must agree with CS.RPL.
    mov rcx, rdx
    and rcx, 3
    test r8, 4
    jz .kernel_frame
    cmp rcx, 3
    jne .invalid
    test r8, 1
    jz .invalid
    jmp .check_optional
.kernel_frame:
    test rcx, rcx
    jnz .invalid

.check_optional:
    ; Old RSP/SS are present only when the corresponding validity bit is set.
    test r8, 1
    jz .no_old_rsp
    cmp qword [rdi + 48], 0
    je .invalid
    cmp qword [rdi + 56], 0
    je .invalid
    cmp qword [rdi + 56], 0xFFFF
    ja .invalid
    mov rcx, [rdi + 56]
    and rcx, 3
    test r8, 4
    jz .kernel_ss
    cmp rcx, 3
    jne .invalid
    jmp .check_cr2
.kernel_ss:
    test rcx, rcx
    jnz .invalid
    jmp .check_cr2
.no_old_rsp:
    cmp qword [rdi + 48], 0
    jne .invalid
    cmp qword [rdi + 56], 0
    jne .invalid

.check_cr2:
    test r8, 2
    jz .no_cr2
    cmp qword [rdi + 8], 14
    jne .invalid
    jmp .check_ist
.no_cr2:
    cmp qword [rdi + 64], 0
    jne .invalid
    cmp qword [rdi + 8], 14
    je .invalid
.check_ist:
    ; Any IST stack switch supplies an old RSP/SS pair.
    test r8, 8
    jz .check_reserved
    test r8, 1
    jz .invalid

.check_reserved:
    lea rsi, [rdi + 80]
    mov ecx, 6
.reserved_loop:
    cmp qword [rsi], 0
    jne .invalid
    add rsi, 8
    loop .reserved_loop
    xor eax, eax
    ret
.invalid:
    mov eax, -1
    ret

; int pros_panic_handoff_try_claim(void)
; A successful claim is permanent in the production path.
pros_panic_handoff_try_claim:
    xor eax, eax
    mov edx, 1
    lock cmpxchg [rel pros_panic_handoff_owner_state], edx
    jne .claim_failed
    xor eax, eax
    ret
.claim_failed:
    mov eax, -1
    ret

; Test-only reset; never called by production exception routing.
pros_panic_handoff_reset_test:
    mov dword [rel pros_panic_handoff_owner_state], 0
    ret

; Isolated test entry. It is not referenced by isr_common or any IDT gate.
; Invalid ABI returns -1 before claiming ownership. A duplicate owner fail-stops.
; A valid first owner copies the complete snapshot before switching stacks.
pros_panic_handoff_enter_test:
    push r12
    mov r12, rdi
    call pros_panic_handoff_validate
    test eax, eax
    jnz .entry_invalid
    call pros_panic_handoff_try_claim
    test eax, eax
    jnz .nested_fail_stop

    cld
    mov rsi, r12
    lea rdi, [rel __panic_frame_storage]
    mov ecx, 16
    rep movsq

    lea rsp, [rel __panic_stack_top]
    and rsp, -16
    lea rdi, [rel __panic_frame_storage]
    call pros_panic_handoff_test_continuation

.nested_fail_stop:
    cli
.halt:
    hlt
    jmp .halt

.entry_invalid:
    pop r12
    mov eax, -1
    ret

; Test-only continuation: RAM marker only, then fail-stop. No C, serial,
; health, filesystem, block, ATA, or ordinary logging calls.
pros_panic_handoff_test_continuation:
    mov dword [rel pros_panic_handoff_test_marker], 0x50414E49
    cli
.continuation_halt:
    hlt
    jmp .continuation_halt
