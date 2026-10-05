BITS 64
default rel

section .text
global _start
_start:
    ; RDI is the initial capability handle granted by the kernel.
    mov r12, rdi

    ; Send a bounded SESSION_READY message through the real IPC syscall.
    lea rsi, [rsp - 8]
    mov rax, 0x54494E4953534553 ; "SESSINIT" marker, 8 bytes
    mov [rsi], rax
    mov rax, 5                  ; PROSENJIT_SYS_IPC_SEND
    mov rdi, r12
    mov rdx, 8
    int 0x80
    test rax, rax
    jnz .halt

    ; Exercise the published ABI before exiting.
    mov rax, 1                  ; PROSENJIT_SYS_GET_VERSION
    int 0x80
    cmp rax, 0x0000000000010000
    jne .halt

    ; Exit through the real process-exit syscall.
    mov rax, 8                  ; PROSENJIT_SYS_PROCESS_EXIT
    xor rdi, rdi
    int 0x80

.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
