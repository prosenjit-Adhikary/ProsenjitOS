BITS 64

section .text
global idt_load
global isr_common
global isr128
extern exception_dispatch
extern syscall_dispatch

; int 0x80 is the initial ProsenjitOS user/kernel syscall ABI entry.
; General registers are saved so the C dispatcher can inspect/return RAX
; without depending on a compiler-specific interrupt ABI.
isr128:
    cli
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    mov rdi, rsp
    lea rsi, [rsp + 120]
    call syscall_dispatch
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

%assign n 0
%rep 32
global isr%+n
isr%+n:
    push qword 0
    push qword n
    jmp isr_common
%assign n n+1
%endrep

idt_load:
    lidt [rdi]
    ret

isr_common:
    cli
    mov rdi, rsp
    sub rsp, 8
    call exception_dispatch
    add rsp, 8
    add rsp, 16
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
