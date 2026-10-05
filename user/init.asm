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

    ; Exit through the real syscall boundary so the kernel can prove this
    ; executable reached CPL3 and returned through the normal syscall path.
    mov rax, 8                  ; PROSENJIT_SYS_PROCESS_EXIT
    xor rdi, rdi
    int 0x80

.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
