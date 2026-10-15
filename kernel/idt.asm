BITS 64

section .text
global idt_load
global isr_common
global isr128
global isr32
extern exception_dispatch
extern syscall_dispatch
extern timer_dispatch
extern timer_should_preempt
extern thread_schedule_interrupt
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
    ; IDT vector 128 uses IST1, whose top is 16-byte aligned.
    ; The CPL3 IRET frame (5 qwords) plus 15 saved GPRs leaves RSP
    ; 0 mod 16, as required before a SysV C call.
    lea rdi, [rsp]
    lea rsi, [rsp + 120]
    call syscall_dispatch
    cmp qword [rsp], 0xFFFFFFFFFFFFFF01
    jne .syscall_return
    lea rdi, [rsp]
    call thread_schedule_interrupt
    test rax, rax
    jz .syscall_return
    mov rsp, rax
    jmp .syscall_restore_switched

.syscall_return:
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
    jmp .syscall_check_exit

.syscall_restore_switched:
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

.syscall_check_exit:
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
    ; Build pros_interrupt_frame in memory order:
    ; rax..r15, vector, error_code, then the CPU iret frame.
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
    push qword 0
    push qword 32
    ; A timer IRQ may interrupt CPL0 at any stack alignment; RSP0 alignment
    ; alone is not enough once execution is already in the kernel. Preserve
    ; this interrupt-frame base in callee-saved RBX (original RBX is in frame),
    ; align RSP down before every SysV C call, then restore the frame base.
    mov rbx, rsp
    and rsp, -16
    lea rdi, [rbx + 16]
    call timer_dispatch
    mov rsp, rbx
    and rsp, -16
    call timer_should_preempt
    mov rsp, rbx
    test eax, eax
    jz .timer_no_switch
    and rsp, -16
    lea rdi, [rbx + 16]
    call thread_schedule_interrupt
    mov rsp, rbx
    test rax, rax
    jz .timer_no_switch

    ; A switched thread supplies a saved pros_interrupt_frame whose layout
    ; is RAX..R15, RIP, CS, RFLAGS. It has no synthetic vector/error pair.
    ; Keep the selected frame across the C EOI helper. R12 is callee-saved,
    ; and the interrupted thread's original R12 is already in its saved frame.
    mov r12, rax
    and rsp, -16
    call pic_eoi_master
    mov rsp, r12
    jmp .timer_restore_switched

.timer_no_switch:
    ; A C call after restoring GPRs would corrupt caller-saved interrupted
    ; registers. Acknowledge the IRQ while the complete register frame is saved.
    mov rsp, rbx
    and rsp, -16
    call pic_eoi_master
    mov rsp, rbx
    ; The current IRQ frame still contains vector/error below RAX..R15.
    ; Restore the GPRs, then skip those two words before IRET.
    add rsp, 16
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

.timer_restore_switched:
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
