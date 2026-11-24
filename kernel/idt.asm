BITS 64

section .text
global idt_load
global isr_common
global isr128
global isr32
extern exception_dispatch
extern pros_panic_handoff_owner_state
extern syscall_dispatch
extern timer_dispatch
extern timer_should_preempt
extern thread_schedule_interrupt
extern pic_eoi_master
extern user_mode_exit_trampoline
extern stack_top

isr128:
    cli
    cld
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
    ; Keep the saved GPR frame in RBX and align the temporary C stack.
    ; CPL3 RSP0 is 16-byte aligned, but this also makes an accidental CPL0
    ; invocation ABI-safe without changing the saved interrupt-frame address.
    mov rbx, rsp
    and rsp, -16
    lea rdi, [rbx]
    lea rsi, [rbx + 120]
    call syscall_dispatch
    mov rsp, rbx
    cmp qword [rsp], 0xFFFFFFFFFFFFFF01
    jne .syscall_return
    and rsp, -16
    lea rdi, [rbx]
    ; Scheduler frame kind: USER=2 when the saved IRET CS has CPL3,
    ; otherwise KERNEL=1. The C ABI receives the enum in ESI.
    mov esi, 1
    test qword [rbx + 128], 3
    jz .syscall_frame_kind_ready
    mov esi, 2
.syscall_frame_kind_ready:
    call thread_schedule_interrupt
    mov rsp, rbx
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
    ; SYS_EXIT reaches this path only after process_exit() has successfully
    ; switched to the kernel CR3 and detached the exiting process. Do not
    ; repeat that switch here: this path has no aligned C-call stack yet.
    ; The bootstrap stack top is 16-byte aligned; CALL supplies the SysV
    ; return-address slot so the C trampoline enters with RSP % 16 == 8.
    mov rsp, stack_top
    call user_mode_exit_trampoline
    ud2

isr32:
    cli
    cld
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
    ; Interrupts may arrive at arbitrary instruction boundaries, so the
    ; interrupted RSP alignment is not a valid C-call alignment guarantee.
    ; RBX is safe as a temporary because its interrupted value is already
    ; saved in the frame and the C ABI preserves RBX across every call.
    mov rbx, rsp
    and rsp, -16
    lea rdi, [rbx + 16]
    call timer_dispatch
    mov rsp, rbx
    and rsp, -16
    call timer_should_preempt
    mov rsp, rbx
    ; Keep the decision across the PIC EOI call (EAX is caller-saved).
    mov r12d, eax

    ; Complete the PIC IRQ before the scheduler can enter its interruptible
    ; idle loop. IF is still clear, so acknowledging early cannot nest an IRQ.
    and rsp, -16
    call pic_eoi_master
    mov rsp, rbx
    test r12d, r12d
    jz .timer_no_switch
    ; CPL3 timer preemption remains explicitly gated off until user-thread
    ; launch and full user-frame scheduling tests are implemented.
    test qword [rbx + 144], 3
    jnz .timer_no_switch
    lea rdi, [rbx + 16]
    ; Frame pointer skips vector/error; CS is 128 bytes into the GPR prefix.
    ; Timer IRQs can therefore preserve either CPL0 or CPL3 return state.
    mov esi, 1
    test qword [rbx + 144], 3
    jz .timer_frame_kind_ready
    mov esi, 2
.timer_frame_kind_ready:
    and rsp, -16
    call thread_schedule_interrupt
    mov rsp, rbx
    test rax, rax
    jz .timer_no_switch

    ; Returning the original IRQ frame means no context switch occurred.
    ; That frame still has vector/error immediately before RAX, so it must
    ; use .timer_no_switch to discard those two words before IRET.
    lea rdx, [rbx + 16]
    cmp rax, rdx
    je .timer_no_switch

    ; A switched thread supplies a saved pros_interrupt_frame whose layout
    ; is RAX..R15, RIP, CS, RFLAGS. It has no synthetic vector/error pair.
    mov rsp, rax
    jmp .timer_restore_switched

.timer_no_switch:
    ; The current IRQ frame starts with exactly two synthetic qwords:
    ; vector and error code. Skip 16 bytes to reach saved RAX, restore all
    ; fifteen GPRs, then IRET from the original RIP/CS/RFLAGS tail.
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
; #CP (21) pushes an architectural error code when CET is enabled.
ISR_WITH_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
; AMD #VC (29) and #SX (30) both push architectural error codes.
ISR_WITH_ERROR 29
ISR_WITH_ERROR 30
ISR_NO_ERROR 31

; Test-only software-INT probe for the vector-8 IST2 stack switch.
; INT 8 does not push a hardware #DF error code, so this stub supplies one.
global isr8_ist_probe
isr8_ist_probe:
    push qword 0
    push qword 8
    jmp isr_common

idt_load:
    lidt [rdi]
    ret

isr_common:
    cli
    cld
    ; Exception entry must preserve the interrupted GPR state just like IRQ0.
    ; The raw vector/error/RIP/CS/RFLAGS frame remains below this saved prefix.
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

    ; Fifteen saved qwords place the original raw exception frame at RBX + 120.
    mov rbx, rsp
    ; Once a production panic owner is published, a nested exception must
    ; fail-stop in assembly before re-entering C or touching diagnostics.
    cmp dword [rel pros_panic_handoff_owner_state], 0
    jne .nested_owner_fail_stop
    and rsp, -16
    lea rdi, [rbx + 120]
    call exception_dispatch
    test rax, rax
    jz .restore_original_exception_frame
    mov rsp, rax
    test rdx, rdx
    jz .restore_normalized_frame
    ; Raw-frame return (RDX != 0) must restore the saved GPR prefix and
    ; discard only vector/error. Do not fall through into nested-owner halt.
    jmp .restore_raw_exception_frame

.nested_owner_fail_stop:
    cli
.nested_owner_halt:
    hlt
    jmp .nested_owner_halt

.restore_raw_exception_frame:
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
    add rsp, 16
    iretq

.restore_original_exception_frame:
    mov rsp, rbx
    jmp .restore_raw_exception_frame

.restore_normalized_frame:
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
