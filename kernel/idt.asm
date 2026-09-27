BITS 64

section .text
global idt_load
global isr_common
extern exception_dispatch
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
    call exception_dispatch
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
