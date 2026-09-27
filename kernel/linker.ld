ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;
    __kernel_text_start = .;

    .text : ALIGN(4K)
    {
        KEEP(*(.multiboot))
        *(.text*)
    }
    __kernel_text_end = .;

    __kernel_rodata_start = .;

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }

    __kernel_rodata_end = .;

    __kernel_data_start = .;
    .data : ALIGN(4K)
    {
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
