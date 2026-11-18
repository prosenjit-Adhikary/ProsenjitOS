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

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
