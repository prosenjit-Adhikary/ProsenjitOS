BITS 16
ORG 0x8000

%macro DEBUG_CHAR 1
    push ax
    push dx
    mov dx, 0x00E9
    mov al, %1
    out dx, al
    pop dx
    pop ax
%endmacro

%ifndef KERNEL_LBA
%define KERNEL_LBA 15
%endif
%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 1
%endif

%define E820_BUFFER 0x5000
%define EDD_BUFFER  0x5400
%define MB2_BUFFER  0x6000
; BIOS reads use a bounded 32 KiB bounce buffer. Each chunk is copied to
; the 1 MiB kernel destination in protected mode before the next BIOS read.
; E820 validation covers the entire bounce buffer and the complete destination.
%define LOAD_BUFFER 0x10000
%define LOAD_LIMIT  0x18000
%define LOAD_CHUNK_SECTORS 64
%define KERNEL_LOAD 0x100000
%define NATIVE_MAGIC 0x534F5250
%define STAGE2_MAX_SECTORS 24

start:
    DEBUG_CHAR 'A'
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    ; Gather the firmware E820 memory map before entering protected mode.
    call collect_e820
    jc fatal
    DEBUG_CHAR 'B'
    call validate_load_ranges
    jc fatal

    ; Ask EDD 3.0 for a verifiable ISA/ATA or PCI/ATA device path. Failure
    ; is non-fatal for boot; the kernel will refuse direct ATA storage access.
    call collect_edd_ata_path

    ; Convert E820 entries and the native device descriptor to Multiboot2.
    call build_mb2_info

    ; Enable A20 before protected-mode copies write above the 1 MiB boundary.
    ; Fast A20 (port 0x92) is supported by the QEMU/PC BIOS targets used here.
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    ; Load the flat kernel image from the native disk layout.
    DEBUG_CHAR 'C'
    call load_kernel
    jc fatal
    DEBUG_CHAR 'F'

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
    ; E820 BIOSes may return either the legacy 20-byte record or the
    ; extended 24-byte record. Default the extension to enabled so a
    ; 20-byte response remains usable; a 24-byte response may override it.
    mov dword [di + 20], 1
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    ; Carry means the firmware failed this E820 request. If this happens
    ; after earlier records, the map is incomplete; never accept it as final.
    jc .fail
    cmp eax, 0x534D4150
    jne .fail
    inc bp
    add di, 24
    ; The fixed E820 buffer holds at most 32 records. Never silently
    ; truncate a longer firmware map: doing so could omit reserved ranges
    ; and make the kernel treat incomplete memory information as complete.
    test ebx, ebx
    jz .done
    cmp bp, 32
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

; Refuse to read or copy kernel bytes outside firmware-reported usable RAM.
; Both the low-memory BIOS staging buffer and the 1 MiB destination must be
; covered by an enabled E820 type-1 range before any disk read is attempted.
validate_load_ranges:
    mov eax, LOAD_BUFFER
    mov edx, LOAD_LIMIT
    call validate_usable_range
    jc .fail
    mov eax, KERNEL_LOAD
    mov edx, KERNEL_LOAD + KERNEL_SECTORS * 512
    call validate_usable_range
    ret
.fail:
    stc
    ret

; Input: EAX=start, EDX=end (exclusive), both below 4 GiB.
; Success only when one enabled E820 type-1 entry covers the full range.
validate_usable_range:
    mov [range_start], eax
    mov [range_end], edx
    mov cx, [E820_BUFFER]
    mov si, E820_BUFFER + 4
.next:
    test cx, cx
    jz .fail
    cmp dword [si + 16], 1
    jne .advance
    ; E820 extended attribute bit 0 marks an entry as enabled.
    test dword [si + 20], 1
    jz .advance
    ; This boot path uses 32-bit physical addresses; a nonzero base high
    ; dword cannot cover the requested low-memory range.
    cmp dword [si + 4], 0
    jne .advance
    mov eax, [si]
    cmp eax, [range_start]
    ja .advance
    ; A nonzero length high dword necessarily extends beyond our <4 GiB end.
    mov edx, [si + 12]
    test edx, edx
    jnz .ok
    mov edx, [si + 8]
    add edx, [si]
    jc .ok
    cmp edx, [range_end]
    jae .ok
.advance:
    add si, 24
    dec cx
    jmp .next
.fail:
    stc
    ret
.ok:
    clc
    ret

collect_edd_ata_path:
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov byte [ata_path_valid], 0
    mov word [ata_io_base], 0
    mov word [ata_ctrl_base], 0
    mov byte [ata_drive], 0
    mov byte [ata_host_bus], 0
    mov byte [ata_channel], 0
    mov dword [ata_pci_path], 0
    mov byte [edd_checksum_length], 0

    ; Reserve the full 74-byte EDD 3.0 result (Phoenix=66, T13=74).
    ; Clear it before asking BIOS so truncated/partial results fail closed.
    mov di, EDD_BUFFER
    xor eax, eax
    mov cx, 37
    rep stosw
    mov word [EDD_BUFFER], 74
    mov si, EDD_BUFFER
    mov dl, [boot_drive]
    mov ah, 0x48
    int 0x13
    pushf
    pop ax
    cli
    mov dx, ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    test dl, 1
    jnz .unavailable
    ; EDD 3.0 returns the legacy parameter-buffer length (normally 30),
    ; even when the caller supplied 74 bytes and the BIOS filled the extended
    ; device-path fields. The requested capacity is 74 (set above); validate
    ; the returned legacy minimum here, then validate path length/signature
    ; and checksum below instead of misreading this word as total bytes.
    cmp word [EDD_BUFFER], 26
    jb .unavailable
    cmp word [EDD_BUFFER + 30], 0xBEDD
    jne .unavailable
    cmp byte [EDD_BUFFER + 33], 0
    jne .unavailable
    cmp word [EDD_BUFFER + 34], 0
    jne .unavailable
    cmp byte [EDD_BUFFER + 32], 36
    je .phoenix_path
    cmp byte [EDD_BUFFER + 32], 44
    jne .unavailable
    ; The 74-byte capacity was supplied on entry. The returned size word is
    ; not the extended-path byte count (SeaBIOS/T13 leave it at 30).
    ; T13's second ATA device-path QWORD and reserved byte must be zero.
    cmp dword [EDD_BUFFER + 64], 0
    jne .unavailable
    cmp dword [EDD_BUFFER + 68], 0
    jne .unavailable
    cmp byte [EDD_BUFFER + 72], 0
    jne .unavailable
    mov byte [edd_checksum_length], 44
    jmp .path_format_valid
.phoenix_path:
    cmp word [EDD_BUFFER], 66
    jb .unavailable
    cmp byte [EDD_BUFFER + 64], 0
    jne .unavailable
    mov byte [edd_checksum_length], 36
.path_format_valid:

    ; ATA interface may be on legacy ISA or PCI IDE compatibility mode.
    ; PCI mode is only a candidate here; the kernel verifies the exact PCI
    ; function class, channel programming interface, and I/O decode state.
    cmp dword [EDD_BUFFER + 40], 0x20415441 ; "ATA "
    je .interface_name_space_padded
    cmp dword [EDD_BUFFER + 40], 0x00415441 ; "ATA" + NUL
    jne .unavailable
    cmp dword [EDD_BUFFER + 44], 0
    jne .unavailable
    jmp .interface_name_valid
.interface_name_space_padded:
    cmp dword [EDD_BUFFER + 44], 0
    je .interface_name_valid
    cmp dword [EDD_BUFFER + 44], 0x20202020 ; space-padded "ATA     "
    jne .unavailable
.interface_name_valid:

    cmp dword [EDD_BUFFER + 36], 0x20415349 ; "ISA "
    je .isa_path
    cmp dword [EDD_BUFFER + 36], 0x20494350 ; "PCI "
    je .pci_path
    jmp .unavailable

.isa_path:
    ; ISA interface path: base I/O word + six reserved zero bytes.
    cmp dword [EDD_BUFFER + 50], 0
    jne .unavailable
    cmp word [EDD_BUFFER + 54], 0
    jne .unavailable
    mov ax, [EDD_BUFFER + 48]
    cmp ax, 0x1F0
    je .isa_primary
    cmp ax, 0x170
    je .isa_secondary
    jmp .unavailable
.isa_primary:
    mov byte [ata_channel], 0
    mov word [ata_ctrl_base], 0x3F6
    jmp .isa_path_valid
.isa_secondary:
    mov byte [ata_channel], 1
    mov word [ata_ctrl_base], 0x376
.isa_path_valid:
    mov byte [ata_host_bus], 1
    mov dword [ata_pci_path], 0
    mov [ata_io_base], ax
    jmp .interface_path_valid

.pci_path:
    ; Phoenix EDD does not define the PCI channel byte. Without it the
    ; primary/secondary mapping is ambiguous, so only T13's 44-byte path
    ; may authorize PCI IDE access.
    cmp byte [edd_checksum_length], 44
    jne .unavailable
    ; PCI interface path: bus, device, function, channel, reserved DWORD.
    cmp dword [EDD_BUFFER + 52], 0
    jne .unavailable
    mov al, [EDD_BUFFER + 49]
    cmp al, 31
    ja .unavailable
    mov al, [EDD_BUFFER + 50]
    cmp al, 7
    ja .unavailable
    mov al, [EDD_BUFFER + 51]
    cmp al, 1
    ja .unavailable
    mov [ata_channel], al
    mov byte [ata_host_bus], 2
    movzx eax, byte [EDD_BUFFER + 48]
    movzx ebx, byte [EDD_BUFFER + 49]
    shl ebx, 8
    or eax, ebx
    movzx ebx, byte [EDD_BUFFER + 50]
    shl ebx, 16
    or eax, ebx
    movzx ebx, byte [EDD_BUFFER + 51]
    shl ebx, 24
    or eax, ebx
    mov [ata_pci_path], eax
    cmp byte [ata_channel], 0
    je .pci_primary
    mov word [ata_io_base], 0x170
    mov word [ata_ctrl_base], 0x376
    jmp .interface_path_valid
.pci_primary:
    mov word [ata_io_base], 0x1F0
    mov word [ata_ctrl_base], 0x3F6

.interface_path_valid:

    ; ATA device path: 0=master, 1=slave; all remaining bytes are reserved.
    mov al, [EDD_BUFFER + 56]
    cmp al, 1
    ja .unavailable
    mov [ata_drive], al
    cmp dword [EDD_BUFFER + 57], 0
    jne .unavailable
    cmp word [EDD_BUFFER + 61], 0
    jne .unavailable
    cmp byte [EDD_BUFFER + 63], 0
    jne .unavailable

.checksum_start:
    ; Phoenix checksum covers bytes 30..65; T13 covers bytes 30..73.
    xor ax, ax
    mov si, EDD_BUFFER + 30
    movzx cx, byte [edd_checksum_length]
.checksum:
    add al, [si]
    inc si
    loop .checksum
    test al, al
    jnz .unavailable

    mov byte [ata_path_valid], 1
    clc
    ret
.unavailable:
    ; Never emit partially validated EDD fields with the valid flag clear.
    mov byte [ata_path_valid], 0
    mov word [ata_io_base], 0
    mov word [ata_ctrl_base], 0
    mov byte [ata_drive], 0
    mov byte [ata_host_bus], 0
    mov byte [ata_channel], 0
    mov dword [ata_pci_path], 0
    clc
    ret

build_mb2_info:
    cld
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
    ; Normalize each E820 record into the Multiboot2 mmap layout. The
    ; Multiboot2 reserved dword is always zero; disabled E820 entries are
    ; retained as reserved memory rather than exposed as available RAM.
    mov eax, [si + 20]
    and eax, 1
    push cx
    mov cx, 6
    rep movsd
    pop cx
    test eax, 1
    jnz .entry_enabled
    mov dword [di - 8], 2
.entry_enabled:
    mov dword [di - 4], 0
    dec cx
    jmp .copy_entry

.end_tag:
    ; ProsenjitOS-specific Multiboot2 tag (type 0x1000), size 48:
    ; version=3, descriptor_size=40, BIOS DL, normalized legacy ports,
    ; ATA master/slave, host bus, and packed PCI BDF/channel when applicable.
    ; PCI candidates are verified by the kernel before native ATA access. Flags
    ; never inferred from the BIOS drive number.
    mov eax, 0x1000
    stosd
    mov eax, 48
    stosd
    mov eax, 3
    stosd
    mov eax, 40
    stosd
    mov eax, 1
    stosd
    movzx eax, byte [boot_drive]
    stosd
    mov eax, 1
    cmp byte [ata_path_valid], 0
    je .no_ata_path_flag
    or eax, 2
.no_ata_path_flag:
    stosd
    movzx eax, word [ata_io_base]
    stosd
    movzx eax, word [ata_ctrl_base]
    stosd
    movzx eax, byte [ata_drive]
    stosd
    movzx eax, byte [ata_host_bus]
    stosd
    mov eax, [ata_pci_path]
    stosd

    ; 8-byte aligned Multiboot2 end tag.
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
    xor eax, eax
    mov ax, KERNEL_LBA
    mov [current_lba_low], eax
    mov dword [current_lba_high], 0
    mov word [remaining], KERNEL_SECTORS

    mov word [load_segment], LOAD_BUFFER >> 4
    mov dword [kernel_copied_bytes], 0
.loop:
    mov ax, [remaining]
    cmp ax, LOAD_CHUNK_SECTORS
    jbe .chunk_ok
    mov ax, LOAD_CHUNK_SECTORS
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
    DEBUG_CHAR 'D'

    mov ax, [chunk_sectors]
    sub [remaining], ax
    movzx eax, ax
    add [current_lba_low], eax
    adc dword [current_lba_high], 0

    ; Copy this bounded BIOS-read chunk to its final physical address before
    ; reusing the low-memory bounce buffer for the next chunk.
    call copy_chunk_to_kernel
    jc .fail
    DEBUG_CHAR 'E'
    movzx eax, word [chunk_sectors]
    shl eax, 9
    add [kernel_copied_bytes], eax

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

BITS 16

; Enter protected mode for one bounded copy, then return to real mode so
; BIOS INT 13h remains available for the next disk chunk.
copy_chunk_to_kernel:
    DEBUG_CHAR 'H'
    cli
    lgdt [gdt32.pointer]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp gdt32.code:copy_chunk_pm

BITS 32
copy_chunk_pm:
    mov dx, 0x00E9
    mov al, 'I'
    out dx, al
    mov ax, gdt32.data
    mov ds, ax
    mov es, ax
    ; Preserve the real-mode caller's stack and its return address. The
    ; call into copy_chunk_to_kernel already pushed that address at 0x7BFE;
    ; resetting ESP here would make the real-mode RET jump to garbage.
    mov ss, ax
    mov dx, 0x00E9
    mov al, 'J'
    out dx, al
    cld
    mov esi, LOAD_BUFFER
    mov edi, KERNEL_LOAD
    add edi, [kernel_copied_bytes]
    movzx ecx, word [chunk_sectors]
    shl ecx, 7
    rep movsd
    mov dx, 0x00E9
    mov al, 'K'
    out dx, al

    ; Enter a dedicated 16-bit protected-mode code segment before clearing
    ; CR0.PE. This makes CS.D=0 before the real-mode transition, so the CPU
    ; will decode the real-mode return stub as 16-bit code.
    db 0x66, 0xEA
    dw copy_chunk_pm16
    dw gdt32.code16

BITS 16
copy_chunk_pm16:
    ; Diagnostic markers are emitted before PE is cleared.
    mov dx, 0x00E9
    mov al, 'P'
    out dx, al
    mov dx, 0x00E9
    mov al, 'L'
    out dx, al

    ; In this 16-bit code segment, operand-size prefixes are explicit.
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    ; PE is now clear. The real-mode far jump must be the next instruction.
    db 0xEA
    dw copy_chunk_rm
    dw 0x0000

copy_chunk_rm:
    ; Stack-free marker: distinguishes a bad far-jump target from a stack
    ; problem in the real-mode return path. Do not use DEBUG_CHAR here because
    ; that macro pushes registers before SS has been reloaded.
    mov dx, 0x00E9
    mov al, 'M'
    out dx, al
    xor ax, ax
    ; Reload SS before any stack operation. The protected-mode data selector
    ; is still cached with SS.B=1 after PE is cleared. Loading real-mode SS=0
    ; restores 16-bit SP addressing while preserving the caller's SP value.
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov dx, 0x00E9
    mov al, 'N'
    out dx, al
    sti
    clc
    ret

BITS 32
protected_entry:
    ; Reached only after every kernel chunk has been read and copied.
    mov dx, 0x00E9
    mov al, 'G'
    out dx, al
    mov ax, gdt32.data
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x9C00

    mov eax, NATIVE_MAGIC
    mov ebx, MB2_BUFFER
    ; The flat kernel begins with the 24-byte Multiboot2 header kept at the
    ; start of the image for GRUB compatibility. Native boot must enter the
    ; actual _start symbol immediately after that header.
    jmp KERNEL_LOAD + KERNEL_ENTRY_OFFSET

gdt32:
    dq 0
.code equ $-gdt32
    dq 0x00CF9A000000FFFF
.data equ $-gdt32
    dq 0x00CF92000000FFFF
.code16 equ $-gdt32
    dq 0x00009A000000FFFF
.pointer:
    dw $-gdt32-1
    dd gdt32

BITS 16
boot_drive db 0
ata_path_valid db 0
edd_checksum_length db 0
ata_drive db 0
ata_host_bus db 0
ata_channel db 0
ata_pci_path dd 0
ata_io_base dw 0
ata_ctrl_base dw 0
remaining dw 0
chunk_sectors dw 0
load_segment dw 0
kernel_copied_bytes dd 0
current_lba_low dd 0
current_lba_high dd 0
range_start dd 0
range_end dd 0

align 4
dap:
    db 0x10, 0
dap_count dw 0
dap_offset dw 0
dap_segment dw 0
dap_lba_low dd 0
dap_lba_high dd 0

error_msg db 'ProsenjitOS native bootloader error', 0

times 12288-($-start) db 0
