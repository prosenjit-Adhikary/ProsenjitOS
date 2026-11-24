BITS 64

section .text
global copy_from_user_asm
global copy_to_user_asm
extern usercopy_fault_active_state
extern usercopy_fault_rip_state

copy_from_user_asm:
    test rdx, rdx
    jz .from_success
    pushfq
    cli
    mov qword [rel usercopy_fault_active_state], 1
    lea rax, [rel .from_fault]
    mov [rel usercopy_fault_rip_state], rax
.from_loop:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    dec rdx
    jnz .from_loop
    mov qword [rel usercopy_fault_active_state], 0
    mov qword [rel usercopy_fault_rip_state], 0
    popfq
    xor eax, eax
    ret
.from_success:
    xor eax, eax
    ret
.from_fault:
    mov qword [rel usercopy_fault_active_state], 0
    mov qword [rel usercopy_fault_rip_state], 0
    popfq
    mov rax, -1
    ret

copy_to_user_asm:
    test rdx, rdx
    jz .to_success
    pushfq
    cli
    mov qword [rel usercopy_fault_active_state], 1
    lea rax, [rel .to_fault]
    mov [rel usercopy_fault_rip_state], rax
.to_loop:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    dec rdx
    jnz .to_loop
    mov qword [rel usercopy_fault_active_state], 0
    mov qword [rel usercopy_fault_rip_state], 0
    popfq
    xor eax, eax
    ret
.to_success:
    xor eax, eax
    ret
.to_fault:
    mov qword [rel usercopy_fault_active_state], 0
    mov qword [rel usercopy_fault_rip_state], 0
    popfq
    mov rax, -1
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
