bits 64
default rel

section .text
global thread_context_switch
global thread_start_trampoline
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
    mov [rdi + 80], rsp

.load_next:
    mov rsp, [rsi + 80]
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

section .note.GNU-stack noalloc noexec nowrite progbits
