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
    __kernel_text_end = .;

    .rodata : ALIGN(4K)
    {
        __kernel_rodata_start = .;
        *(.rodata*)
    }

    __kernel_rodata_end = .;

    .data : ALIGN(4K)
    {
        __kernel_data_start = .;
        *(.data*)
    }
    __kernel_data_end = .;

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss*)
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
