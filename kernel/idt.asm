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
extern pros_ist_stack_probe_rsp

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
ISR_WITH_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_WITH_ERROR 29
ISR_WITH_ERROR 30
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


; Runtime register-preservation probes. The C caller is responsible for
; enabling the INT3 recovery gate around the exception-path probe.
global interrupt_gpr_sentinel_selftest
interrupt_gpr_sentinel_selftest:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov eax, 0x101
    mov ebx, 0x202
    mov ecx, 0x303
    mov edx, 0x404
    mov esi, 0x505
    mov edi, 0x606
    mov ebp, 0x707
    mov r8d, 0x808
    mov r9d, 0x909
    mov r10d, 0xA0A
    mov r11d, 0xB0B
    mov r12d, 0xC0C
    mov r13d, 0xD0D
    mov r14d, 0xE0E
    mov r15d, 0xF0F
    int3
    cmp rax, 0x101
    jne .gpr_fail
    cmp rbx, 0x202
    jne .gpr_fail
    cmp rcx, 0x303
    jne .gpr_fail
    cmp rdx, 0x404
    jne .gpr_fail
    cmp rsi, 0x505
    jne .gpr_fail
    cmp rdi, 0x606
    jne .gpr_fail
    cmp rbp, 0x707
    jne .gpr_fail
    cmp r8, 0x808
    jne .gpr_fail
    cmp r9, 0x909
    jne .gpr_fail
    cmp r10, 0xA0A
    jne .gpr_fail
    cmp r11, 0xB0B
    jne .gpr_fail
    cmp r12, 0xC0C
    jne .gpr_fail
    cmp r13, 0xD0D
    jne .gpr_fail
    cmp r14, 0xE0E
    jne .gpr_fail
    cmp r15, 0xF0F
    jne .gpr_fail
    xor eax, eax
    jmp .gpr_restore
.gpr_fail:
    mov eax, 1
.gpr_restore:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

global timer_gpr_sentinel_selftest
timer_gpr_sentinel_selftest:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov eax, 0x111
    mov ebx, 0x222
    mov ecx, 0x333
    mov edx, 0x444
    mov esi, 0x555
    mov edi, 0x666
    mov ebp, 0x777
    mov r8d, 0x888
    mov r9d, 0x999
    mov r10d, 0xAAA
    mov r11d, 0xBBB
    mov r12d, 0xCCC
    mov r13d, 0xDDD
    mov r14d, 0xEEE
    mov r15d, 0xFFF
    int 0x20
    cmp rax, 0x111
    jne .timer_gpr_fail
    cmp rbx, 0x222
    jne .timer_gpr_fail
    cmp rcx, 0x333
    jne .timer_gpr_fail
    cmp rdx, 0x444
    jne .timer_gpr_fail
    cmp rsi, 0x555
    jne .timer_gpr_fail
    cmp rdi, 0x666
    jne .timer_gpr_fail
    cmp rbp, 0x777
    jne .timer_gpr_fail
    cmp r8, 0x888
    jne .timer_gpr_fail
    cmp r9, 0x999
    jne .timer_gpr_fail
    cmp r10, 0xAAA
    jne .timer_gpr_fail
    cmp r11, 0xBBB
    jne .timer_gpr_fail
    cmp r12, 0xCCC
    jne .timer_gpr_fail
    cmp r13, 0xDDD
    jne .timer_gpr_fail
    cmp r14, 0xEEE
    jne .timer_gpr_fail
    cmp r15, 0xFFF
    jne .timer_gpr_fail
    xor eax, eax
    jmp .timer_gpr_restore
.timer_gpr_fail:
    mov eax, 1
.timer_gpr_restore:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; Reserved software-test vectors exercise real IST stack switching without
; calling C from the handlers. Record the CPU-generated frame pointer, restore
; RAX unchanged, and return through the exact hardware interrupt frame.
global isr_ist1_probe
isr_ist1_probe:
    push rax
    lea rax, [rsp + 8]
    mov [rel pros_ist_stack_probe_rsp], rax
    pop rax
    iretq

global isr_ist2_probe
isr_ist2_probe:
    push rax
    lea rax, [rsp + 8]
    mov [rel pros_ist_stack_probe_rsp + 8], rax
    pop rax
    iretq

global isr_ist3_probe
isr_ist3_probe:
    push rax
    lea rax, [rsp + 8]
    mov [rel pros_ist_stack_probe_rsp + 16], rax
    pop rax
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
