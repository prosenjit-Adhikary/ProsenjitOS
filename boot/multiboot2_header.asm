; ProsenjitOS Multiboot2 header.
; The framebuffer tag requests a preferred 1024x768x32 graphics mode.
; GRUB may choose another compatible mode, which the kernel validates from
; the Multiboot2 framebuffer information tag.

BITS 32

section .multiboot
align 8
mb2_header:
    dd 0xE85250D6              ; Multiboot2 header magic
    dd 0                       ; architecture: i386
    dd mb2_header_end - mb2_header
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header))

    ; Preferred framebuffer: 1024x768x32.
    dw 5                       ; Multiboot2 framebuffer header tag
    dw 0                       ; flags
    dd 20                      ; size
    dd 1024                    ; width
    dd 768                     ; height
    dd 32                      ; depth

    ; End tag
    dw 0
    dw 0
    dd 8
mb2_header_end:

section .note.GNU-stack noalloc noexec nowrite progbits
