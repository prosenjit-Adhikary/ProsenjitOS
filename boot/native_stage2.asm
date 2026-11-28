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
%define BOOT_V1_BUFFER 0x6000
%define BOOT_V1_SIZE   944
%define BOOT_V1_END    (BOOT_V1_BUFFER + BOOT_V1_SIZE)
%define BOOT_V1_MAGIC  0x31534F50
; BIOS reads use a bounded 32 KiB bounce buffer. Each chunk is copied to
; the 1 MiB kernel destination in protected mode before the next BIOS read.
; E820 validation covers the entire bounce buffer and the complete destination.
%define LOAD_BUFFER 0x10000
%define LOAD_LIMIT  0x18000
%define LOAD_CHUNK_SECTORS 64
%define KERNEL_LOAD 0x100000
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

    ; Exercise the bounded response validator with malformed and legacy inputs
    ; before trusting firmware-provided E820 records.
    call e820_response_selftest
    jc fatal

    ; Gather the firmware E820 memory map before entering protected mode.
    call collect_e820
    jc fatal
    DEBUG_CHAR 'B'
    call validate_load_ranges
    jc fatal

    ; Build the fixed Boot Protocol v1 handoff. The allocation is fixed-size
    ; so its complete physical extent can be reserved in the E820-derived map.
    call build_boot_v1_info
    jc fatal

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
    ; The 32-entry staging buffer includes a complete 24-byte slot per entry.
    ; Reset the optional extension before each call so a 20-byte response is
    ; deterministic even if firmware leaves the rest of the slot untouched.
    mov dword [di + 20], 1
    mov [e820_input_token], ebx
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    ; On carry, ignore all returned registers and buffer bytes. ACPI permits
    ; the final record to have been returned on the previous iteration.
    mov [e820_returned_size], ecx
    jc .carry
    cmp eax, 0x534D4150
    jne .fail
    mov ecx, [e820_returned_size]
    call e820_validate_response
    jc .fail
    ; A successful extra response is an actual 33rd record and cannot fit.
    ; The one-record probe uses separate scratch so even malformed firmware
    ; cannot write beyond the fixed E820 staging buffer.
    cmp bp, 32
    jae .fail
    inc bp
    add di, 24
    test ebx, ebx
    jz .done
    cmp bp, 32
    jae .probe_terminal
    jmp .next
.probe_terminal:
    mov di, 0x5400
    jmp .next
.carry:
    call e820_terminal_carry_check
    jc .fail
.done:
    mov [E820_BUFFER], bp
    cmp bp, 0
    je .fail
    clc
    ret
.fail:
    stc
    ret

; Input: ECX=BIOS returned record size, DS:DI=24-byte record slot.
; ECX 20..23 is treated as the legacy 20-byte prefix; any partial extension
; bytes are discarded. A complete 24-byte extension must satisfy the ACPI 6.6
; defined attribute bits: bit 0 set and bits 2:1 clear.
e820_validate_response:
    cmp ecx, 20
    jb .invalid
    cmp ecx, 24
    ja .invalid
    cmp cx, 24
    je .extended
    mov dword [di + 20], 1
    clc
    ret
.extended:
    test dword [di + 20], 1
    jz .invalid
    test dword [di + 20], 6
    jnz .invalid
    ; Bit 3 is the defined hardware-error-log attribute. Bits 31:4 are
    ; reserved and rejected so unknown attributes never become allocatable.
    test dword [di + 20], 0xFFFFFFF0
    jnz .invalid
    clc
    ret
.invalid:
    stc
    ret

; Return CF=0 only for a conventional usable-RAM record. A type-1 range
; carrying AddressRangeErrorLog (extended-attribute bit 3) is reserved for
; hardware error logging and must never be used for the kernel or PMM.
e820_record_usable:
    cmp dword [si + 16], 1
    jne .not_usable
    test dword [si + 20], 8
    jnz .not_usable
    clc
    ret
.not_usable:
    stc
    ret

; A carry is a valid terminal response only when at least one earlier record
; was accepted and this call was issued with a nonzero continuation token.
e820_terminal_carry_check:
    test bp, bp
    jz .invalid
    cmp dword [e820_input_token], 0
    je .invalid
    clc
    ret
.invalid:
    stc
    ret

; Boot-time regression assertions exercise the same validator and carry gate
; used by collect_e820. Scratch at 0x5400 lies outside the 32-entry map buffer.
e820_response_selftest:
    push ax
    push bp
    push cx
    push di
    push si
    mov di, 0x5400

    ; The mandatory 20-byte prefix is the minimum accepted size.
    xor ecx, ecx
    call e820_validate_response
    jnc .fail
    mov ecx, 19
    call e820_validate_response
    jnc .fail
    mov ecx, 20
    mov dword [di + 20], 0xA5A5A5A5
    call e820_validate_response
    jc .fail
    cmp dword [di + 20], 1
    jne .fail

    ; Partial extensions must be ignored and overwritten with the legacy
    ; default rather than leaking partially written attribute bytes.
    mov ecx, 21
    mov dword [di + 20], 0xA5A5A5A5
    call e820_validate_response
    jc .fail
    cmp dword [di + 20], 1
    jne .fail
    mov ecx, 22
    mov dword [di + 20], 0xA5A5A5A5
    call e820_validate_response
    jc .fail
    cmp dword [di + 20], 1
    jne .fail
    mov ecx, 23
    mov dword [di + 20], 0xA5A5A5A5
    call e820_validate_response
    jc .fail
    cmp dword [di + 20], 1
    jne .fail

    ; A complete extension accepts bit 0 alone and rejects missing bit 0,
    ; reserved bits 1:2, and responses larger than the supplied slot.
    mov ecx, 24
    mov dword [di + 20], 1
    call e820_validate_response
    jc .fail
    mov dword [di + 20], 0
    call e820_validate_response
    jnc .fail
    mov dword [di + 20], 2
    call e820_validate_response
    jnc .fail
    mov dword [di + 20], 4
    call e820_validate_response
    jnc .fail

    ; AddressRangeErrorLog is a valid descriptor attribute, but the range is
    ; reserved even when the E820 type field says type 1.
    mov dword [di + 20], 9
    call e820_validate_response
    jc .fail
    mov dword [di + 16], 1
    mov si, di
    call e820_record_usable
    jnc .fail
    mov dword [di + 20], 1
    call e820_record_usable
    jc .fail
    mov dword [di + 16], 2
    call e820_record_usable
    jnc .fail

    ; Unknown reserved attribute bits are rejected rather than propagated.
    mov dword [di + 16], 1
    mov dword [di + 20], 0x11
    call e820_validate_response
    jnc .fail
    mov dword [di + 20], 0x80000001
    call e820_validate_response
    jnc .fail
    mov ecx, 25
    call e820_validate_response
    jnc .fail
    mov ecx, 0x10000014
    call e820_validate_response
    jnc .fail

    ; CF is accepted only after an accepted record and nonzero input token.
    mov bp, 1
    mov dword [e820_input_token], 1
    call e820_terminal_carry_check
    jc .fail
    mov dword [e820_input_token], 0
    call e820_terminal_carry_check
    jnc .fail
    mov bp, 0
    mov dword [e820_input_token], 1
    call e820_terminal_carry_check
    jnc .fail

    clc
    jmp .return
.fail:
    stc
.return:
    pop si
    pop di
    pop cx
    pop bp
    pop ax
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
; Success only when one E820 type-1 entry covers the full range.
validate_usable_range:
    mov [range_start], eax
    mov [range_end], edx
    mov cx, [E820_BUFFER]
    mov si, E820_BUFFER + 4
.next:
    test cx, cx
    jz .fail
    call e820_record_usable
    jc .advance
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

; Build a native ProsenjitOS Boot Protocol v1 block at 0x6000.
; E820 records are required to be sorted and non-overlapping. The output map
; preserves every record, normalizes disabled/non-usable records to reserved,
; and splits the usable record containing the handoff into usable/reserved/
; usable pieces. This ensures the parser can prove the whole handoff allocation
; is firmware-reserved before the PMM accepts the map.
build_boot_v1_info:
    ; Clear the complete fixed-size allocation (128-byte header + room for
    ; 34 map records). The actual map count may be smaller.
    mov di, BOOT_V1_BUFFER
    xor eax, eax
    mov cx, BOOT_V1_SIZE / 4
    rep stosd

    mov dword [BOOT_V1_BUFFER + 0], BOOT_V1_MAGIC
    mov word  [BOOT_V1_BUFFER + 4], 1
    mov word  [BOOT_V1_BUFFER + 6], 128
    mov dword [BOOT_V1_BUFFER + 8], BOOT_V1_SIZE
    mov dword [BOOT_V1_BUFFER + 12], 0
    mov dword [BOOT_V1_BUFFER + 16], BOOT_V1_BUFFER
    mov dword [BOOT_V1_BUFFER + 20], 0
    mov dword [BOOT_V1_BUFFER + 24], 128
    mov dword [BOOT_V1_BUFFER + 32], 24
    ; No modules, framebuffer, ACPI copy, or boot-device blob are advertised.

    mov si, E820_BUFFER + 4
    mov cx, [E820_BUFFER]
    mov di, BOOT_V1_BUFFER + 128
    xor bp, bp
    mov dword [previous_end_low], 0
    mov dword [previous_end_high], 0
    mov byte [handoff_covered], 0

.next_record:
    test cx, cx
    jz .finished

    ; Cache the current 64-bit base, length, and exclusive end.
    mov eax, [si + 0]
    mov [map_base_low], eax
    mov eax, [si + 4]
    mov [map_base_high], eax
    mov eax, [si + 8]
    mov [map_length_low], eax
    mov eax, [si + 12]
    mov [map_length_high], eax
    mov eax, [map_length_low]
    or eax, [map_length_high]
    jz .fail

    ; Require monotonically ordered, non-overlapping E820 ranges.
    mov eax, [map_base_high]
    cmp eax, [previous_end_high]
    jb .fail
    ja .ordered
    mov eax, [map_base_low]
    cmp eax, [previous_end_low]
    jb .fail
.ordered:
    mov eax, [map_base_low]
    mov edx, [map_base_high]
    add eax, [map_length_low]
    adc edx, [map_length_high]
    jc .fail
    mov [map_end_low], eax
    mov [map_end_high], edx
    mov [previous_end_low], eax
    mov [previous_end_high], edx

    ; Treat non-type-1 entries and hardware-error-log ranges as reserved.
    mov dword [map_type], 2
    call e820_record_usable
    jc .class_ready
    mov dword [map_type], 1
.class_ready:

    ; Determine whether this record intersects the handoff allocation.
    ; A partial intersection is malformed and is rejected rather than
    ; accidentally advertising only part of the block as reserved.
    mov eax, [map_base_high]
    test eax, eax
    jnz .emit_whole
    mov eax, [map_base_low]
    cmp eax, BOOT_V1_END
    jae .emit_whole
    mov eax, [map_end_high]
    test eax, eax
    jnz .overlap
    mov eax, [map_end_low]
    cmp eax, BOOT_V1_BUFFER
    jbe .emit_whole
.overlap:
    ; This record intersects [BOOT_V1_BUFFER, BOOT_V1_END). It must contain
    ; the complete allocation, and exactly one record may do so.
    mov eax, [map_base_high]
    test eax, eax
    jnz .fail
    mov eax, [map_base_low]
    cmp eax, BOOT_V1_BUFFER
    ja .fail
    mov eax, [map_end_high]
    test eax, eax
    jnz .contains
    mov eax, [map_end_low]
    cmp eax, BOOT_V1_END
    jb .fail
.contains:
    cmp byte [handoff_covered], 0
    jne .fail
    mov byte [handoff_covered], 1

    ; Reserved source records can be copied as-is. A usable source record is
    ; split so the complete handoff bytes become an explicit reserved range.
    cmp dword [map_type], 1
    jne .emit_whole

    ; Optional usable prefix: [record.base, BOOT_V1_BUFFER).
    mov eax, [map_base_low]
    cmp eax, BOOT_V1_BUFFER
    jae .emit_handoff
    mov [emit_base_low], eax
    mov eax, [map_base_high]
    mov [emit_base_high], eax
    mov eax, BOOT_V1_BUFFER
    sub eax, [map_base_low]
    mov [emit_length_low], eax
    mov dword [emit_length_high], 0
    mov dword [emit_type], 1
    call .emit_map_entry

.emit_handoff:
    mov dword [emit_base_low], BOOT_V1_BUFFER
    mov dword [emit_base_high], 0
    mov dword [emit_length_low], BOOT_V1_SIZE
    mov dword [emit_length_high], 0
    mov dword [emit_type], 2
    call .emit_map_entry

    ; Optional usable suffix: [BOOT_V1_END, record.end).
    mov eax, [map_end_high]
    test eax, eax
    jnz .suffix_present
    mov eax, [map_end_low]
    cmp eax, BOOT_V1_END
    jbe .record_done
.suffix_present:
    mov dword [emit_base_low], BOOT_V1_END
    mov dword [emit_base_high], 0
    mov eax, [map_end_low]
    sub eax, BOOT_V1_END
    mov [emit_length_low], eax
    mov eax, [map_end_high]
    sbb eax, 0
    mov [emit_length_high], eax
    mov dword [emit_type], 1
    call .emit_map_entry
    jmp .record_done

.emit_whole:
    mov eax, [map_base_low]
    mov [emit_base_low], eax
    mov eax, [map_base_high]
    mov [emit_base_high], eax
    mov eax, [map_length_low]
    mov [emit_length_low], eax
    mov eax, [map_length_high]
    mov [emit_length_high], eax
    mov eax, [map_type]
    mov [emit_type], eax
    call .emit_map_entry

.record_done:
    add si, 24
    dec cx
    jmp .next_record

.finished:
    cmp byte [handoff_covered], 1
    jne .fail
    test bp, bp
    jz .fail
    movzx eax, bp
    mov [BOOT_V1_BUFFER + 28], eax
    clc
    ret
.fail:
    stc
    ret

; Append one 24-byte wire memory-map record at ES:DI. BP is the bounded
; output count; the maximum is the 32 firmware records plus two split pieces.
.emit_map_entry:
    cmp bp, 34
    jae .emit_fail
    mov eax, [emit_base_low]
    stosd
    mov eax, [emit_base_high]
    stosd
    mov eax, [emit_length_low]
    stosd
    mov eax, [emit_length_high]
    stosd
    mov eax, [emit_type]
    stosd
    xor eax, eax
    stosd
    inc bp
    ret
.emit_fail:
    stc
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

    mov eax, BOOT_V1_MAGIC
    mov ebx, BOOT_V1_BUFFER
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
remaining dw 0
chunk_sectors dw 0
load_segment dw 0
kernel_copied_bytes dd 0
current_lba_low dd 0
current_lba_high dd 0
range_start dd 0
range_end dd 0
e820_input_token dd 0
e820_returned_size dd 0
handoff_covered db 0
map_base_low dd 0
map_base_high dd 0
map_length_low dd 0
map_length_high dd 0
map_end_low dd 0
map_end_high dd 0
previous_end_low dd 0
previous_end_high dd 0
map_type dd 0
emit_base_low dd 0
emit_base_high dd 0
emit_length_low dd 0
emit_length_high dd 0
emit_type dd 0

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
