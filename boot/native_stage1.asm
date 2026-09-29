BITS 16
ORG 0x7C00

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 32
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc edd_error
    cmp bx, 0xAA55
    jne edd_error
    test cx, 1
    jz edd_error

    mov si, dap
    mov word [dap_count], STAGE2_SECTORS
    mov word [dap_offset], 0x8000
    mov word [dap_segment], 0x0000
    mov dword [dap_lba_low], 1
    mov dword [dap_lba_high], 0
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    jmp 0x0000:0x8000

edd_error:
    mov si, edd_error_msg
    jmp print_error

disk_error:
    mov si, error_msg
    jmp print_error

print_error:
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

boot_drive db 0
error_msg db 'ProsenjitOS bootloader disk read error', 0
edd_error_msg db 'ProsenjitOS BIOS EDD unsupported', 0

align 4
dap:
    db 0x10, 0
dap_count dw 0
dap_offset dw 0
dap_segment dw 0
dap_lba_low dd 0
dap_lba_high dd 0

%ifndef PFS_START_LBA
%define PFS_START_LBA 2048
%endif
%ifndef PFS_SECTORS
%define PFS_SECTORS 512
%endif

; Conventional MBR partition entry for the reserved ProsenjitFS volume.
times 446-($-$$) db 0
db 0x80
db 0x00, 0x02, 0x00
db 0xDA
db 0x00, 0x02, 0x00
dd PFS_START_LBA
dd PFS_SECTORS
times 510-($-$$) db 0
dw 0xAA55
