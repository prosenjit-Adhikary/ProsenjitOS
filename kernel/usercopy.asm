BITS 64

section .text
global copy_from_user_asm
global copy_to_user_asm
global usercopy_fault_active
global usercopy_fault_rip

section .data
align 8
usercopy_fault_active: dq 0
usercopy_fault_rip:    dq 0

section .text
; int copy_from_user_asm(void *dst, const void *src, uint64_t size)
copy_from_user_asm:
    test rdx, rdx
    jz .success
    mov qword [rel usercopy_fault_active], 1
    lea rax, [rel .from_fault]
    mov [rel usercopy_fault_rip], rax
.from_loop:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    dec rdx
    jnz .from_loop
    mov qword [rel usercopy_fault_active], 0
    xor eax, eax
    ret
.from_fault:
    mov qword [rel usercopy_fault_active], 0
    mov rax, -1
    ret

; int copy_to_user_asm(void *dst, const void *src, uint64_t size)
copy_to_user_asm:
    test rdx, rdx
    jz .success
    mov qword [rel usercopy_fault_active], 1
    lea rax, [rel .to_fault]
    mov [rel usercopy_fault_rip], rax
.to_loop:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    dec rdx
    jnz .to_loop
    mov qword [rel usercopy_fault_active], 0
    xor eax, eax
    ret
.to_fault:
    mov qword [rel usercopy_fault_active], 0
    mov rax, -1
    ret

.success:
    xor eax, eax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
