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

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9C00
    mov [boot_drive], dl

    ; Gather the firmware E820 memory map before entering protected mode.
    call collect_e820
    jc fatal

    ; Convert E820 entries to a Multiboot2-compatible memory-map structure.
    call build_mb2_info

    ; Load the flat kernel image from the native disk layout.
    call load_kernel
    jc fatal

    ; Enable A20 before any protected-mode copy crosses the 1 MiB boundary.
    ; Fast A20 (port 0x92) is supported by the QEMU/PC BIOS targets used here.
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

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
    mov word [e820_calls], 0
.next:
    ; ACPI 3.0 E820 callers must set the extended-attribute input bit.
    ; Keep the returned memory map entry densely packed at ES:DI.
    mov dword [es:di + 20], 1
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    ; Preserve state the BIOS interface does not return (notably BP/DI and
    ; segment registers), while keeping EAX/EBX/ECX as the E820 outputs.
    push ds
    push es
    push di
    push bp
    int 0x15
    pop bp
    pop di
    pop es
    pop ds
    jc .fail
    cmp eax, 0x534D4150
    jne .fail
    cmp ecx, 20
    jb .fail
    ; Bound BIOS responses, not just enabled records: disabled E820 ranges
    ; do not advance DI/BP but still consume the BIOS continuation token.
    inc word [e820_calls]
    cmp ecx, 24
    jb .accept_entry
    ; A 24-byte E820 response supplies the ACPI enabled bit. Ignore disabled
    ; ranges; a 20-byte legacy response has no attributes and is accepted.
    test dword [es:di + 20], 1
    jz .next_entry
.accept_entry:
    ; Multiboot2 uses the final dword as reserved, not E820 attributes.
    mov dword [es:di + 20], 0
    inc bp
    add di, 24
.next_entry:
    ; A continuing E820 map beyond the 32-entry buffer is unsafe to truncate:
    ; fail closed rather than accidentally treating an omitted reserved range
    ; as free RAM in the kernel allocator.
    test ebx, ebx
    jz .done
    cmp word [e820_calls], 32
    jae .fail
    jmp .next
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
    ; The BIOS may lower the DAP count on a partial transfer even if carry
    ; was not set. Do not copy/execute a truncated kernel image.
    mov ax, [chunk_sectors]
    cmp word [dap_count], ax
    jne .fail

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
    mov esp, 0x9C00

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
e820_calls dw 0
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
