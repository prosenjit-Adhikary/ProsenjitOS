BITS 32

section .text
global _start
global enter_user_mode
global user_mode_exit_trampoline
global cpu_load_tss
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
    mov rax, rdi
    mov rdx, rsi
    push qword 0x23
    push rdx
    pushfq
    or qword [rsp], 0x200
    push qword 0x1B
    push rax
    iretq

user_mode_exit_trampoline:
    mov rsp, stack_top
    call paging_switch_kernel
    call user_mode_test_complete
.user_halt:
    cli
    hlt
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
    jb .done
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 20)
    jz .done
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 11)
    wrmsr
    mov eax, 1
.done:
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
    ret

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
    ; Write a marker through the isolated user mapping.
    mov rdx, 0x0000004000001000
    mov byte [rdx], 0x5A

    ; Exercise the shared user/kernel ABI and verify the return value.
    mov rax, 1
    int 0x80
    cmp rax, 0x0000000000010000
    jne .user_halt

    ; Ask the kernel to finish the CPL3 test. The kernel validates the
    ; caller privilege and user mapping before returning to its trampoline.
    mov rax, 2
    int 0x80
.user_halt:
    hlt
    jmp .user_halt

section .data
align 8
gdt64:
    dq 0
.code equ $ - gdt64
    dq 0x00AF9A000000FFFF
.data equ $ - gdt64
    dq 0x00AF92000000FFFF
.user_code equ $ - gdt64
    dq 0x00AFFA000000FFFF
.user_data equ $ - gdt64
    dq 0x00AFF2000000FFFF
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
stack_bottom:
    resb 16384
stack_top:

align 16
ist1_stack_bottom:
    resb 8192
ist1_stack_top:

align 16
ist2_stack_bottom:
    resb 8192
ist2_stack_top:

align 16
tss64:
    resb 104
boot_magic: resd 1
boot_info:  resd 1

section .note.GNU-stack noalloc noexec nowrite progbits
