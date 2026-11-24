BITS 64
DEFAULT REL

SECTION .bss align=4
global pros_panic_handoff_owner_state
global pros_panic_handoff_test_marker
pros_panic_handoff_owner_state: resd 1
pros_panic_handoff_test_marker: resd 1
align 8
pros_panic_handoff_test_stack_seen: resq 1
pros_panic_handoff_test_stack_marker: resd 1
pros_panic_handoff_test_return_mode: resd 1
pros_panic_handoff_test_nested_mode: resd 1

SECTION .text
global pros_panic_handoff_validate
global pros_panic_handoff_try_claim
global pros_panic_handoff_reset_test
global pros_panic_handoff_enter_test
global pros_panic_handoff_enter_production
global pros_panic_handoff_stack_probe
global pros_panic_handoff_test_stack_seen
global pros_panic_handoff_test_stack_marker
global pros_panic_handoff_test_return_mode
global pros_panic_handoff_test_nested_entry
global pros_panic_handoff_test_continuation
extern __panic_stack_bottom
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
    ; IA-32e always saves old SS:RSP in every interrupt/exception frame.
    test r8, 1
    jz .invalid
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
    push r13
    push r14
    mov r13, rsp
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

    ; Only the explicit reversible test mode may return from this entry.
    mov rsp, r13
    pop r14
    pop r13
    pop r12
    xor eax, eax
    ret

.nested_fail_stop:
    ; Only the dedicated QEMU regression enables this debugcon marker.
    ; The production nested-owner path performs no I/O before fail-stop.
    cmp dword [rel pros_panic_handoff_test_nested_mode], 1
    jne .nested_halt
    mov dx, 0x00E9
    lea rsi, [rel pros_panic_handoff_nested_test_marker]
.nested_marker_loop:
    mov al, [rsi]
    test al, al
    jz .nested_halt
    out dx, al
    inc rsi
    jmp .nested_marker_loop
.nested_halt:
    cli
.halt:
    hlt
    jmp .halt

.entry_invalid:
    mov rsp, r13
    pop r14
    pop r13
    pop r12
    mov eax, -1
    ret

; Production fatal entry. The caller supplies a complete normalized snapshot.
; This path is permanently non-returning, performs no logging/device I/O, and
; never consults test flags or test continuations.
pros_panic_handoff_enter_production:
    cli
    mov r12, rdi
    call pros_panic_handoff_validate
    test eax, eax
    jnz .production_fail_stop_current
    call pros_panic_handoff_try_claim
    test eax, eax
    jnz .production_fail_stop_current

    cld
    mov rsi, r12
    lea rdi, [rel __panic_frame_storage]
    mov ecx, 16
    rep movsq

    ; No C call follows the switch: the terminal loop is assembly-only.
    lea rsp, [rel __panic_stack_top]
    and rsp, -16
    jmp .production_halt

.production_fail_stop_current:
    cli
.production_current_halt:
    hlt
    jmp .production_current_halt

.production_halt:
    cli
.production_halt_loop:
    hlt
    jmp .production_halt_loop

; Test-only wrapper. The caller must already own the panic handoff.
; A duplicate claim follows the exact same .nested_fail_stop branch above.
pros_panic_handoff_test_nested_entry:
    mov dword [rel pros_panic_handoff_test_nested_mode], 1
    call pros_panic_handoff_enter_test
    mov dword [rel pros_panic_handoff_test_nested_mode], 0
    ret

; Reversible test-only probe of the same reserved stack bounds/alignment.
; It is never used by exception routing and restores the caller's stack.
pros_panic_handoff_stack_probe:
    push r12
    mov r12, rsp
    lea rsp, [rel __panic_stack_top]
    and rsp, -16
    call .probe_continuation
    mov rsp, r12
    pop r12
    xor eax, eax
    ret
.probe_continuation:
    mov [rel pros_panic_handoff_test_stack_seen], rsp
    mov dword [rel pros_panic_handoff_test_stack_marker], 0x5354414B
    ret

; Test-only continuation: RAM marker only, then fail-stop. No C, serial,
; health, filesystem, block, ATA, or ordinary logging calls.
pros_panic_handoff_test_continuation:
    mov [rel pros_panic_handoff_test_stack_seen], rsp
    mov dword [rel pros_panic_handoff_test_marker], 0x50414E49
    cmp dword [rel pros_panic_handoff_test_return_mode], 1
    je .test_return
    cli
.continuation_halt:
    hlt
    jmp .continuation_halt
.test_return:
    ret

section .rodata
pros_panic_handoff_nested_test_marker: db "PANIC_NESTED_FAILSTOP", 10, 0
