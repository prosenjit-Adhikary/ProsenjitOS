BITS 32

section .text
global _start
global enter_user_mode
global user_mode_exit_trampoline
global cpu_load_tss
global stack_top
extern kernel_main
extern cpu_security_init
extern user_mode_test_complete
extern paging_switch_kernel

_start:
    cli
    cld

    ; GRUB supplies:
    ; EAX = Multiboot2 boot magic
    ; EBX = Multiboot2 information structure address
    mov [boot_magic], eax
    mov [boot_info], ebx

    ; Temporary bootstrap stack.
    mov esp, stack_top

    ; Build a minimal identity-mapped 64-bit page table:
    ; 0..1 GiB using 2 MiB pages.
    mov edi, page_tables
    xor eax, eax
    mov ecx, (4096 * 5) / 4
    rep stosd

    ; PML4[0] -> PDPT
    mov eax, pdpt
    or eax, 0x003
    mov [pml4 + 0], eax

    ; PDPT[0] -> page directory
    mov eax, page_directory
    or eax, 0x003
    mov [pdpt + 0], eax

    ; Fill 512 PDEs, each mapping 2 MiB.
    mov edi, page_directory
    mov eax, 0x00000083       ; present + writable + 2 MiB page
    mov ecx, 512
.fill_pd:
    mov [edi], eax
    add eax, 0x00200000
    add edi, 8
    loop .fill_pd

    ; Enable PAE.
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Load PML4.
    mov eax, pml4
    mov cr3, eax

    ; Enable long mode in EFER.
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)          ; LME
    wrmsr

    ; Enable paging.
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; Enter 64-bit code segment.
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_entry

BITS 64
long_mode_entry:
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov rsp, stack_top

    ; Enable CPU memory-protection features that are safe at this stage.
    call cpu_security_init
    test eax, eax
    jz cpu_security_init.security_halt

    ; Pass the original Multiboot2 values to C:
    ; System V x86-64 ABI: RDI, RSI.
    mov edi, [boot_magic]
    mov esi, [boot_info]

    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

enter_user_mode:
    ; RDI=entry, RSI=stack_top, RDX=initial capability handle.
    ; Load CPL3-compatible data segments before IRET so the first user
    ; memory access does not inherit the CPL0 data-segment cache.
    mov ax, gdt64.user_data + 3
    mov ds, ax
    mov es, ax
    mov rax, rdi
    mov r8, rdx
    mov rdx, rsi
    push qword 0x23
    push rdx
    pushfq
    ; User-mode timer preemption is not yet scheduler-safe. Keep IF clear
    ; until user interrupt-frame/address-space switching is implemented.
    and qword [rsp], ~0x200
    push qword 0x1B
    push rax
    mov rdi, r8
    iretq

user_mode_exit_trampoline:
    ; This entry is reached only after CPL3 has already transitioned to
    ; CPL0 through INT 0x80 or an exception path. Restore the canonical
    ; kernel CR3/stack before touching kernel-only state.
    mov rsp, stack_top
    call paging_switch_kernel
    call user_mode_test_complete
.user_halt:
    jmp .user_halt

cpu_security_init:
    xor eax, eax

    ; Enable CR0.WP so supervisor writes obey read-only page permissions.
    mov rax, cr0
    or rax, (1 << 16)
    mov cr0, rax

    ; Enable EFER.NXE when the CPU advertises NX support.
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_nx
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 20)
    jz .no_nx
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 11)
    wrmsr
    ; Load a long-mode TSS with RSP0 pointing at the kernel bootstrap stack.
    mov rax, tss64
    mov word [gdt64 + gdt64.tss_low + 0], 103
    mov word [gdt64 + gdt64.tss_low + 2], ax
    mov rdx, rax
    shr rdx, 16
    mov word [gdt64 + gdt64.tss_low + 4], dx
    mov byte [gdt64 + gdt64.tss_low + 5], 0x89
    mov byte [gdt64 + gdt64.tss_low + 6], 0x00
    mov rdx, rax
    shr rdx, 32
    mov byte [gdt64 + gdt64.tss_low + 7], dl
    mov dword [gdt64 + gdt64.tss_low + 8], edx
    mov dword [gdt64 + gdt64.tss_high], 0
    mov rax, stack_top
    mov [tss64 + 4], rax
    ; Dedicated IST stacks make double-fault/page-fault diagnostics independent
    ; of the current task stack.
    mov rax, ist1_stack_top
    mov [tss64 + 36], rax
    mov rax, ist2_stack_top
    mov [tss64 + 44], rax
    ; Disable the TSS I/O-permission bitmap explicitly.
    mov word [tss64 + 102], 104
    ; TSS descriptor is prepared here; LTR is performed after the IDT is live.
    mov eax, 1
    ret

.no_nx:
    ; The paging layer relies on NX bits for W^X. Do not continue booting
    ; without CPU NX support because setting XD would be a reserved-bit fault.
    xor eax, eax
    ret

.security_halt:
    cli
.halt_security:
    hlt
    jmp .halt_security

cpu_load_tss:
    ; Load the GDT again immediately before LTR so GDTR is known-good.
    lgdt [gdt64.pointer]
    mov ax, gdt64.tss_low
    ltr ax
    ret

section .user_text
align 16
global user_test_entry
user_test_entry:
    ; Preserve the initial IPC capability before exercising syscall arguments.
    mov r12, rdi

    ; Write a marker through the isolated user mapping.
    mov rdx, 0x0000008000001000
    mov r14, rdx
    mov byte [rdx], 0x5A

    ; Exercise the shared user/kernel ABI and verify the return value.
    mov rax, 1
    int 0x80
    cmp rax, 0x0000000000010000
    jne .user_halt

    ; Input polling must reject a null user buffer before touching the device.
    mov rax, 8
    xor rsi, rsi
    int 0x80
    mov rbx, 0xFFFFFFFFFFFFFFF6 ; PROSENJIT_SYSCALL_EBADF
    cmp rax, rbx
    jne .user_halt

    ; A valid user buffer may return 0 (no event) or 1 (event copied).
    ; This exercises the CPL3 -> kernel input syscall path without assuming
    ; that QEMU has a key queued at the instant of the test.
    mov rax, 8
    lea rsi, [r14 + 64]
    int 0x80
    test rax, rax
    jz .input_poll_ok
    cmp rax, 1
    jne .user_halt
.input_poll_ok:

    ; Unknown syscall numbers must fail closed with ENOSYS.
    mov rax, 0x7FFF
    int 0x80
    cmp rax, -38                  ; PROSENJIT_SYSCALL_ENOSYS
    jne .user_halt

    ; Zero requested rights must never manufacture a capability.
    mov rax, 4
    mov rdi, r12
    xor rsi, rsi
    int 0x80
    cmp rax, -13                  ; PROSENJIT_SYSCALL_EPERM
    jne .user_halt

    ; Invalid/unknown capability rights must be rejected before authorization.
    mov rax, 4
    mov rdi, r12
    mov rsi, 0x8000000000000000
    int 0x80
    cmp rax, 0xFFFFFFFFFFFFFFF3 ; PROSENJIT_SYSCALL_EPERM
    jne .user_halt

    ; A zero handle is never a valid capability.
    mov rax, 4
    xor rdi, rdi
    mov rsi, 1
    int 0x80
    cmp rax, 0xFFFFFFFFFFFFFFF3 ; PROSENJIT_SYSCALL_EPERM
    jne .user_halt

    ; Capability duplication must attenuate authority:
    ; READ succeeds, while EXECUTE must be rejected because the source
    ; handle does not carry EXECUTE. Do not request ADMIN here: a denied
    ; ADMIN authorization is intentionally a quarantine decision, which
    ; would terminate the limited test process before the remaining IPC
    ; ABI checks can run.
    mov rax, 4
    mov rdi, r12
    mov rsi, 1                  ; PROSENJIT_CAP_READ
    int 0x80
    ; Handles are bounded generation/slot values. Any non-zero upper
    ; 32 bits means the syscall returned an ABI error, not a handle.
    ; Check the upper 32 bits separately because TEST has no imm64 form.
    mov rcx, rax
    shr rcx, 32
    test ecx, ecx
    jnz .user_halt
    test rax, rax
    jz .user_halt

    mov r13, rax
    mov rax, 4
    mov rdi, r12
    mov rsi, 4                  ; PROSENJIT_CAP_EXECUTE
    int 0x80
    cmp rax, 0xFFFFFFFFFFFFFFF3 ; PROSENJIT_SYSCALL_EPERM
    jne .user_halt

    ; Send a short message through the capability-mediated IPC syscall.
    mov byte [r14 + 1], 'I'
    mov byte [r14 + 2], 'P'
    mov byte [r14 + 3], 'C'
    mov rax, 5
    mov rdi, r12
    lea rsi, [r14 + 1]
    mov rdx, 3
    int 0x80
    test rax, rax
    jnz .user_halt

    ; Receive it back and have the kernel write the message length to user memory.
    mov rax, 6
    mov rdi, r12
    lea rsi, [r14 + 32]
    mov rdx, 64
    lea rcx, [r14 + 16]
    int 0x80
    cmp rax, 3
    jne .user_halt
    cmp dword [r14 + 16], 3
    jne .user_halt
    cmp byte [r14 + 32], 'I'
    jne .user_halt
    cmp byte [r14 + 33], 'P'
    jne .user_halt
    cmp byte [r14 + 34], 'C'
    jne .user_halt

    ; Close the duplicated READ-only capability.
    mov rdi, r13
    mov rax, 3
    int 0x80
    test rax, rax
    jnz .user_halt

    ; Close the original READ|DUP capability.
    mov rdi, r12
    mov rax, 3
    int 0x80
    test rax, rax
    jnz .user_halt

    ; The original handle is now stale. Exercise both syscall failure
    ; paths explicitly so an invalid handle can never be reused as a live one.
    mov rax, 4
    mov rdi, r12
    mov rsi, 1                  ; PROSENJIT_CAP_READ
    int 0x80
    cmp rax, 0xFFFFFFFFFFFFFFF3 ; PROSENJIT_SYSCALL_EPERM
    jne .user_halt

    mov rdi, r12
    mov rax, 3
    int 0x80
    mov rbx, 0xFFFFFFFFFFFFFFF6
    cmp rax, rbx
    jne .user_halt

    ; Ask the kernel to finish the CPL3 test. The kernel validates the
    ; caller privilege and user mapping before returning to its trampoline.
    mov rax, 2
    int 0x80
.user_halt:
    jmp .user_halt

section .data
align 8
gdt64:
    dq 0
.code equ $ - gdt64
    dq 0x00AF9A000000FFFF
.data equ $ - gdt64
    dq 0x00CF92000000FFFF
.user_code equ $ - gdt64
    dq 0x00AFFA000000FFFF
.user_data equ $ - gdt64
    dq 0x00CFF2000000FFFF
.tss_low equ $ - gdt64
    dq 0
.tss_high equ $ - gdt64
    dq 0
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .bss
align 4096
page_tables:
pml4:           resb 4096
pdpt:           resb 4096
page_directory: resb 4096
                resb 4096
                resb 4096

align 16
global stack_bottom
global stack_top
stack_bottom:
    ; kernel_main and NTFS mutation tests can nest a large test frame with
    ; a 64 KiB MFT-record scratch buffer. Keep enough headroom for compiler
    ; stack probing so the bootstrap stack cannot underflow into unmapped RAM.
    resb 131072
stack_top:

align 16
global ist1_stack_bottom
global ist1_stack_top
ist1_stack_bottom:
    resb 8192
ist1_stack_top:

align 16
global ist2_stack_bottom
global ist2_stack_top
ist2_stack_bottom:
    resb 8192
ist2_stack_top:

align 16
tss64:
    resb 104
boot_magic: resd 1
boot_info:  resd 1

section .note.GNU-stack noalloc noexec nowrite progbits
