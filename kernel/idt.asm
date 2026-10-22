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
    ; Keep the register frame and CPU IRET frame addressed independently of
    ; the interrupted stack alignment. IST entry is robust to stack damage, but
    ; C calls still require 16-byte alignment at the CALL instruction.
    mov r12, rsp
    and rsp, -16
    lea rdi, [r12]
    lea rsi, [r12 + 120]
    call syscall_dispatch
    cmp qword [r12], 0xFFFFFFFFFFFFFF01
    jne .syscall_return
    mov rdi, r12
    call thread_schedule_interrupt
    test rax, rax
    jz .syscall_return
    mov rsp, rax
    jmp .syscall_restore_switched

.syscall_return:
    mov rsp, r12
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
    ; IRQ0 is a returning interrupt. Save all GPRs and keep the logical frame
    ; pointer separate from the C-call stack alignment; the interrupted kernel
    ; RSP can be at any alignment because the timer may fire at any instruction.
    ; Frame layout at r12: vector,error,RAX..R15,RIP,CS,RFLAGS.
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

    mov r12, rsp
    and rsp, -16
    lea rdi, [r12 + 16]
    call timer_dispatch
    call timer_should_preempt
    test eax, eax
    jz .timer_no_switch

    lea rdi, [r12 + 16]
    call thread_schedule_interrupt
    test rax, rax
    jz .timer_no_switch

    ; The returned frame belongs to the next thread. Preserve that pointer
    ; across the C EOI helper, which may clobber caller-saved registers.
    mov r12, rax
    call pic_eoi_master
    mov rsp, r12
    jmp .timer_restore_switched

.timer_no_switch:
    ; Send EOI while the interrupted register frame is still intact.
    call pic_eoi_master
    mov rsp, r12
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
    ; Exceptions can be recovered (for example, a usercopy page fault), so
    ; preserve every interrupted GPR rather than treating the exception as a
    ; normal C call. The C frame remains vector,error,RIP,CS,RFLAGS,...
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

    lea rdi, [rsp + 120]
    ; Interrupts can arrive at any instruction, so do not assume the interrupted
    ; stack has C-call alignment. Keep the saved-frame pointer in callee-saved
    ; r12 (its interrupted value is already on the stack), align for the call,
    ; then restore the exact pre-alignment RSP.
    mov r12, rsp
    and rsp, -16
    call exception_dispatch
    mov rsp, r12

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
    add rsp, 16
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
