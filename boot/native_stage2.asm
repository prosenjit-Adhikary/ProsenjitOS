BITS 16
ORG 0x8000

%ifndef KERNEL_LBA
%define KERNEL_LBA 33
%endif
%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 1
%endif

%define E820_BUFFER 0x7000
%define MB2_BUFFER  0x6000
%define LOAD_BUFFER 0x10000
%define KERNEL_LOAD 0x100000
%define NATIVE_MAGIC 0x534F5250

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9C00
    mov [boot_drive], dl

    call collect_e820
    jc fatal
    call build_mb2_info
    call load_kernel
    jc fatal

    cli
    lgdt [gdt32.pointer]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp gdt32.code:protected_entry

collect_e820:
    xor ebx, ebx
    mov di, E820_BUFFER + 4
    xor bp, bp
.next:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .fail
    inc bp
    add di, 24
    cmp bp, 32
    jae .done
    test ebx, ebx
    jnz .next
.done:
    mov [E820_BUFFER], bp
    cmp bp, 0
    je .fail
    clc
    ret
.fail:
    stc
    ret

build_mb2_info:
    mov di, MB2_BUFFER
    xor eax, eax
    stosd
    stosd
    mov eax, 6
    stosd
    mov ax, 16
    mov cx, [E820_BUFFER]
    imul cx, 24
    add ax, cx
    movzx eax, ax
    stosd
    mov eax, 24
    stosd
    xor eax, eax
    stosd
    mov si, E820_BUFFER + 4
    mov cx, [E820_BUFFER]
.copy_entry:
    cmp cx, 0
    je .end_tag
    push cx
    mov cx, 6
    rep movsd
    pop cx
    dec cx
    jmp .copy_entry
.end_tag:
    xor eax, eax
    stosd
    stosd
    mov eax, 8
    stosd
    mov ax, di
    sub ax, MB2_BUFFER
    mov [MB2_BUFFER], ax
    mov word [MB2_BUFFER + 2], 0
    ret

load_kernel:
    mov eax, KERNEL_SECTORS
    cmp eax, 0
    je .fail
    cmp eax, 1536
    ja .fail
    xor eax, eax
    mov ax, KERNEL_LBA
    mov [current_lba_low], eax
    mov dword [current_lba_high], 0
    mov word [remaining], KERNEL_SECTORS
    mov word [load_segment], LOAD_BUFFER >> 4
.loop:
    mov ax, [remaining]
    cmp ax, 64
    jbe .chunk_ok
    mov ax, 64
.chunk_ok:
    mov [chunk_sectors], ax
    mov si, dap
    mov [dap_count], ax
    mov word [dap_offset], 0
    mov ax, [load_segment]
    mov [dap_segment], ax
    mov eax, [current_lba_low]
    mov [dap_lba_low], eax
    mov eax, [current_lba_high]
    mov [dap_lba_high], eax
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .fail
    mov ax, [chunk_sectors]
    sub [remaining], ax
    movzx eax, ax
    add [current_lba_low], eax
    adc dword [current_lba_high], 0
    shl ax, 5
    add [load_segment], ax
    cmp word [remaining], 0
    jne .loop
    clc
    ret
.fail:
    stc
    ret

fatal:
    mov si, error_msg
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

BITS 32
protected_entry:
    mov ax, gdt32.data
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x9C00
    cld
    mov esi, LOAD_BUFFER
    mov edi, KERNEL_LOAD
    mov ecx, KERNEL_SECTORS * 128
    rep movsd
    mov eax, NATIVE_MAGIC
    mov ebx, MB2_BUFFER
    jmp KERNEL_LOAD + 24

gdt32:
    dq 0
.code equ $-gdt32
    dq 0x00CF9A000000FFFF
.data equ $-gdt32
    dq 0x00CF92000000FFFF
.pointer:
    dw $-gdt32-1
    dd gdt32

BITS 16
boot_drive db 0
remaining dw 0
chunk_sectors dw 0
load_segment dw 0
current_lba_low dd 0
current_lba_high dd 0
align 4
dap:
    db 0x10, 0
dap_count dw 0
dap_offset dw 0
dap_segment dw 0
dap_lba_low dd 0
dap_lba_high dd 0
error_msg db 'ProsenjitOS native bootloader error', 0

times 16384-($-$$) db 0
