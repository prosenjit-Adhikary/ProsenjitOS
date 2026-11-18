ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;
    .text : ALIGN(4K)
    {
        __kernel_text_start = .;
        KEEP(*(.multiboot))
        *(.text*)
    }
    __kernel_text_end = ALIGN(4K);

    .user_text : ALIGN(4K)
    {
        __user_text_start = .;
        *(.user_text*)
        __user_text_end = ALIGN(4K);
    }

    .rodata : ALIGN(4K)
    {
        __kernel_rodata_start = .;
        *(.rodata*)
    }

    __kernel_rodata_end = ALIGN(4K);

    .data : ALIGN(4K)
    {
        __kernel_data_start = .;
        *(.data*)
    }
    __kernel_data_end = ALIGN(4K);

    .bss : ALIGN(4K)
    {
        __kernel_bss_start = .;
        *(COMMON)
        *(.bss*)

        /*
         * Phase 1 panic handoff reserves its emergency resources statically.
         * Keep them inside the kernel BSS image so the kernel-wide BSS mapping
         * and __kernel_start..__kernel_end physical reservation cover them.
         * The stack is 16 KiB and page-aligned; the normalized frame slot is
         * separately 16-byte aligned and is written before it is ever read.
         */
        . = ALIGN(4K);
        __panic_stack_bottom = .;
        . += 16384;
        __panic_stack_top = .;

        . = ALIGN(16);
        __panic_frame_storage = .;
        . += 128;
        __panic_frame_storage_end = .;

        __kernel_bss_end = ALIGN(4K);
    }

    . = ALIGN(4K);
    __kernel_end = .;

    /* Fail the link if the emergency panic resources drift from their ABI. */
    ASSERT((__panic_stack_bottom & 0xFFF) == 0,
           "panic stack bottom must be page-aligned")
    ASSERT((__panic_stack_top & 0xF) == 0,
           "panic stack top must be 16-byte aligned")
    ASSERT((__panic_stack_top - __panic_stack_bottom) == 16384,
           "panic stack must remain exactly 16 KiB")
    ASSERT((__panic_frame_storage & 0xF) == 0,
           "panic frame storage must be 16-byte aligned")
    ASSERT((__panic_frame_storage_end - __panic_frame_storage) == 128,
           "panic frame storage must remain exactly 128 bytes")
    ASSERT(__panic_stack_bottom >= __kernel_bss_start &&
           __panic_stack_top <= __kernel_bss_end,
           "panic stack must remain inside kernel BSS")
    ASSERT(__panic_frame_storage >= __kernel_bss_start &&
           __panic_frame_storage_end <= __kernel_bss_end,
           "panic frame storage must remain inside kernel BSS")
    ASSERT((__kernel_bss_end & 3) == 0,
           "kernel BSS extent must be dword-aligned for native startup clear")

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
