BITS 32

section .text
global _start
extern kernel_main

_start:
    cli

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
    mov eax, pdpt | 0x003
    mov [pml4 + 0], eax

    ; PDPT[0] -> page directory
    mov eax, page_directory | 0x003
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

    ; Pass the original Multiboot2 values to C:
    ; System V x86-64 ABI: RDI, RSI.
    mov edi, [boot_magic]
    mov esi, [boot_info]

    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

section .rodata
align 8
gdt64:
    dq 0
.code equ $ - gdt64
    dq 0x00AF9A000000FFFF   ; 64-bit code
.data equ $ - gdt64
    dq 0x00AF92000000FFFF   ; data
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

boot_magic: resd 1
boot_info:  resd 1
