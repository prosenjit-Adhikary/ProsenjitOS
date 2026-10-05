BITS 64
default rel

section .text
global _start
_start:
    ; Real first userspace instruction: exercise the published ABI.
    mov rax, 1                  ; PROSENJIT_SYS_GET_VERSION
    int 0x80
    cmp rax, 0x0000000000010000
    jne .halt
.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
