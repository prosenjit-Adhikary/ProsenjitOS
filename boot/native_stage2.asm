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
%define LOAD_LIMIT  0xD0000
%define KERNEL_LOAD 0x100000
%define NATIVE_MAGIC 0x534F5250
%define STAGE2_MAX_SECTORS 32

; The BIOS staging buffer must never exceed its explicitly reserved window.
%if KERNEL_SECTORS < 1
    %error "KERNEL_SECTORS must be at least 1"
%endif
%if KERNEL_SECTORS > ((LOAD_LIMIT - LOAD_BUFFER) / 512)
    %error "KERNEL_SECTORS exceeds native staging buffer"
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; Keep the real-mode stack above the 16 KiB stage-2 load area (0x8000-0xC000).\n    mov sp, 0xF0000
    mov [boot_drive], dl

    ; Gather the firmware E820 memory map before entering protected mode.
    call collect_e820
    jc fatal

    ; Convert E820 entries to a Multiboot2-compatible memory-map structure.
    call build_mb2_info

    ; Load the flat kernel image from the native disk layout.
    call load_kernel
    jc fatal

    ; Enter 32-bit protected mode. The kernel's own entry point will then
    ; build its final x86-64 page tables and enter long mode.
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
    ; total_size at +0, reserved at +4.
    mov di, MB2_BUFFER
    xor eax, eax
    stosd
    stosd

    ; mmap tag header: type=6, size=16+n*24, entry_size=24, version=0.
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
    ; 8-byte aligned end tag.
    mov ax, 0
    stosw
    stosw
    mov eax, 8
    stosd

    ; total_size = current offset - MB2_BUFFER.
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

    mov eax, KERNEL_LBA
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

    ; 64 sectors = 32 KiB = 0x800 paragraphs.
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
    ; Keep the protected-mode stack outside the stage-2 image as well.\n    mov esp, 0xF0000

    ; Copy the BIOS-loaded flat kernel from 0x10000 to its linked address 1 MiB.
    cld
    mov esi, LOAD_BUFFER
    mov edi, KERNEL_LOAD
    mov ecx, KERNEL_SECTORS * 128
    rep movsd

    mov eax, NATIVE_MAGIC
    mov ebx, MB2_BUFFER
    ; The flat kernel begins with the 24-byte Multiboot2 header kept at the
    ; start of the image for GRUB compatibility. Native boot must enter the
    ; actual _start symbol immediately after that header.
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
