BITS 64

section .text
global copy_from_user_asm
global copy_to_user_asm

; GS base is initialized per logical CPU by percpu_init().
; GS:0 = usercopy_fault_active, GS:8 = usercopy_fault_rip.
copy_from_user_asm:
    test rdx, rdx
    jz .from_success
    pushfq
    cli
    mov qword [gs:0], 1
    lea rax, [rel .from_fault]
    mov [gs:8], rax
.from_loop:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    dec rdx
    jnz .from_loop
    mov qword [gs:0], 0
    popfq
    xor eax, eax
    ret
.from_success:
    xor eax, eax
    ret
.from_fault:
    mov qword [gs:0], 0
    popfq
    mov rax, -1
    ret

copy_to_user_asm:
    test rdx, rdx
    jz .to_success
    pushfq
    cli
    mov qword [gs:0], 1
    lea rax, [rel .to_fault]
    mov [gs:8], rax
.to_loop:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    dec rdx
    jnz .to_loop
    mov qword [gs:0], 0
    popfq
    xor eax, eax
    ret
.to_success:
    xor eax, eax
    ret
.to_fault:
    mov qword [gs:0], 0
    popfq
    mov rax, -1
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
