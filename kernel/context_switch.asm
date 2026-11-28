bits 64
default rel

section .text
global thread_context_switch
global thread_start_trampoline

; RDI = previous thread, RSI = next thread.
; Save/restore the System V callee-saved register set on each thread's
; kernel stack. The CALL return address remains on the active stack, so RET
; resumes execution in the selected thread.

thread_context_switch:
    test rdi, rdi
    jz .load_next

    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi + 88], rsp

.load_next:
    mov rsp, [rsi + 88]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret


extern thread_bootstrap
thread_start_trampoline:
    ; r12 is intentionally reserved as the bootstrap thread pointer.
    mov rdi, r12
    call thread_bootstrap
    ud2


; Enter a scheduler-selected interrupt frame without returning to the boot stack.
; The frame contains the 15-GPR prefix followed by the CPL3 five-word IRET tail.
global thread_enter_interrupt_frame
thread_enter_interrupt_frame:
    ; Match enter_user_mode(): never enter CPL3 with the kernel data selector.
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov rsp, rdi
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
