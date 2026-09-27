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

%macro ISR_NO_ERROR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_WITH_ERROR 1
global isr%1
isr%1:
    ; CPU already pushed the architectural error code.
    push qword %1
    jmp isr_common
%endmacro

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_WITH_ERROR 8
ISR_NO_ERROR 9
ISR_WITH_ERROR 10
ISR_WITH_ERROR 11
ISR_WITH_ERROR 12
ISR_WITH_ERROR 13
ISR_WITH_ERROR 14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_WITH_ERROR 17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_WITH_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_NO_ERROR 29
ISR_NO_ERROR 30
ISR_NO_ERROR 31

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
