BITS 64

section .text
global idt_load
global isr_common
global isr128
global isr32
extern exception_dispatch
extern syscall_dispatch
extern timer_dispatch
extern pic_eoi_master
extern user_mode_exit_trampoline
extern paging_switch_kernel
extern stack_top

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
    cmp rax, 0xFFFFFFFFFFFFFF00
    je .exit_to_kernel
    iretq

.exit_to_kernel:
    ; INT 0x80 already performed the CPL3 -> CPL0 transition. Restore the
    ; kernel address space before entering supervisor-only kernel text.
    call paging_switch_kernel
    mov rsp, stack_top
    jmp user_mode_exit_trampoline

isr32:
    cli
    ; IRQ0 is a returning interrupt, so preserve the complete general
    ; register state before entering C code. This is the minimum safe
    ; foundation for later timer-driven context switching.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push qword 0
    push qword 32
    mov rdi, rsp
    call timer_dispatch
    call timer_should_preempt
    test eax, eax
    jz .timer_no_switch
    ; Context switching from this interrupt frame is intentionally deferred
    ; until the full interrupt-frame scheduler ABI is implemented.
.timer_no_switch:
    add rsp, 16
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    call pic_eoi_master
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
ISR_NO_ERROR 21
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
