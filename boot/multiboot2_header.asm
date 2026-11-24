; ProsenjitOS Multiboot2 header.
; This is an original boot interface layer. GRUB is used as the initial
; Multiboot2 loader while the kernel takes control and enters x86-64 mode.

BITS 32

section .multiboot
align 8
mb2_header:
    dd 0xE85250D6              ; Multiboot2 header magic
    dd 0                       ; architecture: i386
    dd mb2_header_end - mb2_header
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header))

    ; Mandatory information request: the GRUB adapter requires the mmap tag.
    ; flags=0 makes this a required request when the information is available.
    dw 1                       ; information-request header tag
    dw 0                       ; flags: required
    dd 12                      ; tag header + one requested information type
    dd 6                       ; Multiboot2 memory-map information tag
    align 8, db 0

    ; End tag
    dw 0
    dw 0
    dd 8
mb2_header_end:

section .note.GNU-stack noalloc noexec nowrite progbits
